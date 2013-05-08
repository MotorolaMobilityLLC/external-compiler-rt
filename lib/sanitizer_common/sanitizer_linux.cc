//===-- sanitizer_linux.cc ------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is shared between AddressSanitizer and ThreadSanitizer
// run-time libraries and implements linux-specific functions from
// sanitizer_libc.h.
//===----------------------------------------------------------------------===//

#include "sanitizer_platform.h"
#if SANITIZER_LINUX

#include "sanitizer_common.h"
#include "sanitizer_internal_defs.h"
#include "sanitizer_libc.h"
#include "sanitizer_linux.h"
#include "sanitizer_mutex.h"
#include "sanitizer_placement_new.h"
#include "sanitizer_procmaps.h"
#include "sanitizer_stacktrace.h"

#ifdef __x86_64__
#include <asm/prctl.h>
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <unwind.h>

#if !SANITIZER_ANDROID
#include <sys/signal.h>
#endif

// <linux/time.h>
struct kernel_timeval {
  long tv_sec;
  long tv_usec;
};

// <linux/futex.h> is broken on some linux distributions.
const int FUTEX_WAIT = 0;
const int FUTEX_WAKE = 1;

// Are we using 32-bit or 64-bit syscalls?
// x32 (which defines __x86_64__) has SANITIZER_WORDSIZE == 32
// but it still needs to use 64-bit syscalls.
#if defined(__x86_64__) || SANITIZER_WORDSIZE == 64
# define SANITIZER_LINUX_USES_64BIT_SYSCALLS 1
#else
# define SANITIZER_LINUX_USES_64BIT_SYSCALLS 0
#endif

#ifdef __x86_64__
extern "C" int arch_prctl(int code, __sanitizer::uptr *addr);
#endif

