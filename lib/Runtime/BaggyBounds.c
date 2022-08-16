//===-- BaggyBounds.c - BaggyBounds metadata ----------------------*- C -*-===//
///
/// \file
/// Implements the Padding Area MetaData (PAMD) approach proposed by Zhengyang
/// Liu and John Criswell in "Flexible and Efficient Memory Object Metadata".
/// This approach is based on baggy bounds.
///
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

static const unsigned kSlotSizeLog2 = 4;
static const size_t kTableSize = 1UL << 43; ///< Baggy bounds table size

static uint8_t *__baggy_bounds_table;

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
  return bb_nextPow2(AdjustedSize);
}

/// Register an allocated memory object.
///
/// Based on Algorithm 1 on the PAMD paper.
void __bb_register(void *Obj, size_t AllocSize) {
  initBaggyBounds();

  if (!Obj || !AllocSize) {
    return;
  }

  const uintptr_t P = (uintptr_t)Obj;
  const uintptr_t Index = P >> kSlotSize;
  const uint64_t E = bb_log2(AllocSize);
  assert(E >= kSlotSize);
  const size_t Range = 1 << (E - kSlotSize);
  memset(__baggy_bounds_table + Index, E, Range);
}

static void unregisterMemoryObject(void *Obj) {
  initBaggyBounds();

  const uintptr_t P = (uintptr_t)Obj;
  const uintptr_t Index = P >> kSlotSize;
  const unsigned AllocSize = __baggy_bounds_table[Index];
  if (AllocSize != 0) {
    const size_t Range = 1 << (AllocSize - kSlotSize);
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
  size_t AllocSize = calculateAllocSize(Size);
  void *Ptr = NULL;
  posix_memalign(&Ptr, AllocSize, AllocSize);
  __bb_register(Ptr, AllocSize);
  tag_t *TagAddr = (tag_t *)((uintptr_t)Ptr + AllocSize - kMetaSize);
  *TagAddr = Tag;
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

  const uintptr_t OldIndex = (uintptr_t)Ptr >> kSlotSize;
  const unsigned OldE = __baggy_bounds_table[OldIndex];
  const size_t OldSize = 1 << OldE;

  const uintptr_t NewIndex = (uintptr_t)NewPtr >> kSlotSize;
  const unsigned NewE = __baggy_bounds_table[NewIndex];
  const size_t NewSize = 1 << NewE;

  if (NewSize > OldSize) {
    memcpy(NewPtr, Ptr, OldSize);
  } else {
    memcpy(NewPtr, Ptr, NewSize);
  }

  __bb_free(Ptr);
  return NewPtr;
}

tag_t __bb_lookup(void *Ptr, uintptr_t *Base) {
  if (!Ptr) {
    *Base = 0;
    return kFuzzallocDefaultTag;
  }

  const uintptr_t P = (uintptr_t)Ptr;
  const uintptr_t Index = P >> kSlotSizeLog2;
  const unsigned E = __baggy_bounds_table[Index];
  if (!E) {
    *Base = 0;
    return kFuzzallocDefaultTag;
  }

  const size_t AllocSize = 1 << E;
  *Base = P & ~(AllocSize - 1);

  tag_t *TagAddr = (tag_t *)(*Base + AllocSize - kMetaSize);
  return *TagAddr;
}

void __bb_dbg_use(void *Ptr, size_t Size) {
  uintptr_t Base;
  tag_t Tag = __bb_lookup(Ptr, &Base);
  ptrdiff_t Offset = (uintptr_t)Ptr - Base;

  fprintf(stderr,
          "[datAFLow] accessing def site 0x%" PRIx16
          " from %p (offset=%td, size=%zu)\n",
          Tag, __builtin_return_address(0), Offset, Size);
}

void *malloc(size_t Size) { return __bb_malloc(kFuzzallocDefaultTag, Size); }

void *calloc(size_t NMemb, size_t Size) {
  return __bb_calloc(kFuzzallocDefaultTag, NMemb, Size);
}

void *realloc(void *Ptr, size_t Size) {
  return __bb_realloc(kFuzzallocDefaultTag, Ptr, Size);
}