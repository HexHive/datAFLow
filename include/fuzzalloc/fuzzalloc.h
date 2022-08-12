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

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

/// Number of bits in a tag
#define kNumTagBits (16)

/// Tag type
typedef uint16_t tag_t;

/// The default def site tag. Used by default for non-instrumented code
#define kFuzzallocDefaultTag (1)

/// The default minimum tag value
#define kFuzzallocTagMin (kFuzzallocDefaultTag + 1)

/// The default maximum tag value
#define kFuzzallocTagMax ((tag_t)((~0) - 1))

void __bb_free(void *);
void *__bb_malloc(tag_t, size_t);
void *__bb_calloc(tag_t, size_t, size_t);
void *__bb_realloc(tag_t, void *, size_t);
tag_t __bb_lookup(void *);

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // FUZZALLOC_H
