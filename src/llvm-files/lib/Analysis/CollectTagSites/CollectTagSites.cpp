//===-- CollectTagSites.cpp - Collects sites to tag in later passes -------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This pass collects values (functions, global variables/alias, and structs)
/// that will require tagging by the \c TagDynamicAllocs pass.
///
//===----------------------------------------------------------------------===//

#include <map>

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SpecialCaseList.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Utils/FuzzallocUtils.h"
#include "debug.h" // from afl

using namespace llvm;
using namespace llvm::sys::fs;

#define DEBUG_TYPE "fuzzalloc-collect-tag-sites"

static cl::opt<std::string>
    ClLogPath("fuzzalloc-tag-log",
              cl::desc("Path to log file containing values to tag"),
              cl::Required);

static cl::opt<std::string> ClMemFuncs(
    "fuzzalloc-mem-funcs",
    cl::desc("Path to file listing custom memory allocation functions"));

STATISTIC(NumOfFunctions, "Number of functions to tag.");
STATISTIC(NumOfGlobalVariables, "Number of global variables to tag.");
STATISTIC(NumOfGlobalAliases, "Number of global aliases to tag.");
STATISTIC(NumOfStructOffsets, "Number of struct offsets to tag.");
STATISTIC(NumOfFunctionArgs, "Number of function arguments to tag.");

namespace {

/// List of dynamic memory allocation wrapper functions
class FuzzallocMemFuncList {
private:
  std::unique_ptr<SpecialCaseList> SCL;

public:
  FuzzallocMemFuncList() = default;

  FuzzallocMemFuncList(std::unique_ptr<SpecialCaseList> List)
      : SCL(std::move(List)){};

  bool isIn(const Function &F) const {
    return SCL && SCL->inSection("fuzzalloc", "fun", F.getName());
  }
};

/// Log values that require tagging later on
class CollectTagSites : public ModulePass {
private:
  Module *Mod;
  FuzzallocMemFuncList MemFuncs;

  SmallPtrSet<const Function *, 8> FunctionsToTag;
  SmallPtrSet<const GlobalVariable *, 8> GlobalVariablesToTag;
  SmallPtrSet<const GlobalAlias *, 8> GlobalAliasesToTag;
  std::map<StructOffset, const Function *> StructOffsetsToTag;
  SmallPtrSet<const Argument *, 8> FunctionArgsToTag;

  void tagUser(const User *, const Function *, const TargetLibraryInfo *);
  void saveTagSites() const;

public:
  static char ID;
  CollectTagSites() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &) const override;
  bool doInitialization(Module &) override;
  bool runOnModule(Module &) override;
};

} // anonymous namespace

char CollectTagSites::ID = 0;

static FuzzallocMemFuncList getMemFunclist() {
  if (ClMemFuncs.empty()) {
    return FuzzallocMemFuncList();
  }

  if (!sys::fs::exists(ClMemFuncs)) {
    std::string Err;
    raw_string_ostream OS(Err);
    OS << "fuzzalloc memory function list does not exist at " << ClMemFuncs;
    OS.flush();
    report_fatal_error(Err);
  }

  return FuzzallocMemFuncList(
      SpecialCaseList::createOrDie({ClMemFuncs}, *vfs::getRealFileSystem()));
}

void CollectTagSites::tagUser(const User *U, const Function *F,
                              const TargetLibraryInfo *TLI) {
  LLVM_DEBUG(dbgs() << "recording user " << *U << " of tagged function "
                    << F->getName() << '\n');

  if (auto *CB = dyn_cast<CallBase>(U)) {
    // The result of a dynamic memory allocation function call is typically
    // cast. Strip this cast to determine the actual function being called
    auto *CalledValue = CB->getCalledOperand()->stripPointerCasts();

    // Ignore calls to dynamic memory allocation functions - we can just tag
    // them directly later
    if (CalledValue == F) {
      return;
    }

    // Otherwise the user must be a function argument
    for (unsigned I = 0; I < CB->getNumArgOperands(); ++I) {
      if (CB->getArgOperand(I) == F) {
        this->FunctionArgsToTag.insert(
            cast<Function>(CalledValue)->arg_begin() + I);
        NumOfFunctionArgs++;
      }
    }
  } else if (auto *Store = dyn_cast<StoreInst>(U)) {
    auto *StorePtrOp = Store->getPointerOperand();

    // Strip away bitcasts before we do anything else
    if (Operator::getOpcode(StorePtrOp) == Instruction::BitCast) {
      StorePtrOp = cast<Operator>(StorePtrOp)->getOperand(0);
    }

    if (auto *GV = dyn_cast<GlobalVariable>(StorePtrOp)) {
      // Store to a global variable
      this->GlobalVariablesToTag.insert(GV);
      NumOfGlobalVariables++;
    } else if (auto *GEP = dyn_cast<GEPOperator>(StorePtrOp)) {
      // Store to an offset within some object. We only handle stores to structs
      //
      // If the store is storing to something pointed to by a GEP, first check
      // that the store is to a struct (based on the GEP pointer type).
      //
      // If it is a struct, get the byte offset that we are storing to within
      // that struct. Then walk the struct (and any nested struct) to get the
      // actual index that the dynamic allocation function is actually being
      // stored to.

      //  TODO check that the store is actually to a struct
      assert(isa<StructType>(
          GEP->getPointerOperandType()->getPointerElementType()));
      StructType *StructTy = cast<StructType>(
          GEP->getPointerOperandType()->getPointerElementType());

      const DataLayout &DL = this->Mod->getDataLayout();
      APInt GEPOffset(DL.getIndexTypeSizeInBits(StorePtrOp->getType()), 0);
      if (!GEP->accumulateConstantOffset(DL, GEPOffset)) {
        // TODO handle this
        assert(false);
      }

      auto StructOffset =
          getStructOffset(StructTy, GEPOffset.getSExtValue(), DL);
      assert(StructOffset.hasValue());
      this->StructOffsetsToTag.emplace(*StructOffset, F);
      NumOfStructOffsets++;
    } else {
      assert(false && "Unsupported store pointer operand");
    }
  } else if (auto *ConstStruct = dyn_cast<ConstantStruct>(U)) {
    // Constant struct user
    unsigned Idx = 0;
    for (auto &Op : ConstStruct->operands()) {
      if (Op == F) {
        this->StructOffsetsToTag.emplace(
            std::make_pair(ConstStruct->getType(), Idx), F);
        NumOfStructOffsets++;
      }
      Idx++;
    }
  } else if (auto *GV = dyn_cast<GlobalVariable>(U)) {
    // Global variable user
    this->GlobalVariablesToTag.insert(GV);
    NumOfGlobalVariables++;
  } else if (auto *GA = dyn_cast<GlobalAlias>(U)) {
    // Global alias user
    this->GlobalAliasesToTag.insert(GA);
    NumOfGlobalAliases++;
  } else {
    // Warn on unsupported users
    std::string UserStr;
    raw_string_ostream OS(UserStr);
    OS << *U;

    WARNF("[%s] Unsupported user %s", this->Mod->getName().str().c_str(),
          UserStr.c_str());
  }
}

