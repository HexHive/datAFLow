//===-- Utils.cpp - Common instrumentation functionality --------*- C++ -*-===//
///
/// \file
/// Common instrumentation functionality
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>

#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

#include "Utils.h"

using namespace llvm;

// Adapted from http://c-faq.com/lib/randrange.html
#define RAND(x, y) ((tag_t)((x) + random() / (RAND_MAX / ((y) - (x) + 1) + 1)))

cl::opt<InstType> ClInstType(
    cl::desc("Use site instrumentation"),
    cl::values(clEnumValN(InstType::InstNone, "fuzzalloc-use-inst-none",
                          "No instrumentation"),
               clEnumValN(InstType::InstAFL, "fuzzalloc-use-inst-afl",
                          "AFL instrumentation"),
               clEnumValN(InstType::InstTrace, "fuzzalloc-use-inst-tracer",
                          "Tracer instrumentation")));

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

//
// Tracer functionality
//

Value *readPC(Instruction *InsertPt) {
  auto *M = InsertPt->getFunction()->getParent();
  auto &Ctx = M->getContext();

  auto *ReadAsmPCTy =
      FunctionType::get(Type::getInt64Ty(Ctx), /*isVarArg=*/false);
  auto ReadPCAsm =
      FunctionCallee(ReadAsmPCTy, InlineAsm::get(ReadAsmPCTy, "leaq (%rip), $0",
                                                 /*Constraints=*/"=r",
                                                 /*hasSideEffects=*/false));

  return CallInst::Create(ReadPCAsm, "", InsertPt);
}

Instruction *tracerLogDef(Constant *DefMetadata, Instruction *InsertPt) {
  auto *M = InsertPt->getFunction()->getParent();
  auto &Ctx = M->getContext();

  auto *TracerDefFnTy =
      FunctionType::get(Type::getVoidTy(Ctx), {DefMetadata->getType()},
                        /*isVarArg=*/false);
  auto TracerDefFn = M->getOrInsertFunction("__tracer_def", TracerDefFnTy);
  assert(TracerDefFn);

  return CallInst::Create(TracerDefFn, {DefMetadata}, "", InsertPt);
}

static GlobalVariable *createTracerGlobalVariable(Constant *Initializer,
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

Constant *tracerCreateDef(const VarInfo &SrcVar, Module *M) {
  auto &Ctx = M->getContext();
  auto &DL = M->getDataLayout();

  auto *Int8PtrTy = Type::getInt8PtrTy(Ctx);
  auto *IntPtrTy = DL.getIntPtrType(Ctx);

  auto *TracerSrcLocationTy =
      StructType::create({Int8PtrTy, Int8PtrTy, IntPtrTy, IntPtrTy},
                         "fuzzalloc.SrcLocation", /*isPacked=*/true);
  auto *TracerSrcDefTy = StructType::create(
      {TracerSrcLocationTy, Int8PtrTy}, "fuzzalloc.SrcDef", /*isPacked=*/true);

  const auto *DIVar = SrcVar.getDbgVar();
  const auto *Loc = SrcVar.getLoc();

  auto *Filename = ConstantDataArray::getString(Ctx, DIVar->getFilename());
  auto *FilenameGV = createTracerGlobalVariable(Filename, M);
  auto *FilenamePtr = createGlobalVariablePtr(FilenameGV);

  auto *FuncName = [&]() -> Constant * {
    if (auto *DILocal = dyn_cast<DILocalVariable>(DIVar)) {
      auto *SP = getDISubprogram(DILocal->getScope());
      return ConstantDataArray::getString(Ctx, SP->getName());
    } else {
      return ConstantDataArray::getString(Ctx, "");
    }
  }();

  auto *FuncNameGV = createTracerGlobalVariable(FuncName, M);
  auto *FuncNamePtr = createGlobalVariablePtr(FuncNameGV);

  auto *Line = ConstantInt::get(IntPtrTy, DIVar->getLine());
  auto *Col = ConstantInt::get(IntPtrTy, Loc ? Loc->getCol() : 0);

  auto *VarName = ConstantDataArray::getString(Ctx, DIVar->getName());
  auto *VarNameGV = createTracerGlobalVariable(VarName, M);
  auto *VarNamePtr = createGlobalVariablePtr(VarNameGV);

  auto *SrcLocation = ConstantStruct::get(
      TracerSrcLocationTy, {FilenamePtr, FuncNamePtr, Line, Col});
  auto *Def = ConstantStruct::get(TracerSrcDefTy, {SrcLocation, VarNamePtr});

  return createTracerGlobalVariable(Def, M);
}

Constant *tracerCreateUse(Instruction *I, Module *M) {
  auto &Ctx = M->getContext();
  auto &DL = M->getDataLayout();

  auto *Int8PtrTy = Type::getInt8PtrTy(Ctx);
  auto *IntPtrTy = DL.getIntPtrType(Ctx);

  // Get debug location info
  const auto &Loc = I->getDebugLoc();
  Constant *FilenamePtr = Constant::getNullValue(Int8PtrTy);
  Constant *FuncNamePtr = Constant::getNullValue(Int8PtrTy);
  Constant *Line = Constant::getNullValue(IntPtrTy);
  Constant *Col = Constant::getNullValue(IntPtrTy);
  if (Loc) {
    auto *SP = getDISubprogram(Loc.getScope());

    auto *Filename =
        ConstantDataArray::getString(Ctx, SP->getFile()->getFilename());
    auto *FilenameGV = createTracerGlobalVariable(Filename, M);
    FilenamePtr = createGlobalVariablePtr(FilenameGV);

    auto *FuncName = ConstantDataArray::getString(Ctx, SP->getName());
    auto *FuncNameGV = createTracerGlobalVariable(FuncName, M);
    FuncNamePtr = createGlobalVariablePtr(FuncNameGV);

    Line = ConstantInt::get(IntPtrTy, Loc.getLine());
    Col = ConstantInt::get(IntPtrTy, Loc.getCol());
  }

  auto *TracerSrcLocationTy =
      StructType::create({Int8PtrTy, Int8PtrTy, IntPtrTy, IntPtrTy},
                         "fuzzalloc.SrcLocation", /*isPacked=*/true);
  auto *Use = ConstantStruct::get(TracerSrcLocationTy,
                                  {FilenamePtr, FuncNamePtr, Line, Col});
  return createTracerGlobalVariable(Use, M);
}