namespace __sanitizer {

#ifdef __x86_64__
#include "sanitizer_syscall_linux_x86_64.inc"
#else
#include "sanitizer_syscall_generic.inc"
#endif

// --------------- sanitizer_libc.h
uptr internal_mmap(void *addr, uptr length, int prot, int flags,
                    int fd, u64 offset) {
#if SANITIZER_LINUX_USES_64BIT_SYSCALLS
  return internal_syscall(__NR_mmap, addr, length, prot, flags, fd, offset);
#else
  return internal_syscall(__NR_mmap2, addr, length, prot, flags, fd, offset);
#endif
}

uptr internal_munmap(void *addr, uptr length) {
  return internal_syscall(__NR_munmap, addr, length);
}

uptr internal_close(fd_t fd) {
  return internal_syscall(__NR_close, fd);
}

uptr internal_open(const char *filename, int flags) {
  return internal_syscall(__NR_open, filename, flags);
}

uptr internal_open(const char *filename, int flags, u32 mode) {
  return internal_syscall(__NR_open, filename, flags, mode);
}

uptr OpenFile(const char *filename, bool write) {
  return internal_open(filename,
      write ? O_WRONLY | O_CREAT /*| O_CLOEXEC*/ : O_RDONLY, 0660);
}

uptr internal_read(fd_t fd, void *buf, uptr count) {
  sptr res;
  HANDLE_EINTR(res, (sptr)internal_syscall(__NR_read, fd, buf, count));
  return res;
}

uptr internal_write(fd_t fd, const void *buf, uptr count) {
  sptr res;
  HANDLE_EINTR(res, (sptr)internal_syscall(__NR_write, fd, buf, count));
  return res;
}

#if !SANITIZER_LINUX_USES_64BIT_SYSCALLS
static void stat64_to_stat(struct stat64 *in, struct stat *out) {
  internal_memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atime = in->st_atime;
  out->st_mtime = in->st_mtime;
  out->st_ctime = in->st_ctime;
  out->st_ino = in->st_ino;
}
#endif

uptr internal_stat(const char *path, void *buf) {
#if SANITIZER_LINUX_USES_64BIT_SYSCALLS
  return internal_syscall(__NR_stat, path, buf);
#else
  struct stat64 buf64;
  int res = internal_syscall(__NR_stat64, path, &buf64);
  stat64_to_stat(&buf64, (struct stat *)buf);
  return res;
#endif
}

uptr internal_lstat(const char *path, void *buf) {
#if SANITIZER_LINUX_USES_64BIT_SYSCALLS
  return internal_syscall(__NR_lstat, path, buf);
#else
  struct stat64 buf64;
  int res = internal_syscall(__NR_lstat64, path, &buf64);
  stat64_to_stat(&buf64, (struct stat *)buf);
  return res;
#endif
}

uptr internal_fstat(fd_t fd, void *buf) {
#if SANITIZER_LINUX_USES_64BIT_SYSCALLS
  return internal_syscall(__NR_fstat, fd, buf);
#else
  struct stat64 buf64;
  int res = internal_syscall(__NR_fstat64, fd, &buf64);
  stat64_to_stat(&buf64, (struct stat *)buf);
  return res;
#endif
}

uptr internal_filesize(fd_t fd) {
  struct stat st;
  if (internal_fstat(fd, &st))
    return -1;
  return (uptr)st.st_size;
}

uptr internal_dup2(int oldfd, int newfd) {
  return internal_syscall(__NR_dup2, oldfd, newfd);
}

uptr internal_readlink(const char *path, char *buf, uptr bufsize) {
  return internal_syscall(__NR_readlink, path, buf, bufsize);
}

uptr internal_unlink(const char *path) {
  return internal_syscall(__NR_unlink, path);
}

uptr internal_sched_yield() {
  return internal_syscall(__NR_sched_yield);
}

void internal__exit(int exitcode) {
  internal_syscall(__NR_exit_group, exitcode);
  Die();  // Unreachable.
}

uptr internal_execve(const char *filename, char *const argv[],
                     char *const envp[]) {
  return internal_syscall(__NR_execve, filename, argv, envp);
}

// ----------------- sanitizer_common.h
bool FileExists(const char *filename) {
  struct stat st;
  if (internal_stat(filename, &st))
    return false;
  // Sanity check: filename is a regular file.
  return S_ISREG(st.st_mode);
}

uptr GetTid() {
  return internal_syscall(__NR_gettid);
}

u64 NanoTime() {
  kernel_timeval tv;
  internal_syscall(__NR_gettimeofday, &tv, 0);
  return (u64)tv.tv_sec * 1000*1000*1000 + tv.tv_usec * 1000;
}

void GetThreadStackTopAndBottom(bool at_initialization, uptr *stack_top,
                                uptr *stack_bottom) {
  static const uptr kMaxThreadStackSize = 256 * (1 << 20);  // 256M
  CHECK(stack_top);
  CHECK(stack_bottom);
  if (at_initialization) {
    // This is the main thread. Libpthread may not be initialized yet.
    struct rlimit rl;
    CHECK_EQ(getrlimit(RLIMIT_STACK, &rl), 0);

    // Find the mapping that contains a stack variable.
    MemoryMappingLayout proc_maps(/*cache_enabled*/true);
    uptr start, end, offset;
    uptr prev_end = 0;
    while (proc_maps.Next(&start, &end, &offset, 0, 0, /* protection */0)) {
      if ((uptr)&rl < end)
        break;
      prev_end = end;
    }
    CHECK((uptr)&rl >= start && (uptr)&rl < end);

    // Get stacksize from rlimit, but clip it so that it does not overlap
    // with other mappings.
    uptr stacksize = rl.rlim_cur;
    if (stacksize > end - prev_end)
      stacksize = end - prev_end;
    // When running with unlimited stack size, we still want to set some limit.
    // The unlimited stack size is caused by 'ulimit -s unlimited'.
    // Also, for some reason, GNU make spawns subprocesses with unlimited stack.
    if (stacksize > kMaxThreadStackSize)
      stacksize = kMaxThreadStackSize;
    *stack_top = end;
    *stack_bottom = end - stacksize;
    return;
  }
  pthread_attr_t attr;
  CHECK_EQ(pthread_getattr_np(pthread_self(), &attr), 0);
  uptr stacksize = 0;
  void *stackaddr = 0;
  pthread_attr_getstack(&attr, &stackaddr, (size_t*)&stacksize);
  pthread_attr_destroy(&attr);

  *stack_top = (uptr)stackaddr + stacksize;
  *stack_bottom = (uptr)stackaddr;
  CHECK(stacksize < kMaxThreadStackSize);  // Sanity check.
}

// Like getenv, but reads env directly from /proc and does not use libc.
// This function should be called first inside __asan_init.
const char *GetEnv(const char *name) {
  static char *environ;
  static uptr len;
  static bool inited;
  if (!inited) {
    inited = true;
    uptr environ_size;
    len = ReadFileToBuffer("/proc/self/environ",
                           &environ, &environ_size, 1 << 26);
  }
  if (!environ || len == 0) return 0;
  uptr namelen = internal_strlen(name);
  const char *p = environ;
  while (*p != '\0') {  // will happen at the \0\0 that terminates the buffer
    // proc file has the format NAME=value\0NAME=value\0NAME=value\0...
    const char* endp =
        (char*)internal_memchr(p, '\0', len - (p - environ));
    if (endp == 0)  // this entry isn't NUL terminated
      return 0;
    else if (!internal_memcmp(p, name, namelen) && p[namelen] == '=')  // Match.
      return p + namelen + 1;  // point after =
    p = endp + 1;
  }
  return 0;  // Not found.
}

// Does not compile for Go because dlsym() requires -ldl
#ifndef SANITIZER_GO
bool SetEnv(const char *name, const char *value) {
  void *f = dlsym(RTLD_NEXT, "setenv");
  if (f == 0)
    return false;
  typedef int(*setenv_ft)(const char *name, const char *value, int overwrite);
  setenv_ft setenv_f;
  CHECK_EQ(sizeof(setenv_f), sizeof(f));
  internal_memcpy(&setenv_f, &f, sizeof(f));
  return setenv_f(name, value, 1) == 0;
}
#endif

#ifdef __GLIBC__

extern "C" {
  extern void *__libc_stack_end;
}

static void GetArgsAndEnv(char ***argv, char ***envp) {
  uptr *stack_end = (uptr *)__libc_stack_end;
  int argc = *stack_end;
  *argv = (char**)(stack_end + 1);
  *envp = (char**)(stack_end + argc + 2);
}

#else  // __GLIBC__

static void ReadNullSepFileToArray(const char *path, char ***arr,
                                   int arr_size) {
  char *buff;
  uptr buff_size = 0;
  *arr = (char **)MmapOrDie(arr_size * sizeof(char *), "NullSepFileArray");
  ReadFileToBuffer(path, &buff, &buff_size, 1024 * 1024);
  (*arr)[0] = buff;
  int count, i;
  for (count = 1, i = 1; ; i++) {
    if (buff[i] == 0) {
      if (buff[i+1] == 0) break;
      (*arr)[count] = &buff[i+1];
      CHECK_LE(count, arr_size - 1);  // FIXME: make this more flexible.
      count++;
    }
  }
  (*arr)[count] = 0;
}

static void GetArgsAndEnv(char ***argv, char ***envp) {
  static const int kMaxArgv = 2000, kMaxEnvp = 2000;
  ReadNullSepFileToArray("/proc/self/cmdline", argv, kMaxArgv);
  ReadNullSepFileToArray("/proc/self/environ", envp, kMaxEnvp);
}

#endif  // __GLIBC__

void ReExec() {
  char **argv, **envp;
  GetArgsAndEnv(&argv, &envp);
  uptr rv = internal_execve("/proc/self/exe", argv, envp);
  int rverrno;
  CHECK_EQ(internal_iserror(rv, &rverrno), true);
  Printf("execve failed, errno %d\n", rverrno);
  Die();
}

void PrepareForSandboxing() {
  // Some kinds of sandboxes may forbid filesystem access, so we won't be able
  // to read the file mappings from /proc/self/maps. Luckily, neither the
  // process will be able to load additional libraries, so it's fine to use the
  // cached mappings.
  MemoryMappingLayout::CacheMemoryMappings();
}

// ----------------- sanitizer_procmaps.h
// Linker initialized.
ProcSelfMapsBuff MemoryMappingLayout::cached_proc_self_maps_;
StaticSpinMutex MemoryMappingLayout::cache_lock_;  // Linker initialized.

MemoryMappingLayout::MemoryMappingLayout(bool cache_enabled) {
  proc_self_maps_.len =
      ReadFileToBuffer("/proc/self/maps", &proc_self_maps_.data,
                       &proc_self_maps_.mmaped_size, 1 << 26);
  if (cache_enabled) {
    if (proc_self_maps_.mmaped_size == 0) {
      LoadFromCache();
      CHECK_GT(proc_self_maps_.len, 0);
    }
  } else {
    CHECK_GT(proc_self_maps_.mmaped_size, 0);
  }
  Reset();
  // FIXME: in the future we may want to cache the mappings on demand only.
  if (cache_enabled)
    CacheMemoryMappings();
}

MemoryMappingLayout::~MemoryMappingLayout() {
  // Only unmap the buffer if it is different from the cached one. Otherwise
  // it will be unmapped when the cache is refreshed.
  if (proc_self_maps_.data != cached_proc_self_maps_.data) {
    UnmapOrDie(proc_self_maps_.data, proc_self_maps_.mmaped_size);
  }
}

void MemoryMappingLayout::Reset() {
  current_ = proc_self_maps_.data;
}

// static
void MemoryMappingLayout::CacheMemoryMappings() {
  SpinMutexLock l(&cache_lock_);
  // Don't invalidate the cache if the mappings are unavailable.
  ProcSelfMapsBuff old_proc_self_maps;
  old_proc_self_maps = cached_proc_self_maps_;
  cached_proc_self_maps_.len =
      ReadFileToBuffer("/proc/self/maps", &cached_proc_self_maps_.data,
                       &cached_proc_self_maps_.mmaped_size, 1 << 26);
  if (cached_proc_self_maps_.mmaped_size == 0) {
    cached_proc_self_maps_ = old_proc_self_maps;
  } else {
    if (old_proc_self_maps.mmaped_size) {
      UnmapOrDie(old_proc_self_maps.data,
                 old_proc_self_maps.mmaped_size);
    }
  }
}

void MemoryMappingLayout::LoadFromCache() {
  SpinMutexLock l(&cache_lock_);
  if (cached_proc_self_maps_.data) {
    proc_self_maps_ = cached_proc_self_maps_;
  }
}

// Parse a hex value in str and update str.
static uptr ParseHex(char **str) {
  uptr x = 0;
  char *s;
  for (s = *str; ; s++) {
    char c = *s;
    uptr v = 0;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      break;
    x = x * 16 + v;
  }
  *str = s;
  return x;
}

static bool IsOneOf(char c, char c1, char c2) {
  return c == c1 || c == c2;
}

static bool IsDecimal(char c) {
  return c >= '0' && c <= '9';
}

bool MemoryMappingLayout::Next(uptr *start, uptr *end, uptr *offset,
                               char filename[], uptr filename_size,
                               uptr *protection) {
  char *last = proc_self_maps_.data + proc_self_maps_.len;
  if (current_ >= last) return false;
  uptr dummy;
  if (!start) start = &dummy;
  if (!end) end = &dummy;
  if (!offset) offset = &dummy;
  char *next_line = (char*)internal_memchr(current_, '\n', last - current_);
  if (next_line == 0)
    next_line = last;
  // Example: 08048000-08056000 r-xp 00000000 03:0c 64593   /foo/bar
  *start = ParseHex(&current_);
  CHECK_EQ(*current_++, '-');
  *end = ParseHex(&current_);
  CHECK_EQ(*current_++, ' ');
  uptr local_protection = 0;
  CHECK(IsOneOf(*current_, '-', 'r'));
  if (*current_++ == 'r')
    local_protection |= kProtectionRead;
  CHECK(IsOneOf(*current_, '-', 'w'));
  if (*current_++ == 'w')
    local_protection |= kProtectionWrite;
  CHECK(IsOneOf(*current_, '-', 'x'));
  if (*current_++ == 'x')
    local_protection |= kProtectionExecute;
  CHECK(IsOneOf(*current_, 's', 'p'));
  if (*current_++ == 's')
    local_protection |= kProtectionShared;
  if (protection) {
    *protection = local_protection;
  }
  CHECK_EQ(*current_++, ' ');
  *offset = ParseHex(&current_);
  CHECK_EQ(*current_++, ' ');
  ParseHex(&current_);
  CHECK_EQ(*current_++, ':');
  ParseHex(&current_);
  CHECK_EQ(*current_++, ' ');
  while (IsDecimal(*current_))
    current_++;
  CHECK_EQ(*current_++, ' ');
  // Skip spaces.
  while (current_ < next_line && *current_ == ' ')
    current_++;
  // Fill in the filename.
  uptr i = 0;
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

// Gets the object name and the offset by walking MemoryMappingLayout.
bool MemoryMappingLayout::GetObjectNameAndOffset(uptr addr, uptr *offset,
                                                 char filename[],
                                                 uptr filename_size,
                                                 uptr *protection) {
  return IterateForObjectNameAndOffset(addr, offset, filename, filename_size,
                                       protection);
}

bool SanitizerSetThreadName(const char *name) {
#ifdef PR_SET_NAME
  return 0 == prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);  // NOLINT
#else
  return false;
#endif
}

bool SanitizerGetThreadName(char *name, int max_len) {
#ifdef PR_GET_NAME
  char buff[17];
  if (prctl(PR_GET_NAME, (unsigned long)buff, 0, 0, 0))  // NOLINT
    return false;
  internal_strncpy(name, buff, max_len);
  name[max_len] = 0;
  return true;
#else
  return false;
#endif
}

#ifndef SANITIZER_GO
//------------------------- SlowUnwindStack -----------------------------------
#ifdef __arm__
#define UNWIND_STOP _URC_END_OF_STACK
#define UNWIND_CONTINUE _URC_NO_REASON
#else
#define UNWIND_STOP _URC_NORMAL_STOP
#define UNWIND_CONTINUE _URC_NO_REASON
#endif

uptr Unwind_GetIP(struct _Unwind_Context *ctx) {
#ifdef __arm__
  uptr val;
  _Unwind_VRS_Result res = _Unwind_VRS_Get(ctx, _UVRSC_CORE,
      15 /* r15 = PC */, _UVRSD_UINT32, &val);
  CHECK(res == _UVRSR_OK && "_Unwind_VRS_Get failed");
  // Clear the Thumb bit.
  return val & ~(uptr)1;
#else
  return _Unwind_GetIP(ctx);
#endif
}

_Unwind_Reason_Code Unwind_Trace(struct _Unwind_Context *ctx, void *param) {
  StackTrace *b = (StackTrace*)param;
  CHECK(b->size < b->max_size);
  uptr pc = Unwind_GetIP(ctx);
  b->trace[b->size++] = pc;
  if (b->size == b->max_size) return UNWIND_STOP;
  return UNWIND_CONTINUE;
}

static bool MatchPc(uptr cur_pc, uptr trace_pc) {
  return cur_pc - trace_pc <= 64 || trace_pc - cur_pc <= 64;
}

void StackTrace::SlowUnwindStack(uptr pc, uptr max_depth) {
  this->size = 0;
  this->max_size = max_depth;
  if (max_depth > 1) {
    _Unwind_Backtrace(Unwind_Trace, this);
    // We need to pop a few frames so that pc is on top.
    // trace[0] belongs to the current function so we always pop it.
    int to_pop = 1;
    /**/ if (size > 1 && MatchPc(pc, trace[1])) to_pop = 1;
    else if (size > 2 && MatchPc(pc, trace[2])) to_pop = 2;
    else if (size > 3 && MatchPc(pc, trace[3])) to_pop = 3;
    else if (size > 4 && MatchPc(pc, trace[4])) to_pop = 4;
    else if (size > 5 && MatchPc(pc, trace[5])) to_pop = 5;
    this->PopStackFrames(to_pop);
  }
  this->trace[0] = pc;
}

#endif  // #ifndef SANITIZER_GO

enum MutexState {
  MtxUnlocked = 0,
  MtxLocked = 1,
  MtxSleeping = 2
};

BlockingMutex::BlockingMutex(LinkerInitialized) {
  CHECK_EQ(owner_, 0);
}

BlockingMutex::BlockingMutex() {
  internal_memset(this, 0, sizeof(*this));
}

void BlockingMutex::Lock() {
  atomic_uint32_t *m = reinterpret_cast<atomic_uint32_t *>(&opaque_storage_);
  if (atomic_exchange(m, MtxLocked, memory_order_acquire) == MtxUnlocked)
    return;
  while (atomic_exchange(m, MtxSleeping, memory_order_acquire) != MtxUnlocked)
    internal_syscall(__NR_futex, m, FUTEX_WAIT, MtxSleeping, 0, 0, 0);
}

void BlockingMutex::Unlock() {
  atomic_uint32_t *m = reinterpret_cast<atomic_uint32_t *>(&opaque_storage_);
  u32 v = atomic_exchange(m, MtxUnlocked, memory_order_relaxed);
  CHECK_NE(v, MtxUnlocked);
  if (v == MtxSleeping)
    internal_syscall(__NR_futex, m, FUTEX_WAKE, 1, 0, 0, 0);
}

void BlockingMutex::CheckLocked() {
  atomic_uint32_t *m = reinterpret_cast<atomic_uint32_t *>(&opaque_storage_);
  CHECK_NE(MtxUnlocked, atomic_load(m, memory_order_relaxed));
}

// ----------------- sanitizer_linux.h
// The actual size of this structure is specified by d_reclen.
// Note that getdents64 uses a different structure format. We only provide the
// 32-bit syscall here.
struct linux_dirent {
  unsigned long      d_ino;
  unsigned long      d_off;
  unsigned short     d_reclen;
  char               d_name[256];
};

// Syscall wrappers.
uptr internal_ptrace(int request, int pid, void *addr, void *data) {
  return internal_syscall(__NR_ptrace, request, pid, addr, data);
}

uptr internal_waitpid(int pid, int *status, int options) {
  return internal_syscall(__NR_wait4, pid, status, options, 0 /* rusage */);
}

uptr internal_getpid() {
  return internal_syscall(__NR_getpid);
}

uptr internal_getppid() {
  return internal_syscall(__NR_getppid);
}

uptr internal_getdents(fd_t fd, struct linux_dirent *dirp, unsigned int count) {
  return internal_syscall(__NR_getdents, fd, dirp, count);
}

uptr internal_lseek(fd_t fd, OFF_T offset, int whence) {
  return internal_syscall(__NR_lseek, fd, offset, whence);
}

uptr internal_prctl(int option, uptr arg2, uptr arg3, uptr arg4, uptr arg5) {
  return internal_syscall(__NR_prctl, option, arg2, arg3, arg4, arg5);
}

uptr internal_sigaltstack(const struct sigaltstack *ss,
                         struct sigaltstack *oss) {
  return internal_syscall(__NR_sigaltstack, ss, oss);
}

// ThreadLister implementation.
ThreadLister::ThreadLister(int pid)
  : pid_(pid),
    descriptor_(-1),
    buffer_(4096),
    error_(true),
    entry_((struct linux_dirent *)buffer_.data()),
    bytes_read_(0) {
  char task_directory_path[80];
  internal_snprintf(task_directory_path, sizeof(task_directory_path),
                    "/proc/%d/task/", pid);
  uptr openrv = internal_open(task_directory_path, O_RDONLY | O_DIRECTORY);
  if (internal_iserror(openrv)) {
    error_ = true;
    Report("Can't open /proc/%d/task for reading.\n", pid);
  } else {
    error_ = false;
    descriptor_ = openrv;
  }
}

int ThreadLister::GetNextTID() {
  int tid = -1;
  do {
    if (error_)
      return -1;
    if ((char *)entry_ >= &buffer_[bytes_read_] && !GetDirectoryEntries())
      return -1;
    if (entry_->d_ino != 0 && entry_->d_name[0] >= '0' &&
        entry_->d_name[0] <= '9') {
      // Found a valid tid.
      tid = (int)internal_atoll(entry_->d_name);
    }
    entry_ = (struct linux_dirent *)(((char *)entry_) + entry_->d_reclen);
  } while (tid < 0);
  return tid;
}

void ThreadLister::Reset() {
  if (error_ || descriptor_ < 0)
    return;
  internal_lseek(descriptor_, 0, SEEK_SET);
}

ThreadLister::~ThreadLister() {
  if (descriptor_ >= 0)
    internal_close(descriptor_);
}

bool ThreadLister::error() { return error_; }

bool ThreadLister::GetDirectoryEntries() {
  CHECK_GE(descriptor_, 0);
  CHECK_NE(error_, true);
  bytes_read_ = internal_getdents(descriptor_,
                                  (struct linux_dirent *)buffer_.data(),
                                  buffer_.size());
  if (internal_iserror(bytes_read_)) {
    Report("Can't read directory entries from /proc/%d/task.\n", pid_);
    error_ = true;
    return false;
  } else if (bytes_read_ == 0) {
    return false;
  }
  entry_ = (struct linux_dirent *)buffer_.data();
  return true;
}

static uptr g_tls_size;

#ifdef __i386__
# define DL_INTERNAL_FUNCTION __attribute__((regparm(3), stdcall))
#else
# define DL_INTERNAL_FUNCTION
#endif

void InitTlsSize() {
#if !defined(SANITIZER_GO) && !SANITIZER_ANDROID
  typedef void (*get_tls_func)(size_t*, size_t*) DL_INTERNAL_FUNCTION;
  get_tls_func get_tls;
  void *get_tls_static_info_ptr = dlsym(RTLD_NEXT, "_dl_get_tls_static_info");
  CHECK_EQ(sizeof(get_tls), sizeof(get_tls_static_info_ptr));
  internal_memcpy(&get_tls, &get_tls_static_info_ptr,
                  sizeof(get_tls_static_info_ptr));
  CHECK_NE(get_tls, 0);
  size_t tls_size = 0;
  size_t tls_align = 0;
  get_tls(&tls_size, &tls_align);
  g_tls_size = tls_size;
#endif
}

uptr GetTlsSize() {
  return g_tls_size;
}

// sizeof(struct thread) from glibc.
#ifdef __x86_64__
const uptr kThreadDescriptorSize = 2304;

uptr ThreadDescriptorSize() {
  return kThreadDescriptorSize;
}
#endif

void GetThreadStackAndTls(bool main, uptr *stk_addr, uptr *stk_size,
                          uptr *tls_addr, uptr *tls_size) {
#ifndef SANITIZER_GO
#ifdef __x86_64__
  arch_prctl(ARCH_GET_FS, tls_addr);
  *tls_size = GetTlsSize();
  *tls_addr -= *tls_size;
  *tls_addr += kThreadDescriptorSize;
#else
  *tls_addr = 0;
  *tls_size = 0;
#endif

  uptr stack_top, stack_bottom;
  GetThreadStackTopAndBottom(main, &stack_top, &stack_bottom);
  *stk_addr = stack_bottom;
  *stk_size = stack_top - stack_bottom;

  if (!main) {
    // If stack and tls intersect, make them non-intersecting.
    if (*tls_addr > *stk_addr && *tls_addr < *stk_addr + *stk_size) {
      CHECK_GT(*tls_addr + *tls_size, *stk_addr);
      CHECK_LE(*tls_addr + *tls_size, *stk_addr + *stk_size);
      *stk_size -= *tls_size;
      *tls_addr = *stk_addr + *stk_size;
    }
  }
#else  // SANITIZER_GO
  *stk_addr = 0;
  *stk_size = 0;
  *tls_addr = 0;
  *tls_size = 0;
#endif  // SANITIZER_GO
}

void AdjustStackSizeLinux(void *attr_, int verbosity) {
  pthread_attr_t *attr = (pthread_attr_t *)attr_;
  uptr stackaddr = 0;
  size_t stacksize = 0;
  pthread_attr_getstack(attr, (void**)&stackaddr, &stacksize);
  // GLibC will return (0 - stacksize) as the stack address in the case when
  // stacksize is set, but stackaddr is not.
  bool stack_set = (stackaddr != 0) && (stackaddr + stacksize != 0);
  // We place a lot of tool data into TLS, account for that.
  const uptr minstacksize = GetTlsSize() + 128*1024;
  if (stacksize < minstacksize) {
    if (!stack_set) {
      if (verbosity && stacksize != 0)
        Printf("Sanitizer: increasing stacksize %zu->%zu\n", stacksize,
               minstacksize);
      pthread_attr_setstacksize(attr, minstacksize);
    } else {
      Printf("Sanitizer: pre-allocated stack size is insufficient: "
             "%zu < %zu\n", stacksize, minstacksize);
      Printf("Sanitizer: pthread_create is likely to fail.\n");
    }
  }
}

}  // namespace __sanitizer

#endif  // SANITIZER_LINUX
