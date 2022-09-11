//===-- region-cov.cpp - Static region coverage analysis --------*- C++ -*-===//
///
/// \file
/// Static analysis of Clang's region coverage.
///
//===----------------------------------------------------------------------===//

#include <llvm/IR/LLVMContext.h>
#include <llvm/ProfileData/Coverage/CoverageMappingReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>

using namespace llvm;

namespace {
//
// Command-line options
//

static cl::OptionCategory CRCat("Clang source-based coverage analysis options");
static cl::list<std::string> ObjFilenames(cl::Positional,
                                          cl::desc("<Obj file>"),
                                          cl::value_desc("path"), cl::OneOrMore,
                                          cl::cat(CRCat));

//
// Classes
//

/// Clang region statistics
struct RegionStatistics {
  unsigned NumCodeRegions;
  unsigned NumExpansionRegions;
  unsigned NumSkippedRegions;
  unsigned NumGapRegions;
  unsigned NumBranchRegions;

  RegionStatistics()
      : NumCodeRegions(0), NumExpansionRegions(0), NumSkippedRegions(0),
        NumGapRegions(0) {}

  void updateRegion(const coverage::CounterMappingRegion &R) {
    switch (R.Kind) {
    case coverage::CounterMappingRegion::RegionKind::CodeRegion:
      NumCodeRegions++;
      break;
    case coverage::CounterMappingRegion::RegionKind::ExpansionRegion:
      NumExpansionRegions++;
      break;
    case coverage::CounterMappingRegion::RegionKind::SkippedRegion:
      NumSkippedRegions++;
      break;
    case coverage::CounterMappingRegion::RegionKind::GapRegion:
      NumGapRegions++;
      break;
    case coverage::CounterMappingRegion::RegionKind::BranchRegion:
      NumBranchRegions++;
      break;
    }
  }

  /// Dump statistics
  void dump(raw_ostream &OS) const {
    OS << "num_code_regions=" << NumCodeRegions << '\n';
    OS << "num_expansion_regions=" << NumExpansionRegions << '\n';
    OS << "num_skipped_regions=" << NumSkippedRegions << '\n';
    OS << "num_gap_regions=" << NumGapRegions << '\n';
  }
};

//
// Helper functions
//

raw_ostream &operator<<(raw_ostream &OS, RegionStatistics &S) {
  S.dump(OS);
  return OS;
}

//
// Global variables
//

static const ExitOnError ExitOnErr("region-cov: ");
} // anonymous namespace

int main(int argc, char *argv[]) {
  // Parse command-line arguments
  cl::HideUnrelatedOptions(CRCat);
  cl::ParseCommandLineOptions(argc, argv,
                              "Clang source-based coverage analysis\n");

  RegionStatistics Stats;

  for (const auto &File : ObjFilenames) {
    // Load object file
    const auto &CovMapping = ExitOnErr(errorOrToExpected(MemoryBuffer::getFile(
        File, /*FileSize=*/-1, /*RequiresNullTerminator=*/false)));
    const auto &CovMappingBufRef = CovMapping->getMemBufferRef();

    // Create readers for coverage mapping data (stored in the object file)
    SmallVector<std::unique_ptr<MemoryBuffer>, 4> Buffers;
    auto CovReaders = ExitOnErr(
        coverage::BinaryCoverageReader::create(CovMappingBufRef, "", Buffers));

    // Read mapping regions and update stats
    for (const auto &CovReader : CovReaders) {
      for (auto RecordOrErr : *CovReader) {
        const auto &Record = ExitOnErr(std::move(RecordOrErr));

        for (const auto &Region : Record.MappingRegions) {
          Stats.updateRegion(Region);
        }
      }
    }
  }

  // Print stats
  errs() << Stats;

  return 0;
}
