//===-- PAMD.c - Padding Area MetaData ----------------------------*- C -*-===//
///
/// \file
/// Implements the Padding Area MetaData (PAMD) approach proposed by Zhengyang
/// Liu and John Criswell in "Flexible and Efficient Memory Object Metadata"
///
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fuzzalloc/baggy_bounds.h"
#include "fuzzalloc/fuzzalloc.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

static const size_t kTableSize = 1UL << 43; ///< Baggy bounds table size

uint8_t *__baggy_bounds_table;

/// Efficiently calculate the next power-of-2 of `X`
static uint64_t nextPow2(uint64_t X) {
  return X == 1 ? 1 : 1 << (64 - __builtin_clzl(X - 1));
}

/// Efficiently calculate log2 of `X`
static uint64_t log2(uint64_t X) {
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

  __baggy_bounds_table =
      (uint8_t *)mmap(0, kTableSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (__baggy_bounds_table == MAP_FAILED) {
    fprintf(stderr, "mmap failed: %s\n", strerror(errno));
    abort();
  }
}

/// Calculate an allocation size
static uint64_t calculateAllocSize(size_t Size) {
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

  const uintptr_t P = (uintptr_t)Obj;
  const uint64_t SlotSize = kSlotSizeLog2;
  const uintptr_t Index = P >> SlotSize;
  const uint64_t E = log2(AllocSize);
  assert(E >= SlotSize);
  const uint64_t Range = 1 << (E - SlotSize);
  memset(__baggy_bounds_table + Index, E, Range);

  tag_t *TagAddr = (tag_t *)(P + AllocSize - kMetaSize);
  *TagAddr = Tag;
}

static void unregisterMemoryObject(void *Obj) {
  initBaggyBounds();

  const uintptr_t P = (uintptr_t)Obj;
  const uint64_t SlotSize = kSlotSizeLog2;
  const uintptr_t Index = P >> SlotSize;
  const unsigned AllocSize = __baggy_bounds_table[Index];
  if (AllocSize != 0) {
    const uint64_t Range = 1 << (AllocSize - SlotSize);
    memset(__baggy_bounds_table + Index, 0, Range);
  }
}

//
// External API
//

void __bb_free(void *Ptr) {
  unregisterMemoryObject(Ptr);
  free(Ptr);
}

void *__bb_malloc(tag_t Tag, size_t Size) {
  uint64_t AllocSize = calculateAllocSize(Size);
  void *Ptr = NULL;
  posix_memalign(&Ptr, AllocSize, AllocSize);
  registerMemoryObject(Tag, Ptr, AllocSize);
  return Ptr;
}

void *__bb_calloc(tag_t Tag, size_t NMemb, size_t Size) {
  void *Ptr = __bb_malloc(Tag, NMemb * Size);
  if (Ptr) {
    bzero(Ptr, NMemb * Size);
  }
  return Ptr;
}

void *__bb_realloc(tag_t Tag, void *Ptr, size_t Size) {
  if (!Ptr) {
    return __bb_malloc(Tag, Size);
  }
  if (!Size) {
    free(Ptr);
    return Ptr;
  }

  void *NewPtr = __bb_malloc(Tag, Size);
  // TODO memcpy data
  __bb_free(Ptr);
  return NewPtr;
}

tag_t __bb_lookup(void *Ptr, uintptr_t *Base) {
  if (!Ptr) {
    *Base = 0;
    return 0;
  }

  const uintptr_t P = (uintptr_t)Ptr;
  const uintptr_t Index = P >> kSlotSizeLog2;
  const unsigned E = __baggy_bounds_table[Index];
  if (!E) {
    *Base = 0;
    return 0;
  }

  const uint64_t AllocSize = 1 << E;
  *Base = P & ~(AllocSize - 1);

  tag_t *TagAddr = (tag_t *)(*Base + AllocSize - kMetaSize);
  return *TagAddr;
}

#ifdef _DEBUG
void __bb_dbg_use(tag_t Def, uintptr_t Offset) {
  fprintf(stderr, "[datAFLow] accessing def site %#x from %p (offset=%ld)\n", Def,
          __builtin_return_address(0), Offset);
}
#endif // _DEBUG

void *malloc(size_t Size) { return __bb_malloc(kFuzzallocDefaultTag, Size); }

void *calloc(size_t NMemb, size_t Size) {
  return __bb_calloc(kFuzzallocDefaultTag, NMemb, Size);
}

void *realloc(void *Ptr, size_t Size) {
  return __bb_realloc(kFuzzallocDefaultTag, Ptr, Size);
}