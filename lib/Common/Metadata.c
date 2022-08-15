//===-- Metadata.c - Metadata kinds -------------------------------*- C -*-===//
///
/// \file
/// Fuzzalloc LLVM metadata kinds
///
//===----------------------------------------------------------------------===//

#include "fuzzalloc/Metadata.h"

const char *kFuzzallocInstrumentedUseSiteMD = "fuzzalloc.instrumented_use";
const char *kFuzzallocNoInstrumentMD = "fuzzalloc.noinstrument";
const char *kFuzzallocTaggVarMD = "fuzzalloc.tagged_var";
const char *kFuzzallocLoweredNewMD = "fuzzalloc.lowered_new";
const char *kFuzzallocLoweredDeleteMD = "fuzzalloc.lowered_delete";

const char *kNoSanitizeMD = "nosanitize";