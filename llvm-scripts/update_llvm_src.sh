#!/bin/bash
#
# This script will update the LLVM structure in the current working directory

set -ex

THIS_DIR=$(dirname $(readlink -f ${0}))
SRC=${THIS_DIR}/../src
LLVM_SRC=${SRC}/llvm-files/
INC=${SRC}/include

LLVM=$(pwd)
LLVM_DEST=${LLVM}/llvm

ln -sf ${LLVM_SRC}/lib/Transforms/Instrumentation/AddressSanitizer.cpp ${LLVM_DEST}/lib/Transforms/Instrumentation/AddressSanitizer.cpp
ln -sf ${LLVM_SRC}/lib/Transforms/Instrumentation/CMakeLists.txt ${LLVM_DEST}/lib/Transforms/Instrumentation/CMakeLists.txt

ln -sf ${INC}/fuzzalloc.h ${LLVM_DEST}/lib/Transforms/Instrumentation/fuzzalloc.h
