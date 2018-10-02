//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#include <stdio.h>

#include "debug.h"
#include "malloc_internal.h"

void __mem_access(tag_t def_site, int64_t offset) {
  DEBUG_MSG("accessing def site %#x from %p (at offset %ld)\n", def_site,
            __builtin_return_address(0), offset);
}
