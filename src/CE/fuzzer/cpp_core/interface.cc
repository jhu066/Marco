#include <unordered_map>
#include <unordered_set>
#include <stdio.h>
#include "util.h"
#include "ctpl.h"
#include "union_table.h"
#include "rgd_op.h"
#include "queue.h"
#include "proto/brctuples.pb.h"
#include <z3++.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <iostream>
#include <fstream>
#include <fcntl.h>           /* For O_* constants */
#include <sys/stat.h>        /* For mode constants */
#include <semaphore.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <glob.h>

#define B_FLIPPED 0x1
#define THREAD_POOL_SIZE 1
#define XXH_STATIC_LINKING_ONLY   /* access advanced declarations */
#define XXH_IMPLEMENTATION
#include "xxhash.h"
//global variables

// addconstr::WholeTrace new_trace;
static std::atomic<uint64_t> fid;      // depth of the current branch
static std::atomic<uint64_t> ce_count; // output file id
int named_pipe_fd;
std::ifstream pcsetpipe;

bool SAVING_WHOLE;

XXH32_hash_t call_stack_hash_;
static uint32_t max_label_;
uint32_t dump_tree_id_;

static z3::context __z3_context;
static z3::solver __z3_solver(__z3_context, "QF_BV");
static const dfsan_label kInitializingLabel = -1;
static uint32_t max_label_per_session = 0;
sem_t * semagra;
sem_t * semace;
sem_t * semafzr;

uint8_t BRC_MODE = 0;
int count_extra_cons = 0;
uint32_t total_symb_brc = 0;
uint64_t total_time = 0;
uint64_t total_solving_time = 0;
uint64_t total_reload_time = 0;
uint64_t total_rebuild_time = 0;
uint64_t total_updateG_time = 0;
uint64_t total_extra_time = 0;
uint64_t total_getdeps_cost = 0;
uint32_t total_pruned_ones = 0;

// uint64_t init_count = 0;
uint64_t untaken_update_ifsat = 0; // carry the pathprefix of untaken branch, in case of sat nested solving, mark it;
std::string input_file = "/outroot/tmp/cur_input_2";

static dfsan_label_info *__union_table;

struct RGDSolution {
    std::unordered_map<uint32_t, uint8_t> sol;
  //the intended branch for this solution
    uint32_t fid;  //the seed
    uint64_t addr;
    uint64_t ctx;
    uint32_t order;
};

moodycamel::ConcurrentQueue<RGDSolution> solution_queue;


// dependencies
struct dedup_hash {
  std::size_t operator()(const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &operand) const {
    return std::hash<uint64_t>{}(std::get<0>(operand))^
            std::hash<uint64_t>{}(std::get<1>(operand))^
            std::hash<uint64_t>{}(std::get<2>(operand))^
            std::hash<uint32_t>{}(std::get<3>(operand));
  }
};

struct dedup_equal {
  bool operator()(const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &lhs, const std::tuple<uint64_t,uint64_t,uint64_t,uint32_t> &rhs) const {
    return std::get<0>(lhs) == std::get<0>(rhs) &&
          std::get<1>(lhs) == std::get<1>(rhs) &&
          std::get<2>(lhs) == std::get<2>(rhs) &&
          std::get<3>(lhs) == std::get<3>(rhs);
  }
};

static std::unordered_set<std::tuple<uint64_t, uint64_t, uint64_t, uint32_t>, dedup_hash, dedup_equal> fmemcmp_dedup;

static std::unordered_set<uint32_t> visited_;

std::unordered_map<uint32_t,z3::expr> expr_cache;
std::unordered_map<uint32_t,std::unordered_set<uint32_t>> deps_cache;

// dependencies
struct expr_hash {
  std::size_t operator()(const z3::expr &expr) const {
    return expr.hash();
  }
};
struct expr_equal {
  bool operator()(const z3::expr &lhs, const z3::expr &rhs) const {
    return lhs.id() == rhs.id();
  }
};
typedef std::unordered_set<z3::expr, expr_hash, expr_equal> expr_set_t;

struct labeltuple_hash {
  std::size_t operator()(const std::tuple<uint32_t, uint32_t> &x) const {
    return std::get<0>(x) ^ std::get<1>(x);
  }
};

typedef std::unordered_set<std::tuple<uint32_t, uint32_t>, labeltuple_hash> labeltuple_set_t;

typedef struct {
  std::unordered_set<dfsan_label> input_deps;
  labeltuple_set_t label_tuples;
} branch_dep_t;

static std::vector<branch_dep_t*> *__branch_deps;

static inline dfsan_label_info* get_label_info(dfsan_label label) {
  return &__union_table[label];
}

