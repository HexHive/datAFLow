//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#ifndef FUZZALLOC_H
#define FUZZALLOC_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

/// The number of usable bits on the X86-64 architecture
#define NUM_USABLE_BITS (48)

/// Number of bits in a tag
#define NUM_TAG_BITS (16)

/// Tag shift amount
#define FUZZALLOC_TAG_SHIFT (NUM_USABLE_BITS - NUM_TAG_BITS)

/// Tag type
typedef uint16_t tag_t;

/// The default def site tag. Used by default for non-instrumented code
#define FUZZALLOC_DEFAULT_TAG (1)

/// The default minimum tag value
#define FUZZALLOC_TAG_MIN (FUZZALLOC_DEFAULT_TAG + 1)

/// Tag mask
#define FUZZALLOC_TAG_MASK ((1UL << NUM_TAG_BITS) - 1)

/// The default maximum tag value
#define FUZZALLOC_TAG_MAX 0x7FFEUL

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // FUZZALLOC_H
