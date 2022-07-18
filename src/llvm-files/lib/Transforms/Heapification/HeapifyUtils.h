//===-- HeapifyUtils.h - Heapify static arrays ----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common functionality for static array/struct heapification.
///
//===----------------------------------------------------------------------===//

#ifndef FUZZALLOC_HEAPIFY_UTILS_H
#define FUZZALLOC_HEAPIFY_UTILS_H

#include "llvm/IR/IRBuilder.h"

namespace llvm {
class DataLayout;
class GetElementPtrInst;
class Instruction;
class LLVMContext;
class StructType;
class Type;
class Twine;
class Value;
} // namespace llvm

/// Priority for heapified global variable constructor
const unsigned kHeapifyGVCtorAndDtorPriority = 0;

/// Update a GEP instruction to use the given value
llvm::Value *updateGEP(llvm::GetElementPtrInst *, llvm::Value *, llvm::Type *);

/// Returns \c true if the given type is heapifiable
bool isHeapifiableType(llvm::Type *);

/// Returns \c true if the given value is from the C++ standard library and
/// hence never defined (and hence not heapifiable)
bool isFromLibCpp(const llvm::Value *);

/// Create a call to \c malloc that will create an array on the heap.
llvm::Instruction *createArrayMalloc(llvm::LLVMContext &,
                                     const llvm::DataLayout &,
                                     llvm::IRBuilder<> &, llvm::Type *,
                                     uint64_t, const llvm::Twine & = "");

/// Create a call to \c malloc that will create a non-array variable on the
/// heap.
llvm::Instruction *createMalloc(llvm::LLVMContext &, const llvm::DataLayout &,
                                llvm::IRBuilder<> &, llvm::Type *,
                                const llvm::Twine & = "");

/// Insert a call to \c free for the given alloca (with the given type)
void insertFree(llvm::Type *, llvm::Value *, llvm::Instruction *);

#endif // FUZZALLOC_HEAPIFY_UTILS_H
