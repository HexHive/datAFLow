//===-- llvm-cov-json.cpp - LLVM coverage over time -------------*- C++ -*-===//
///
/// \file
/// Generate coverage data over time/executions by replaying sampled testcases
/// through an LLVM SanitizerCoverage-instrumented binary.
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
#include <llvm/Support/Program.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/raw_ostream.h>

#include "fuzzalloc/Streams.h"

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
// Classes
//

/// Accumulated coverage
struct Coverage {
  using TestcaseCov = std::pair</*Path=*/StringRef, /*Region count=*/int64_t>;

  const std::vector<TestcaseCov> Testcases; ///< Per testcase coverage
  const std::unique_ptr<coverage::CoverageMapping>
      Accumulated; ///< Accumulated coverage over all testcases

  Coverage() = delete;
  Coverage(const std::vector<TestcaseCov> &&TCs,
           std::unique_ptr<coverage::CoverageMapping> Accum)
      : Testcases(TCs), Accumulated(std::move(Accum)) {}
};
} // anonymous namespace

namespace llvm {
json::Value toJSON(const Coverage::TestcaseCov &Cov) {
  return {Cov.first, Cov.second};
}
} // namespace llvm

namespace {
//
// Global variables
//

static const ExitOnError ExitOnErr("llvm-cov-json: ");

//
// Helper functions
//

static Expected<size_t> getNumFiles(const StringRef &P) {
  std::error_code EC;
  size_t N = 0;
  for (sys::fs::directory_iterator F(P, EC), E; F != E && !EC;
       F.increment(EC)) {
    N++;
  }
  if (EC) {
    return errorCodeToError(EC);
  }
  return N;
}

static Expected<std::vector<std::string>> getTestcases(const StringRef &Dir) {
  std::vector<std::string> TCs;
  if (!sys::fs::is_directory(Dir)) {
    return createStringError(inconvertibleErrorCode(), "%s is not a directory",
                             Dir.str().c_str());
  }

  std::error_code EC;
  for (sys::fs::directory_iterator F(Dir, EC), E; F != E && !EC;
       F.increment(EC)) {
    TCs.push_back(F->path());
  }
  if (EC) {
    return errorCodeToError(EC);
  }

  return TCs;
}

//
// JSON functions
//
// Most of these functions are adapted from llvm-cov/CoverageExporterJson.cpp
//

static int64_t clamp_uint64_to_int64(uint64_t u) {
  return std::min(u,
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}

static json::Array renderRegion(const coverage::CountedRegion &Region) {
  return json::Array(
      {Region.LineStart, Region.ColumnStart, Region.LineEnd, Region.ColumnEnd,
       clamp_uint64_to_int64(Region.ExecutionCount), Region.FileID,
       Region.ExpandedFileID, static_cast<int64_t>(Region.Kind)});
}

static json::Array renderRegions(ArrayRef<coverage::CountedRegion> Regions) {
  json::Array RegionArray;
  for (const auto &Region : Regions) {
    if (Region.Kind == coverage::CountedRegion::RegionKind::CodeRegion &&
        Region.ExecutionCount > 0) {
      RegionArray.push_back(renderRegion(Region));
    }
  }
  return RegionArray;
}

static json::Array renderFunctions(
    const iterator_range<coverage::FunctionRecordIterator> &Functions) {
  json::Array FunctionArray;
  for (const auto &F : Functions) {
    FunctionArray.push_back(
        json::Object({{"name", F.Name},
                      {"count", clamp_uint64_to_int64(F.ExecutionCount)},
                      {"regions", renderRegions(F.CountedRegions)},
                      {"filenames", json::Array(F.Filenames)}}));
  }

  return FunctionArray;
}

/// Write final JSON file
static Error
writeJSON(const StringRef &Out, ///< Output JSON file
          const Coverage &Cov   ///< Accumulated coverage over all testcases
) {
  std::error_code EC;
  raw_fd_ostream OS(Out, EC, sys::fs::OF_Text);
  if (EC) {
    return errorCodeToError(EC);
  }

  OS << json::Object(
      {{"coverage", Cov.Testcases},
       {"accum_coverage",
        renderFunctions(Cov.Accumulated->getCoveredFunctions())}});
  OS.close();

  return Error::success();
}

//
// Coverage functions
//

static Error genCoverage(
    const StringRef &Target,                 ///< Path to instrumented target
    const ArrayRef<std::string> &TargetArgs, ///< Target program arguments
    const StringRef &InDir,  ///< Directory containing target inputs
    const StringRef &OutDir, ///< Directory storing coverage results
    unsigned NumThreads = 0  ///< Number of simultaneous threads
) {
  static const Optional<StringRef> Redirects[3] = {None, StringRef(),
                                                   StringRef()};

  const auto AtAtIt =
      std::find_if(TargetArgs.begin(), TargetArgs.end(),
                   [](const auto &S) { return S.compare("@@") == 0; });

  //
  // Initialize thread pool
  //

  if (NumThreads == 0) {
    NumThreads = (ExitOnErr((getNumFiles(InDir))) + 1) / 2;
    NumThreads =
        std::min(hardware_concurrency().compute_thread_count(), NumThreads);
  }
  ThreadPool Pool(hardware_concurrency(NumThreads));

  //
  // Generate raw coverage files
  //

  const auto GenProfRaw = [&](const auto &Testcase) {
    // Construct target command line
    SmallVector<StringRef, 16> ProfInstArgs{Target};
    ProfInstArgs.append(TargetArgs.begin(), TargetArgs.end());
    if (AtAtIt == TargetArgs.end()) {
      ProfInstArgs.push_back(Testcase);
    } else {
      ProfInstArgs[std::distance(TargetArgs.begin(), AtAtIt) + 1] = Testcase;
    }

    // Configure environment
    auto Env = toStringRefArray(environ);
    SmallString<32> ProfrawEnvVal;
    sys::path::append(ProfrawEnvVal, OutDir, sys::path::filename(Testcase));
    const auto ProfrawEnv =
        "LLVM_PROFILE_FILE=" + std::string(ProfrawEnvVal.c_str());
    Env.push_back(ProfrawEnv);

    // Run target. Ignore output and return code
    sys::ExecuteAndWait(ProfInstArgs[0], ProfInstArgs, ArrayRef(Env), Redirects,
                        /*SecondsToWait=*/10);
  };

  std::error_code EC;
  for (sys::fs::directory_iterator F(InDir, EC), E; F != E && !EC;
       F.increment(EC)) {
    Pool.async(GenProfRaw, F->path());
  }
  if (EC) {
    return errorCodeToError(EC);
  }

  Pool.wait();

  return Error::success();
}

/// Accumulate coverage over all testcases
static Expected<std::unique_ptr<Coverage>> accumulateCoverage(
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

  std::vector<Coverage::TestcaseCov> TestcaseCovs;
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
      return std::move(E);
    }
    const auto &ProfReader = std::move(*ProfReaderOrErr);

    //
    // Accumulate and index the raw coverage profile
    //

    if (ProfWriter.setIsIRLevelProfile(ProfReader->isIRLevelProfile(),
                                       ProfReader->hasCSIRLevelProfile())) {
      return createStringError(
          inconvertibleErrorCode(),
          "Merging IR generated profile with clang generated profile");
    }

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

    TestcaseCovs.emplace_back(CovFile, Count);

    const auto &Idx = CovFileEnum.index();
    if (Idx % ((NumCovFiles + (10 - 1)) / 10) == 0) {
      status_stream() << "  ";
      write_double(outs(), static_cast<float>(Idx) / NumCovFiles,
                   FloatStyle::Percent);
      outs() << " raw profiles parsed (count = " << Count << ")\r";
    }
  }
  outs() << '\n';

  return std::make_unique<Coverage>(std::move(TestcaseCovs), std::move(CovMap));
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
      "Generate coverage over time/executions by replaying sampled test cases "
      "through an LLVM SanCov-instrumented binary\n");

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
                  << " testcases using target `" << Target << "`...\n";
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
  ExitOnErr(writeJSON(OutJSON, *Cov));

  return 0;
}