void CollectTagSites::saveTagSites() const {
  std::error_code EC;
  raw_fd_ostream Output(ClLogPath, EC,
                        OpenFlags::OF_Text | OpenFlags::OF_Append);
  if (EC) {
    std::string Err;
    raw_string_ostream OS(Err);
    OS << "unable to open fuzzalloc tag log at " << ClLogPath << ": "
       << EC.message();
    OS.flush();
    report_fatal_error(Err);
  }

  // Add a comment
  Output << CommentStart << this->Mod->getName() << '\n';

  // Save functions
  for (auto *F : this->FunctionsToTag) {
    if (!F) {
      continue;
    }

    Output << FunctionLogPrefix << LogSeparator << F->getName() << LogSeparator
           << *F->getFunctionType() << '\n';
  }

  // Save global variables
  for (auto *GV : this->GlobalVariablesToTag) {
    Output << GlobalVariableLogPrefix << LogSeparator << GV->getName() << '\n';
  }

  // Save global aliases
  for (auto *GA : this->GlobalAliasesToTag) {
    Output << GlobalAliasLogPrefix << LogSeparator << GA->getName() << '\n';
  }

  // Save struct mappings
  for (const auto &StructWithFunc : this->StructOffsetsToTag) {
    auto *StructTy = StructWithFunc.first.first;
    unsigned Offset = StructWithFunc.first.second;
    auto *F = StructWithFunc.second;

    if (!StructTy->hasName()) {
      continue;
    }

    Output << StructOffsetLogPrefix << LogSeparator << StructTy->getName()
           << LogSeparator << Offset << LogSeparator << F->getName()
           << LogSeparator << *F->getFunctionType() << '\n';
  }

  // Save function arguments
  for (auto *Arg : this->FunctionArgsToTag) {
    Output << FunctionArgLogPrefix << LogSeparator
           << Arg->getParent()->getName() << LogSeparator << Arg->getArgNo()
           << '\n';
  }
}

void CollectTagSites::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool CollectTagSites::doInitialization(Module &M) {
  this->Mod = &M;
  this->MemFuncs = getMemFunclist();

  return false;
}

bool CollectTagSites::runOnModule(Module &M) {
  // Collect all the values to tag

  if (const auto *MallocF = M.getFunction("malloc")) {
    this->FunctionsToTag.insert(MallocF);
  }
  if (const auto *CallocF = M.getFunction("calloc")) {
    this->FunctionsToTag.insert(CallocF);
  }
  if (const auto *ReallocF = M.getFunction("realloc")) {
    this->FunctionsToTag.insert(ReallocF);
  }

  for (const auto &F : M.functions()) {
    if (this->MemFuncs.isIn(F)) {
      this->FunctionsToTag.insert(&F);
      NumOfFunctions++;
    }
  }

  for (auto *F : this->FunctionsToTag) {
    if (!F) {
      continue;
    }

    for (auto *U : F->users()) {
      const TargetLibraryInfo *TLI =
          &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(*F);
      tagUser(U, F, TLI);
    }
  }

  // Save the collected values
  saveTagSites();

  printStatistic(M, NumOfFunctions);
  printStatistic(M, NumOfFunctionArgs);
  printStatistic(M, NumOfGlobalVariables);
  printStatistic(M, NumOfGlobalAliases);
  printStatistic(M, NumOfStructOffsets);

  return false;
}

static RegisterPass<CollectTagSites>
    X("fuzzalloc-collect-tag-sites", "Collect values that will require tagging",
      false, false);

static void registerCollectTagSitesPass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new CollectTagSites());
}

static RegisterStandardPasses
    RegisterCollectTagSitesPass(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                registerCollectTagSitesPass);

static RegisterStandardPasses
    RegisterCollectTagSitesPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerCollectTagSitesPass);
