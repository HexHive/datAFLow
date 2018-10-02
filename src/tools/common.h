//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#ifndef FUZZALLOC_TOOLS_COMMON_H
#define FUZZALLOC_TOOLS_COMMON_H

// AFL include files
#include "types.h"

u8 prefix(const char *str, const char *pre);

u8 check_if_assembler(u32 argc, const char **argv);

#endif // FUZZALLOC_TOOLS_COMMON_H
