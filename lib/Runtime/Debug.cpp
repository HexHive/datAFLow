//===-- Debug.cpp - Debug runtime -------------------------------*- C++ -*-===//
///
/// \file
/// Debug runtime for analysing def-use chains
///
//===----------------------------------------------------------------------===//

#include <map>
#include <mutex>
#include <set>

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

using namespace llvm;

namespace {
/// Source code location
struct Location {
  const char *File;    ///< File name
  const char *Func;    ///< Function name
  const size_t LineNo; ///< Line number
  const uintptr_t PC;  ///< Program counter

  Location(const char *File, const char *Func, size_t Line, uintptr_t PC)
      : File(File), Func(Func), LineNo(Line), PC(PC) {}
  Location() = delete;

  bool operator<(const Location &Other) const { return PC < Other.PC; }
};

static json::Value toJSON(const Location &Loc) {
  return {Loc.File, Loc.Func, Loc.LineNo, Loc.PC};
}

/// Variable def site
struct DefInfo {
  const Location Loc; ///< Location
  const char *Var;    ///< Variable name

  DefInfo(const Location &Loc, const char *Var) : Loc(Loc), Var(Var) {}
  DefInfo() = delete;

  bool operator<(const DefInfo &Other) const { return Loc < Other.Loc; }
};

static json::Value toJSON(const DefInfo &Def) { return {Def.Loc, Def.Var}; }

using DefMap = std::map<tag_t, std::set<DefInfo>>;
using LocationCountMap = std::map<Location, size_t>;
using UseMap = std::map<DefInfo, LocationCountMap>;

static json::Value toJSON(const LocationCountMap &Locs) {
  json::Array Arr;

  for (const auto &[Loc, Count] : Locs) {
    Arr.push_back({Loc, Count});
  }

  return std::move(Arr);
}

static json::Value toJSON(const UseMap &Uses) {
  json::Array Arr;

  for (const auto &[Def, Locs] : Uses) {
    Arr.push_back({Def, Locs});
  }

  return std::move(Arr);
}

class VarLogger {
public:
  VarLogger() {
    std::string OutPath;
    raw_string_ostream SS(OutPath);

    if (const auto *Log = getenv("FUZZALLOC_TRACE_FILE")) {
      SS << Log;
    } else {
      SS << "dua." << getpid() << ".json";
    }
    SS.flush();

    std::error_code EC;
    OS.emplace(SS.str(), EC);
  }

  VarLogger(const VarLogger &) = delete;

  ~VarLogger() {
    // Cancel timer
    struct itimerval It;
    bzero(&It, sizeof(It));
    setitimer(ITIMER_REAL, &It, nullptr);

    // Serialize to JSON
    serialize();
  }

  void serialize() {
    if (!OS) {
      return;
    }

    json::Value V = [&]() -> json::Value {
      std::scoped_lock SL(Lock);
      return Uses;
    }();

    *OS << std::move(V);

    // Close and cleanup output stream
    OS->flush();
    OS->close();
    OS.reset();
  }

  void addDef(tag_t Tag, const char *File, const char *Func, size_t LineNo,
              uintptr_t PC, const char *Var) {
    Location Loc(File, Func, LineNo, PC);
    DefInfo Def(Loc, Var);

    std::scoped_lock SL(Lock);
    Defs[Tag].insert(Def);
  }

  void addUse(tag_t Tag, ptrdiff_t Offset, const char *File, const char *Func,
              size_t LineNo, uintptr_t PC) {
    Location Loc(File, Func, LineNo, PC);

    std::scoped_lock SL(Lock);
    for (auto &Def : Defs.at(Tag)) {
      Uses[Def][Loc]++;
    }
  }

private:
  Optional<raw_fd_ostream> OS;
  DefMap Defs;
  UseMap Uses;
  std::mutex Lock;
};

static VarLogger Log;

static void handleTimeout(int) { Log.serialize(); }

__attribute__((constructor)) static void initializeTimeout() {
  struct sigaction SA;
  struct itimerval It;

  if (const auto *Timeout = getenv("FUZZCOMET_TIMEOUT")) {
    unsigned T;
    if (to_integer(Timeout, T)) {
      bzero(&SA, sizeof(struct sigaction));
      SA.sa_handler = handleTimeout;
      sigaction(SIGALRM, &SA, nullptr);

      bzero(&It, sizeof(struct itimerval));
      It.it_value.tv_sec = T / 1000;
      It.it_value.tv_usec = (T % 1000) * 1000;
      setitimer(ITIMER_REAL, &It, nullptr);
    }
  }
}
} // anonymous namespace

//
// Callbacks
//

extern "C" {
void __dbg_def(tag_t Tag, const char *File, const char *Func, size_t LineNo,
               const char *Var) {
  Log.addDef(Tag, File, Func, LineNo, (uintptr_t)__builtin_return_address(0),
             Var);
}

void __dbg_use(void *Ptr, size_t Size, const char *File, const char *Func,
               size_t LineNo) {
  uintptr_t Base;
  tag_t DefTag = __bb_lookup(Ptr, &Base);
  if (likely(DefTag != kFuzzallocDefaultTag)) {
    ptrdiff_t Offset = (uintptr_t)Ptr - Base;
    Log.addUse(DefTag, Offset, File, Func, LineNo,
               (uintptr_t)__builtin_return_address(0));
  }
}
}
