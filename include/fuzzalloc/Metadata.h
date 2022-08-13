//===-- Metadata.h - Metadata kinds -----------------------------*- C++ -*-===//
///
/// \file
/// Fuzzalloc LLVM metadata kinds
///
//===----------------------------------------------------------------------===//

#ifndef METADATA_H
#define METADATA_H

const char *kFuzzallocHeapifiedAllocaMD = "fuzzalloc.heapified_alloca";
const char *kFuzzallocInstrumentedDerefMD = "fuzzalloc.instrumented_deref";
const char *kFuzzallocNoInstrumentMD = "fuzzalloc.noinstrument";
const char *kFuzzallocTaggedAllocMD = "fuzzalloc.tagged_alloc";
const char *kFuzzallocLoweredNewMD = "fuzzalloc.lowered_new";
const char *kFuzzallocLoweredDeleteMD = "fuzzalloc.lowered_delete";

const char *kNoSanitizeMD = "nosanitize";

#endif