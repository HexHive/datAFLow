//
// fuzzalloc
// A memory allocator for fuzzing
//
// Author: Adrian Herrera
//

#include <string.h>

#include "common.h"

u8 prefix(const char *str, const char *pre) {
  return strncmp(pre, str, strlen(pre)) == 0;
}

u8 check_if_assembler(u32 argc, const char **argv) {
  while (--argc) {
    u8 *cur = *(++argv);

    const u8 *ext = strrchr(cur, '.');
    if (ext && (!strcmp(ext + 1, "s") || !strcmp(ext + 1, "S"))) {
      return 1;
    }
  }

  return 0;
}
