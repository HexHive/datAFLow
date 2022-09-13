//===-- LLVMCov.cpp - Clang source-based coverage runtime ---------*- C -*-===//
///
/// \file
/// Runtime for fine-grained control of Clang's source-based coverage
///
//===----------------------------------------------------------------------===//

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/time.h>

int __llvm_profile_runtime = 0;
void __llvm_profile_initialize_file(void);
int __llvm_profile_write_file(void);

static void writeProfile() {
  struct itimerval It;

  // Cancel timer
  bzero(&It, sizeof(struct itimerval));
  setitimer(ITIMER_REAL, &It, NULL);

  __llvm_profile_initialize_file();
  __llvm_profile_write_file();
}

static void handleTimeout(int Sig) { exit(0); }

__attribute__((constructor)) static void __llvm_cov_initialize_timeout() {
  const char *Timeout = getenv("LLVM_PROFILE_TIMEOUT");
  struct sigaction SA;
  struct itimerval It;

  if (Timeout) {
    long long T = atoll(Timeout);
    bzero(&SA, sizeof(struct sigaction));
    SA.sa_handler = handleTimeout;
    sigaction(SIGALRM, &SA, NULL);

    bzero(&It, sizeof(struct itimerval));
    It.it_value.tv_sec = T / 1000;
    It.it_value.tv_usec = (T % 1000) * 1000;
    setitimer(ITIMER_REAL, &It, NULL);
  }

  atexit(writeProfile);
}
