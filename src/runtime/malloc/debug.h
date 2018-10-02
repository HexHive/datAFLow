//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#ifndef _MALLOC_DEBUG_H_
#define _MALLOC_DEBUG_H_

#include <stdint.h>

#if !defined(NDEBUG)
#include <assert.h>
#include <stdio.h>

uint64_t get_cur_time(void);

#define DEBUG_MSG(format, args...)                                             \
  fprintf(stderr, "[%lu] %s: " format, get_cur_time(), __func__, ##args)
#else // NDEBUG
#define DEBUG_MSG(format, ...)
#define assert(x)
#endif // !defined(NDEBUG)

#endif // _MALLOC_DEBUG_H_