static inline branch_dep_t* get_branch_dep(size_t n) {
  if (n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  return __branch_deps->at(n);
}

static inline void set_branch_dep(size_t n, branch_dep_t* dep) {
  if (n >= __branch_deps->size()) {
    __branch_deps->resize(n + 1);
  }
  __branch_deps->at(n) = dep;
}

static z3::expr get_cmd(z3::expr const &lhs, z3::expr const &rhs, uint32_t predicate) {
  switch (predicate) {
    case DFSAN_BVEQ:  return lhs == rhs;
    case DFSAN_BVNEQ: return lhs != rhs;
    case DFSAN_BVUGT: return z3::ugt(lhs, rhs);
    case DFSAN_BVUGE: return z3::uge(lhs, rhs);
    case DFSAN_BVULT: return z3::ult(lhs, rhs);
    case DFSAN_BVULE: return z3::ule(lhs, rhs);
    case DFSAN_BVSGT: return lhs > rhs;
    case DFSAN_BVSGE: return lhs >= rhs;
    case DFSAN_BVSLT: return lhs < rhs;
    case DFSAN_BVSLE: return lhs <= rhs;
    default:
      printf("FATAL: unsupported predicate: %u\n", predicate);
      // throw z3::exception("unsupported predicate");
      break;
  }
  // should never reach here
  //Die();
}

static inline z3::expr cache_expr(dfsan_label label, z3::expr const &e, std::unordered_set<uint32_t> &deps) {
  if (label != 0)  {
    expr_cache.insert({label,e});
    deps_cache.insert({label,deps});
  }
  return e;
}

// iteratively get all input deps of the current label
static void get_input_deps(dfsan_label label, std::unordered_set<uint32_t> &deps) {
  if (label < CONST_OFFSET || label == kInitializingLabel) {
    throw z3::exception("invalid label");
  }
  if (label > max_label_per_session) {
    max_label_per_session = label;
  }

  dfsan_label_info *info = get_label_info(label);
  if (info->depth > 500) {
    // printf("WARNING: tree depth too large: %d\n", info->depth);
    throw z3::exception("tree too deep");
  }

  // special ops
  if (info->op == 0) {
    deps.insert(info->op1);
    return;
  } else if (info->op == DFSAN_LOAD) {
    uint64_t offset = get_label_info(info->l1)->op1;
    deps.insert(offset);
    for (uint32_t i = 1; i < info->l2; i++) {
      deps.insert(offset + i);
    }
    return;
  } else if (info->op == DFSAN_ZEXT || info->op == DFSAN_SEXT || info->op == DFSAN_TRUNC || info->op == DFSAN_EXTRACT) {
    get_input_deps(info->l1, deps);
    return;
  } else if (info->op == DFSAN_NOT || info->op == DFSAN_NEG) {
    get_input_deps(info->l2, deps);
    return;
  }

  // common ops
  if (info->l1 >= CONST_OFFSET) {
    get_input_deps(info->l1, deps);
  }
  if (info->l2 >= CONST_OFFSET) {
    std::unordered_set<uint32_t> deps2;
    get_input_deps(info->l2, deps2);
    deps.insert(deps2.begin(),deps2.end());
  }
  return;
}

// get the extra [label, dir] in the prefix for completing the nested set;
std::string get_extra_tuple(dfsan_label label, uint32_t tkdir, int ifmemorize) {
  std::string res = "";
  // deps of the label alone
  std::unordered_set<dfsan_label> inputs;
  try {
    uint64_t t_getdep = getTimeStamp();
    get_input_deps(label, inputs);
    total_getdeps_cost += (getTimeStamp() - t_getdep);

    // collect additional input deps
    std::vector<dfsan_label> worklist;
    worklist.insert(worklist.begin(), inputs.begin(), inputs.end());
    while (!worklist.empty()) {
      auto off = worklist.back();
      worklist.pop_back();

      auto deps = get_branch_dep(off);
      if (deps != nullptr) {
        for (auto i : deps->input_deps) {
          if (inputs.insert(i).second)
            worklist.push_back(i);
        }
      }
    }
    count_extra_cons = inputs.size();

    // get tuples from branches with deps in inputs
    if (inputs.size() == 0) return res;

    if (ifmemorize) {
      // addconstr::Extracon* extra_con = new_trace.add_extracon();
      // extra_con->set_id(label);

      labeltuple_set_t added;
      for (auto off : inputs) {
        auto deps = get_branch_dep(off);
        if (deps != nullptr) {
          for (auto &expr : deps->label_tuples) {
            if (added.insert(expr).second) {
              res += (std::to_string(std::get<0>(expr)) + "," + std::to_string(std::get<1>(expr)) + ".");
              // addconstr::Extracon::LabelTuple* new_tuple = extra_con->add_labeltuples();
              // new_tuple->set_eid(std::get<0>(expr));
              // new_tuple->set_edir(std::get<1>(expr));
            }
          }
        }
      }
    } else {
      total_pruned_ones += 1;
    }


    // update the labellist of each in inputs
    for (auto off : inputs) {
      auto c = get_branch_dep(off);
      if (c == nullptr) {
        c = new branch_dep_t();
        if (c == nullptr) {
          printf("WARNING: out of memory\n");
        } else {
          set_branch_dep(off, c);
          c->input_deps.insert(inputs.begin(), inputs.end());
          c->label_tuples.insert(std::make_tuple(label, tkdir));
        }
      } else {
        c->input_deps.insert(inputs.begin(), inputs.end());
        c->label_tuples.insert(std::make_tuple(label, tkdir));
      }
    }
  } catch (z3::exception e) {
    printf("WARNING: solving error: %s\n", e.msg());
    return res;
  }
  return res;
}

static z3::expr serialize(dfsan_label label, std::unordered_set<uint32_t> &deps) {
  if (label < CONST_OFFSET || label == kInitializingLabel) {
    // printf("WARNING: invalid label: %d\n", label);
    throw z3::exception("invalid label");
  }

  if (label > max_label_per_session) {
    max_label_per_session = label;
  }


  dfsan_label_info *info = get_label_info(label);

  if (info->depth > 500) {
    // printf("WARNING: tree depth too large: %d\n", info->depth);
    throw z3::exception("tree too deep");
  }

  auto itr_expr = expr_cache.find(label);
  auto itr_deps = deps_cache.find(label);
  if (label !=0 && itr_expr != expr_cache.end() && itr_deps != deps_cache.end() ) {
    deps.insert(itr_deps->second.begin(), itr_deps->second.end());
    return itr_expr->second;
  }

  // special ops
  if (info->op == 0) {
    // input
    z3::symbol symbol = __z3_context.int_symbol(info->op1);
    z3::sort sort = __z3_context.bv_sort(8);
    info->tree_size = 1; // lazy init
    deps.insert(info->op1);
    // caching is not super helpful
    return __z3_context.constant(symbol, sort);
  } else if (info->op == DFSAN_LOAD) {
    uint64_t offset = get_label_info(info->l1)->op1;
    z3::symbol symbol = __z3_context.int_symbol(offset);
    z3::sort sort = __z3_context.bv_sort(8);
    z3::expr out = __z3_context.constant(symbol, sort);
    deps.insert(offset);
    for (uint32_t i = 1; i < info->l2; i++) {
      symbol = __z3_context.int_symbol(offset + i);
      out = z3::concat(__z3_context.constant(symbol, sort), out);
      deps.insert(offset + i);
    }
    info->tree_size = 1; // lazy init
    return cache_expr(label, out, deps);
  } else if (info->op == DFSAN_ZEXT) {
    z3::expr base = serialize(info->l1, deps);
    if (base.is_bool()) // dirty hack since llvm lacks bool
      base = z3::ite(base, __z3_context.bv_val(1, 1),
          __z3_context.bv_val(0, 1));
    uint32_t base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, z3::zext(base, info->size - base_size), deps);
  } else if (info->op == DFSAN_SEXT) {
    z3::expr base = serialize(info->l1, deps);
    if (base.is_bool()) // dirty hack since llvm lacks bool
      base = z3::ite(base, __z3_context.bv_val(1, 1),
          __z3_context.bv_val(0, 1));
    uint32_t base_size = base.get_sort().bv_size();
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, z3::sext(base, info->size - base_size), deps);
  } else if (info->op == DFSAN_TRUNC) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, base.extract(info->size - 1, 0), deps);
  } else if (info->op == DFSAN_EXTRACT) {
    z3::expr base = serialize(info->l1, deps);
    info->tree_size = get_label_info(info->l1)->tree_size; // lazy init
    return cache_expr(label, base.extract((info->op2 + info->size) - 1, info->op2), deps);
  } else if (info->op == DFSAN_NOT) {
    // if (info->l2 == 0 || info->size != 1) {
    //   throw z3::exception("invalid Not operation");
    // }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    // if (!e.is_bool()) {
    //   throw z3::exception("Only LNot should be recorded");
    // }
    return cache_expr(label, !e, deps);
  } else if (info->op == DFSAN_NEG) {
    // if (info->l2 == 0) {
    //   throw z3::exception("invalid Neg predicate");
    // }
    z3::expr e = serialize(info->l2, deps);
    info->tree_size = get_label_info(info->l2)->tree_size; // lazy init
    return cache_expr(label, -e, deps);
  }
  // common ops
  uint8_t size = info->size;
  // size for concat is a bit complicated ...
  if (info->op == DFSAN_CONCAT && info->l1 == 0) {
    assert(info->l2 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l2)->size;
  }
  z3::expr op1 = __z3_context.bv_val((uint64_t)info->op1, size);
  if (info->l1 >= CONST_OFFSET) {
    op1 = serialize(info->l1, deps).simplify();
  } else if (info->size == 1) {
    op1 = __z3_context.bool_val(info->op1 == 1);
  }
  if (info->op == DFSAN_CONCAT && info->l2 == 0) {
    assert(info->l1 >= CONST_OFFSET);
    size = info->size - get_label_info(info->l1)->size;
  }
  z3::expr op2 = __z3_context.bv_val((uint64_t)info->op2, size);
  if (info->l2 >= CONST_OFFSET) {
    std::unordered_set<uint32_t> deps2;
    op2 = serialize(info->l2, deps2).simplify();
    deps.insert(deps2.begin(),deps2.end());
  } else if (info->size == 1) {
    op2 = __z3_context.bool_val(info->op2 == 1); }
  // update tree_size
  info->tree_size = get_label_info(info->l1)->tree_size +
    get_label_info(info->l2)->tree_size;

  switch((info->op & 0xff)) {
    // llvm doesn't distinguish between logical and bitwise and/or/xor
    case DFSAN_AND:     return cache_expr(label, info->size != 1 ? (op1 & op2) : (op1 && op2), deps);
    case DFSAN_OR:      return cache_expr(label, info->size != 1 ? (op1 | op2) : (op1 || op2), deps);
    case DFSAN_XOR:     return cache_expr(label, op1 ^ op2, deps);
    case DFSAN_SHL:     return cache_expr(label, z3::shl(op1, op2), deps);
    case DFSAN_LSHR:    return cache_expr(label, z3::lshr(op1, op2), deps);
    case DFSAN_ASHR:    return cache_expr(label, z3::ashr(op1, op2), deps);
    case DFSAN_ADD:     return cache_expr(label, op1 + op2, deps);
    case DFSAN_SUB:     return cache_expr(label, op1 - op2, deps);
    case DFSAN_MUL:     return cache_expr(label, op1 * op2, deps);
    case DFSAN_UDIV:    return cache_expr(label, z3::udiv(op1, op2), deps);
    case DFSAN_SDIV:    return cache_expr(label, op1 / op2, deps);
    case DFSAN_UREM:    return cache_expr(label, z3::urem(op1, op2), deps);
    case DFSAN_SREM:    return cache_expr(label, z3::srem(op1, op2), deps);
                  // relational
    case DFSAN_ICMP:    return cache_expr(label, get_cmd(op1, op2, info->op >> 8), deps);
                  // concat
    case DFSAN_CONCAT:  return cache_expr(label, z3::concat(op2, op1), deps); // little endian
    default:
                  printf("FATAL: unsupported op: %u\n", info->op);
                  // throw z3::exception("unsupported operator");
                  break;
  }
  // should never reach here
  //Die();
}

