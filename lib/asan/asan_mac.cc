//===-- asan_mac.cc -------------------------------------------------------===//
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
// Mac-specific details.
//===----------------------------------------------------------------------===//

#ifdef __APPLE__

#include "asan_mac.h"

#include "asan_internal.h"
#include "asan_procmaps.h"
#include "asan_stack.h"
#include "asan_thread.h"
#include "asan_thread_registry.h"

#include <crt_externs.h>  // for _NSGetEnviron
#include <mach-o/dyld.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ucontext.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <libkern/OSAtomic.h>

namespace __asan {

void *island_allocator_pos = NULL;

extern dispatch_async_f_f real_dispatch_async_f;
extern dispatch_sync_f_f real_dispatch_sync_f;
extern dispatch_after_f_f real_dispatch_after_f;
extern dispatch_barrier_async_f_f real_dispatch_barrier_async_f;
extern dispatch_group_async_f_f real_dispatch_group_async_f;
extern pthread_workqueue_additem_np_f real_pthread_workqueue_additem_np;

void GetPcSpBp(void *context, uintptr_t *pc, uintptr_t *sp, uintptr_t *bp) {
  ucontext_t *ucontext = (ucontext_t*)context;
# if __WORDSIZE == 64
  *pc = ucontext->uc_mcontext->__ss.__rip;
  *bp = ucontext->uc_mcontext->__ss.__rbp;
  *sp = ucontext->uc_mcontext->__ss.__rsp;
# else
  *pc = ucontext->uc_mcontext->__ss.__eip;
  *bp = ucontext->uc_mcontext->__ss.__ebp;
  *sp = ucontext->uc_mcontext->__ss.__esp;
# endif  // __WORDSIZE
}

// No-op. Mac does not support static linkage anyway.
void *AsanDoesNotSupportStaticLinkage() {
  return NULL;
}

bool AsanInterceptsSignal(int signum) {
  return (signum == SIGSEGV || signum == SIGBUS) && FLAG_handle_segv;
}

static void *asan_mmap(void *addr, size_t length, int prot, int flags,
                int fd, uint64_t offset) {
  return mmap(addr, length, prot, flags, fd, offset);
}

size_t AsanWrite(int fd, const void *buf, size_t count) {
  return write(fd, buf, count);
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
  int res = munmap(addr, size);
  if (res != 0) {
    Report("Failed to unmap\n");
    AsanDie();
  }
}

int AsanOpenReadonly(const char* filename) {
  return open(filename, O_RDONLY);
}

const char *AsanGetEnv(const char *name) {
  char ***env_ptr = _NSGetEnviron();
  CHECK(env_ptr);
  char **environ = *env_ptr;
  CHECK(environ);
  size_t name_len = internal_strlen(name);
  while (*environ != NULL) {
    size_t len = internal_strlen(*environ);
    if (len > name_len) {
      const char *p = *environ;
      if (!internal_memcmp(p, name, name_len) &&
          p[name_len] == '=') {  // Match.
        return *environ + name_len + 1;  // String starting after =.
      }
    }
    environ++;
  }
  return NULL;
}

size_t AsanRead(int fd, void *buf, size_t count) {
  return read(fd, buf, count);
}

int AsanClose(int fd) {
  return close(fd);
}

AsanProcMaps::AsanProcMaps() {
  Reset();
}

AsanProcMaps::~AsanProcMaps() {
}

void AsanProcMaps::Reset() {
  // Count down from the top.
  // TODO(glider): as per man 3 dyld, iterating over the headers with
  // _dyld_image_count is thread-unsafe.
  current_image_ = _dyld_image_count();
  current_load_cmd_ = -1;
}

// Similar code is used in Google Perftools,
// http://code.google.com/p/google-perftools.
template<uint32_t kMagic, uint32_t kLCSegment,
         typename MachHeader, typename SegmentCommand>
static bool NextExtMachHelper(const mach_header* hdr,
                              int current_image, int current_load_cmd,
                              uintptr_t *start, uintptr_t *end,
                              uintptr_t *offset,
                              char filename[], size_t filename_size) {
  if (hdr->magic != kMagic)
    return false;
  const char* lc = (const char *)hdr + sizeof(MachHeader);
  // TODO(csilvers): make this not-quadradic (increment and hold state)
  for (int j = 0; j < current_load_cmd; j++)  // advance to *our* load_cmd
    lc += ((const load_command *)lc)->cmdsize;
  if (((const load_command *)lc)->cmd == kLCSegment) {
    const intptr_t dlloff = _dyld_get_image_vmaddr_slide(current_image);
    const SegmentCommand* sc = (const SegmentCommand *)lc;
    if (start) *start = sc->vmaddr + dlloff;
    if (end) *end = sc->vmaddr + sc->vmsize + dlloff;
    if (offset) *offset = sc->fileoff;
    if (filename) {
      real_strncpy(filename, _dyld_get_image_name(current_image),
                   filename_size);
    }
    return true;
  }
  return false;
}

bool AsanProcMaps::Next(uintptr_t *start, uintptr_t *end,
                        uintptr_t *offset, char filename[],
                        size_t filename_size) {
  // We return a separate entry for each segment in the DLL. (TODO(csilvers):
  // can we do better?)  A DLL ("image") has load-commands, some of which
  // talk about segment boundaries.
  // cf image_for_address from http://svn.digium.com/view/asterisk/team/oej/minivoicemail/dlfcn.c?revision=53912
  for (; current_image_ >= 0; current_image_--) {
    const mach_header* hdr = _dyld_get_image_header(current_image_);
    if (!hdr) continue;
    if (current_load_cmd_ < 0)   // set up for this image
      current_load_cmd_ = hdr->ncmds;  // again, go from the top down

    // We start with the next load command (we've already looked at this one).
    for (current_load_cmd_--; current_load_cmd_ >= 0; current_load_cmd_--) {
#ifdef MH_MAGIC_64
      if (NextExtMachHelper<MH_MAGIC_64, LC_SEGMENT_64,
                            struct mach_header_64, struct segment_command_64>(
                                hdr, current_image_, current_load_cmd_,
                                start, end, offset, filename, filename_size)) {
        return true;
      }
#endif
      if (NextExtMachHelper<MH_MAGIC, LC_SEGMENT,
                            struct mach_header, struct segment_command>(
                                hdr, current_image_, current_load_cmd_,
                                start, end, offset, filename, filename_size)) {
        return true;
      }
    }
    // If we get here, no more load_cmd's in this image talk about
    // segments.  Go on to the next image.
  }
  // We didn't find anything.
  return false;
}

bool AsanProcMaps::GetObjectNameAndOffset(uintptr_t addr, uintptr_t *offset,
                                          char filename[],
                                          size_t filename_size) {
  return IterateForObjectNameAndOffset(addr, offset, filename, filename_size);
}

void AsanThread::SetThreadStackTopAndBottom() {
  size_t stacksize = pthread_get_stacksize_np(pthread_self());
  void *stackaddr = pthread_get_stackaddr_np(pthread_self());
  stack_top_ = (uintptr_t)stackaddr;
  stack_bottom_ = stack_top_ - stacksize;
  int local;
  CHECK(AddrIsInStack((uintptr_t)&local));
}

AsanLock::AsanLock(LinkerInitialized) {
  // We assume that OS_SPINLOCK_INIT is zero
}

void AsanLock::Lock() {
  CHECK(sizeof(OSSpinLock) <= sizeof(opaque_storage_));
  CHECK(OS_SPINLOCK_INIT == 0);
  CHECK(owner_ != (uintptr_t)pthread_self());
  OSSpinLockLock((OSSpinLock*)&opaque_storage_);
  CHECK(!owner_);
  owner_ = (uintptr_t)pthread_self();
}

void AsanLock::Unlock() {
  CHECK(owner_ == (uintptr_t)pthread_self());
  owner_ = 0;
  OSSpinLockUnlock((OSSpinLock*)&opaque_storage_);
}

void AsanStackTrace::GetStackTrace(size_t max_s, uintptr_t pc, uintptr_t bp) {
  size = 0;
  trace[0] = pc;
  if ((max_s) > 1) {
    max_size = max_s;
    FastUnwindStack(pc, bp);
  }
}

// The range of pages to be used by __asan_mach_override_ptr for escape
// islands.
// TODO(glider): instead of mapping a fixed range we must find a range of
// unmapped pages in vmmap and take them.
// These constants were chosen empirically and may not work if the shadow
// memory layout changes. Unfortunately they do necessarily depend on
// kHighMemBeg or kHighMemEnd.
#if __WORDSIZE == 32
#define kIslandEnd (0xffdf0000 - kPageSize)
#define kIslandBeg (kIslandEnd - 256 * kPageSize)
#else
#define kIslandEnd (0x7fffffdf0000 - kPageSize)
#define kIslandBeg (kIslandEnd - 256 * kPageSize)
#endif

extern "C"
mach_error_t __asan_allocate_island(void **ptr,
                                    size_t unused_size,
                                    void *unused_hint) {
  if (!island_allocator_pos) {
    island_allocator_pos =
        asan_mmap((void*)kIslandBeg, kIslandEnd - kIslandBeg,
                  PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANON | MAP_FIXED,
                 -1, 0);
    if (island_allocator_pos != (void*)kIslandBeg) {
      return KERN_NO_SPACE;
    }
  };
  *ptr = island_allocator_pos;
  island_allocator_pos = (char*)island_allocator_pos + kPageSize;
  return err_none;
}

extern "C"
mach_error_t __asan_deallocate_island(void *ptr) {
  // Do nothing.
  // TODO(glider): allow to free and reuse the island memory.
  return err_none;
}

// Support for the following functions from libdispatch on Mac OS:
//   dispatch_async_f()
//   dispatch_async()
//   dispatch_sync_f()
//   dispatch_sync()
//   dispatch_after_f()
//   dispatch_after()
//   dispatch_group_async_f()
//   dispatch_group_async()
// TODO(glider): libdispatch API contains other functions that we don't support
// yet.
//
// dispatch_sync() and dispatch_sync_f() are synchronous, although chances are
// they can cause jobs to run on a thread different from the current one.
// TODO(glider): if so, we need a test for this (otherwise we should remove
// them).
//
// The following functions use dispatch_barrier_async_f() (which isn't a library
// function but is exported) and are thus supported:
//   dispatch_source_set_cancel_handler_f()
//   dispatch_source_set_cancel_handler()
//   dispatch_source_set_event_handler_f()
//   dispatch_source_set_event_handler()
//
// The reference manual for Grand Central Dispatch is available at
//   http://developer.apple.com/library/mac/#documentation/Performance/Reference/GCD_libdispatch_Ref/Reference/reference.html
// The implementation details are at
//   http://libdispatch.macosforge.org/trac/browser/trunk/src/queue.c

extern "C"
void asan_dispatch_call_block_and_release(void *block) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *context = (asan_block_context_t*)block;
  if (FLAG_v >= 2) {
    Report("asan_dispatch_call_block_and_release(): "
           "context: %p, pthread_self: %p\n",
           block, pthread_self());
  }
  AsanThread *t = asanThreadRegistry().GetCurrent();
  if (!t) {
    t = AsanThread::Create(context->parent_tid, NULL, NULL, &stack);
    asanThreadRegistry().RegisterThread(t);
    t->Init();
    asanThreadRegistry().SetCurrent(t);
  }
  // Call the original dispatcher for the block.
  context->func(context->block);
  asan_free(context, &stack);
}

}  // namespace __asan

