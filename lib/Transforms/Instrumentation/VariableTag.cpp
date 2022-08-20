//===-- VariableTag.cpp - Common tagging functionality ----------*- C++ -*-===//
///
/// \file
/// Common tagging functionality
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

#include "VariableTag.h"

using namespace llvm;

// Adapted from http://c-faq.com/lib/randrange.html
#define RAND(x, y) ((tag_t)((x) + random() / (RAND_MAX / ((y) - (x) + 1) + 1)))

ConstantInt *generateTag(IntegerType *TagTy) {
  return ConstantInt::get(
      TagTy, static_cast<uint64_t>(RAND(kFuzzallocTagMin, kFuzzallocTagMax)));
}

size_t getTaggedVarSize(const TypeSize &Size) {
  auto AdjustedSize = Size + kMetaSize;
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  return bb_nextPow2(AdjustedSize);
}

Instruction *insertMalloc(Type *Ty, Value *Ptr, Instruction *InsertPt) {
  auto *Mod = InsertPt->getFunction()->getParent();
  auto &Ctx = Mod->getContext();
  auto &DL = Mod->getDataLayout();

  auto *IntPtrTy = DL.getIntPtrType(Ctx);
  auto *MallocCall = [&]() -> Instruction * {
    const auto &Name = Ptr->hasName() ? Ptr->getName().str() + ".malloc" : "";

    if (auto *ArrayTy = dyn_cast<ArrayType>(Ty)) {
      // Insert array malloc call
      auto *ElemTy = ArrayTy->getArrayElementType();
      auto TySize = DL.getTypeAllocSize(ElemTy);
      auto NumElems = ArrayTy->getNumElements();
      return CallInst::CreateMalloc(
          InsertPt, IntPtrTy, ElemTy, ConstantInt::get(IntPtrTy, TySize),
          ConstantInt::get(IntPtrTy, NumElems), nullptr, Name);
    } else {
      // Insert non-array malloc call
      return CallInst::CreateMalloc(InsertPt, IntPtrTy, Ty,
                                    ConstantExpr::getSizeOf(Ty), nullptr,
                                    nullptr, Name);
    }
  }();

  auto *MallocStore = new StoreInst(MallocCall, Ptr, InsertPt);
  MallocStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                           MDNode::get(Ctx, None));
  MallocStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                           MDNode::get(Ctx, None));

  return MallocCall;
}

Instruction *insertFree(Type *Ty, Value *Ptr, Instruction *InsertPt) {
  auto *Mod = InsertPt->getFunction()->getParent();
  auto &Ctx = Mod->getContext();

  // Load the pointer to the dynamically allocated memory and free it
  auto *Load = new LoadInst(Ty, Ptr, "", InsertPt);
  Load->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                    MDNode::get(Ctx, None));
  Load->setMetadata(Mod->getMDKindID(kNoSanitizeMD), MDNode::get(Ctx, None));

  return CallInst::CreateFree(Load, InsertPt);
}