void init(bool saving_whole) {
  SAVING_WHOLE = saving_whole;
  __branch_deps = new std::vector<branch_dep_t*>(100000, nullptr);
}

void cleanup1();
void cleanup2();
int cleanup_deps();
bool check_pp(uint64_t digest);
void mark_pp(uint64_t digest);

static void generate_solution(z3::model &m, std::unordered_map<uint32_t, uint8_t> &solu) {
  unsigned num_constants = m.num_consts();
  for(unsigned i = 0; i< num_constants; i++) {
    z3::func_decl decl = m.get_const_decl(i);
    z3::expr e = m.get_const_interp(decl);
    z3::symbol name = decl.name();
    if(name.kind() == Z3_INT_SYMBOL) {
      uint8_t value = (uint8_t)e.get_numeral_int();
      solu[name.to_int()] = value;
    }
    if (name.kind() == Z3_STRING_SYMBOL) {
      int index = std::stoi(name.str().substr(2)) / 10;
      uint8_t value = (uint8_t)e.get_numeral_int();
      solu[index] = value;
    }
  }
}


// int build_nested_set(int extra, uint32_t label, uint32_t conc_dir, std::string src_tscs, std::string deps_file) {
//   std::string entry;
//   uint32_t e_label;
//   uint32_t e_dir;

//   int token_index = 0;
//   size_t pos1 = 0;
//   size_t pos2 = 0;

//   // get the opt set first.
//   z3::expr result = __z3_context.bool_val(conc_dir);
//   std::unordered_map<uint32_t, uint8_t> opt_sol;
//   std::unordered_map<uint32_t, uint8_t> sol;

//   try {
//     std::unordered_set<dfsan_label> inputs;
//     z3::expr cond = serialize(label, inputs);

