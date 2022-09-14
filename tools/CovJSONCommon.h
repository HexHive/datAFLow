//===-- CovJSONCommon.h - Common code for coverage generation ---*- C++ -*-===//
///
/// \file
/// Common code for coverage accumulation.
///
//===----------------------------------------------------------------------===//

#ifndef COV_JSON_COMMON_H
#define COV_JSON_COMMON_H

#include <stdint.h>

#include <vector>

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include "absl/container/btree_set.h"

//
// Classes
//

/// Accumulated coverage
template <typename T> struct Coverage {
  using TestcaseCov = std::pair</*Path=*/std::string, /*Count=*/int64_t>;

  const std::vector<TestcaseCov> Testcases; ///< Per testcase coverage
  const std::unique_ptr<T>
      Accumulated; ///< Accumulated coverage over all testcases

  Coverage() = delete;
  Coverage(const std::vector<TestcaseCov> &&TCs, std::unique_ptr<T> Accum)
      : Testcases(TCs), Accumulated(std::move(Accum)) {}
};

//
// Helper functions
//

/// Get the number of files in the given directory
llvm::Expected<size_t> getNumFiles(const llvm::StringRef &);

/// Get an ordered set of testcases in the given directory
llvm::Expected<absl::btree_set<std::string>>
getTestcases(const llvm::StringRef &);

int64_t clamp_uint64_to_int64(uint64_t);

/// Write final JSON file
template <typename T>
llvm::Error writeJSON(const llvm::StringRef &Out, const Coverage<T> &Cov) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Out, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    return llvm::errorCodeToError(EC);
  }

  OS << toJSON(Cov);
  OS.close();

  return llvm::Error::success();
}

#endif // COV_JSON_COMMON_H
