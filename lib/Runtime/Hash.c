//===-- Hash.c - AFL coverage hash --------------------------------*- C -*-===//
///
/// \file
/// Hash function for updating the AFL coverage map
///
//===----------------------------------------------------------------------===//

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "fuzzalloc/Runtime/BaggyBounds.h"

// AFL++ headers
#include "config.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#undef XXH_INLINE_ALL

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

static const XXH64_hash_t kSeed = 0;

/// Hash def-use chain
static inline XXH64_hash_t __hash_def_use_common(tag_t Tag, uintptr_t Use) {
  uint64_t Data[] = {Tag, Use};
  XXH64_hash_t Hash = XXH64(Data, sizeof(Data), kSeed);
#ifdef _DEBUG
  fprintf(stderr, "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ") -> %zu\n", Tag,
          Use, Hash);
#endif
  return Hash;
}

XXH64_hash_t __afl_hash_def_use(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);

  return __hash_def_use_common(Tag, Use);
}

XXH64_hash_t __afl_hash_def_use_offset(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);

  // If this is an untagged variable, all we can do is hash the def-use chain
  // itself
  if (unlikely(Tag == kFuzzallocDefaultTag)) {
    return __hash_def_use_common(Tag, Use);
  }

  // Otherwise, we can compute the baggy bound's offset and integrate it into
  // the hash
  ptrdiff_t Offset = (uintptr_t)Ptr - Base;
  uint64_t Data[] = {Tag, Use, Offset};
  XXH64_hash_t Hash = XXH64(Data, sizeof(Data), kSeed);
#ifdef _DEBUG
  fprintf(stderr,
          "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ", 0x%" PRIx64 ") -> %zu\n",
          Tag, Offset, Use, Hash);
#endif
  return Hash;
}

XXH64_hash_t __afl_hash_def_use_value(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  uintptr_t Use = (uintptr_t)__builtin_return_address(0);

  // If this is an untagged variable, all we can do is hash the def-use chain
  // itself
  if (unlikely(Tag == kFuzzallocDefaultTag)) {
    return __hash_def_use_commong(Tag, Use);
  }

  // Otherwise, we setup a rolling hash for hashing the (variable-lengthed)
  // variable value

  // Initialize the hash state
  static XXH64_state_t *State = NULL;
  if (unlikely(!State)) {
    State = XXH64_createState();
    if (unlikely(!State)) {
      abort();
    }
  }

  // Reset hash state
  if (unlikely(XXH64_reset(State, kSeed) == XXH_ERROR)) {
    abort();
  }

  ptrdiff_t Offset = (uintptr_t)Ptr - Base;
  uint64_t Data[] = {Tag, Use, Offset};

  if (unlikely(XXH64_update(State, Data, sizeof(Data)) == XXH_ERROR)) {
    abort();
  }

  // Hash the value at the use site
  if (unlikely(XXH64_update(State, Ptr, Size) == XXH_ERROR)) {
    abort();
  }

  // Produce the digest
  XXH64_hash_t Hash = XXH64_digest(State);

#ifdef _DEBUG
  fprintf(stderr,
          "[datAFLow] hash(0x%" PRIx16 ", %" PRIu64 ", 0x%" PRIx64
          ", %p, %zu) -> %zu\n",
          Tag, Offset, Use, Ptr, Size, Hash);
#endif
  return Hash;
}