//===-- PAMD.cpp - Padding Area MetaData ------------------------*- C++ -*-===//
///
/// \file
/// Implements the Padding Area MetaData (PAMD) approach proposed by Zhengyang
/// Liu and John Criswell in "Flexible and Efficient Memory Object Metadata"
///
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <functional>

#include "fuzzalloc/fuzzalloc.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

namespace {
static constexpr size_t kTableSize = 1UL << 43;    ///< Baggy bounds table size
static constexpr size_t kSlotSize = 16;            ///< Slot size (in bytes)
static constexpr size_t kMetaSize = sizeof(tag_t); ///< Size of metadata

static uint8_t *__BaggyBoundsTablePtr;

/// Efficiently calculate the next power-of-2 of `X`
static constexpr uint64_t nextPow2(uint64_t X) {
  return X == 1 ? 1 : 1 << (64 - __builtin_clzl(X - 1));
}

/// Efficiently calculate log2 of `X`
static constexpr uint64_t log2(uint64_t X) {
  return ((CHAR_BIT * sizeof(uint64_t)) - 1) - __builtin_clzll(X);
}

/// Initialize the baggy bounds table
static void initBaggyBounds() {
  static bool Initialized = false;
  if (likely(Initialized)) {
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

/// Calculate an allocation size
static constexpr uint64_t calculateAllocSize(size_t Size) {
  size_t AdjustedSize = Size + kMetaSize;
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  return nextPow2(AdjustedSize);
}

/// Register an allocated memory object.
///
/// Based on Algorithm 1 on the PAMD paper.
static void registerMemoryObject(tag_t Tag, void *Obj, size_t AllocSize) {
  initBaggyBounds();

  if (!Obj || !AllocSize) {
    return;
  }

  const auto P = (uintptr_t)Obj;
  const auto SlotSize = log2(kSlotSize);
  const auto Index = P >> SlotSize;
  const auto E = log2(AllocSize);
  assert(E >= SlotSize);
  const auto Range = 1 << (E - SlotSize);
  memset(__BaggyBoundsTablePtr + Index, E, Range);

  auto *TagAddr = (tag_t*)(P + AllocSize - kMetaSize);
  *TagAddr = Tag;
}

static void unregisterMemoryObject(void *Obj) {
  initBaggyBounds();

  const auto P = (uintptr_t)Obj;
  const auto SlotSize = log2(kSlotSize);
  const auto Index = P >> SlotSize;
  const auto AllocSize = __BaggyBoundsTablePtr[Index];
  if (AllocSize != 0) {
    const auto Range = 1 << (AllocSize - SlotSize);
    memset(__BaggyBoundsTablePtr + Index, 0, Range);
  }
}
} // anonymous namespace

//
// External API
//

extern "C" void __bb_free(void *Ptr) {
  unregisterMemoryObject(Ptr);
  free(Ptr);
}

extern "C" void *__bb_malloc(tag_t Tag, size_t Size) {
  auto AllocSize = calculateAllocSize(Size);
  void *Ptr = nullptr;
  posix_memalign(&Ptr, AllocSize, AllocSize);
  registerMemoryObject(Tag, Ptr, AllocSize);
  return Ptr;
}

extern "C" void *__bb_calloc(tag_t Tag, size_t NMemb, size_t Size) {
  void *Ptr = __bb_malloc(Tag, NMemb * Size);
  if (Ptr) {
    bzero(Ptr, NMemb * Size);
  }
  return Ptr;
}

extern "C" void *__bb_realloc(tag_t Tag, void *Ptr, size_t Size) {
  if (!Ptr) {
    return __bb_malloc(Tag, Size);
  }
  if (!Size) {
    free(Ptr);
    return Ptr;
  }

  auto *NewPtr = __bb_malloc(Tag, Size);
  // TODO memcpy data
  __bb_free(Ptr);
  return NewPtr;
}

extern "C" tag_t __bb_lookup(void *Ptr) {
  const auto P = (uintptr_t)Ptr;
  const auto Index = P >> log2(kSlotSize);
  const auto E = __BaggyBoundsTablePtr[Index];
  if (!E) {
    return kFuzzallocDefaultTag;
  }
  const auto AllocSize = 1 << E;
  const auto Base = P & ~(AllocSize - 1);

  auto *TagAddr = (tag_t*)(Base + AllocSize - kMetaSize);
  return *TagAddr;
}

extern "C" void *malloc(size_t Size) {
  return __bb_malloc(kFuzzallocDefaultTag, Size);
}

extern "C" void *calloc(size_t NMemb, size_t Size) {
  return __bb_calloc(kFuzzallocDefaultTag, NMemb, Size);
}

extern "C" void *realloc(void *Ptr, size_t Size) {
  return __bb_realloc(kFuzzallocDefaultTag, Ptr, Size);
}