//     __z3_solver.reset();
//     __z3_solver.add(cond != result);
//     z3::check_result res = __z3_solver.check();
//     // check if opt set is sat
//     if (res == z3::sat) {
//       z3::model m_opt = __z3_solver.get_model();
//       __z3_solver.push();

//       // collect additional constraints
//       if (extra == 1) {
//         addconstr::WholeTrace new_trace1;
//         std::fstream input(deps_file, std::ios::in | std::ios::binary);
//         if (!new_trace1.ParseFromIstream(&input)) {
//           fprintf(stderr, "[build_nested_set]: cannot open file to write: %s\n", deps_file.c_str());
//           return -1;
//         }
//         for (int i = 0; i < new_trace1.extracon_size(); i++) {
//           const addconstr::Extracon& brc = new_trace1.extracon(i);
//           if (brc.id() == label) {
//             // std::cout << "found record: " << brc.id() << "=" << label << std::endl;
//             for (int j = 0; j < brc.labeltuples_size(); j++) {
//                 const addconstr::Extracon::LabelTuple& label_tuple = brc.labeltuples(j);
//                 // std::cout << label_tuple.eid() << "," <<label_tuple.edir() << std::endl;

//                 std::unordered_set<dfsan_label> e_inputs;
//                 z3::expr e_cond = serialize(label_tuple.eid(), e_inputs);
//                 z3::expr e_result = __z3_context.bool_val(label_tuple.edir());
//                 __z3_solver.add(e_cond == e_result);
//             }
//             break;
//           }
//         }
//         // nested done
//         res = __z3_solver.check();
//       }

//       if (res == z3::sat) {
//         mark_pp(untaken_update_ifsat);
//         z3::model m = __z3_solver.get_model();
//         // std::cout << "PCset1: \n" << __z3_solver.to_smt2().c_str() << std::endl;
//         sol.clear();
//         generate_solution(m, sol);
//         generate_input(sol, src_tscs, "./fifo", ce_count+=1);
//         std::cout << "(nested)new file id " << ce_count << std::endl;
//         return 1;
//       } else {
//         __z3_solver.pop();
//         // std::cout << "PCset1: \n" << __z3_solver.to_smt2().c_str() << std::endl;
//         opt_sol.clear();
//         generate_solution(m_opt, opt_sol);
//         generate_input(opt_sol, src_tscs, "./fifo", ce_count+=1);
//         std::cout << "(opt)new file id " << ce_count << std::endl;

//         return 2; // optimistic sat
//       }
//     } else {
//       mark_pp(untaken_update_ifsat);
//       return 0;
//     }
//   } catch (z3::exception e) {
//     printf("WARNING: solving error: %s\n", e.msg());
//     return 0;
//   }
// }

int build_nested_set_old(std::string extra, uint32_t label, uint32_t conc_dir, std::string src_tscs) {
  std::string entry;
  uint32_t e_label;
  uint32_t e_dir;

  int token_index = 0;
  size_t pos1 = 0;
  size_t pos2 = 0;

  // get the opt set first.
  z3::expr result = __z3_context.bool_val(conc_dir);
  std::unordered_map<uint32_t, uint8_t> opt_sol;
  std::unordered_map<uint32_t, uint8_t> sol;

  std::cout << "build_nested_set_old 576" << std::endl;

  try {
    std::unordered_set<dfsan_label> inputs;
    z3::expr cond = serialize(label, inputs);

    __z3_solver.reset();
    __z3_solver.add(cond != result);
    z3::check_result res = __z3_solver.check();
    // check if opt set is sat
    if (res == z3::sat) {
      std::cout << "build_nested_set_old: opt sat" << std::endl;
      z3::model m_opt = __z3_solver.get_model();
      __z3_solver.push();

      // collect additional constraints
      while ((pos1 = extra.find("#")) != std::string::npos) {
        entry = extra.substr(0, pos1);
        if ((pos2 = entry.find(".")) != std::string::npos) {
          e_label = stoul(entry.substr(0, pos2));
          entry.erase(0, pos2+1);
          e_dir = stoul(entry);
          std::unordered_set<dfsan_label> e_inputs;
          z3::expr e_cond = serialize(e_label, e_inputs);
          z3::expr e_result = __z3_context.bool_val(e_dir);
          __z3_solver.add(e_cond == e_result);
        }
        extra.erase(0, pos1 + 1);
      }
      std::cout << "build_nested_set_old: nested set building done" << std::endl;
      // nested done
      res = __z3_solver.check();
      if (res == z3::sat) {
        std::cout << "build_nested_set_old: nested sat" << std::endl;
        mark_pp(untaken_update_ifsat);
        z3::model m = __z3_solver.get_model();
        sol.clear();
        generate_solution(m, sol);
        generate_input(sol, src_tscs, "./fifo", ce_count+=1);
        std::cout << "(nested)new file id " << ce_count << std::endl;
        return 1;
      } else {
        std::cout << "build_nested_set_old: nested unsat" << std::endl;
        __z3_solver.pop();
        opt_sol.clear();
        generate_solution(m_opt, opt_sol);
        generate_input(opt_sol, src_tscs, "./fifo", ce_count+=1);
        std::cout << "(opt)new file id " << ce_count << std::endl;
        return 2; // optimistic sat
      }
    } else {
      std::cout << "unsat solving; quick escape" << std::endl;
      // unsat
      mark_pp(untaken_update_ifsat);
      return 0;
    }
  } catch (z3::exception e) {
    printf("WARNING: solving error: %s\n", e.msg());
    return 0;
  }
}

