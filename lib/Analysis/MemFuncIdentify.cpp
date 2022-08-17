//===-- MemFuncIdentify.cpp - Identify memory allocation funcs --*- C++ -*-===//
///
/// \file
/// Identify dynamic memory allocation function calls
///
//===----------------------------------------------------------------------===//

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SpecialCaseList.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/MemFuncIdentify.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-mem-func-identify"

namespace {
//
// Classes
//

class MemFuncList {
private:
  std::unique_ptr<SpecialCaseList> SCL;

public:
  MemFuncList() = default;
  MemFuncList(std::unique_ptr<SpecialCaseList> List) : SCL(std::move(List)) {}

  bool isIn(const Function &F) const {
    return SCL && SCL->inSection("fuzzalloc", "fun", F.getName());
  }
};

//
// Command-line options
//

static cl::opt<std::string> ClMemFuncs(
    "fuzzalloc-custom-mem-funcs",
    cl::desc("Special case list of custom memory allocation functions"),
    cl::value_desc("path"));

//
// Global variables
//

static unsigned NumMemAllocFuncs = 0;

//
// Helper functions
//

static MemFuncList getMemFuncList() {
  if (ClMemFuncs.empty()) {
    return MemFuncList();
  }

  if (!sys::fs::exists(ClMemFuncs)) {
    std::string Err;
    raw_string_ostream OS(Err);
    OS << "fuzzalloc memory function list does not exist at " << ClMemFuncs;
    OS.flush();
    report_fatal_error(Err);
  }

  return MemFuncList(
      SpecialCaseList::createOrDie({ClMemFuncs}, *vfs::getRealFileSystem()));
}

} // anonymous namespace

char MemFuncIdentify::ID = 0;

void MemFuncIdentify::getMemoryBuiltins(const Function &F) {
  const auto *TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);

  for (const auto &I : instructions(F)) {
    const auto *CB = dyn_cast<CallBase>(&I);
    if (!CB || CB->isIndirectCall()) {
      continue;
    }

    // Check if the call is to a builtin memory allocation function
    auto *Callee = CB->getCalledFunction();
    if (isAllocationFn(CB, TLI)) {
      MemFuncs.insert(Callee);
    }
  }
}

void MemFuncIdentify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool MemFuncIdentify::runOnModule(Module &M) {
  const auto &CustomMemFuncs = getMemFuncList();

  for (auto &F : M) {
    // Check for calls to builtin dynamic memory allocation functions. Doing it
    // via calls (rather than the functions themselves) allows us to reuse
    // LLVM's MemoryBuiltins functionality
    getMemoryBuiltins(F);

    // Check for custom memory allocation functions
    if (CustomMemFuncs.isIn(F)) {
      MemFuncs.insert(&F);
    }
  }

  NumMemAllocFuncs = MemFuncs.size();

  return false;
}

//
// Pass registration
//

static RegisterPass<MemFuncIdentify>
    X(DEBUG_TYPE, "Identify dynamic memory allocation calls", true, true);

static void registerMemFuncIdentifyPass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new MemFuncIdentify());
}

static RegisterStandardPasses
    RegisterMemFuncIdentifyPass(PassManagerBuilder::EP_OptimizerLast,
                                registerMemFuncIdentifyPass);

static RegisterStandardPasses
    RegisterMemFuncIdentifyPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerMemFuncIdentifyPass);
