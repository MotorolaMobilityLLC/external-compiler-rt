//===-- asan_linux.cc -----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
//
// Posix-specific details.
//===----------------------------------------------------------------------===//
#if defined(__linux__) || defined(__APPLE__)

#include "asan_internal.h"
#include "asan_interceptors.h"
#include "asan_stack.h"
#include "asan_thread_registry.h"

#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

namespace __asan {

static void MaybeInstallSigaction(int signum,
                                  void (*handler)(int, siginfo_t *, void *)) {
  if (!AsanInterceptsSignal(signum))
    return;
  struct sigaction sigact;
  real_memset(&sigact, 0, sizeof(sigact));
  sigact.sa_sigaction = handler;
  sigact.sa_flags = SA_SIGINFO;
  CHECK(0 == real_sigaction(signum, &sigact, 0));
}

static void     ASAN_OnSIGSEGV(int, siginfo_t *siginfo, void *context) {
  uintptr_t addr = (uintptr_t)siginfo->si_addr;
  // Write the first message using the bullet-proof write.
  if (13 != AsanWrite(2, "ASAN:SIGSEGV\n", 13)) AsanDie();
  uintptr_t pc, sp, bp;
  GetPcSpBp(context, &pc, &sp, &bp);
  Report("ERROR: AddressSanitizer crashed on unknown address %p"
         " (pc %p sp %p bp %p T%d)\n",
         addr, pc, sp, bp,
         asanThreadRegistry().GetCurrentTidOrMinusOne());
  Printf("AddressSanitizer can not provide additional info. ABORTING\n");
  GET_STACK_TRACE_WITH_PC_AND_BP(kStackTraceMax, false, pc, bp);
  stack.PrintStack();
  ShowStatsAndAbort();
}

void InstallSignalHandlers() {
  MaybeInstallSigaction(SIGSEGV, ASAN_OnSIGSEGV);
  MaybeInstallSigaction(SIGBUS, ASAN_OnSIGSEGV);
}

void AsanDisableCoreDumper() {
  struct rlimit nocore;
  nocore.rlim_cur = 0;
  nocore.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &nocore);
}

void AsanDie() {
  _exit(FLAG_exitcode);
}

int GetPid() {
  return getpid();
}

uintptr_t GetThreadSelf() {
  return (uintptr_t)pthread_self();
}

// ---------------------- TSD ---------------- {{{1

static pthread_key_t tsd_key;
static bool tsd_key_inited = false;
void AsanTSDInit() {
  CHECK(!tsd_key_inited);
  tsd_key_inited = true;
  CHECK(0 == pthread_key_create(&tsd_key, 0));
}

void *AsanTSDGet() {
  CHECK(tsd_key_inited);
  return pthread_getspecific(tsd_key);
}

void AsanTSDSet(void *tsd) {
  CHECK(tsd_key_inited);
  pthread_setspecific(tsd_key, tsd);
}

}  // namespace __asan

#endif  // __linux__ || __APPLE_