int gen_solve_pc(uint32_t queueid, uint32_t tree_id, uint32_t label, uint32_t conc_dir, uint32_t cur_label_loc, std::string extra) {
  struct stat st;
  size_t sread;
  FILE *fp;
  int res = 1;
  uint64_t one_start = getTimeStamp();

  std::string tree_idstr = std::to_string(tree_id % 1000000);
  std::string tree_file = "./tree" + std::to_string(queueid) + "/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  std::cout << "qid = " << queueid << std::endl;
  std::string src_tscs;
  if (queueid == 0) {
    src_tscs = "./afl-slave/queue/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr + "*";
    glob_t globbuf;
    glob(src_tscs.c_str(), 0, NULL, &globbuf);
    if (globbuf.gl_pathc > 0) {
      src_tscs = globbuf.gl_pathv[0];
    }
    globfree(&globbuf);

  } else if (queueid == 1) {
    src_tscs = "./fifo/queue/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  }
  std::cout << "src_tscs: " << src_tscs << std::endl;
  std::string deps_file = "./deps/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;

  // prep1: reinstate tree
  stat(tree_file.c_str(), &st);
  sread = st.st_size;

  // tree size -1 is max label_
  max_label_ = sread / sizeof(dfsan_label_info) - 1; // 1st being 0
  std::cout << "tree size (label count) is " << max_label_ << std::endl;

  fp = fopen(tree_file.c_str(), "rb");
  fread(__union_table, sread, 1, fp);
  fclose(fp);

  total_reload_time += (getTimeStamp() - one_start);

  // prep2: reset the max label tracker; upper bound is new max_label_
  max_label_per_session = 0;

  // gen and solve new PC set
  // res = build_nested_set(extra, label, conc_dir, src_tscs, deps_file); // protobuf version
  res = build_nested_set_old(extra, label, conc_dir, src_tscs); // string conversion

  // clean up after solving
  cleanup1(); // reset the memory in union table
  max_label_per_session = 0;
  int dele = cleanup_deps();

  return res;
}


int generate_next_tscs(std::ifstream &pcsetpipe) {
  std::string line;
  std::string token;
  std::string extra;

  uint32_t queueid;
  uint32_t tree_id;
  uint32_t node_id;
  uint32_t conc_dir;
  uint32_t cur_label_loc;

  size_t pos = 0;

  z3::context ctx;
  z3::solver solver(ctx, "QF_BV");

  while (1) {
    if (std::getline(pcsetpipe, line)) {
      int token_index = 0;
      std::cout << "line: " << line << std::endl;
      while ((pos = line.find(",")) != std::string::npos) {
          token = line.substr(0, pos);
          switch (token_index) {
              case 0: queueid = stoul(token); break;
              case 1: tree_id = stoul(token); break;
              case 2: node_id = stoul(token); break;
              case 3: conc_dir = stoul(token); break;
              case 4: cur_label_loc = stoul(token); break;
              case 5: untaken_update_ifsat = stoull(token); break;
              case 6: extra = token; break;
              default: break;
          }
          line.erase(0, pos + 1);
          token_index++;
      }

      if (!BRC_MODE && !check_pp(untaken_update_ifsat)) {
        std::cout << "dup pp, skip!" << std::endl;
        return -1; // skip it, query next one!
      }
      return gen_solve_pc(queueid, tree_id, node_id, conc_dir, cur_label_loc, extra);
    }
  }
}

#if 1
const int pfxkMapSize  = 1<<27;
uint8_t pfx_pp_map[pfxkMapSize];
uint16_t node_map[pfxkMapSize];
const int kMapSize = 1 << 16;
uint8_t pp_map[kMapSize];
uint8_t context_map_[kMapSize];
uint8_t virgin_map_[kMapSize];
uint8_t trace_map_[kMapSize];
uint32_t prev_loc_ = 0;
#endif

bool check_pp(uint64_t digest) {
  uint32_t hash = digest % (pfxkMapSize * CHAR_BIT);
  uint32_t idx = hash / CHAR_BIT;
  uint32_t mask = 1 << (hash % CHAR_BIT);
  return (pfx_pp_map[idx] & mask) == 0;
}

void mark_pp(uint64_t digest) {
  uint32_t hash = digest % (pfxkMapSize * CHAR_BIT);
  uint32_t idx = hash / CHAR_BIT;
  uint32_t mask = 1 << (hash % CHAR_BIT);
  pfx_pp_map[idx] |= mask;
}

// addr, ctx, tkdir, qid, tscsid, label => pipe to scheduler
static int update_graph(dfsan_label label, uint64_t pc, uint32_t tkdir,
    bool try_solve, uint32_t inputid, uint32_t queueid, int uniq_pcset, int ifmemorize) {

    // if concrete branch , skip
    if (!label) return 0;
    if ((get_label_info(label)->flags & B_FLIPPED)) return 0;

    // mark this one off for branch within this trace
    get_label_info(label)->flags |= B_FLIPPED;

    // proceed to update graph, pipe the record to python scheduler
    std::string record;

    // if filtered by policy, or already picked it for this trace, or pruned, update visit of concrete branch only
    if(!try_solve && uniq_pcset == 0) {

      record = std::to_string(pc) \
               + "-" + std::to_string(call_stack_hash_) \
               + "-" + std::to_string(tkdir) \
               + "-" + std::to_string(label) \
               + "-" + std::to_string(inputid) \
               + "-" + std::to_string(queueid) \
               + "@none@@\n";
    } else {
      if (get_label_info(label)->tree_size > 50000) {
        return 1;
      }
      if (get_label_info(label)->depth > 500) {
        return 1;
      }

      uint64_t t_extra = getTimeStamp();
      std::string res = get_extra_tuple(label, tkdir, ifmemorize);
      total_extra_time += (getTimeStamp() - t_extra);

      record = std::to_string(pc) \
               + "-" + std::to_string(call_stack_hash_) \
               + "-" + std::to_string(tkdir) \
               + "-" + std::to_string(label) \
               + "-" + std::to_string(inputid) \
               + "-" + std::to_string(queueid) \
               + "@" \
               + std::to_string(uniq_pcset) \
               + "-" + std::to_string(queueid) \
               + "-" + std::to_string(untaken_update_ifsat) \
               + "-" + std::to_string(get_label_info(label)->depth) \
               + "#" + res \
               + "@@\n";
    }
    write(named_pipe_fd, record.c_str(), strlen(record.c_str()));
    fsync(named_pipe_fd);

    return 0;
}

// for bb pruning
const int kBitmapSize = 65536;
const int kStride = 8;
uint16_t bitmap_[kBitmapSize];
bool is_interesting_ = false ;

// borrow from qsym code for bb pruning
static bool isPowerOfTwo(uint16_t x) {
    return (x & (x - 1)) == 0;
}

void computeHash(uint32_t ctx) { // call stack context hashing
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &ctx, sizeof(ctx));
    call_stack_hash_ = XXH32_digest(&state);
}

