#!/bin/bash
#
# This script will update the LLVM structure in the current working directory

set -ex

THIS_DIR=$(dirname $(readlink -f ${0}))
SRC=${THIS_DIR}/../src
RT_SRC=${SRC}/compiler-rt-files/
INC=${SRC}/include

LLVM=$(pwd)
RT_DEST=${LLVM}/compiler-rt

ln -sf ${RT_SRC}/lib/asan/asan_allocator.cpp ${RT_DEST}/lib/asan/asan_allocator.cpp
ln -sf ${RT_SRC}/lib/asan/asan_allocator.h ${RT_DEST}/lib/asan/asan_allocator.h
ln -sf ${RT_SRC}/lib/asan/asan_interceptors.cpp ${RT_DEST}/lib/asan/asan_interceptors.cpp
ln -sf ${RT_SRC}/lib/asan/asan_malloc_linux.cpp ${RT_DEST}/lib/asan/asan_malloc_linux.cpp
ln -sf ${RT_SRC}/lib/asan/asan_mapping.h ${RT_DEST}/lib/asan/asan_mapping.h
ln -sf ${RT_SRC}/lib/asan/CMakeLists.txt ${RT_DEST}/lib/asan/CMakeLists.txt
ln -sf ${RT_SRC}/lib/asan/tests/CMakeLists.txt ${RT_DEST}/lib/asan/tests/CMakeLists.txt

ln -sf ${RT_SRC}/lib/sanitizer_common/sanitizer_allocator.h ${RT_DEST}/lib/sanitizer_common/sanitizer_allocator.h
ln -sf ${RT_SRC}/lib/sanitizer_common/sanitizer_fuzzalloc_allocator.h ${RT_DEST}/lib/sanitizer_common/sanitizer_fuzzalloc_allocator.h

ln -sf ${RT_SRC}/test/asan/CMakeLists.txt ${RT_DEST}/test/asan/CMakeLists.txt
ln -sf ${RT_SRC}/test/asan/lit.site.cfg.py.in ${RT_DEST}/test/asan/lit.site.cfg.py.in
ln -sf ${RT_SRC}/test/asan/lit.cfg.py ${RT_DEST}/test/asan/lit.cfg.py

ln -sf ${INC}/fuzzalloc.h ${RT_DEST}/lib/sanitizer_common/fuzzalloc.h
ln -sf ${INC}/fuzzalloc.h ${RT_DEST}/lib/fuzzer/fuzzalloc.h
