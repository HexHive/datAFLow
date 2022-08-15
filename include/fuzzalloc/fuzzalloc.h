//===-- fuzzalloc.h - fuzzalloc interface -------------------------*- C -*-===//
///
/// \file
/// A memory allocator for data-flow fuzzing
///
//===----------------------------------------------------------------------===//

#ifndef FUZZALLOC_H
#define FUZZALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

/// Tag type
typedef uint16_t tag_t;

/// Number of bits in a tag
const unsigned kNumTagBits = 16;

/// The default def site tag. Used by default for non-instrumented code
const tag_t kFuzzallocDefaultTag = 1;

/// The default minimum tag value
const tag_t kFuzzallocTagMin = kFuzzallocDefaultTag + 1;

/// The default maximum tag value
const tag_t kFuzzallocTagMax = (tag_t)((~0) - 1);

// Adapted from http://c-faq.com/lib/randrange.html
#define RAND(x, y) ((tag_t)((x) + random() / (RAND_MAX / ((y) - (x) + 1) + 1)))

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // FUZZALLOC_H
