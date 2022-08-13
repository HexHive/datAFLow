//===-- baggy_bounds.h - baggy_bounds interface -------------------*- C -*-===//
///
/// \file
/// BaggyBounds constants
///
//===----------------------------------------------------------------------===//

#ifndef BAGGY_BOUNDS_H
#define BAGGY_BOUNDS_H

#include <stdint.h>

#include "fuzzalloc.h"

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

/// Slot size (in bytes)
const unsigned kSlotSize = 16;

/// Binary logarithm of slot size
const unsigned kSlotSizeLog2 = 4;

extern uint8_t *__baggy_bounds_table;

void __bb_free(void *Ptr);
void *__bb_malloc(tag_t Tag, size_t Size);
void *__bb_calloc(tag_t Tag, size_t NMemb, size_t Size);
void *__bb_realloc(tag_t Tag, void *Ptr, size_t Size);
tag_t __bb_lookup(void *Ptr);

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // BAGGY_BOUNDS_H