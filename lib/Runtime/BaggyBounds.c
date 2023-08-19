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
static bool Initialized = false;

/// Initialize the baggy bounds table
static void initBaggyBounds() {
#ifdef _DEBUG
  fprintf(stderr, "[datAFLow] Initializing baggy-bounds table\n");
#endif

  __baggy_bounds_table =
      (uint8_t *)mmap(0, kTableSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (__baggy_bounds_table == MAP_FAILED) {
    fprintf(stderr, "[datAFLow] mmap failed: %s\n", strerror(errno));
    abort();
  }

  Initialized = true;
}

/// Calculate an allocation size
static uint64_t calculateAllocSize(size_t Size, size_t MetaSize) {
  size_t AdjustedSize = Size + MetaSize;
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  return bb_nextPow2(AdjustedSize);
}

/// Register an allocated memory object.
///
/// Based on Algorithm 1 on the PAMD paper.
void __bb_register(void *Obj, size_t AllocSize) {
  if (unlikely(!Initialized)) {
    initBaggyBounds();
  }

  if (!Obj || !AllocSize) {
    return;
  }

  const uintptr_t P = (uintptr_t)Obj;
  const uintptr_t Index = P >> kSlotSizeLog2;
  const uint64_t E = bb_log2(AllocSize);
  assert(E >= kSlotSizeLog2);
  const size_t Range = 1 << (E - kSlotSizeLog2);
  memset(__baggy_bounds_table + Index, E, Range);
}

void __bb_deregister(void *Obj) {
  if (unlikely(!Initialized)) {
    initBaggyBounds();
  }

  const uintptr_t P = (uintptr_t)Obj;
  const uintptr_t Index = P >> kSlotSizeLog2;
  const unsigned AllocSize = __baggy_bounds_table[Index];
  if (AllocSize != 0) {
    const size_t Range = 1 << (AllocSize - kSlotSizeLog2);
    memset(__baggy_bounds_table + Index, 0, Range);
  }
}

//
// External API
//

void __bb_free(void *Ptr) {
  __bb_deregister(Ptr);
  free(Ptr);
}

void *__bb_malloc(tag_t Tag, size_t Size) {
  size_t AllocSize = calculateAllocSize(Size, sizeof(Tag));
  void *Ptr;
  posix_memalign(&Ptr, AllocSize, AllocSize);
  __bb_register(Ptr, AllocSize);
  tag_t *TagAddr = (tag_t *)((uintptr_t)Ptr + AllocSize - sizeof(Tag));
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

  const uintptr_t OldIndex = (uintptr_t)Ptr >> kSlotSizeLog2;
  const unsigned OldE = __baggy_bounds_table[OldIndex];
  const size_t OldSize = 1 << OldE;

  const uintptr_t NewIndex = (uintptr_t)NewPtr >> kSlotSizeLog2;
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

char *__bb_strdup(tag_t Tag, const char *S) {
  if (!S) {
    return NULL;
  }

  const size_t Len = strlen(S) + 1;
  void *Ptr = __bb_malloc(Tag, Len);
  if (Ptr) {
    memcpy(Ptr, S, Len);
  }
  return Ptr;
}

char *__bb_strndup(tag_t Tag, const char *S, size_t Size) {
  if (!S) {
    return NULL;
  }

  size_t Len = strlen(S) + 1;
  if (Len > Size) {
    Len = Size;
  }
  void *Ptr = __bb_malloc(Tag, Len);
  if (Ptr) {
    memcpy(Ptr, S, Len);
    ((char *)Ptr)[Len - 1] = '\0';
  }
  return Ptr;
}

void *__bb_lookup(void *Ptr, uintptr_t *Base, size_t MetaSize) {
  if (!Ptr) {
    *Base = 0;
    return NULL;
  }

  if (unlikely(!Initialized)) {
    initBaggyBounds();
  }

  const uintptr_t P = (uintptr_t)Ptr;
  const uintptr_t Index = P >> kSlotSizeLog2;
  const unsigned E = __baggy_bounds_table[Index];
  if (!E) {
    *Base = 0;
    return NULL;
  }

  const size_t AllocSize = 1 << E;
  *Base = P & ~(AllocSize - 1);

  return *Base + AllocSize - MetaSize;
}
