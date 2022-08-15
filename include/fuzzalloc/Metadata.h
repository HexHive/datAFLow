//===-- Metadata.h - Metadata kinds -------------------------------*- C -*-===//
///
/// \file
/// Fuzzalloc LLVM metadata kinds
///
//===----------------------------------------------------------------------===//

#ifndef METADATA_H
#define METADATA_H

#if defined(__cplusplus)
extern "C" {
#endif // __cplusplus

extern const char *kFuzzallocHeapifiedAllocaMD;
extern const char *kFuzzallocInstrumentedDerefMD;
extern const char *kFuzzallocNoInstrumentMD;
extern const char *kFuzzallocBBAllocMD;
extern const char *kFuzzallocLoweredNewMD;
extern const char *kFuzzallocLoweredDeleteMD;

extern const char *kNoSanitizeMD;

#if defined(__cplusplus)
}
#endif // __cplusplus

#endif // METADATA_H