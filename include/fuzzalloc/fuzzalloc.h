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
extern const unsigned kNumTagBits;

/// The default def site tag. Used by default for non-instrumented code
extern const tag_t kFuzzallocDefaultTag;

/// The default minimum tag value
extern const tag_t kFuzzallocTagMin;

/// The default maximum tag value
extern const tag_t kFuzzallocTagMax;

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // FUZZALLOC_H
