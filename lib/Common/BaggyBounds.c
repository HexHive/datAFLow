//===-- BaggyBounds.c - Common functionality ----------------------*- C -*-===//
///
/// \file
/// Common BaggyBounds functionality
///
//===----------------------------------------------------------------------===//

#include "fuzzalloc/Runtime/BaggyBounds.h"

uint64_t bb_nextPow2(uint64_t X) {
  return X == 1 ? 1 : 1 << (64 - __builtin_clzl(X - 1));
}

uint64_t bb_log2(uint64_t X) {
  return ((CHAR_BIT * sizeof(uint64_t)) - 1) - __builtin_clzll(X);
}
