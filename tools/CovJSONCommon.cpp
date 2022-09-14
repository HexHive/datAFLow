//===-- CovJSONCommon.cpp - Common code for coverage generation -*- C++ -*-===//
///
/// \file
/// Common code for coverage accumulation.
///
//===----------------------------------------------------------------------===//

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/NativeFormatting.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>

#include "CovJSONCommon.h"

using namespace llvm;

namespace {
static int64_t clamp_uint64_to_int64(uint64_t u) {
  return std::min(u,
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}
} // anonymous namespace

Expected<size_t> getNumFiles(const StringRef &P) {
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

Expected<absl::btree_set<std::string>> getTestcases(const StringRef &Dir) {
  absl::btree_set<std::string> TCs;
  if (!sys::fs::is_directory(Dir)) {
    return createStringError(inconvertibleErrorCode(), "%s is not a directory",
                             Dir.str().c_str());
  }

  std::error_code EC;
  for (sys::fs::directory_iterator F(Dir, EC), E; F != E && !EC;
       F.increment(EC)) {
    TCs.insert(F->path());
  }
  if (EC) {
    return errorCodeToError(EC);
  }

  return TCs;
}

Error genCoverage(
    const StringRef &Target,                 ///< Path to instrumented target
    const ArrayRef<std::string> &TargetArgs, ///< Target program arguments
    const StringRef &InDir,  ///< Directory containing target inputs
    const StringRef &OutDir, ///< Directory storing coverage results
    unsigned NumThreads      ///< Number of simultaneous threads
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
    auto NumFilesOrErr = getNumFiles(InDir);
    if (auto E = NumFilesOrErr.takeError()) {
      return E;
    }

    NumThreads = ((*NumFilesOrErr) + 1) / 2;
    NumThreads =
        std::min(hardware_concurrency().compute_thread_count(), NumThreads);
  }
  ThreadPool Pool(hardware_concurrency(NumThreads));

  //
  // Generate raw coverage files
  //

  const auto GenProf = [&](const auto &Testcase) {
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

    // Set a default timeout. Assumes the target has been linked with the
    // LLVMCovRuntime or TracerRuntime
    //
    // TODO check this by reading the target's symbol table
    if (!getenv("LLVM_PROFILE_TIMEOUT")) {
      const auto Timeout = "LLVM_PROFILE_TIMEOUT=10000";
      Env.push_back(Timeout);
    }

    // Run target. Ignore output and return code
    sys::ExecuteAndWait(ProfInstArgs[0], ProfInstArgs, ArrayRef(Env),
                        Redirects);
  };

  std::error_code EC;
  for (sys::fs::directory_iterator F(InDir, EC), E; F != E && !EC;
       F.increment(EC)) {
    Pool.async(GenProf, F->path());
  }
  if (EC) {
    return errorCodeToError(EC);
  }

  Pool.wait();

  return Error::success();
}

json::Value toJSON(const TestcaseCoverage &Cov) {
  return {Cov.Path, clamp_uint64_to_int64(Cov.Count)};
}

llvm::Error writeJSON(const llvm::StringRef &Out,
                      const TestcaseCoverages &Cov) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Out, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    return llvm::errorCodeToError(EC);
  }

  OS << json::Value(Cov);
  OS.close();

  return llvm::Error::success();
}
