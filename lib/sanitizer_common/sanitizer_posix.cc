//===-- sanitizer_posix.cc ------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is shared between AddressSanitizer and ThreadSanitizer
// run-time libraries and implements POSIX-specific functions from
// sanitizer_libc.h.
//===----------------------------------------------------------------------===//

#include "sanitizer_platform.h"
#if SANITIZER_LINUX || SANITIZER_MAC

#include "sanitizer_common.h"
#include "sanitizer_libc.h"
#include "sanitizer_procmaps.h"
#include "sanitizer_stacktrace.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace __sanitizer {

// ------------- sanitizer_common.h
uptr GetPageSize() {
  return sysconf(_SC_PAGESIZE);
}

uptr GetMmapGranularity() {
  return GetPageSize();
}

u32 GetUid() {
  return getuid();
}

uptr GetThreadSelf() {
  return (uptr)pthread_self();
}

void *MmapOrDie(uptr size, const char *mem_type) {
  size = RoundUpTo(size, GetPageSizeCached());
  uptr res = internal_mmap(0, size,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
  int reserrno;
  if (internal_iserror(res, &reserrno)) {
    static int recursion_count;
    if (recursion_count) {
      // The Report() and CHECK calls below may call mmap recursively and fail.
      // If we went into recursion, just die.
      RawWrite("ERROR: Failed to mmap\n");
      Die();
    }
    recursion_count++;
    Report("ERROR: %s failed to allocate 0x%zx (%zd) bytes of %s: %d\n",
           SanitizerToolName, size, size, mem_type, reserrno);
    DumpProcessMap();
    CHECK("unable to mmap" && 0);
  }
  return (void *)res;
}

void UnmapOrDie(void *addr, uptr size) {
  if (!addr || !size) return;
  uptr res = internal_munmap(addr, size);
  if (internal_iserror(res)) {
    Report("ERROR: %s failed to deallocate 0x%zx (%zd) bytes at address %p\n",
           SanitizerToolName, size, size, addr);
    CHECK("unable to unmap" && 0);
  }
}

void *MmapFixedNoReserve(uptr fixed_addr, uptr size) {
  uptr PageSize = GetPageSizeCached();
  uptr p = internal_mmap((void*)(fixed_addr & ~(PageSize - 1)),
      RoundUpTo(size, PageSize),
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
      -1, 0);
  int reserrno;
  if (internal_iserror(p, &reserrno))
    Report("ERROR: "
           "%s failed to allocate 0x%zx (%zd) bytes at address %p (%d)\n",
           SanitizerToolName, size, size, fixed_addr, reserrno);
  return (void *)p;
}

void *MmapFixedOrDie(uptr fixed_addr, uptr size) {
  uptr PageSize = GetPageSizeCached();
  uptr p = internal_mmap((void*)(fixed_addr & ~(PageSize - 1)),
      RoundUpTo(size, PageSize),
      PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANON | MAP_FIXED,
      -1, 0);
  int reserrno;
  if (internal_iserror(p, &reserrno)) {
    Report("ERROR:"
           " %s failed to allocate 0x%zx (%zd) bytes at address %p (%d)\n",
           SanitizerToolName, size, size, fixed_addr, reserrno);
    CHECK("unable to mmap" && 0);
  }
  return (void *)p;
}

void *Mprotect(uptr fixed_addr, uptr size) {
  return (void *)internal_mmap((void*)fixed_addr, size,
                               PROT_NONE,
                               MAP_PRIVATE | MAP_ANON | MAP_FIXED |
                               MAP_NORESERVE, -1, 0);
}

void FlushUnneededShadowMemory(uptr addr, uptr size) {
  madvise((void*)addr, size, MADV_DONTNEED);
}

void *MapFileToMemory(const char *file_name, uptr *buff_size) {
  uptr openrv = OpenFile(file_name, false);
  CHECK(!internal_iserror(openrv));
  fd_t fd = openrv;
  uptr fsize = internal_filesize(fd);
  CHECK_NE(fsize, (uptr)-1);
  CHECK_GT(fsize, 0);
  *buff_size = RoundUpTo(fsize, GetPageSizeCached());
  uptr map = internal_mmap(0, *buff_size, PROT_READ, MAP_PRIVATE, fd, 0);
  return internal_iserror(map) ? 0 : (void *)map;
}


static inline bool IntervalsAreSeparate(uptr start1, uptr end1,
                                        uptr start2, uptr end2) {
  CHECK(start1 <= end1);
  CHECK(start2 <= end2);
  return (end1 < start2) || (end2 < start1);
}

// FIXME: this is thread-unsafe, but should not cause problems most of the time.
// When the shadow is mapped only a single thread usually exists (plus maybe
// several worker threads on Mac, which aren't expected to map big chunks of
// memory).
bool MemoryRangeIsAvailable(uptr range_start, uptr range_end) {
  MemoryMappingLayout proc_maps(/*cache_enabled*/true);
  uptr start, end;
  while (proc_maps.Next(&start, &end,
                        /*offset*/0, /*filename*/0, /*filename_size*/0,
                        /*protection*/0)) {
    if (!IntervalsAreSeparate(start, end, range_start, range_end))
      return false;
  }
  return true;
}

void DumpProcessMap() {
  MemoryMappingLayout proc_maps(/*cache_enabled*/true);
  uptr start, end;
  const sptr kBufSize = 4095;
  char *filename = (char*)MmapOrDie(kBufSize, __FUNCTION__);
  Report("Process memory map follows:\n");
  while (proc_maps.Next(&start, &end, /* file_offset */0,
                        filename, kBufSize, /* protection */0)) {
    Printf("\t%p-%p\t%s\n", (void*)start, (void*)end, filename);
  }
  Report("End of process memory map.\n");
  UnmapOrDie(filename, kBufSize);
}

const char *GetPwd() {
  return GetEnv("PWD");
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
void GetStackTrace(StackTrace *stack, uptr max_s, uptr pc, uptr bp,
                   uptr stack_top, uptr stack_bottom, bool fast) {
#if !SANITIZER_CAN_FAST_UNWIND
  fast = false;
#endif
#if SANITIZER_MAC
  // Always unwind fast on Mac.
  (void)fast;
#else
  if (!fast || (stack_top == stack_bottom))
    return stack->SlowUnwindStack(pc, max_s);
#endif  // SANITIZER_MAC
  stack->size = 0;
  stack->trace[0] = pc;
  if (max_s > 1) {
    stack->max_size = max_s;
    stack->FastUnwindStack(pc, bp, stack_top, stack_bottom);
  }
}
#endif  // SANITIZER_GO

}  // namespace __sanitizer

#endif  // SANITIZER_LINUX || SANITIZER_MAC
