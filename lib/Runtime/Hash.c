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

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#if (MAP_SIZE_POW2 <= 16)
typedef uint16_t cov_idx_t;
#define PRIxHash PRIx16
#elif (MAP_SIZE_POW2 <= 32)
typedef uint32_t cov_idx_t;
#define PRIxHash PRIx32
#else
typedef uint64_t cov_idx_t;
#define PRIxHash PRIx64
#endif

extern uint8_t *__afl_area_ptr;

/// Update AFL coverage bitmap
static inline void __afl_update_cov(cov_idx_t Idx) {
  uint8_t *P = &__afl_area_ptr[Idx];
#if __GNUC__
  const uint8_t C = __builtin_add_overflow(*P, 1, P);
  *P += C;
#else
  *P += 1 + ((uint8_t)(1 + *P) == 0);
#endif
}

//
// "External" interface
//

void __afl_hash_def_use(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  cov_idx_t Hash = 0;

  if (likely(Tag != kFuzzallocDefaultTag)) {
    // Compute the hash
    const uintptr_t Use = (uintptr_t)__builtin_return_address(0);
    Hash = (Tag - kFuzzallocDefaultTag) ^ Use;

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(tag=0x%" PRIx16 ", use=%" PRIx64 ") -> %" PRIxHash
            "\n",
            Tag, Use, Hash);
#endif
  }

  __afl_update_cov(Hash);
}

void __afl_hash_def_use_offset(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  cov_idx_t Hash = 0;

  if (likely(Tag != kFuzzallocDefaultTag)) {
    // Compute the hash
    const uintptr_t Use = (uintptr_t)__builtin_return_address(0);
    const ptrdiff_t Offset = (uintptr_t)Ptr - Base;
    Hash = (Tag - kFuzzallocDefaultTag) ^ (Use + Offset);

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(tag=0x%" PRIx16 ", use=%" PRIx64
            ", offset=0x%" PRIu64 ") -> %" PRIxHash "\n",
            Tag, Use, Offset, Hash);
#endif
  }

  __afl_update_cov(Hash);
}

void __afl_hash_def_use_value(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  cov_idx_t Hash = 0;

  if (likely(Tag != kFuzzallocDefaultTag)) {
    // Compute the hash
    const uintptr_t Use = (uintptr_t)__builtin_return_address(0);
    const ptrdiff_t Offset = (uintptr_t)Ptr - Base;

    Hash = (Tag - kFuzzallocDefaultTag) ^ (Use + Offset);
    for (uint8_t *I = Ptr, *End = Ptr + Size; I < End; ++I) {
      Hash ^= *I;
    }

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(tag=0x%" PRIx16 ", use=%" PRIx64
            ", offset=0x%" PRIu64 ", obj=%p, size=%zu) -> %" PRIxHash "\n",
            Tag, Use, Offset, Ptr, Size, Hash);
#endif
  }

  __afl_update_cov(Hash);
}
