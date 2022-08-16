//===-- BaggyBounds.c - Common functionality ----------------------*- C -*-===//
///
/// \file
/// Common BaggyBounds functionality
///
//===----------------------------------------------------------------------===//

#include <limits.h>

#include "fuzzalloc/Runtime/BaggyBounds.h"

const unsigned kNumTagBits = 16;
const tag_t kFuzzallocDefaultTag = 1;
const tag_t kFuzzallocTagMin = kFuzzallocDefaultTag + 1;
const tag_t kFuzzallocTagMax = (tag_t)((~0) - 1);

const unsigned kSlotSize = 16;
const size_t kMetaSize = sizeof(tag_t);

uint64_t bb_nextPow2(uint64_t X) {
  return X == 1 ? 1 : 1 << (64 - __builtin_clzl(X - 1));
}

uint64_t bb_log2(uint64_t X) {
  return ((CHAR_BIT * sizeof(uint64_t)) - 1) - __builtin_clzll(X);
}