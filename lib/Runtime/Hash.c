//===-- Hash.c - AFL coverage hash --------------------------------*- C -*-===//
///
/// \file
/// Hash function for updating the AFL coverage map
///
//===----------------------------------------------------------------------===//

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "fuzzalloc/fuzzalloc.h"

// AFL++ headers
#include "config.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#undef XXH_INLINE_ALL

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#ifdef NDEBUG
__attribute__((always_inline))
#else
__attribute__((noinline))
#endif
XXH64_hash_t
__afl_hash(tag_t Tag, size_t Offset) {
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);
  uint64_t Data[] = {Tag, Offset, Use};
  XXH64_hash_t Hash = XXH3_64bits(Data, sizeof(Data));
#ifdef _DEBUG
  fprintf(stderr,
          "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ", 0x%" PRIx64 ") -> %zu\n",
          Tag, Offset, Use, Hash);
#endif
  return Hash;
}

#ifdef NDEBUG
__attribute__((always_inline))
#else
__attribute__((noinline))
#endif
XXH64_hash_t
__afl_hash_with_val(tag_t Tag, size_t Offset, void *Val, size_t ValSize) {
  // Initialize the hash state
  static XXH64_state_t *State = NULL;
  if (unlikely(!State)) {
    State = XXH64_createState();
    if (unlikely(!State)) {
      abort();
    }
  }

  // Reset hash state
  if (unlikely(XXH64_reset(State, /*Seed=*/0) == XXH_ERROR)) {
    abort();
  }

  // Hash the def-site tag, use-site identifier, and access offset
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);
  uint64_t Data[] = {Tag, Offset, Use};

  if (unlikely(XXH64_update(State, Data, sizeof(Data)) == XXH_ERROR)) {
    abort();
  }

  // Hash the value at the use site
  if (unlikely(XXH64_update(State, Val, ValSize) == XXH_ERROR)) {
    abort();
  }

  // Produce the digest
  XXH64_hash_t Hash = XXH64_digest(State);

#ifdef _DEBUG
  fprintf(stderr,
          "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ", 0x%" PRIx64
          ", %p, %zu) -> %zu\n",
          Tag, Offset, Use, Val, ValSize, Hash);
#endif
  return Hash;
}