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
// Linux-specific details.
//===----------------------------------------------------------------------===//
#ifdef __linux__

#include "asan_interceptors.h"
#include "asan_internal.h"
#include "asan_procmaps.h"
#include "asan_thread.h"

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

extern char _DYNAMIC[];

namespace __asan {

void *AsanDoesNotSupportStaticLinkage() {
  // This will fail to link with -static.
  return &_DYNAMIC;
}

static void *asan_mmap(void *addr, size_t length, int prot, int flags,
                int fd, uint64_t offset) {
# if __WORDSIZE == 64
  return (void *)syscall(__NR_mmap, addr, length, prot, flags, fd, offset);
# else
  return (void *)syscall(__NR_mmap2, addr, length, prot, flags, fd, offset);
# endif
}

void *AsanMmapSomewhereOrDie(size_t size, const char *mem_type) {
  size = RoundUpTo(size, kPageSize);
  void *res = asan_mmap(0, size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  if (res == (void*)-1) {
    OutOfMemoryMessageAndDie(mem_type, size);
  }
  return res;
}

void *AsanMmapFixedNoReserve(uintptr_t fixed_addr, size_t size) {
  return asan_mmap((void*)fixed_addr, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
                   0, 0);
}

void *AsanMmapFixedReserve(uintptr_t fixed_addr, size_t size) {
  return asan_mmap((void*)fixed_addr, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                   0, 0);
}

void *AsanMprotect(uintptr_t fixed_addr, size_t size) {
  return asan_mmap((void*)fixed_addr, size,
                   PROT_NONE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
                   0, 0);
}

void AsanUnmapOrDie(void *addr, size_t size) {
  if (!addr || !size) return;
  int res = syscall(__NR_munmap, addr, size);
  if (res != 0) {
    Report("Failed to unmap\n");
    ASAN_DIE;
  }
}

ssize_t AsanWrite(int fd, const void *buf, size_t count) {
  return (ssize_t)syscall(__NR_write, fd, buf, count);
}

int AsanOpenReadonly(const char* filename) {
  return open(filename, O_RDONLY);
}

ssize_t AsanRead(int fd, void *buf, size_t count) {
  return (ssize_t)syscall(__NR_read, fd, buf, count);
}

int AsanClose(int fd) {
  return close(fd);
}

AsanProcMaps::AsanProcMaps() {
  proc_self_maps_buff_len_ =
      ReadFileToBuffer("/proc/self/maps", &proc_self_maps_buff_,
                       &proc_self_maps_buff_mmaped_size_, 1 << 20);
  CHECK(proc_self_maps_buff_len_ > 0);
  // AsanWrite(2, proc_self_maps_buff_, proc_self_maps_buff_len_);
  Reset();
}

AsanProcMaps::~AsanProcMaps() {
  AsanUnmapOrDie(proc_self_maps_buff_, proc_self_maps_buff_mmaped_size_);
}

void AsanProcMaps::Reset() {
  current_ = proc_self_maps_buff_;
}

bool AsanProcMaps::Next(uint64_t *start, uint64_t *end,
                        uint64_t *offset, char filename[],
                        size_t filename_size) {
  char *last = proc_self_maps_buff_ + proc_self_maps_buff_len_;
  if (current_ >= last) return false;
  int consumed = 0;
  char flags[10];
  int major, minor;
  uint64_t inode;
  char *next_line = (char*)internal_memchr(current_, '\n', last - current_);
  if (next_line == NULL)
    next_line = last;
  if (SScanf(current_,
             "%llx-%llx %4s %llx %x:%x %lld %n",
             start, end, flags, offset, &major, &minor,
             &inode, &consumed) != 7)
    return false;
  current_ += consumed;
  // Skip spaces.
  while (current_ < next_line && *current_ == ' ')
    current_++;
  // Fill in the filename.
  size_t i = 0;
  while (current_ < next_line) {
    if (filename && i < filename_size - 1)
      filename[i++] = *current_;
    current_++;
  }
  if (filename && i < filename_size)
    filename[i] = 0;
  current_ = next_line + 1;
  return true;
}

void AsanThread::SetThreadStackTopAndBottom() {
  if (tid() == 0) {
    // This is the main thread. Libpthread may not be initialized yet.
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0);

    // Find the mapping that contains a stack variable.
    AsanProcMaps proc_maps;
    uint64_t start, end, offset;
    uint64_t prev_end = 0;
    while (proc_maps.Next(&start, &end, &offset, NULL, 0)) {
      if ((uintptr_t)&rl < end)
        break;
      prev_end = end;
    }
    CHECK((uintptr_t)&rl >= start && (uintptr_t)&rl < end);

    // Get stacksize from rlimit, but clip it so that it does not overlap
    // with other mappings.
    size_t stacksize = rl.rlim_cur;
    if (stacksize > end - prev_end)
      stacksize = end - prev_end;
    if (stacksize > kMaxThreadStackSize)
      stacksize = kMaxThreadStackSize;
    stack_top_ = end;
    stack_bottom_ = end - stacksize;
    CHECK(AddrIsInStack((uintptr_t)&rl));
    return;
  }
  pthread_attr_t attr;
  CHECK(pthread_getattr_np(pthread_self(), &attr) == 0);
  size_t stacksize = 0;
  void *stackaddr = NULL;
  pthread_attr_getstack(&attr, &stackaddr, &stacksize);
  pthread_attr_destroy(&attr);

  stack_top_ = (uintptr_t)stackaddr + stacksize;
  stack_bottom_ = (uintptr_t)stackaddr;
  // When running with unlimited stack size, we still want to set some limit.
  // The unlimited stack size is caused by 'ulimit -s unlimited'.
  // Also, for some reason, GNU make spawns subrocesses with unlimited stack.
  if (stacksize > kMaxThreadStackSize) {
    stack_bottom_ = stack_top_ - kMaxThreadStackSize;
  }
  CHECK(AddrIsInStack((uintptr_t)&attr));
}


}  // namespace __asan

#endif  // __linux__