void updateBitmap(void *last_pc_, uint64_t ctx) {
    // Lazy update the bitmap when symbolic operation is happened

    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &last_pc_, sizeof(last_pc_));
    XXH32_update(&state, &ctx, sizeof(uint64_t));

    uint32_t h = XXH32_digest(&state);
    uint32_t index = h % kBitmapSize;

    // Use strided exponential backoff, which is interesting if the strided
    // bitmap meets exponential requirements. For example, {0, 1, 2, ..., 7}
    // maps to 0, {8, ..., 15} maps to 1, and so on. {0, 1, 2, ..., 7} is
    // interesting because it maps to 0, which is in the {0, 1, 2, 4, ...}.
    // But {24, ... 31} is not, because it maps to 3.
    is_interesting_ = isPowerOfTwo(bitmap_[index] / kStride);
    bitmap_[index]++;
}

//check if we need to solve a branch given
// labe: if 0 concreate
// addr: branch address
// output: true: solve the constraints false: don't solve the constraints
bool bcount_filter(uint64_t addr, uint64_t ctx, uint64_t direction, uint32_t order) {
  std::tuple<uint64_t,uint64_t, uint64_t, uint32_t> key{addr,ctx,direction,order};
  if (fmemcmp_dedup.find(key) != fmemcmp_dedup.end()) {
    return false;
  } else {
    fmemcmp_dedup.insert(key);
    return true;
  }
}
inline bool isPowerofTwoOrZero(uint32_t x) {
  return ((x & (x - 1)) == 0);
}

XXH32_hash_t hashPc(uint64_t pc, bool taken) {
  XXH32_state_t state;
  XXH32_reset(&state, 0);
  XXH32_update(&state, &pc, sizeof(pc));
  XXH32_update(&state, &taken, sizeof(taken));
  return XXH32_digest(&state) % kMapSize;
}

uint32_t getIndex(uint32_t h) {
  return ((prev_loc_ >> 1) ^ h) % kMapSize;
}

bool isInterestingContext(uint32_t h, uint32_t bits) {
  bool interesting = false;
  if (!isPowerofTwoOrZero(bits))
    return false;
  for (auto it = visited_.begin();
      it != visited_.end();
      it++) {
    uint32_t prev_h = *it;

    // Calculate hash(prev_h || h)
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &prev_h, sizeof(prev_h));
    XXH32_update(&state, &h, sizeof(h));

    uint32_t hash = XXH32_digest(&state) % (kMapSize * CHAR_BIT);
    uint32_t idx = hash / CHAR_BIT;
    uint32_t mask = 1 << (hash % CHAR_BIT);

    if ((context_map_[idx] & mask) == 0) {
      context_map_[idx] |= mask;
      interesting = true;
    }
  }

  if (bits == 0)
    visited_.insert(h);

  return interesting;
}

//roll in branch
uint64_t roll_in_pp(uint32_t label, uint64_t addr, uint64_t direction,
    XXH64_state_t* path_prefix) {

  //address
  XXH64_state_t tmp;
  XXH64_reset(&tmp, 0);

  // roll in pc first
  XXH64_update(path_prefix, &addr, sizeof(addr));

  // roll in: ifconcrete and direction;
  uint8_t deter = 0;
  if (label == 0)  {
    deter = 1;
    XXH64_update(path_prefix, &deter, sizeof(deter));
    XXH64_update(path_prefix, &direction, sizeof(direction));
    return 0;
  }

  // if this is a symbolic branch, validate if shall solve
  uint64_t direction_sym = 1 - direction;

  //digest
  uint64_t taken_digest;
  uint64_t untaken_digest;

  // roll in branch state: ifsymbolic
  XXH64_update(path_prefix, &deter, sizeof(deter));
  XXH64_copyState(&tmp,path_prefix);

  // for untaken branch, calculate the hash
  XXH64_update(&tmp, &direction_sym, sizeof(direction_sym));
  untaken_digest = XXH64_digest(&tmp);

  // for taken branch, calculate the hash
  XXH64_update(path_prefix, &direction, sizeof(direction));
  taken_digest = XXH64_digest(path_prefix);

  // mark the taken branch for visited
  mark_pp(taken_digest);
  // std::cout << "tk: " << taken_digest << " utk: " << untaken_digest << std::endl;
  return untaken_digest;
}



bool isInterestingPathPrefix(uint64_t pc, bool taken, uint32_t label, XXH64_state_t* path_prefix) {
  // reset value for every new query
  untaken_update_ifsat = 0;
  // acquire untaken_hash, mark the taken_hash
  uint64_t untaken_digest = roll_in_pp(pc,label,taken,path_prefix);
  // done rolling in the branch state and pc, if concrete, return false directly
  if (!label) return false;
  // if symbolic and untaken_hash is touched already, return false
  if (!check_pp(untaken_digest)) return false;
  // else it's fresh, solve it, return true
  untaken_update_ifsat = untaken_digest;
  return true;

}

int isInterestingNode(uint64_t pc, bool taken, uint64_t ctx) {
    XXH32_state_t state;
    XXH32_reset(&state, 0);
    XXH32_update(&state, &pc, sizeof(pc));
    XXH32_update(&state, &ctx, sizeof(ctx));
    XXH32_update(&state, &taken, sizeof(taken));

    uint32_t h = XXH32_digest(&state);
    uint32_t index = h % pfxkMapSize;

    int res = 0;

    node_map[index]++;
    // if visit count is power of 2, nested-solve it; otherwise, opt-solve it.
    // add the miss-of-3 case.
    if ((!(node_map[index] & (node_map[index]-1)))) {
      res = 1;
    }
    // if (node_map[index] == 3) {
    //   res = 1;
    // }

    return res;
}

bool isInterestingBranch(uint64_t pc, bool taken, uint64_t ctx) {

  // here do the bb pruning:
  updateBitmap((void *)pc, ctx);
  if (!is_interesting_) return false; // if pruned, do't proceed anymore, treat this brc as concrete basically.

  uint32_t h = hashPc(pc, taken);
  uint32_t idx = getIndex(h);
  bool new_context = isInterestingContext(h, virgin_map_[idx]);
  bool ret = true;

  virgin_map_[idx]++;

  if ((virgin_map_[idx] | trace_map_[idx]) != trace_map_[idx]) {
    uint32_t inv_h = hashPc(pc, !taken);
    uint32_t inv_idx = getIndex(inv_h);

    trace_map_[idx] |= virgin_map_[idx];

    // mark the inverse case, because it's already covered by current testcase
    virgin_map_[inv_idx]++;

    trace_map_[inv_idx] |= virgin_map_[inv_idx];

    virgin_map_[inv_idx]--;
    ret = true;
  }
  else if (new_context) {
    ret = true;
  }
  else
    ret = false;

  prev_loc_ = h;
  return ret;
}

