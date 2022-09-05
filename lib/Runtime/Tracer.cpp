//===-- Tracer.cpp - Tracer runtime -----------------------------*- C++ -*-===//
///
/// \file
/// Tracer runtime for analysing def-use chains
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

extern "C" {
/// Source code location
struct __attribute__((packed)) SrcLocation {
  const char *File;    ///< File name
  const char *Func;    ///< Function name
  const size_t Line;   ///< Line number
  const size_t Column; ///< Column number
};

/// Variable def site
struct __attribute__((packed)) DefInfo {
  const SrcLocation Loc; ///< Location
  const char *Var;       ///< Variable name
};
}

namespace std {
template <> struct less<SrcLocation> {
  constexpr bool operator()(const SrcLocation &LHS,
                            const SrcLocation &RHS) const {
    return LHS.File < RHS.File;
  }
};

template <> struct less<DefInfo> {
  constexpr bool operator()(const DefInfo &LHS, const DefInfo &RHS) const {
    return std::less<SrcLocation>{}(LHS.Loc, RHS.Loc);
  }
};
} // namespace std

namespace {
static inline SrcLocation mkSrcLocation(const char *File, const char *Func,
                                        size_t Line, size_t Column) {
  return {File, Func, Line, Column};
}

static inline DefInfo mkDefInfo(const SrcLocation &Loc, const char *Var) {
  return {Loc, Var};
}

static json::Value toJSON(const SrcLocation &Loc) {
  return {Loc.File, Loc.Func, Loc.Line, Loc.Column};
}

static json::Value toJSON(const DefInfo &Def) {
  return {toJSON(Def.Loc), Def.Var};
}

using DefMap = std::map<tag_t, std::set<DefInfo>>;
using SrcLocationCountMap = std::map<SrcLocation, size_t>;
using UseMap = std::map<DefInfo, SrcLocationCountMap>;

static json::Value toJSON(const SrcLocationCountMap &Locs) {
  json::Array Arr;

  for (const auto &[Loc, Count] : Locs) {
    Arr.push_back({toJSON(Loc), Count});
  }

  return std::move(Arr);
}

static json::Value toJSON(const UseMap &Uses) {
  json::Array Arr;

  for (const auto &[Def, Locs] : Uses) {
    Arr.push_back({toJSON(Def), toJSON(Locs)});
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
      return toJSON(Uses);
    }();

    *OS << std::move(V);

    // Close and cleanup output stream
    OS->flush();
    OS->close();
    OS.reset();
  }

  void addDef(const char *File, const char *Func, size_t Line, size_t Column,
              uintptr_t PC, const char *Var) {
    SrcLocation Loc = mkSrcLocation(File, Func, Line, Column);
    DefInfo Def = mkDefInfo(Loc, Var);

    std::scoped_lock SL(Lock);
  }

  void addDef(tag_t Tag, const char *File, const char *Func, size_t Line,
              size_t Column, uintptr_t PC, const char *Var) {
    SrcLocation Loc = mkSrcLocation(File, Func, Line, Column);
    DefInfo Def = mkDefInfo(Loc, Var);

    std::scoped_lock SL(Lock);
    Defs[Tag].insert(Def);
  }

  void addUse(const DefInfo *Def, ptrdiff_t Offset, SrcLocation &Loc,
              uintptr_t PC) {
    std::scoped_lock SL(Lock);
    // for (auto &Def : Defs.at(Tag)) {
    // Uses[Def][Loc]++;
    //}
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
void __tracer_def(tag_t Tag, const char *File, const char *Func, size_t Line,
               size_t Column, const char *Var) {
  Log.addDef(Tag, File, Func, Line, Column,
             (uintptr_t)__builtin_return_address(0), Var);
}

void __tracer_use(void *Ptr, size_t Size, const char *File, const char *Func,
               size_t Line, size_t Column) {
  uintptr_t Base;
  DefInfo *Def = (DefInfo *)__bb_lookup(Ptr, &Base, sizeof(DefInfo));
  if (likely(Def != nullptr)) {
    ptrdiff_t Offset = (uintptr_t)Ptr - Base;
    SrcLocation Loc = mkSrcLocation(File, Func, Line, Column);
//    Log.addUse(*Def, Offset, Loc, (uintptr_t)__builtin_return_address(0));
  }
}
}