using namespace __asan;  // NOLINT

// Wrap |ctxt| and |func| into an asan_block_context_t.
// The caller retains control of the allocated context.
extern "C"
asan_block_context_t *alloc_asan_context(void *ctxt, dispatch_function_t func,
                                         AsanStackTrace *stack) {
  asan_block_context_t *asan_ctxt =
      (asan_block_context_t*) asan_malloc(sizeof(asan_block_context_t), stack);
  asan_ctxt->block = ctxt;
  asan_ctxt->func = func;
  asan_ctxt->parent_tid = asanThreadRegistry().GetCurrentTidOrMinusOne();
  return asan_ctxt;
}

// TODO(glider): can we reduce code duplication by introducing a macro?
extern "C"
int WRAP(dispatch_async_f)(dispatch_queue_t dq,
                           void *ctxt,
                           dispatch_function_t func) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt = alloc_asan_context(ctxt, func, &stack);
  if (FLAG_v >= 2) {
    Report("dispatch_async_f(): context: %p, pthread_self: %p\n",
        asan_ctxt, pthread_self());
    PRINT_CURRENT_STACK();
  }
  return real_dispatch_async_f(dq, (void*)asan_ctxt,
                               asan_dispatch_call_block_and_release);
}

extern "C"
int WRAP(dispatch_sync_f)(dispatch_queue_t dq,
                          void *ctxt,
                          dispatch_function_t func) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt = alloc_asan_context(ctxt, func, &stack);
  if (FLAG_v >= 2) {
    Report("dispatch_sync_f(): context: %p, pthread_self: %p\n",
        asan_ctxt, pthread_self());
    PRINT_CURRENT_STACK();
  }
  return real_dispatch_sync_f(dq, (void*)asan_ctxt,
                              asan_dispatch_call_block_and_release);
}