// dump the tree and flush the union table to get ready for the solving request
void generate_tree_dump(int qid) {
  std::string tree_idstr = std::to_string(dump_tree_id_ % 1000000);
  std::string output_file = "./tree" + std::to_string(qid) + "/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  size_t swrite;
  FILE *fp;
  if ((fp = fopen(output_file.c_str(), "wb")) == NULL) {
    fprintf(stderr, "[generate_tree_dump]1: cannot open file to write: %s\n", output_file.c_str());
    return;
  }

  std::cout << "max_label_ = " << max_label_ << ", max_label_per_session = " << max_label_per_session << std::endl;

  swrite = fwrite((void *)__union_table, sizeof(dfsan_label_info), max_label_+1, fp);
  if (swrite != (max_label_+1)) {
    fprintf(stderr, "[generate_tree_dump]1: write error %d\n", swrite);
  }
  fclose(fp);

  // generate deps protobuf dump
  // std::string output_file1 = "./deps/id:" + std::string(6-tree_idstr.size(),'0') + tree_idstr;
  // std::fstream output(output_file1, std::ios::out | std::ios::trunc | std::ios::binary);
  // if (!new_trace.SerializeToOstream(&output)) {
  //     fprintf(stderr, "[generate_tree_dump]2: cannot open file to write: %s\n", output_file1.c_str());
  //     return;
  // }
  // std::cout << "entry in this trace before:" << new_trace.extracon_size() << std::endl;
  // new_trace.mutable_extracon()->DeleteSubrange(0, new_trace.extracon_size());
  // std::cout << "entry in this trace after: " << new_trace.extracon_size() << std::endl;

}

void handle_fmemcmp(uint8_t* data, uint64_t index, uint32_t size, uint32_t tid, uint64_t addr) {
  std::unordered_map<uint32_t, uint8_t> rgd_solution;
  for(uint32_t i=0;i<size;i++) {
    //rgd_solution[(uint32_t)index+i] = (uint8_t) (data & 0xff);
    rgd_solution[(uint32_t)index+i] = data[i];
    //data = data >> 8 ;
  }
  if (SAVING_WHOLE) {
    generate_input(rgd_solution, input_file, "./ce_output", fid++);
  }
  else {
    RGDSolution sol = {rgd_solution, tid, addr, 0, 0};
    solution_queue.enqueue(sol);
  }
}

// clean up the union table
void cleanup1() {
  if (max_label_per_session > max_label_) {
    std::cout << "warning! max_label_per_session > max_label_"
              << ", max_label_per_session=" << max_label_per_session
              << ", max_label_=" << max_label_
              << std::endl;
  }
  // in this trace, max_label_ is passed from the other end
  for(int i = 0; i <= max_label_; i++) {
    dfsan_label_info* info = get_label_info(i);
    memset(info, 0, sizeof(dfsan_label_info));
  }
  // shmdt(__union_table);
  max_label_ = 0;
  max_label_per_session = 0;
}

void cleanup2() {
  std::cout << "cleanup2(max_label_per_session): "
          << " max_label_per_session=" << max_label_per_session
          << " max_label_=" << max_label_ << std::endl;

  // in this trace, max_label_ is passed from the other end
  for(int i = 0; i <= max_label_per_session; i++) {
    dfsan_label_info* info = get_label_info(i);
    memset(info, 0, sizeof(dfsan_label_info));
  }
  // shmdt(__union_table);
  max_label_per_session = 0;
}

int cleanup_deps() {
  expr_cache.clear();
  deps_cache.clear();
  int count = 0;
  for (int i = 0 ; i < __branch_deps->size(); i++) {
    branch_dep_t* slot =  __branch_deps->at(i);
    if (slot) {
      count += 1;
      delete slot;
      __branch_deps->at(i) = nullptr;
    }
  }
  return count;
}


