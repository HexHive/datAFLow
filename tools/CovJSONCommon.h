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

struct TestcaseCoverage {
  const std::string Path; ///< Testcase path
  const uint64_t Count;   ///< Number of coverage elements hit

  TestcaseCoverage() = delete;
  TestcaseCoverage(const llvm::StringRef &P, uint64_t C)
      : Path(P.str()), Count(C) {}
};

using TestcaseCoverages = std::vector<TestcaseCoverage>;

//
// Helper functions
//

/// Get the number of files in the given directory
llvm::Expected<size_t> getNumFiles(const llvm::StringRef &);

/// Get an ordered set of testcases in the given directory
llvm::Expected<absl::btree_set<std::string>>
getTestcases(const llvm::StringRef &);

llvm::Error genCoverage(const llvm::StringRef &,
                        const llvm::ArrayRef<std::string> &,
                        const llvm::StringRef &, const llvm::StringRef &,
                        unsigned = 0);

/// Write final JSON file
llvm::Error writeJSON(const llvm::StringRef &, const TestcaseCoverages &);

#endif // COV_JSON_COMMON_H