extern "C"
int WRAP(dispatch_after_f)(dispatch_time_t when,
                           dispatch_queue_t dq,
                           void *ctxt,
                           dispatch_function_t func) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt = alloc_asan_context(ctxt, func, &stack);
  if (FLAG_v >= 2) {
    Report("dispatch_after_f: %p\n", asan_ctxt);
    PRINT_CURRENT_STACK();
  }
  return real_dispatch_after_f(when, dq, (void*)asan_ctxt,
                               asan_dispatch_call_block_and_release);
}

extern "C"
void WRAP(dispatch_barrier_async_f)(dispatch_queue_t dq,
                                    void *ctxt, dispatch_function_t func) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt = alloc_asan_context(ctxt, func, &stack);
  if (FLAG_v >= 2) {
    Report("dispatch_barrier_async_f(): context: %p, pthread_self: %p\n",
           asan_ctxt, pthread_self());
    PRINT_CURRENT_STACK();
  }
  real_dispatch_barrier_async_f(dq, (void*)asan_ctxt,
                                asan_dispatch_call_block_and_release);
}

extern "C"
void WRAP(dispatch_group_async_f)(dispatch_group_t group,
                                  dispatch_queue_t dq,
                                  void *ctxt, dispatch_function_t func) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt = alloc_asan_context(ctxt, func, &stack);
  if (FLAG_v >= 2) {
    Report("dispatch_group_async_f(): context: %p, pthread_self: %p\n",
           asan_ctxt, pthread_self());
    PRINT_CURRENT_STACK();
  }
  real_dispatch_group_async_f(group, dq, (void*)asan_ctxt,
                              asan_dispatch_call_block_and_release);
}

