//===-- Metadata.c - Metadata kinds -------------------------------*- C -*-===//
///
/// \file
/// Fuzzalloc LLVM metadata kinds
///
//===----------------------------------------------------------------------===//

#include "fuzzalloc/Metadata.h"

const char *kFuzzallocHeapifiedAllocaMD = "fuzzalloc.heapified_alloca";
const char *kFuzzallocHeapifiedGlobalMD = "fuzzalloc.heapified_global";
const char *kFuzzallocInstrumentedUseSiteMD = "fuzzalloc.instrumented_use";
const char *kFuzzallocNoInstrumentMD = "fuzzalloc.noinstrument";
const char *kFuzzallocTagVarMD = "fuzzalloc.tagged_var";
const char *kFuzzallocLoweredNewMD = "fuzzalloc.lowered_new";
const char *kFuzzallocLoweredDeleteMD = "fuzzalloc.lowered_delete";

const char *kNoSanitizeMD = "nosanitize";
