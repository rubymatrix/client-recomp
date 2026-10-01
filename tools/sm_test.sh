#!/bin/sh
# Translate every shader in a folder (tests/sm_test.c) and compile each result with Apple's Metal
# compiler; prints the compile failures and a count.  usage: tools/sm_test.sh build/shaders/exe
cd "$(dirname "$0")/.." || exit 1
DIR=${1:-build/shaders/exe}
clang -O1 -I runtime/portable runtime/portable/sm_msl.c tests/sm_test.c -o build/sm_test || exit 1
build/sm_test "$DIR" | tail -20
[ -x build/metal_compile ] || clang -O1 -fobjc-arc tests/metal_compile.m -framework Metal -framework Foundation -o build/metal_compile
build/metal_compile "$DIR"/*.metal
