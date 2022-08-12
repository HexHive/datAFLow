#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fuzzalloc/fuzzalloc.h"

namespace {
static uint8_t *__BaggyBoundsTablePtr;
static bool Initialized = false;

static constexpr size_t kTableSize = 1UL << 43; ///< Baggy bounds table size
static constexpr size_t kSlotSize = 4;          ///< Slot size (in bytes)

uint64_t NextPow2(uint64_t X) {
  return X == 1 ? 1 : 1 << (64 - __builtin_clzl(X - 1));
}

/// Initialize the baggy bounds table
static void init() {
  if (Initialized) {
    return;
  } else {
    Initialized = true;
  }

  __BaggyBoundsTablePtr =
      (uint8_t *)mmap(0, kTableSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (__BaggyBoundsTablePtr == MAP_FAILED) {
    fprintf(stderr, "mmap failed: %s\n", strerror(errno));
    abort();
  }
}
} // anonymous namespace

extern "C" void *__tagged_malloc(tag_t Tag, size_t Size) {
  size_t AdjustedSize = Size + sizeof(Tag);
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  const uint64_t AllocSize = NextPow2(AdjustedSize);
  void *Ptr = nullptr;
  posix_memalign(&Ptr, AllocSize, AllocSize);
  if (Ptr) {
    bzero(Ptr, Size);
  }
  return Ptr;
}

extern "C" void *__tagged_calloc(tag_t Tag, size_t NMemb, size_t Size) {}

extern "C" void *__tagged_realloc(tag_t Tag, void *Ptr, size_t Size) {}

extern "C" void *malloc(size_t Size) {
  return __tagged_malloc(kFuzzallocDefaultTag, Size);
}

extern "C" void *calloc(size_t NMemb, size_t Size) {
  return __tagged_calloc(kFuzzallocDefaultTag, NMemb, Size);
}

extern "C" void *realloc(void *Ptr, size_t Size) {
  return __tagged_realloc(kFuzzallocDefaultTag, Ptr, Size);
}

extern "C" void free(void *Ptr) {}