uint32_t solve(int shmid, uint32_t pipeid, uint32_t brc_flip, std::ifstream &pcsetpipe) {

  std::ifstream myfile;
  myfile.open("/tmp/wp2");

  __union_table = (dfsan_label_info*)shmat(shmid, nullptr, 0);
  if (__union_table == (void*)(-1)) {
    printf("error %s\n",strerror(errno));
    return 0;
  }

  memset(virgin_map_, 0, kMapSize);
  memset(node_map, 0, pfxkMapSize * sizeof(uint16_t)); // a per trace bitmap, for localvis bucketization pruning;
  prev_loc_ = 0;
  std::string line;
  size_t pos = 0;
  std::string token;
  std::string delimiter = ",";
  uint32_t maxlabel = 0;
  uint32_t tid = 0;
  uint32_t qid = 0;
  uint32_t label = 0;
  uint32_t direction = 0;
  uint64_t addr = 0;
  uint64_t ctx  = 0;
  uint32_t order = 0;
  //constratint type: conditional, gep, add_constraints
  uint32_t cons_type = 0;

  uint32_t filtered_count = 0;
  //create global state for one session
  XXH64_state_t path_prefix;
  XXH64_reset(&path_prefix,0);
  uint64_t acc_time = 0;
  uint64_t one_start = getTimeStamp();
  bool skip_rest = false;
  while (std::getline(myfile, line))
  {
    int token_index = 0;
    while ((pos = line.find(delimiter)) != std::string::npos) {
      token = line.substr(0, pos);

      switch (token_index) {
        case 0: qid = stoul(token); break;  // queue id; for sage usage
        case 1: label = stoul(token); break;  // index
        case 2: direction  = stoul(token); break;
        case 3: addr  = stoull(token); break;
        case 4: ctx = stoull(token); break;
        case 5: order = stoul(token); break;
        case 6: cons_type = stoul(token); break;
        case 7: // testcase id
                tid = stoul(token);
                dump_tree_id_ = tid;
                break;
        case 8: // the maximum entry count in the union table
                max_label_ = stoull(token);
                break;
        default: break;
      }
      line.erase(0, pos + delimiter.length());
      token_index++;
    }
    std::unordered_map<uint32_t, uint8_t> sol;
    std::unordered_map<uint32_t, uint8_t> opt_sol;

    if (skip_rest) continue;

    computeHash(ctx); // get the call_stack_hash_ ready

    if (cons_type == 0) {
      bool try_solve = false;
      int uniq_pcset = 0;
      int ifmemorize = 0;
      switch (brc_flip) {
        case 0: // using qsym style branch filter
          BRC_MODE = 1;
          if (label) {
            try_solve = isInterestingBranch(addr, direction, ctx);
            if (try_solve) uniq_pcset = 1;
          }
          break;
        case 1: // using pathprefix branch filter
          BRC_MODE = 0;
          try_solve = isInterestingPathPrefix(addr, direction, label, &path_prefix);
          if (isInterestingBranch(addr, direction, ctx)) uniq_pcset = 1; // promote it in the PC queue
          if (isInterestingNode(addr, direction, ctx)) ifmemorize = 1; // pruning by local visitcount
          break;
      }
      if (try_solve) {
        filtered_count++;
      }
      uint64_t t_update = getTimeStamp();
      update_graph(label, addr, direction, try_solve, tid, qid, uniq_pcset, ifmemorize);
      total_updateG_time += (getTimeStamp() - t_update);
    }
    else if (cons_type == 2) {
      if (std::getline(myfile,line)) {
        uint32_t memcmp_datasize = label;
        uint8_t data[1024];
        int token_index = 0;
        while ((pos = line.find(delimiter)) != std::string::npos) {
          token = line.substr(0,pos);
          data[token_index++] = stoul(token);
          line.erase(0, pos + delimiter.length());
        }
        data[token_index++] = stoul(line);
        bool try_solve = bcount_filter(addr, ctx, 0, order);
        std::cout << "going for handle_fmemcmp branch" << std::endl;
        if (try_solve)
          handle_fmemcmp(data, direction, label, tid, addr);
      } else {
        break;
      }
    }
    acc_time = getTimeStamp() - one_start; // time spent on one single seed
    if (acc_time > 30000000) skip_rest = true; // 10s timeout per seed
  }
  std::cout << "end of one input" << std::endl;
  // end of one input
  total_symb_brc += filtered_count;

  fid = 0; // reset for next input seed
  // at the end of each execution, dump tree and flush everything of the running seed
  generate_tree_dump(qid);

  cleanup1(); // flush the union table of the running seed
  cleanup_deps(); // flush the dependency tree

  acc_time = getTimeStamp() - one_start;
  total_time += acc_time;

  if (skip_rest) std::cout << "timeout!" << std::endl;
  if (ce_count % 50 == 0) {
    std::cout << "total uniqpp PC count= " << total_symb_brc
              << "\ncur total_pruned_ones = " << total_pruned_ones
              << "\ntotal (exec;no solving)cost  " << total_time / 1000  << "ms"
              << "\ntotal updateG time " << total_updateG_time / 1000  << "ms"
              << "\ntotal tupling time " << total_extra_time / 1000  << "ms"
              << "\ntotal getdeps time " << total_getdeps_cost / 1000  << "ms"
              << "\ncur (exec;no solving)cost: " << acc_time / 1000  << "ms"
              << "\ntotal reload time " << total_reload_time / 1000  << "ms"
              << "\ntotal solving(reload included) time " << total_solving_time / 1000  << "ms"
              << std::endl;
  }
  return tid;
}

extern "C" {
  void init_core(bool saving_whole, uint32_t initial_count) {
    init(saving_whole);
    named_pipe_fd = open("/tmp/pcpipe", O_WRONLY);
    pcsetpipe.open("/tmp/myfifo");

    ce_count = -1;
    // init_count =  initial_count - 1;

    printf("the length of union_table is %u\n", 0xC00000000/sizeof(dfsan_label_info));
    __z3_solver.set("timeout", 1000U);
    memset(pfx_pp_map, 0, pfxkMapSize);
    memset(node_map, 0, pfxkMapSize * sizeof(uint16_t));
    memset(pp_map, 0, kMapSize);
    memset(trace_map_, 0, kMapSize);
    memset(context_map_, 0, kMapSize);
    memset(bitmap_, 0, kBitmapSize * sizeof(uint16_t));
  }

  uint32_t run_solver(int shmid, uint32_t pipeid, uint32_t brc_flip, uint32_t lastone) {
    // reset for every new episode
    max_label_ = 0;
    uint32_t cur_inid = solve(shmid, pipeid, brc_flip, pcsetpipe);
    // make sure there's one seed generated at the end of this execution
    uint64_t one_start;
    int res;
    std::cout << "cur_inid=" << cur_inid << std::endl;
    std::cout << "run_solver:lastone=" << lastone << std::endl;
    std::string endtoken;
    if (!lastone) {
      endtoken = "END@@\n"; // still consuming tscs
      write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
      fsync(named_pipe_fd);
      std::cout << "pending seeds exist, no solving attempt!" << std::endl;
      return 0;
    }
    endtoken = "ENDFIN@@\n"; // final one, prompt a decision
    write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
    fsync(named_pipe_fd);
    std::cout << "no pending seeds exist,prompt one!" << std::endl;

    // retry until a new seed is produced
    while (1) {
      one_start = getTimeStamp();
      res = generate_next_tscs(pcsetpipe);
      total_solving_time += (getTimeStamp() - one_start);
      if (res > 0) { // with outcome, CE will pick up new seed to run
        std::cout << "new outcome, move on to sync new batch" << std::endl;
        shmdt(__union_table); // reset for next epi
        endtoken = "ENDNEW@@\n";  // a new seed is generated!
        write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
        fsync(named_pipe_fd);
        break;
      } else if (res == -1) {
        endtoken = "ENDDUP@@\n";
        write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
        fsync(named_pipe_fd);
        std::cout << "failure solving, DUP!" << std::endl;
      } else {
        endtoken = "ENDUNSAT@@\n";
        write(named_pipe_fd, endtoken.c_str(), strlen(endtoken.c_str()));
        fsync(named_pipe_fd);
        std::cout << "failure solving, UNSAT!" << std::endl;
      }
    }
    return 0;
  }

  void wait_ce() {
    sem_wait(semace);
  }

  void post_gra() {
    sem_post(semagra);
  }

  void post_fzr() {
    sem_post(semafzr);
  }

};
