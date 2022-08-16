//===-- Hash.c - AFL coverage hash --------------------------------*- C -*-===//
///
/// \file
/// Hash function for updating the AFL coverage map
///
//===----------------------------------------------------------------------===//

#include <stdio.h>

#include "fuzzalloc/Runtime/Hash.h"
#include "fuzzalloc/fuzzalloc.h"

// AFL++ headers
#define XXH_INLINE_ALL
#include "xxhash.h"
#undef XXH_INLINE_ALL

#ifdef NDEBUG
__attribute__((always_inline))
#else
__attribute__((noinline))
#endif
HASH_T
__afl_hash(tag_t Tag, size_t Offset) {
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);
  uint64_t Data[] = {Tag, Offset, Use};
  HASH_T Hash = XXH3_64bits(Data, sizeof(Data));
#ifdef _DEBUG
  fprintf(stderr,
          "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ", 0x%" PRIx64
          ") -> 0x%" HASH_PRI "\n",
          Tag, Offset, Use, Hash);
#endif
  return Hash;
}