//===-- fuzzalloc.h - fuzzalloc interface -------------------------*- C -*-===//
///
/// \file
/// A memory allocator for data-flow fuzzing
///
//===----------------------------------------------------------------------===//

#ifndef FUZZALLOC_H
#define FUZZALLOC_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef uint16_t tag_t;             ///< Tag type
#define kNumTagBits UINT16_WIDTH    ///< Number of bits in a tag
#define kFuzzallocTagMax UINT16_MAX ///< The maximum tag value
#define PRIxTag PRIx16              ///< Print format specifier for tags (hex)
#define PRIuTag PRIu16              ///< Print format specifier for tags (int)

#define kFuzzallocDefaultTag (0) ///< The default tag (for uninstrumented code)
#define kFuzzallocTagMin (kFuzzallocDefaultTag + 1) ///< The minimum tag value

#endif // FUZZALLOC_H
