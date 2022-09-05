//===-- VariableTag.cpp - Common tagging functionality ----------*- C++ -*-===//
///
/// \file
/// Common tagging functionality
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
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

size_t getTaggedVarSize(const TypeSize &Size, size_t MetadataSize) {
  auto AdjustedSize = Size + MetadataSize;
  if (AdjustedSize < kSlotSize) {
    AdjustedSize = kSlotSize;
  }
  return bb_nextPow2(AdjustedSize);
}

Instruction *insertMalloc(Type *Ty, Value *Ptr, Instruction *InsertPt,
                          bool StoreResult) {
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

  if (StoreResult) {
    auto *MallocStore = new StoreInst(MallocCall, Ptr, InsertPt);
    MallocStore->setMetadata(Mod->getMDKindID(kFuzzallocNoInstrumentMD),
                             MDNode::get(Ctx, None));
    MallocStore->setMetadata(Mod->getMDKindID(kNoSanitizeMD),
                             MDNode::get(Ctx, None));
  }

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

void logDbgDef(const DIVariable *DIVar, Module *M, IRBuilder<> &IRB) {
  auto &DL = M->getDataLayout();
  auto &Ctx = M->getContext();
  auto *IntPtrTy = IRB.getIntPtrTy(DL);

  auto *FileNamePtr = IRB.CreateGlobalStringPtr(DIVar->getFilename());
  auto *FuncNamePtr = [&]() {
    if (auto *DILocal = dyn_cast<DILocalVariable>(DIVar)) {
      auto *SP = getDISubprogram(DILocal->getScope());
      return IRB.CreateGlobalStringPtr(SP->getName());
    }
    return Constant::getNullValue(IRB.getInt8PtrTy());
  }();
  auto *Line = ConstantInt::get(IntPtrTy, DIVar->getLine());
  // TODO get column
  auto *Column = ConstantInt::get(IntPtrTy, 0);
  auto *VarNamePtr = IRB.CreateGlobalStringPtr(DIVar->getName());

  auto *MapFnTy = FunctionType::get(Type::getVoidTy(Ctx),
                                    {FileNamePtr->getType(),
                                     FuncNamePtr->getType(), Line->getType(),
                                     Column->getType(), VarNamePtr->getType()},
                                    /*isVarArg=*/false);
  auto MapFn = M->getOrInsertFunction("__dbg_def", MapFnTy);
  assert(MapFn);

  CallInst::Create(MapFn, {FileNamePtr, FuncNamePtr, Line, Column, VarNamePtr});
}

static GlobalVariable *createDebugGlobalVariable(Constant *Initializer,
                                                 Module *M) {
  auto *GV = new GlobalVariable(*M, Initializer->getType(), /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, Initializer);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return GV;
}

static Constant *createGlobalVariablePtr(GlobalVariable *GV) {
  auto *Mod = GV->getParent();
  auto &Ctx = Mod->getContext();

  auto *Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Constant *Indices[] = {Zero, Zero};

  return ConstantExpr::getInBoundsGetElementPtr(GV->getValueType(), GV,
                                                Indices);
}

Constant *createDebugMetadata(const VarInfo &SrcVar, Module *M) {
  auto &Ctx = M->getContext();
  auto &DL = M->getDataLayout();

  auto *Int8PtrTy = Type::getInt8PtrTy(Ctx);
  auto *IntPtrTy = DL.getIntPtrType(Ctx);

  auto *DbgLocationTy =
      StructType::create({Int8PtrTy, Int8PtrTy, IntPtrTy, IntPtrTy},
                         "fuzzalloc.Location", /*isPacked=*/true);
  auto *DbgDefInfoTy = StructType::create(
      {DbgLocationTy, Int8PtrTy}, "fuzzalloc.DefInfo", /*isPacked=*/true);

  const auto *DIVar = SrcVar.getDbgVar();
  const auto *Loc = SrcVar.getLoc();

  auto *FileName = ConstantDataArray::getString(Ctx, DIVar->getFilename());
  auto *FileNameGV = createDebugGlobalVariable(FileName, M);

  auto *FuncName = [&]() {
    if (auto *DILocal = dyn_cast<DILocalVariable>(DIVar)) {
      auto *SP = getDISubprogram(DILocal->getScope());
      return ConstantDataArray::getString(Ctx, SP->getName());
    }
    return Constant::getNullValue(Int8PtrTy);
  }();
  auto *FuncNameGV = createDebugGlobalVariable(FuncName, M);

  auto *Line = ConstantInt::get(IntPtrTy, DIVar->getLine());
  auto *Col = ConstantInt::get(IntPtrTy, Loc ? Loc->getCol() : 0);

  auto *VarName = ConstantDataArray::getString(Ctx, DIVar->getName());
  auto *VarNameGV = createDebugGlobalVariable(VarName, M);

  auto *Location = ConstantStruct::get(
      DbgLocationTy, {createGlobalVariablePtr(FileNameGV),
                      createGlobalVariablePtr(FuncNameGV), Line, Col});
  auto *DefInfo = ConstantStruct::get(
      DbgDefInfoTy, {Location, createGlobalVariablePtr(VarNameGV)});

  return DefInfo;
}
