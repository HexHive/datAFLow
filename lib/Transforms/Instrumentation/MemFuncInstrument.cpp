//===-- MemFuncInstrument.cpp - Instrument dynamic memory funcs -*- C++ -*-===//
///
/// \file
/// Instrument dynamic memory allocation functions
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueMap.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "fuzzalloc/Analysis/MemFuncIdentify.h"
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-def-site-inst"

class MemFuncInstrument : public ModulePass {
public:
  static char ID;
  MemFuncInstrument() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  FunctionType *getTaggedFunctionType(const Function *) const;
  Function *getTaggedFunction(const Function *) const;
  Function *tagFunction(const Function *) const;
  Instruction *tagCall(CallBase *, Function *) const;
  void tagUse(Use *, Function *) const;

  Module *Mod;
  LLVMContext *Ctx;
  Type *TagTy;
  ValueMap<Function *, Function *> TaggedFuncs;
};

char MemFuncInstrument::ID = 0;

FunctionType *
MemFuncInstrument::getTaggedFunctionType(const Function *F) const {
  auto *FTy = F->getFunctionType();
  SmallVector<Type *, 4> TaggedFuncParams = {TagTy};
  TaggedFuncParams.append(FTy->param_begin(), FTy->param_end());
  return FunctionType::get(FTy->getReturnType(), TaggedFuncParams,
                           FTy->isVarArg());
}

Function *MemFuncInstrument::getTaggedFunction(const Function *OrigF) const {
  const auto &Name = "__tagged_" + OrigF->getName().str();
  auto *TaggedFTy = getTaggedFunctionType(OrigF);
  auto TaggedCallee = Mod->getOrInsertFunction(Name, TaggedFTy);
  assert(TaggedCallee && "Tagged function not inserted");
  auto *TaggedF = cast<Function>(TaggedCallee.getCallee()->stripPointerCasts());

  TaggedF->getArg(0)->setName("tag");
  for (unsigned I = 0; I < OrigF->arg_size(); ++I) {
    TaggedF->getArg(I + 1)->setName(OrigF->getArg(I)->getName());
  }

  return TaggedF;
}

Function *MemFuncInstrument::tagFunction(const Function *OrigF) const {
  LLVM_DEBUG(dbgs() << "tagging function " << OrigF->getName() << '\n');

  // Insert (or get, if it already exists) the tagged function declaration
  auto *TaggedF = getTaggedFunction(OrigF);

  // If this function is defined outside this module, nothing more to do
  if (OrigF->isDeclaration()) {
    return TaggedF;
  }

  // Map the original function arguments to the new version of the allocation
  // wrapper function. Skip the tag argument (i.e., first argument)
  ValueToValueMapTy VMap;
  auto NewFuncArgIt = TaggedF->arg_begin() + 1;
  for (auto &Arg : OrigF->args()) {
    VMap[&Arg] = &(*NewFuncArgIt++);
  }

  // Insert the tagged function body
  SmallVector<ReturnInst *, 8> Returns;
  CloneFunctionInto(TaggedF, OrigF, VMap, /*ModuleLevelChanges=*/true, Returns);

  // Update allocsize attribute (if it exists). Just move the allocsize index up
  // one (to take into account the tag being inserted as the first function
  // parameter)
  if (TaggedF->hasFnAttribute(Attribute::AllocSize)) {
    Attribute Attr = TaggedF->getFnAttribute(Attribute::AllocSize);
    std::pair<unsigned, Optional<unsigned>> Args = Attr.getAllocSizeArgs();
    Args.first += 1;
    Args.second = Args.second.hasValue()
                      ? Optional<unsigned>(Args.second.getValue() + 1)
                      : None;

    TaggedF->removeFnAttr(Attribute::AllocSize);
    TaggedF->addFnAttr(Attribute::getWithAllocSizeArgs(
        TaggedF->getContext(), Args.first, Args.second));
  }

  return TaggedF;
}

Instruction *MemFuncInstrument::tagCall(CallBase *CB, Function *TaggedF) const {
  LLVM_DEBUG(dbgs() << "tagging call " << *CB << " (in function "
                    << CB->getFunction()->getName() << ") with call to "
                    << TaggedF->getName() << '\n');

  auto *ParentF = CB->getFunction();
}

void MemFuncInstrument::tagUse(Use *U, Function *F) const {
  LLVM_DEBUG(dbgs() << "replacing user " << *U->getUser()
                    << " of tagged function " << F->getName() << '\n');

  auto *User = U->getUser();
  auto *TaggedF = TaggedFuncs.lookup(F);

  if (auto *CB = dyn_cast<CallBase>(User)) {

  } else if (auto *Store = dyn_cast<StoreInst>(User)) {

  } else {
  }
}

void MemFuncInstrument::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MemFuncIdentify>();
}

bool MemFuncInstrument::runOnModule(Module &M) {
  auto &MemFuncs = getAnalysis<MemFuncIdentify>().getFuncs();

  if (MemFuncs.empty()) {
    return false;
  }

  this->Mod = &M;
  this->Ctx = &M.getContext();
  this->TagTy = Type::getIntNTy(*Ctx, kNumTagBits);

  // Create the tagged memory allocation functions. These functions take the
  // same arguments as the original dynamic memory allocation function, except
  // the first argument is a tag identifying the allocation site
  for (auto *F : MemFuncs) {
    auto *TaggedF = tagFunction(F);
    TaggedFuncs.insert({F, TaggedF});
  }

  for (auto [F, _] : TaggedFuncs) {
    SmallVector<Use *, 16> Uses(
        map_range(F->uses(), [](Use &U) { return &U; }));
    for (auto *U : Uses) {
      tagUse(U, F);
    }
  }

  return true;
}

//
// Pass registration
//

static RegisterPass<MemFuncInstrument> X(DEBUG_TYPE, "Instrument def sites",
                                         false, false);

static void registerUseSiteInstrumentPass(const PassManagerBuilder &,
                                          legacy::PassManagerBase &PM) {
  PM.add(new MemFuncInstrument());
}

static RegisterStandardPasses
    RegisterUseSiteInstrumentPass(PassManagerBuilder::EP_OptimizerLast,
                                  registerUseSiteInstrumentPass);

static RegisterStandardPasses
    RegisterUseSiteInstrumentPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                   registerUseSiteInstrumentPass);
