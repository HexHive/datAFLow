/*
   american fuzzy lop - LLVM-mode wrapper for clang
   ------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This program is a drop-in replacement for clang, similar in most respects
   to ../afl-gcc. It tries to figure out compilation mode, adds a bunch
   of flags, and then calls the real compiler.

 */

#define AFL_MAIN

// AFL include files
#include "alloc-inl.h"
#include "config.h"
#include "debug.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "fuzzalloc.h"

static u8 *obj_path;       /* Path to runtime libraries         */
static u8 **cc_params;     /* Parameters passed to the real CC  */
static u32 cc_par_cnt = 1; /* Param count, including argv0      */

/* Try to find the runtime libraries. If that fails, abort. */

static void find_obj(u8 *argv0) {
  u8 *afl_path = getenv("AFL_PATH");
  u8 *slash, *tmp;

  if (afl_path) {
    tmp = alloc_printf("%s/afl-compiler-rt.o", afl_path);

    if (!access(tmp, R_OK)) {
      obj_path = afl_path;
      ck_free(tmp);
      return;
    }

    ck_free(tmp);
  }

  slash = strrchr(argv0, '/');

  if (slash) {
    u8 *dir;

    *slash = 0;
    dir = ck_strdup(argv0);
    *slash = '/';

    tmp = alloc_printf("%s/afl-compiler-rt.o", dir);

    if (!access(tmp, R_OK)) {
      obj_path = dir;
      ck_free(tmp);
      return;
    }

    ck_free(tmp);
    ck_free(dir);
  }

  FATAL("Unable to find 'afl-compiler-rt.o'. Please set AFL_PATH");
}

/* Copy argv to cc_params, making the necessary edits. */

