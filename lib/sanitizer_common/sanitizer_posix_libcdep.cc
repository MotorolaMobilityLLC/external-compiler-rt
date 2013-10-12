//===-- sanitizer_posix_libcdep.cc ----------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is shared between AddressSanitizer and ThreadSanitizer
// run-time libraries and implements libc-dependent POSIX-specific functions
// from sanitizer_libc.h.
//===----------------------------------------------------------------------===//

#include "sanitizer_platform.h"

#if SANITIZER_LINUX || SANITIZER_MAC
#include "sanitizer_common.h"
#include "sanitizer_stacktrace.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace __sanitizer {

u32 GetUid() {
  return getuid();
}

uptr GetThreadSelf() {
  return (uptr)pthread_self();
}

void FlushUnneededShadowMemory(uptr addr, uptr size) {
  madvise((void*)addr, size, MADV_DONTNEED);
}

void DisableCoreDumper() {
  struct rlimit nocore;
  nocore.rlim_cur = 0;
  nocore.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &nocore);
}

bool StackSizeIsUnlimited() {
  struct rlimit rlim;
  CHECK_EQ(0, getrlimit(RLIMIT_STACK, &rlim));
  return (rlim.rlim_cur == (uptr)-1);
}

void SetStackSizeLimitInBytes(uptr limit) {
  struct rlimit rlim;
  rlim.rlim_cur = limit;
  rlim.rlim_max = limit;
  if (setrlimit(RLIMIT_STACK, &rlim)) {
    Report("ERROR: %s setrlimit() failed %d\n", SanitizerToolName, errno);
    Die();
  }
  CHECK(!StackSizeIsUnlimited());
}

void SleepForSeconds(int seconds) {
  sleep(seconds);
}

void SleepForMillis(int millis) {
  usleep(millis * 1000);
}

void Abort() {
  abort();
}

int Atexit(void (*function)(void)) {
#ifndef SANITIZER_GO
  return atexit(function);
#else
  return 0;
#endif
}

int internal_isatty(fd_t fd) {
  return isatty(fd);
}

#ifndef SANITIZER_GO
void StackTrace::Unwind(uptr max_depth, uptr pc, uptr bp, uptr stack_top,
                        uptr stack_bottom, bool fast) {
  // Check if fast unwind is available. Fast unwind is the only option on Mac.
  if (!SANITIZER_CAN_FAST_UNWIND)
    fast = false;
  else if (SANITIZER_MAC)
    fast = true;

  if (!fast)
    SlowUnwindStack(pc, max_depth);
  else
    FastUnwindStack(pc, bp, stack_top, stack_bottom, max_depth);
}
#endif  // SANITIZER_GO

}  // namespace __sanitizer

#endif
