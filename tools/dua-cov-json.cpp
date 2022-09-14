//===-- dua-cov-json.cpp - DUA coverage over time ---------------*- C++ -*-===//
///
/// \file
/// Generate coverage data over time by replaying sampled testcases
/// through a tracer-instrumented binary.
///
//===----------------------------------------------------------------------===//

#include <unistd.h>

#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

#include "fuzzalloc/Streams.h"

#include "CovJSONCommon.h"

using namespace llvm;

namespace {
//
// Classes
//

struct Location {
  const std::string File;
  const std::string Func;
  const size_t Line;
  const size_t Column;
  const Optional<uintptr_t> PC;

  Location() = delete;
  Location(const StringRef &File, const StringRef &Func, size_t Line,
           size_t Column, uintptr_t PC)
      : File(File.str()), Func(Func.str()), Line(Line), Column(Column), PC(PC) {
  }
  Location(const StringRef &File, const StringRef &Func, size_t Line,
           size_t Column)
      : File(File.str()), Func(Func.str()), Line(Line), Column(Column),
        PC(None) {}

  constexpr bool operator==(const Location &Other) const {
    return File == Other.File && Func == Other.Func && Line == Other.Line &&
           Column == Other.Column && PC == Other.PC;
  }

  template <typename H> friend H AbslHashValue(H Hash, const Location &Loc) {
    return H::combine(std::move(Hash), Loc.File, Loc.Func, Loc.Line, Loc.Column,
                      Loc.PC.getValueOr(0));
  }
};

struct Definition {
  const Location Loc;
  const std::string Var;

  Definition() = delete;
  Definition(const Location &Loc, const StringRef &V)
      : Loc(Loc), Var(V.str()) {}

  constexpr bool operator==(const Definition &Other) const {
    return Loc == Other.Loc && Var == Other.Var;
  }

