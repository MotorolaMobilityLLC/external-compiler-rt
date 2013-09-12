//===-- asan_fake_stack.cc ------------------------------------------------===//
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
// FakeStack is used to detect use-after-return bugs.
//===----------------------------------------------------------------------===//
#include "asan_allocator.h"
#include "asan_poisoning.h"
#include "asan_thread.h"

namespace __asan {

void FakeStack::PoisonAll(u8 magic) {
  PoisonShadow(reinterpret_cast<uptr>(this), RequiredSize(stack_size_log()),
               magic);
}

FakeFrame *FakeStack::Allocate(uptr stack_size_log, uptr class_id,
                               uptr real_stack) {
  CHECK_LT(class_id, kNumberOfSizeClasses);
  uptr &hint_position = hint_position_[class_id];
  const int num_iter = NumberOfFrames(stack_size_log, class_id);
  u8 *flags = GetFlags(stack_size_log, class_id);
  for (int i = 0; i < num_iter; i++) {
    uptr pos = ModuloNumberOfFrames(stack_size_log, class_id, hint_position++);
    if (flags[pos]) continue;
    // FIXME: this does not have to be thread-safe, just async-signal-safe.
    if (0 == atomic_exchange((atomic_uint8_t *)&flags[pos], 1,
                             memory_order_relaxed)) {
      FakeFrame *res = reinterpret_cast<FakeFrame *>(
          GetFrame(stack_size_log, class_id, pos));
      res->real_stack = real_stack;
      res->class_id = class_id;
      return res;
    }
  }
  CHECK(0 && "Failed to allocate a fake stack frame");
  return 0;
}

void FakeStack::Deallocate(FakeFrame *ff, uptr stack_size_log, uptr class_id,
                           uptr real_stack) {
  u8 *base = GetFrame(stack_size_log, class_id, 0);
  u8 *cur = reinterpret_cast<u8 *>(ff);
  CHECK_LE(base, cur);
  CHECK_LT(cur, base + (1UL << stack_size_log));
  uptr pos = (cur - base) >> (kMinStackFrameSizeLog + class_id);
  u8 *flags = GetFlags(stack_size_log, class_id);
  CHECK_EQ(flags[pos], 1);
  flags[pos] = 0;
}

uptr FakeStack::AddrIsInFakeStack(uptr ptr) {
  uptr stack_size_log = this->stack_size_log();
  uptr beg = reinterpret_cast<uptr>(GetFrame(stack_size_log, 0, 0));
  uptr end = reinterpret_cast<uptr>(this) + RequiredSize(stack_size_log);
  if (ptr < beg || ptr >= end) return 0;
  uptr class_id = (ptr - beg) >> stack_size_log;
  uptr base = beg + (class_id << stack_size_log);
  CHECK_LE(base, ptr);
  CHECK_LT(ptr, base + (1UL << stack_size_log));
  uptr pos = (ptr - base) >> (kMinStackFrameSizeLog + class_id);
  return base + pos * BytesInSizeClass(class_id);
}

ALWAYS_INLINE uptr OnMalloc(uptr class_id, uptr size, uptr real_stack) {
  AsanThread *t = GetCurrentThread();
  if (!t) return real_stack;
  FakeStack *fs = t->fake_stack();
  FakeFrame *ff = fs->Allocate(fs->stack_size_log(), class_id, real_stack);
  uptr ptr = reinterpret_cast<uptr>(ff);
  PoisonShadow(ptr, size, 0);
  return ptr;
}

ALWAYS_INLINE void OnFree(uptr ptr, uptr class_id, uptr size, uptr real_stack) {
  if (ptr == real_stack)
    return;
  AsanThread *t = GetCurrentThread();
  if (!t) return;
  FakeStack *fs = t->fake_stack();
  FakeFrame *ff = reinterpret_cast<FakeFrame *>(ptr);
  fs->Deallocate(ff, fs->stack_size_log(), class_id, real_stack);
  PoisonShadow(ptr, size, kAsanStackAfterReturnMagic);
}

}  // namespace __asan

// ---------------------- Interface ---------------- {{{1
#define DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(class_id)                       \
  extern "C" SANITIZER_INTERFACE_ATTRIBUTE uptr                                \
  __asan_stack_malloc_##class_id(uptr size, uptr real_stack) {                 \
    return __asan::OnMalloc(class_id, size, real_stack);                       \
  }                                                                            \
  extern "C" SANITIZER_INTERFACE_ATTRIBUTE void __asan_stack_free_##class_id(  \
      uptr ptr, uptr size, uptr real_stack) {                                  \
    __asan::OnFree(ptr, class_id, size, real_stack);                           \
  }

DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(0)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(1)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(2)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(3)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(4)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(5)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(6)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(7)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(8)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(9)
DEFINE_STACK_MALLOC_FREE_WITH_CLASS_ID(10)
