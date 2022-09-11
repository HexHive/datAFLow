//===-- subsumption.cpp - Static def/use subsumption analysis ---*- C++ -*-===//
///
/// \file
/// Perform a static def-use subsumption analysis.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Support/CommandLine.h>

#include "fuzzalloc/Analysis/DefUseChain.h"
#include "fuzzalloc/Streams.h"

using namespace llvm;

namespace {
//
// Command-line options
//

static cl::OptionCategory Cat("Static def-use subsumption analysis");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
} // anonymous namespace

int main(int argc, char *argv[]) {
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv,
                              "Static def-use subsumption analysis");

  // Parse bitcode
  status_stream() << "Parsing " << BCFilename << "...\n";
  LLVMContext Ctx;
  SMDiagnostic Err;
  auto Mod = parseIRFile(BCFilename, Err, Ctx);
  if (!Mod) {
    error_stream() << "Failed to parse `" << BCFilename
                   << "`: " << Err.getMessage() << '\n';
    ::exit(1);
  }

  // Get static def-use chains
  status_stream() << "Running LLVM passes...\n";

  auto &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);

  legacy::PassManager PM;
  auto *DUA = new DefUseChain;
  PM.add(DUA);
  PM.run(*Mod);

  const auto &DefUseChains = DUA->getDefUseChains();
  for (const auto &[Def, Uses] : DefUseChains) {
  }

  return 0;
}
