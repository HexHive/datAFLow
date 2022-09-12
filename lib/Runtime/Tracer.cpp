//===-- Tracer.cpp - Tracer runtime -----------------------------*- C++ -*-===//
///
/// \file
/// Tracer runtime for analysing def-use chains
///
//===----------------------------------------------------------------------===//

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

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"

#include "fuzzalloc/Runtime/BaggyBounds.h"
#include "fuzzalloc/fuzzalloc.h"

#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

using namespace llvm;

extern "C" {
/// Source-level location
struct __attribute__((packed)) SrcLocation {
  const char *File;    ///< File name
  const char *Func;    ///< Function name
  const size_t Line;   ///< Line number
  const size_t Column; ///< Column number
};

/// Source-level variable def site
struct __attribute__((packed)) SrcDef {
  const SrcLocation Loc; ///< Location
  const char *Var;       ///< Variable name
};
} // extern "C"

namespace {
static json::Value toJSON(const SrcLocation &Loc) {
  return {Loc.File, Loc.Func, Loc.Line, Loc.Column};
}

static json::Value toJSON(const SrcDef &Def) {
  return {Def.Var, toJSON(Def.Loc)};
}

using LocationCountMap = absl::flat_hash_map<const SrcLocation *, size_t>;
using DefUseMap = absl::flat_hash_map<const SrcDef *, LocationCountMap>;

static json::Value toJSON(const LocationCountMap &Locs) {
  std::vector<json::Value> Vec;

  for (const auto &[Loc, Count] : Locs) {
    Vec.push_back({toJSON(*Loc), Count});
  }

  return Vec;
}

static json::Value toJSON(const DefUseMap &DefUses) {
  std::vector<json::Value> Vec;

  for (const auto &[Def, Locs] : DefUses) {
    Vec.push_back({toJSON(*Def), toJSON(Locs)});
  }

  return Vec;
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
      absl::MutexLock ML(&Lock);
      return toJSON(DefUses);
    }();

    *OS << std::move(V);

    // Close and cleanup output stream
    OS->flush();
    OS->close();
    OS.reset();
  }

  void addDef(const SrcDef *Def) {
    absl::MutexLock ML(&Lock);
    DefUses.emplace(Def, LocationCountMap());
  }

  void addUse(const SrcDef *Def, ptrdiff_t Offset, const SrcLocation *Loc) {
    absl::MutexLock ML(&Lock);
    DefUses[Def][Loc]++;
  }

private:
  Optional<raw_fd_ostream> OS;
  DefUseMap DefUses;
  absl::Mutex Lock;
};

static VarLogger Log;

static void handleTimeout(int) { Log.serialize(); }

__attribute__((constructor)) static void __dua_trace_initialize_timeout() {
  struct sigaction SA;
  struct itimerval It;

  if (const auto *Timeout = getenv("FUZZALLOC_TIMEOUT")) {
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
void __tracer_def(const SrcDef *Def) { Log.addDef(Def); }

void __tracer_use(const SrcLocation *Loc, void *Ptr, size_t Size) {
  uintptr_t Base;
  SrcDef **Def = (SrcDef **)__bb_lookup(Ptr, &Base, sizeof(SrcDef *));

  if (likely(Def != nullptr)) {
    ptrdiff_t Offset = (uintptr_t)Ptr - Base;
    Log.addUse(*Def, Offset, Loc);
  }
}
}
