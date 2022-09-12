//===-- static-dua.cpp - Static def/use analysis ----------------*- C++ -*-===//
///
/// \file
/// Perform a static def-use chain analysis.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>

#include "MemoryModel/PointerAnalysis.h"

#include "fuzzalloc/Analysis/DefUseChain.h"
#include "fuzzalloc/Streams.h"

using namespace llvm;
using namespace SVF;

namespace {
//
// Command-line options
//

static cl::OptionCategory Cat("Static def-use chain analysis");
static cl::opt<std::string> BCFilename(cl::Positional, cl::desc("<BC file>"),
                                       cl::value_desc("path"), cl::Required,
                                       cl::cat(Cat));
static cl::opt<std::string> OutJSON("out", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::cat(Cat));
} // anonymous namespace

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv, "Static def-use chain analysis");

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
  auto &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);

  legacy::PassManager PM;
  auto *DUA = new DefUseChain;
  PM.add(DUA);
  PM.run(*Mod);

  const auto &DefUseChains = DUA->getDefUseChains();

  // Save Output JSON
  if (!OutJSON.empty()) {
    const auto &NumDefs = DefUseChains.size();
    json::Array J;
    J.reserve(NumDefs);

    status_stream() << "Serializing def/use chains to JSON...\n";
    for (const auto &DUEnum : enumerate(DefUseChains)) {
      const auto &[Def, Uses] = DUEnum.value();

      J.push_back({toJSON(Def), toJSON(Uses)});

      const auto &Idx = DUEnum.index();
      if (Idx % ((NumDefs + (10 - 1)) / 10) == 0) {
        status_stream() << "  ";
        write_double(outs(), static_cast<float>(Idx) / NumDefs,
                     FloatStyle::Percent);
        outs() << " defs serialized\r";
      }
    }

    std::error_code EC;
    raw_fd_ostream OS(OutJSON, EC, sys::fs::OF_Text);
    if (EC) {
      error_stream() << "Unable to open " << OutJSON << '\n';
      ::exit(1);
    }

    status_stream() << "Writing to " << OutJSON << "...\n";
    OS << std::move(J);
    OS.flush();
    OS.close();
  }

  // Cleanup
  llvm_shutdown();

  return 0;
}
