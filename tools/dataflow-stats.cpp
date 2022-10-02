//===-- dataflow-stats.cpp - Collect instrumentation stats ------*- C++ -*-===//
///
/// \file
/// Get some instrumentation stats
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/WithColor.h>

#include "fuzzalloc/Analysis/CollectStats.h"
#include "fuzzalloc/Streams.h"

using namespace llvm;

namespace {
//
// Command-line options
//

static cl::OptionCategory Cat("Collect fuzzalloc statistics");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
} // anonymous namespace

int main(int argc, char *argv[]) {
  cl::HideUnrelatedOptions(Cat);
  cl::ParseCommandLineOptions(argc, argv, "Collect fuzzalloc statistics");

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

  // Run pass
  status_stream() << "Collecting stats...\n";
  legacy::PassManager PM;
  auto *GetStats = new CollectStats;
  PM.add(GetStats);
  PM.run(*Mod);

  GetStats->print(errs(), Mod.get());

  // Cleanup
  llvm_shutdown();

  return 0;
}
