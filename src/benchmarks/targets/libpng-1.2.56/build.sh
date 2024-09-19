#!/bin/bash
MODE=${1}
CODENAME="libpng-1.2.56"

ROOTDIR="/data/src/benchmarks/targets"
MODENAME="${MODE}_targets"
TARG_DIR="$ROOTDIR/$CODENAME/$MODENAME"
rm -rf "$TARG_DIR"
mkdir -p "$TARG_DIR"

apt-get install -y \
    make \
    autoconf \
    automake \
    libtool \
    wget \
    zlib1g-dev

make distclean


if [ "$MODE" == "ce" ]; then
    export CC="/data/src/CE/bin/ko-clang"
    export CXX="/data/src/CE/bin/ko-clang++"
    export KO_CC="clang-6.0"
    export KO_CXX="clang++-6.0"
    export KO_DONT_OPTIMIZE=1
fi 

if [ "$MODE" == "cov" ]; then
    export CC=clang-6.0
    export CXX=clang++-6.0
    export CFLAGS="-fsanitize-coverage=edge,no-prune,trace-pc-guard -fsanitize=address"
    export CXXFLAGS="-fsanitize-coverage=edge,no-prune,trace-pc-guard -fsanitize=address"
fi 


# build lib
./configure --disable-shared
make -j

rm /driver.o
rm /driver.a
rm /usr/lib/libFuzzingEngine.a

# build driver
KO_DONT_OPTIMIZE=1 $CC -c $ROOTDIR/driver/StandaloneFuzzTargetMain.c -fPIC -o /driver.o
ar r /driver.a /driver.o
cp /driver.a /usr/lib/libFuzzingEngine.a

# export FUZZER_LIB=/driver.a

# build targ prog
$CXX -std=c++11 "$ROOTDIR/$CODENAME/prep/target.cc"  .libs/libpng12.a /usr/lib/libFuzzingEngine.a -I . -lz  -o "$ROOTDIR/$CODENAME/$MODENAME/libpng_read_fuzzer"
