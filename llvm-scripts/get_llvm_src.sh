#!/bin/bash
#
# This script will place everything in the current working directory

set -ex

# Get LLVM
git clone -b release/12.x --single-branch --depth 1 https://github.com/llvm/llvm-project.git
mv -f llvm-project/{.[!.],}* .