  template <typename H> friend H AbslHashValue(H Hash, const Definition &Def) {
    return H::combine(std::move(Hash), Def.Loc, Def.Var);
  }
};

//
// Aliases
//

using LocationSet = absl::flat_hash_set<Location>;
using DefUseMap = absl::flat_hash_map<Definition, LocationSet>;

//
// Command-line options
//

static cl::OptionCategory DUACovJSON("DUA coverage options");

static cl::opt<std::string>
    QueueDir("i", cl::desc("Queue directory (containing fuzzer test cases)"),
             cl::value_desc("path"), cl::Required, cl::cat(DUACovJSON));
static cl::opt<std::string> OutJSON("o", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::Required,
                                    cl::cat(DUACovJSON));
static cl::opt<unsigned> NumThreads("j", cl::desc("Number of threads"),
                                    cl::value_desc("N"), cl::init(0),
                                    cl::cat(DUACovJSON));
static cl::opt<std::string> Target(cl::Positional, cl::desc("<target>"),
                                   cl::Required, cl::cat(DUACovJSON));
static cl::list<std::string> TargetArgs(cl::ConsumeAfter, cl::desc("[...]"),
                                        cl::cat(DUACovJSON));

//
// Global variables
//

static const ExitOnError ExitOnErr("dua-cov-json: ");

//
// Helper functions
//

/// Accumulate coverage over all testcases
static Expected<TestcaseCoverages> accumulateCoverage(
    const StringRef &CovDir, ///< Directory containing raw coverage files
    const StringRef &Target  ///< Clang source-code-instrumented target program
) {
  // Get the number of coverage files
  auto NumCovFilesOrErr = getNumFiles(CovDir);
  if (auto E = NumCovFilesOrErr.takeError()) {
    return std::move(E);
  }
  const auto NumCovFiles = *NumCovFilesOrErr;

  // Get the actual coverage JSON files
  auto TestcasesOrErr = getTestcases(CovDir);
  if (auto E = TestcasesOrErr.takeError()) {
    return std::move(E);
  }

  // Load the target (for profile instrumentation)
  auto CovMappingBufOrErr = MemoryBuffer::getFile(Target);
  if (const auto &EC = CovMappingBufOrErr.getError()) {
    return errorCodeToError(EC);
  }

  TestcaseCoverages TestcaseCovs;
  TestcaseCovs.reserve(NumCovFiles);

  DefUseMap AccumDefUses;
  DefUseMap DefUses;

  //
  // Parse tracer coverage
  //

  for (const auto &CovFileEnum : enumerate(*TestcasesOrErr)) {
    const auto &CovFile = CovFileEnum.value();
    DefUses.clear();

    //
    // Parse JSON coverage
    //

    const auto CovOrErr = MemoryBuffer::getFile(CovFile);
    if (const auto &EC = CovOrErr.getError()) {
      return errorCodeToError(EC);
    }

    auto CovJSONOrErr = json::parse(CovOrErr.get()->getBuffer());
    if (auto E = CovJSONOrErr.takeError()) {
      return std::move(E);
    }

    assert(CovJSONOrErr->getAsArray());
    for (const auto &JDUAs : *CovJSONOrErr->getAsArray()) {
      assert(JDUAs.getAsArray());
      assert(JDUAs.getAsArray()->size() == 2);

      const auto &JDef = (*JDUAs.getAsArray())[0];
      const auto &JUses = (*JDUAs.getAsArray())[1];

      // Parse def
      assert(JDef.getAsArray());
      assert(JDef.getAsArray()->size() == 2);

      assert((*JDef.getAsArray())[0].getAsString());
      assert((*JDef.getAsArray())[1].getAsArray());
      assert((*JDef.getAsArray())[1].getAsArray()->size() == 4);

      const auto &JDefLoc = *(*JDef.getAsArray())[1].getAsArray();

      const auto &DefVar = (*JDef.getAsArray())[0].getAsString();
      const auto &DefFile = JDefLoc[0].getAsString();
      const auto &DefFunc = JDefLoc[1].getAsString();
      const auto &DefLine = JDefLoc[2].getAsInteger();
      const auto &DefColumn = JDefLoc[3].getAsInteger();

      const Location DefLoc(*DefFile, *DefFunc, *DefLine, *DefColumn);
      const Definition Def(DefLoc, *DefVar);

      // Parse uses (ignore the count)
      LocationSet Uses;
      assert(JUses.getAsArray());
      for (const auto &JUseAndCount : *JUses.getAsArray()) {
        assert(JUseAndCount.getAsArray());
        assert(JUseAndCount.getAsArray()->size() == 2);
        assert((*JUseAndCount.getAsArray())[0].getAsArray());

        // Parse use
        const auto &JUseLoc = *(*JUseAndCount.getAsArray())[0].getAsArray();
        assert(JUseLoc.size() == 5);

        const auto &UseFile = JUseLoc[0].getAsString();
        const auto &UseFunc = JUseLoc[1].getAsString();
        const auto &UseLine = JUseLoc[2].getAsInteger();
        const auto &UseColumn = JUseLoc[3].getAsInteger();
        const auto &UsePC = JUseLoc[4].getAsInteger();

        Uses.emplace(*UseFile, *UseFunc, *UseLine, *UseColumn, *UsePC);
      }

      DefUses.try_emplace(Def, Uses);
    }

    //
    // Calculate coverage
    //

    auto Count = 0;
    for (const auto &[Def, Uses] : DefUses) {
      for (const auto &Use : Uses) {
        if (AccumDefUses[Def].emplace(Use).second) {
          Count++;
        }
      }
    }

    TestcaseCovs.emplace_back(sys::path::filename(CovFile).str(), Count);

    const auto &Idx = CovFileEnum.index();
    if (Idx % ((NumCovFiles + (10 - 1)) / 10) == 0) {
      status_stream() << "  ";
      write_double(outs(), static_cast<float>(Idx) / NumCovFiles,
                   FloatStyle::Percent);
      outs() << " raw profiles parsed (count = " << Count << ")\r";
    }
  }
  outs() << '\n';

  return TestcaseCovs;
}
} // anonymous namespace

//
// The main function
//

int main(int argc, char *argv[]) {
  // Parse command-line arguments
  cl::HideUnrelatedOptions(DUACovJSON);
  cl::ParseCommandLineOptions(
      argc, argv,
      "Generate coverage over time by replaying sampled test cases through a "
      "tracer-instrumented binary\n");

  if (!sys::fs::is_directory(QueueDir)) {
    error_stream() << QueueDir << " is an invalid directory\n";
    return 1;
  }

  SmallString<16> CovDir;
  ExitOnErr(
      errorCodeToError(sys::fs::createUniqueDirectory("coverage", CovDir)));

  // Collect raw coverage
  const auto NumTestcases = ExitOnErr(getNumFiles(QueueDir));
  status_stream() << "Generating raw profiles for " << NumTestcases
                  << " testcases (in `" << QueueDir << "`) using target `"
                  << Target << "`...\n";
  ExitOnErr(genCoverage(Target, TargetArgs, QueueDir, CovDir, NumThreads));
  const auto NumCovFiles = ExitOnErr(getNumFiles(CovDir));
  success_stream() << NumCovFiles << " raw profiles generated\n";

  // Accumulate coverage
  status_stream() << "Accumulating " << NumCovFiles << " raw profiles in "
                  << CovDir << '\n';
  const auto &Cov = ExitOnErr(accumulateCoverage(CovDir, Target));
  sys::fs::remove_directories(CovDir);
  success_stream() << "Coverage accumulation complete\n";

  // Write to JSON
  status_stream() << "Writing coverage to " << OutJSON << "...\n";
  ExitOnErr(writeJSON(OutJSON, Cov));

  return 0;
}
