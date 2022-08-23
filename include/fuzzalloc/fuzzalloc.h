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

#include "config.h" // AFL++

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

#if (MAP_SIZE_POW2 <= 16)

typedef uint16_t tag_t;             ///< Tag type
#define kNumTagBits UINT16_WIDTH    ///< Number of bits in a tag
#define kFuzzallocTagMax UINT16_MAX ///< The maximum tag value
#define PRIxTag PRIx16              ///< Print format specifier for tags (hex)
#define PRIuTag PRIu16              ///< Print format specifier for tags (int)

#elif (MAP_SIZE_POW2 <= 32)

typedef uint32_t tag_t; ///< Tag type
#define kNumTagBits UINT32_WIDTH    ///< Number of bits in a tag
#define kFuzzallocTagMax UINT32_MAX ///< The maximum tag value
#define PRIxTag PRIx32              ///< Print format specifier for tags (hex)
#define PRIuTag PRIu32              ///< Print format specifier for tags (int)

#else

typedef uint64_t tag_t; ///< Tag type
#define kNumTagBits UINT64_WIDTH    ///< Number of bits in a tag
#define kFuzzallocTagMax UINT64_MAX ///< The maximum tag value
#define PRIxTag PRIx64              ///< Print format specifier for tags (hex)
#define PRIuTag PRIu64              ///< Print format specifier for tags (int)

#endif

#define kFuzzallocDefaultTag (0) ///< The default tag (for uninstrumented code)
#define kFuzzallocTagMin (kFuzzallocDefaultTag + 1) ///< The minimum tag value

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // FUZZALLOC_H
