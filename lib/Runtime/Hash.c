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
#include "fuzzalloc/fuzzalloc.h"

// AFL++ headers
#include "config.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

extern uint8_t *__afl_area_ptr;

/// Update AFL coverage bitmap
static inline void __afl_update_cov(tag_t Idx) {
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

void __afl_hash_def_use(tag_t UseTag, void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t *DefTag = __bb_lookup(Ptr, &Base, sizeof(tag_t));
  tag_t Hash = 0;

  if (likely(*DefTag != kFuzzallocDefaultTag)) {
    // Compute the hash
    Hash = (*DefTag - kFuzzallocDefaultTag) ^ UseTag;

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(def=0x%" PRIx16 ", use=0x%" PRIx64 ") -> %" PRIuTag
            "\n",
            DefTag, UseTag, Hash);
#endif
  }

  __afl_update_cov(Hash);
}

void __afl_hash_def_use_offset(tag_t UseTag, void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t *DefTag = __bb_lookup(Ptr, &Base, sizeof(tag_t));
  tag_t Hash = 0;

  if (likely(*DefTag != kFuzzallocDefaultTag)) {
    // Compute the hash
    const ptrdiff_t Offset = (uintptr_t)Ptr - Base;
    Hash = (*DefTag - kFuzzallocDefaultTag) ^ (UseTag + Offset);

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(def=0x%" PRIx16 ", use=0x%" PRIx64
            ", offset=%" PRIu64 ") -> %" PRIuTag "\n",
            DefTag, UseTag, Offset, Hash);
#endif
  }

  __afl_update_cov(Hash);
}

void __afl_hash_def_use_value(tag_t UseTag, void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t *DefTag = __bb_lookup(Ptr, &Base, sizeof(tag_t));
  tag_t Hash = 0;

  if (likely(*DefTag != kFuzzallocDefaultTag)) {
    // Compute the hash
    const ptrdiff_t Offset = (uintptr_t)Ptr - Base;

    Hash = (*DefTag - kFuzzallocDefaultTag) ^ (UseTag + Offset);
    if (MAP_SIZE_POW2 > 16) {
      Hash <<= 4;
    }
    for (uint8_t *I = Ptr, *End = Ptr + Size; I < End; ++I) {
      Hash ^= *I;
    }

#ifdef _DEBUG
    fprintf(stderr,
            "[datAFLow] hash(def=0x%" PRIx16 ", use=0x%" PRIx64
            ", offset=%" PRIu64 ", obj=%p, size=%zu) -> %" PRIuTag "\n",
            DefTag, UseTag, Offset, Ptr, Size, Hash);
#endif
  }

  __afl_update_cov(Hash);
}