static void edit_params(u32 argc, char **argv) {
  u8 fortify_set = 0, asan_set = 0, x_set = 0, libfuzzer_set = 0,
     maybe_linking = 1, bit_mode = 0, maybe_assembler = 0;
  u8 *name;

  cc_params = ck_alloc((argc + 512) * sizeof(u8 *));

  name = strrchr(argv[0], '/');
  if (!name) {
    name = argv[0];
  } else {
    name++;
  }

  if (!strcmp(name, "dataflow-cc++")) {
    u8 *alt_cxx = getenv("AFL_CXX");
    cc_params[0] = alt_cxx ? alt_cxx : (u8 *)"clang++";
  } else {
    u8 *alt_cc = getenv("AFL_CC");
    cc_params[0] = alt_cc ? alt_cc : (u8 *)"clang";
  }

  /* Cannot analyze bitcode if we are running the assembler */

  maybe_assembler = check_if_assembler(argc, argv);

  cc_params[cc_par_cnt++] = "-Xclang";
  cc_params[cc_par_cnt++] = "-mdisable-const-array-pack";

  cc_params[cc_par_cnt++] =
      "-fplugin=" FUZZALLOC_LLVM_DIR "/Utils/libfuzzalloc-utils.so";

  /* Rewrite calls to new with calls to malloc */

  cc_params[cc_par_cnt++] = "-fplugin=" FUZZALLOC_LLVM_DIR
                            "/Transforms/RewriteNews/"
                            "libfuzzalloc-rewrite-news.so";

  /* Expand global variables with static ConstantAggregate initializers */

  cc_params[cc_par_cnt++] = "-fplugin=" FUZZALLOC_LLVM_DIR
                            "/Transforms/HeapifyObjects/"
                            "libfuzzalloc-expand-gv-initializers.so";

  /* Heapify static arrays to dynamically allocated arrays */

  cc_params[cc_par_cnt++] = "-fplugin=" FUZZALLOC_LLVM_DIR
                            "/Transforms/HeapifyObjects/"
                            "libfuzzalloc-heapify-allocas.so";
  cc_params[cc_par_cnt++] = "-fplugin=" FUZZALLOC_LLVM_DIR
                            "/Transforms/HeapifyObjects/"
                            "libfuzzalloc-heapify-global-vars.so";

  if (getenv("FUZZALLOC_HEAPIFY_STRUCTS")) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-fuzzalloc-heapify-structs";
  }

  /* Tag dynamically allocated arrays and redirect them to the fuzzalloc
   * allocator library */

  cc_params[cc_par_cnt++] =
      "-fplugin=" FUZZALLOC_LLVM_DIR
      "/Transforms/TagDynamicAllocs/libfuzzalloc-tag-dyn-allocs.so";

  char *fuzzalloc_tag_log = getenv("FUZZALLOC_TAG_LOG");
  if (fuzzalloc_tag_log && !maybe_assembler) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] =
        alloc_printf("-fuzzalloc-tag-log=%s", fuzzalloc_tag_log);
  }

  /* Instrument memory accesses */

  cc_params[cc_par_cnt++] =
      "-fplugin=" FUZZALLOC_LLVM_DIR
      "/Transforms/LowerAtomics/libfuzzalloc-lower-atomics.so";

  cc_params[cc_par_cnt++] =
      "-fplugin=" FUZZALLOC_LLVM_DIR
      "/Transforms/LowerMemIntrinsics/libfuzzalloc-lower-mem-intrinsics.so";

  if (!maybe_assembler) {
    cc_params[cc_par_cnt++] =
        "-fplugin=" FUZZALLOC_LLVM_DIR
        "/Transforms/InstrumentMemAccesses/libfuzzalloc-inst-mem-accesses.so";

    char *fuzzalloc_fuzzer = getenv("FUZZALLOC_FUZZER");
    if (fuzzalloc_fuzzer) {
      cc_params[cc_par_cnt++] = "-mllvm";
      cc_params[cc_par_cnt++] = alloc_printf("-%s", fuzzalloc_fuzzer);
    }

    char *fuzzalloc_sensitivity = getenv("FUZZALLOC_SENSITIVITY");
    if (fuzzalloc_sensitivity) {
      cc_params[cc_par_cnt++] = "-mllvm";
      cc_params[cc_par_cnt++] = alloc_printf("-%s", fuzzalloc_sensitivity);
    }
  }

  cc_params[cc_par_cnt++] = "-Qunused-arguments";

  if (getenv("FUZZALLOC_DEBUG")) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-debug";
  }

  if (getenv("FUZZALLOC_STATS")) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-stats";
  }

  /* Detect stray -v calls from ./configure scripts. */

  if (argc == 1 && !strcmp(argv[1], "-v")) {
    maybe_linking = 0;
  }

  while (--argc) {
    u8 *cur = *(++argv);

    if (!strcmp(cur, "-m32")) {
      bit_mode = 32;
    }
    if (!strcmp(cur, "-m64")) {
      bit_mode = 64;
    }

    if (!strcmp(cur, "-x")) {
      x_set = 1;
    }

    if (!strcmp(cur, "-c") || !strcmp(cur, "-S") || !strcmp(cur, "-E")) {
      maybe_linking = 0;
    }

    if (prefix(cur, "-fsanitize=")) {
      if (strstr(cur, "address") || strstr(cur, "memory")) {
        asan_set = 1;
      }
      if (strstr(cur, "fuzzer")) {
        libfuzzer_set = 1;
      }
    }

    if (strstr(cur, "FORTIFY_SOURCE")) {
      fortify_set = 1;
    }

    if (!strcmp(cur, "-shared")) {
      maybe_linking = 0;
    }

    if (!strcmp(cur, "-Wl,-z,defs") || !strcmp(cur, "-Wl,--no-undefined")) {
      continue;
    }

    cc_params[cc_par_cnt++] = cur;
  }

  if (getenv("AFL_HARDEN")) {
    cc_params[cc_par_cnt++] = "-fstack-protector-all";

    if (!fortify_set) {
      cc_params[cc_par_cnt++] = "-D_FORTIFY_SOURCE=2";
    }
  }

  if (!asan_set) {
    if (getenv("AFL_USE_ASAN")) {
      if (getenv("AFL_USE_MSAN")) {
        FATAL("ASAN and MSAN are mutually exclusive");
      }

      if (getenv("AFL_HARDEN")) {
        FATAL("ASAN and AFL_HARDEN are mutually exclusive");
      }

      cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
      cc_params[cc_par_cnt++] = "-fsanitize=address";

      asan_set = 1;
    } else if (getenv("AFL_USE_MSAN")) {
      if (getenv("AFL_USE_ASAN")) {
        FATAL("ASAN and MSAN are mutually exclusive");
      }

      if (getenv("AFL_HARDEN")) {
        FATAL("MSAN and AFL_HARDEN are mutually exclusive");
      }

      cc_params[cc_par_cnt++] = "-U_FORTIFY_SOURCE";
      cc_params[cc_par_cnt++] = "-fsanitize=memory";
    }
  }

  if (asan_set && !maybe_assembler) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-asan-with-fuzzalloc";
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] =
        alloc_printf("-fuzzalloc-tag-min=%d", FUZZALLOC_ASAN_TAG_MIN);
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] =
        alloc_printf("-fuzzalloc-tag-max=%d", FUZZALLOC_ASAN_TAG_MAX);
  }

  if (libfuzzer_set && !maybe_assembler) {
    cc_params[cc_par_cnt++] = "-mllvm";
    cc_params[cc_par_cnt++] = "-fuzzalloc-libfuzzer";
  }

  if (!getenv("AFL_DONT_OPTIMIZE")) {
    cc_params[cc_par_cnt++] = "-g";
    cc_params[cc_par_cnt++] = "-O3";
    cc_params[cc_par_cnt++] = "-funroll-loops";
  }

  if (getenv("AFL_NO_BUILTIN")) {
    cc_params[cc_par_cnt++] = "-fno-builtin-strcmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strcasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-strncasecmp";
    cc_params[cc_par_cnt++] = "-fno-builtin-memcmp";
  }

  cc_params[cc_par_cnt++] = "-D__AFL_HAVE_MANUAL_CONTROL=1";
  cc_params[cc_par_cnt++] = "-D__AFL_COMPILER=1";
  cc_params[cc_par_cnt++] = "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION=1";

  /* When the user tries to use persistent or deferred forkserver modes by
     appending a single line to the program, we want to reliably inject a
     signature into the binary (to be picked up by afl-fuzz) and we want
     to call a function from the runtime .o file. This is unnecessarily
     painful for three reasons:

     1) We need to convince the compiler not to optimize out the signature.
        This is done with __attribute__((used)).

     2) We need to convince the linker, when called with -Wl,--gc-sections,
        not to do the same. This is done by forcing an assignment to a
        'volatile' pointer.

     3) We need to declare __afl_persistent_loop() in the global namespace,
        but doing this within a method in a class is hard - :: and extern "C"
        are forbidden and __attribute__((alias(...))) doesn't work. Hence the
        __asm__ aliasing trick.

   */

  cc_params[cc_par_cnt++] =
      "-D__AFL_LOOP(_A)="
      "({ static volatile char *_B __attribute__((used)); "
      " _B = (char*)\"" PERSIST_SIG "\"; "
