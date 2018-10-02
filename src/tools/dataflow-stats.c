// AFL include files
#include "alloc-inl.h"
#include "config.h"
#include "debug.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static u8 **cc_params;     /* Parameters passed to the opt */
static u32 cc_par_cnt = 1; /* Param count, including argv0 */

static void edit_params(u32 argc, char **argv) {
  u8 *name;

  cc_params = ck_alloc((argc + 512) * sizeof(u8 *));

  name = strrchr(argv[0], '/');
  if (!name) {
    name = argv[0];
  } else {
    name++;
  }

  cc_params[0] = (u8 *)"opt";

  cc_params[cc_par_cnt++] = "-analyze";

  cc_params[cc_par_cnt++] = "-load";
  cc_params[cc_par_cnt++] = FUZZALLOC_LLVM_DIR "/Utils/libfuzzalloc-utils.so";

  /* Collect useful statistics */

  cc_params[cc_par_cnt++] = "-load";
  cc_params[cc_par_cnt++] =
      FUZZALLOC_LLVM_DIR "/Analysis/CollectStats/fuzzalloc-collect-stats.so";
  cc_params[cc_par_cnt++] = "-fuzzalloc-collect-stats";

  if (getenv("FUZZALLOC_DEBUG")) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-debug";
  }

  if (getenv("FUZZALLOC_STATS")) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-stats";
  }

  while (--argc) {
    u8 *cur = *(++argv);

    cc_params[cc_par_cnt++] = cur;
  }

  cc_params[cc_par_cnt] = NULL;
}

int main(int argc, char **argv) {
  if (isatty(2) && !getenv("AFL_QUIET")) {
    SAYF(cCYA "dataflow-stats" cBRI VERSION cRST
              " by <adrian.herrera@anu.edu.au>\n");
  }

  if (argc < 2) {
    SAYF("\n"
         "This is a helper application for collecting some useful statistics "
         "from a target bitcode (bc) file. A typical usage would be:\n\n"

         "  dataflow-stats /path/to/bc/file\n\n");

    exit(1);
  }

  edit_params(argc, argv);

  execvp(cc_params[0], (char **)cc_params);

  FATAL("Oops, failed to execute '%s' - check your PATH", cc_params[0]);

  return 0;
}
