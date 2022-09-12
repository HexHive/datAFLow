//===-- BaggyBoundsMemALloc.c - BaggyBounds metadata --------------*- C -*-===//
///
/// \file
/// Wrappers around standard memory allocation routines.
///
//===----------------------------------------------------------------------===//

#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

void *malloc(size_t Size) { return __bb_malloc(kFuzzallocDefaultTag, Size); }

void *calloc(size_t NMemb, size_t Size) {
  return __bb_calloc(kFuzzallocDefaultTag, NMemb, Size);
}

void *realloc(void *Ptr, size_t Size) {
  return __bb_realloc(kFuzzallocDefaultTag, Ptr, Size);
}
