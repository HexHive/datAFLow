//===-- CovJSONCommon.cpp - Common code for coverage generation -*- C++ -*-===//
///
/// \file
/// Common code for coverage accumulation.
///
//===----------------------------------------------------------------------===//

#include <llvm/Support/FileSystem.h>

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
