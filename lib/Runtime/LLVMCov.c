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
void __llvm_profile_register_write_file_atexit(void);
int __llvm_profile_write_file(void);

static void handleTimeout(int Sig) {
  if (__llvm_profile_write_file()) {
    fprintf(stderr, "[llvm-cov] Failed to write profile\n");
  }
}

__attribute__((constructor)) static void initializeTimeout() {
  const char *Timeout = getenv("LLVM_PROFILE_TIMEOUT");
  struct sigaction SA;
  struct itimerval It;

  __llvm_profile_initialize_file();

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

  __llvm_profile_register_write_file_atexit();
}