#ifdef __APPLE__
      "__attribute__((visibility(\"default\"))) "
      "int _L(unsigned int) __asm__(\"___afl_persistent_loop\"); "
#else
      "__attribute__((visibility(\"default\"))) "
      "int _L(unsigned int) __asm__(\"__afl_persistent_loop\"); "
#endif /* ^__APPLE__ */
      "_L(_A); })";

  cc_params[cc_par_cnt++] =
      "-D__AFL_INIT()="
      "do { static volatile char *_A __attribute__((used)); "
      " _A = (char*)\"" DEFER_SIG "\"; "
#ifdef __APPLE__
      "__attribute__((visibility(\"default\"))) "
      "void _I(void) __asm__(\"___afl_manual_init\"); "
#else
      "__attribute__((visibility(\"default\"))) "
      "void _I(void) __asm__(\"__afl_manual_init\"); "
#endif /* ^__APPLE__ */
      "_I(); } while (0)";

  if (maybe_linking) {
    if (x_set) {
      cc_params[cc_par_cnt++] = "-x";
      cc_params[cc_par_cnt++] = "none";
    }

    switch (bit_mode) {
    case 0:
      cc_params[cc_par_cnt++] = alloc_printf("%s/afl-compiler-rt.o", obj_path);
      break;

    case 32:
      cc_params[cc_par_cnt++] = alloc_printf("%s/afl-compiler-rt-32.o", obj_path);

      if (access(cc_params[cc_par_cnt - 1], R_OK)) {
        FATAL("-m32 is not supported by your compiler");
      }
      break;

    case 64:
      cc_params[cc_par_cnt++] = alloc_printf("%s/afl-compiler-rt-64.o", obj_path);

      if (access(cc_params[cc_par_cnt - 1], R_OK)) {
        FATAL("-m64 is not supported by your compiler");
      }
      break;
    }

    /* Link with the fuzzalloc allocator library */

    cc_params[cc_par_cnt++] = "-L" FUZZALLOC_MALLOC_DIR;
    cc_params[cc_par_cnt++] = "-lfuzzalloc";
  }

  cc_params[cc_par_cnt] = NULL;
}

/* Main entry point */

int main(int argc, char **argv) {
  if (isatty(2) && !getenv("AFL_QUIET")) {
    SAYF(cCYA "dataflow-cc " cBRI VERSION cRST
              " by <lszekeres@google.com, adrian.herrera@anu.edu.au>\n");
  }

  if (argc < 2) {
    SAYF(
        "\n"
        "This is a helper application for afl-fuzz. It serves as a drop-in "
        "replacement\n"
        "for clang, letting you recompile third-party code with the required "
        "runtime\n"
        "instrumentation. A common use pattern would be one of the "
        "following:\n\n"

        "  CC=dataflow-cc ./configure\n"
        "  CXX=dataflow-cc++ ./configure\n\n"

        "You can specify custom next-stage toolchain via AFL_CC and AFL_CXX. "
        "Setting\n"
        "AFL_HARDEN enables hardening optimizations in the compiled code.\n\n");

    exit(1);
  }

  find_obj(argv[0]);

  edit_params(argc, argv);

  execvp(cc_params[0], (char **)cc_params);

  FATAL("Oops, failed to execute '%s' - check your PATH", cc_params[0]);

  return 0;
}
