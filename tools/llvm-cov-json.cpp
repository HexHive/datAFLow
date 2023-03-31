//===-- llvm-cov-json.cpp - LLVM coverage over time -------------*- C++ -*-===//
///
/// \file
/// Generate coverage data over time by replaying sampled testcases through an
/// LLVM SanitizerCoverage-instrumented binary.
///
//===----------------------------------------------------------------------===//

#include <unistd.h>

#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ProfileData/Coverage/CoverageMappingReader.h>
#include <llvm/ProfileData/InstrProfReader.h>
#include <llvm/ProfileData/InstrProfWriter.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/NativeFormatting.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/raw_ostream.h>

#include "fuzzalloc/Streams.h"

#include "CovJSONCommon.h"

using namespace llvm;

namespace {
//
// Command-line options
//

static cl::OptionCategory LLVMCovJSON("LLVM coverage options");

static cl::opt<std::string>
    QueueDir("i", cl::desc("Queue directory (containing fuzzer test cases)"),
             cl::value_desc("path"), cl::Required, cl::cat(LLVMCovJSON));
static cl::opt<std::string> OutJSON("o", cl::desc("Output JSON"),
                                    cl::value_desc("path"), cl::Required,
                                    cl::cat(LLVMCovJSON));
static cl::opt<unsigned> NumThreads("j", cl::desc("Number of threads"),
                                    cl::value_desc("N"), cl::init(0),
                                    cl::cat(LLVMCovJSON));
static cl::opt<std::string> Target(cl::Positional, cl::desc("<target>"),
                                   cl::Required, cl::cat(LLVMCovJSON));
static cl::list<std::string> TargetArgs(cl::ConsumeAfter, cl::desc("[...]"),
                                        cl::cat(LLVMCovJSON));

//
// Global variables
//

static const ExitOnError ExitOnErr("llvm-cov-json: ");

//
// Coverage functions
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

  // Get the actual coverage files
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

  // Initialize a profile writer (for indexing the raw coverage profiles)
  SmallVector<std::unique_ptr<coverage::CoverageMappingReader>, 4> CovReaders;
  SmallVector<std::unique_ptr<MemoryBuffer>, 4> Buffers;
  std::unique_ptr<coverage::CoverageMapping> CovMap;

  InstrProfWriter ProfWriter(/*Sparse=*/true);

  //
  // Parse llvm-cov coverage
  //

  for (const auto &CovFileEnum : enumerate(*TestcasesOrErr)) {
    const auto &CovFile = CovFileEnum.value();

    // Create instrumentation profile reader
    auto ProfReaderOrErr = InstrProfReader::create(CovFile.c_str());
    if (auto E = ProfReaderOrErr.takeError()) {
      warning_stream() << '`' << CovFile << "`: " << E << ". Skipping...\n";
      continue;
    }
    const auto &ProfReader = std::move(*ProfReaderOrErr);

    //
    // Accumulate and index the raw coverage profile
    //

    for (auto &Func : *ProfReader) {
      ProfWriter.addRecord(std::move(Func), /*Weight=*/1, nullptr);
    }

    if (ProfReader->hasError()) {
      if (auto E = ProfReader->getError()) {
        return std::move(E);
      }
    }

    auto IndexedReaderOrErr =
        IndexedInstrProfReader::create(ProfWriter.writeBuffer());
    if (auto E = IndexedReaderOrErr.takeError()) {
      return std::move(E);
    }

    //
    // Initialize coverage readers
    //

    Buffers.clear();
    auto CovReadersOrErr = coverage::BinaryCoverageReader::create(
        CovMappingBufOrErr.get()->getMemBufferRef(), "", Buffers);
    if (auto E = CovReadersOrErr.takeError()) {
      return std::move(E);
    }

    CovReaders.clear();
    for (auto &Reader : *CovReadersOrErr) {
      CovReaders.push_back(std::move(Reader));
    }

    //
    // Load coverage mapping
    //

    auto CovMapOrErr =
        coverage::CoverageMapping::load(CovReaders, *IndexedReaderOrErr.get());
    if (auto E = CovMapOrErr.takeError()) {
      return std::move(E);
    }
    CovMap = std::move(*CovMapOrErr);

    //
    // Calculate coverage
    //

    auto Count = 0;
    for (const auto &Func : CovMap->getCoveredFunctions()) {
      // This function was never executed
      if (!Func.ExecutionCount) {
        continue;
      }

      // We only care about code regions
      for (const auto &R : Func.CountedRegions) {
        if (R.Kind == coverage::CountedRegion::RegionKind::CodeRegion &&
            R.ExecutionCount > 0) {
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
  cl::HideUnrelatedOptions(LLVMCovJSON);
  cl::ParseCommandLineOptions(
      argc, argv,
      "Generate coverage over time by replaying sampled test cases through an "
      "LLVM SanCov-instrumented binary\n");

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
