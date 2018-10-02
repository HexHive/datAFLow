//===-- HeapifyUtils.cpp - Heapify static arrays --------------------------===//
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

#include <cxxabi.h>

#include "llvm/IR/Instructions.h"

#include "HeapifyUtils.h"

using namespace llvm;

Value *updateGEP(GetElementPtrInst *GEP, Value *MallocPtr, Type *MallocTy) {
  // Load the pointer to the dynamically allocated array and create a new GEP
  // instruction. It seems that the simplest way is to cast the loaded pointer
  // to the original array type
  auto *LoadMallocPtr = new LoadInst(MallocTy, MallocPtr, "", GEP);
  auto *BitCastMallocPtr = CastInst::CreatePointerCast(
      LoadMallocPtr, GEP->getOperand(0)->getType(), "", GEP);
  auto *MallocPtrGEP = GetElementPtrInst::CreateInBounds(
      BitCastMallocPtr,
      SmallVector<Value *, 4>(GEP->idx_begin(), GEP->idx_end()), "", GEP);
  MallocPtrGEP->takeName(GEP);

  // Update all the users of the original GEP instruction to use the updated
  // GEP. The updated GEP is correctly typed for the malloc pointer
  GEP->replaceAllUsesWith(MallocPtrGEP);
  GEP->eraseFromParent();

  return MallocPtrGEP;
}

bool isHeapifiableType(Type *Ty) {
  if (!Ty->isArrayTy()) {
    return false;
  }

  // Don't heapify va_list (i.e., variable arguments): it's too hard and for
  // some reason everything breaks :(
  if (auto *StructTy = dyn_cast<StructType>(Ty->getArrayElementType())) {
    if (!StructTy->isLiteral() &&
        StructTy->getName().equals("struct.__va_list_tag")) {
      return false;
    }
  }

  return true;
}

bool isFromLibCpp(const Value *V) {
  if (!isa<GlobalVariable>(V)) {
    return false;
  }

  int DemangleStatus;
  char *DemangleNameCStr = abi::__cxa_demangle(
      V->getName().str().c_str(), nullptr, nullptr, &DemangleStatus);
  if (DemangleStatus == 0) {
    StringRef DemangleName = StringRef(DemangleNameCStr);

    if (DemangleName.startswith("std::tr1::__")) {
      return true;
    }
  }

  return false;
}

Instruction *createArrayMalloc(LLVMContext &C, const DataLayout &DL,
                               IRBuilder<> &IRB, Type *AllocTy,
                               uint64_t ArrayNumElems, const Twine &Name) {
  IntegerType *IntPtrTy = DL.getIntPtrType(C);
  uint64_t TypeSize = DL.getTypeAllocSize(AllocTy);

  return CallInst::CreateMalloc(&*IRB.GetInsertPoint(), IntPtrTy, AllocTy,
                                ConstantInt::get(IntPtrTy, TypeSize),
                                ConstantInt::get(IntPtrTy, ArrayNumElems),
                                nullptr, Name);
}

Instruction *createMalloc(LLVMContext &C, const DataLayout &DL,
                          IRBuilder<> &IRB, Type *AllocTy, const Twine &Name) {
  IntegerType *IntPtrTy = DL.getIntPtrType(C);

  return CallInst::CreateMalloc(&*IRB.GetInsertPoint(), IntPtrTy, AllocTy,
                                ConstantExpr::getSizeOf(AllocTy), nullptr,
                                nullptr, Name);
}

void insertFree(Type *MallocPtrTy, Value *MallocPtr, Instruction *Inst) {
  // Load the pointer to the dynamically allocated memory and pass it to free
  auto *LoadMalloc = new LoadInst(MallocPtrTy, MallocPtr, "", Inst);
  CallInst::CreateFree(LoadMalloc, Inst);
}