// The following stuff has been extremely helpful while looking for the
// unhandled functions that spawned jobs on Chromium shutdown. If the verbosity
// level is 2 or greater, we wrap pthread_workqueue_additem_np() in order to
// find the points of worker thread creation (each of such threads may be used
// to run several tasks, that's why this is not enough to support the whole
// libdispatch API.
extern "C"
void *wrap_workitem_func(void *arg) {
  if (FLAG_v >= 2) {
    Report("wrap_workitem_func: %p, pthread_self: %p\n", arg, pthread_self());
  }
  asan_block_context_t *ctxt = (asan_block_context_t*)arg;
  worker_t fn = (worker_t)(ctxt->func);
  void *result =  fn(ctxt->block);
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_free(arg, &stack);
  return result;
}

extern "C"
int WRAP(pthread_workqueue_additem_np)(pthread_workqueue_t workq,
    void *(*workitem_func)(void *), void * workitem_arg,
    pthread_workitem_handle_t * itemhandlep, unsigned int *gencountp) {
  GET_STACK_TRACE_HERE(kStackTraceMax);
  asan_block_context_t *asan_ctxt =
      (asan_block_context_t*) asan_malloc(sizeof(asan_block_context_t), &stack);
  asan_ctxt->block = workitem_arg;
  asan_ctxt->func = (dispatch_function_t)workitem_func;
  asan_ctxt->parent_tid = asanThreadRegistry().GetCurrentTidOrMinusOne();
  if (FLAG_v >= 2) {
    Report("pthread_workqueue_additem_np: %p\n", asan_ctxt);
    PRINT_CURRENT_STACK();
  }
  return real_pthread_workqueue_additem_np(workq, wrap_workitem_func, asan_ctxt,
                                           itemhandlep, gencountp);
}

#endif  // __APPLE__
