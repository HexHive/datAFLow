//===-- MemFuncInstrument.cpp - Instrument dynamic memory funcs -*- C++ -*-===//
///
/// \file
/// Instrument dynamic memory allocation functions
///
//===----------------------------------------------------------------------===//

#include <stdlib.h>

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
#include "fuzzalloc/Metadata.h"
#include "fuzzalloc/fuzzalloc.h"

using namespace llvm;

// Adapted from http://c-faq.com/lib/randrange.html
#define RAND(x, y) ((tag_t)((x) + random() / (RAND_MAX / ((y) - (x) + 1) + 1)))

#define DEBUG_TYPE "fuzzalloc-def-site-inst"

class MemFuncInstrument : public ModulePass {
public:
  static char ID;
  MemFuncInstrument() : ModulePass(ID) {}

  virtual void getAnalysisUsage(AnalysisUsage &) const override;
  virtual bool runOnModule(Module &) override;

private:
  ConstantInt *generateTag() const;
  FunctionType *getTaggedFunctionType(const Function *) const;
  Function *getTaggedFunction(const Function *) const;
  Function *tagFunction(const Function *) const;
  Instruction *tagCall(CallBase *, FunctionCallee) const;
  void tagUse(Use *, Function *) const;

  Module *Mod;
  LLVMContext *Ctx;
  IntegerType *TagTy;

  SmallPtrSet<Function *, 8> TaggedFuncs;
  ValueMap</* Original function */ Function *, /* Tagged function */ Function *>
      TaggedFuncMap;
};

char MemFuncInstrument::ID = 0;

ConstantInt *MemFuncInstrument::generateTag() const {
  return ConstantInt::get(TagTy, RAND(kFuzzallocTagMin, kFuzzallocTagMax));
}

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

Instruction *MemFuncInstrument::tagCall(CallBase *CB,
                                        FunctionCallee TaggedF) const {
  LLVM_DEBUG(dbgs() << "tagging call " << *CB << " (in function "
                    << CB->getFunction()->getName() << ") with call to "
                    << TaggedF.getCallee()->getName() << '\n');

  // The tag values depends on where the function call _is_. If the (tagged)
  // function is being called from within another tagged function, then just
  // pass the first argument (which is always the tag) straight through.
  // Otherwise, generate a new tag
  auto *Tag = [&]() -> Value * {
    auto *ParentF = CB->getFunction();
    if (TaggedFuncs.count(ParentF) > 0) {
      return ParentF->arg_begin();
    }
    return generateTag();
  }();

  // Make the tag the first argument and copy the original call's arguments
  // after
  SmallVector<Value *, 4> TaggedCallArgs = {Tag};
  TaggedCallArgs.append(CB->arg_begin(), CB->arg_end());

  // Create the tagged call
  auto *TaggedCall = [&]() -> CallBase * {
    const auto &Name = CB->getName() + ".tagged";
    if (isa<CallInst>(CB)) {
      return CallInst::Create(TaggedF, TaggedCallArgs, Name, CB);
    } else if (auto *Invoke = dyn_cast<InvokeInst>(CB)) {
      return InvokeInst::Create(TaggedF, Invoke->getNormalDest(),
                                Invoke->getUnwindDest(), TaggedCallArgs, Name,
                                CB);
    }
    llvm_unreachable("Unsupported call isntruction");
  }();
  TaggedCall->takeName(CB);
  TaggedCall->setCallingConv(CB->getCallingConv());
  TaggedCall->setAttributes(CB->getAttributes());
  TaggedCall->setMetadata(Mod->getMDKindID(kFuzzallocTaggedAllocMD),
                          MDNode::get(*Ctx, None));

  // Replace the original call
  CB->replaceAllUsesWith(TaggedCall);
  CB->eraseFromParent();

  return TaggedCall;
}

/// Replace the use of a memory allocation function with the tagged version
void MemFuncInstrument::tagUse(Use *U, Function *F) const {
  LLVM_DEBUG(dbgs() << "replacing user " << *U->getUser()
                    << " of tagged function " << F->getName() << '\n');

  auto *User = U->getUser();
  auto *TaggedF = TaggedFuncMap.lookup(F);

  if (auto *CB = dyn_cast<CallBase>(User)) {
    tagCall(CB, FunctionCallee(TaggedF));
  } else {
    llvm_unreachable("User not supported");
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

    TaggedFuncs.insert(TaggedF);
    TaggedFuncMap.insert({F, TaggedF});
  }

  for (auto [F, _] : TaggedFuncMap) {
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
