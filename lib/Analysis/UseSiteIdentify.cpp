//===-- UseSiteIdentify.cpp - Identify use sites to track -------*- C++ -*-===//
///
/// \file
/// Identify use sites to track
///
//===----------------------------------------------------------------------===//

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "fuzzalloc/Analysis/UseSiteIdentify.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-use-site-identify"

STATISTIC(NumUseSites, "Number of use sites identified");

namespace {
//
// Command-line options
//

static cl::bits<UseSiteIdentify::UseSiteTypes> ClUseSitesToTrack(
    cl::desc("Use site type (to track)"),
    cl::values(clEnumValN(UseSiteIdentify::UseSiteTypes::Read,
                          "fuzzalloc-use-read", "Track reads (uses)"),
               clEnumValN(UseSiteIdentify::UseSiteTypes::Write,
                          "fuzzalloc-use-write", "Track writes (uses)"),
               clEnumValN(UseSiteIdentify::UseSiteTypes::Access,
                          "fuzzalloc-use-access", "Track accesses (uses)")));
} // anonymous namespace

char UseSiteIdentify::ID = 0;

void UseSiteIdentify::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool UseSiteIdentify::runOnFunction(Function &F) {
  for (const auto &I : instructions(F)) {
  }

  return false;
}

//
// Pass registration
//

static RegisterPass<UseSiteIdentify> X(DEBUG_TYPE, "Identify use sites", true,
                                       true);

static void registerUseSiteIdentifyPass(const PassManagerBuilder &,
                                        legacy::PassManagerBase &PM) {
  PM.add(new UseSiteIdentify());
}

static RegisterStandardPasses
    RegisterUseSiteIdentifyPass(PassManagerBuilder::EP_OptimizerLast,
                                registerUseSiteIdentifyPass);

static RegisterStandardPasses
    RegisterUseSiteIdentifyPass0(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                 registerUseSiteIdentifyPass);
