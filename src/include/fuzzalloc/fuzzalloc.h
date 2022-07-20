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
#if FUZZALLOC_ASAN
// Need a separate namespace so that we don't conflict with tag_t in hwasan
namespace __fuzzalloc {
#endif // FUZZALLOC_ASAN

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

/// ASan's quarantine region gets its own mspace. Note that the quarantine
/// region is only defined if we're building with ASan
#define FUZZALLOC_ASAN_QUARANTINE_TAG (FUZZALLOC_DEFAULT_TAG + 1)

/// The default minimum tag value
#define FUZZALLOC_TAG_MIN (FUZZALLOC_DEFAULT_TAG + 1)

/// The default minimum tag value when compiling with ASan
#define FUZZALLOC_ASAN_TAG_MIN (FUZZALLOC_ASAN_QUARANTINE_TAG + 1)

/// Tag mask
#define FUZZALLOC_TAG_MASK ((1UL << NUM_TAG_BITS) - 1)

/// The default maximum tag value
#define FUZZALLOC_TAG_MAX 0x7FFEUL

/// The default maximum tag value when compiling with ASan
#define FUZZALLOC_ASAN_TAG_MAX 0x6FFDUL

/// The ASan shadow memory offset when using fuzzalloc. Ensure that it is far
/// enough from the maximum fuzzalloc tag so that an mspace doesn't get
/// allocated in the ASan shadow memory
#define FUZZALLOC_ASAN_SHADOW_OFFSET                                           \
  ((FUZZALLOC_ASAN_TAG_MAX + 2) << FUZZALLOC_TAG_SHIFT)

#if defined(__cplusplus)
}

#if FUZZALLOC_ASAN
} // namespace __fuzzalloc
#endif // FUZZALLOC_ASAN
#endif // __cplusplus

#endif // FUZZALLOC_H
