#!/bin/bash
#
# This script will update the LLVM structure in the current working directory

set -ex

THIS_DIR=$(dirname $(readlink -f ${0}))
SRC=${THIS_DIR}/../src
CLANG_SRC=${SRC}/clang-files/
INC=${SRC}/include

LLVM=$(pwd)
CLANG_DEST=${LLVM}/clang

ln -sf ${CLANG_SRC}/include/clang/Driver/Options.td ${CLANG_DEST}/include/clang/Driver/Options.td
ln -sf ${CLANG_SRC}/include/clang/Basic/CodeGenOptions.def ${CLANG_DEST}/include/clang/Basic/CodeGenOptions.def

ln -sf ${CLANG_SRC}/lib/CodeGen/CGExprConstant.cpp ${CLANG_DEST}/lib/CodeGen/CGExprConstant.cpp
ln -sf ${CLANG_SRC}/lib/Frontend/CompilerInvocation.cpp ${CLANG_DEST}/lib/Frontend/CompilerInvocation.cpp
