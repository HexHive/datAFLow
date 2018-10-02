#!/bin/bash

set -ex

ANGORA_DIR=$(realpath ./angora)

git clone https://github.com/AngoraFuzzer/angora $ANGORA_DIR

# Get LLVM
export PREFIX="$ANGORA_DIR/angora_llvm"
mkdir $PREFIX
#export LINUX_VER="ubuntu-18.04"
#export LLVM_VER="7.0.1"
$ANGORA_DIR/build/install_llvm.sh

# Build Angora
export PATH=$PREFIX/clang+llvm/bin:$PATH
export LD_LIBRARY_PATH=$PREFIX/clang+llvm/lib:$LD_LIBRARY_PATH
unset PREFIX
(cd $ANGORA_DIR && ./build/build.sh)

echo "Now export PATH=$ANGORA_DIR/angora_llvm/clang+llvm/bin:\$PATH and LD_LIBRARY_PATH=$PREFIX/clang+llvm/lib:\$LD_LIBRARY_PATH"
echo "angora-clang located in $ANGORA_DIR/bin"
