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

static const u64 kMagic1 = kAsanStackAfterReturnMagic;
static const u64 kMagic2 = (kMagic1 << 8) | kMagic1;
static const u64 kMagic4 = (kMagic2 << 16) | kMagic2;
static const u64 kMagic8 = (kMagic4 << 32) | kMagic4;

// For small size classes inline PoisonShadow for better performance.
ALWAYS_INLINE void SetShadow(uptr ptr, uptr size, uptr class_id, u64 magic) {
  CHECK_EQ(SHADOW_SCALE, 3);  // This code expects SHADOW_SCALE=3.
  u64 *shadow = reinterpret_cast<u64*>(MemToShadow(ptr));
  if (class_id <= 6) {
    for (uptr i = 0; i < (1 << class_id); i++)
      shadow[i] = magic;
  } else {
    // The size class is too big, it's cheaper to poison only size bytes.
    PoisonShadow(ptr, size, magic);
  }
}

void FakeStack::PoisonAll(u8 magic) {
  PoisonShadow(reinterpret_cast<uptr>(this), RequiredSize(stack_size_log()),
               magic);
}

ALWAYS_INLINE USED
FakeFrame *FakeStack::Allocate(uptr stack_size_log, uptr class_id,
                               uptr real_stack) {
  CHECK_LT(class_id, kNumberOfSizeClasses);
  if (needs_gc_)
    GC(real_stack);
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
      allocated_from_size_class_mask_ |= 1UL << class_id;
      return res;
    }
  }
  CHECK(0 && "Failed to allocate a fake stack frame");
  return 0;
}

ALWAYS_INLINE USED
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

void FakeStack::HandleNoReturn() {
  needs_gc_ = true;
}

// When throw, longjmp or some such happens we don't call OnFree() and
// as the result may leak one or more fake frames, but the good news is that
// we are notified about all such events by HandleNoReturn().
// If we recently had such no-return event we need to collect garbage frames.
// We do it based on their 'real_stack' values -- everything that is lower
// than the current real_stack is garbage.
NOINLINE void FakeStack::GC(uptr real_stack) {
  uptr collected = 0;
  for (uptr class_id = 0; class_id < kNumberOfSizeClasses; class_id++) {
    if (!(allocated_from_size_class_mask_ & (1UL << class_id))) continue;
    u8 *flags = GetFlags(stack_size_log(), class_id);
    for (uptr i = 0, n = NumberOfFrames(stack_size_log(), class_id); i < n;
         i++) {
      if (flags[i] == 0) continue;  // not allocated.
      FakeFrame *ff = reinterpret_cast<FakeFrame *>(
          GetFrame(stack_size_log(), class_id, i));
      if (ff->real_stack < real_stack) {
        flags[i] = 0;
        collected++;
      }
    }
  }
  needs_gc_ = false;
}

#if SANITIZER_LINUX
static THREADLOCAL FakeStack *fake_stack_tls;

FakeStack *GetTLSFakeStack() {
  return fake_stack_tls;
}
void SetTLSFakeStack(FakeStack *fs) {
  fake_stack_tls = fs;
}
#else
FakeStack *GetTLSFakeStack() { return 0; }
void SetTLSFakeStack(FakeStack *fs) { }
#endif  // SANITIZER_LINUX

static FakeStack *GetFakeStack() {
  AsanThread *t = GetCurrentThread();
  if (!t) return 0;
  return t->fake_stack();
}

static FakeStack *GetFakeStackFast() {
  if (FakeStack *fs = GetTLSFakeStack())
    return fs;
  return GetFakeStack();
}

ALWAYS_INLINE uptr OnMalloc(uptr class_id, uptr size, uptr real_stack) {
  FakeStack *fs = GetFakeStackFast();
  if (!fs) return real_stack;
  FakeFrame *ff = fs->Allocate(fs->stack_size_log(), class_id, real_stack);
  uptr ptr = reinterpret_cast<uptr>(ff);
  SetShadow(ptr, size, class_id, 0);
  return ptr;
}

ALWAYS_INLINE void OnFree(uptr ptr, uptr class_id, uptr size, uptr real_stack) {
  if (ptr == real_stack)
    return;
  FakeStack *fs = GetFakeStackFast();  // Must not be 0.
  FakeFrame *ff = reinterpret_cast<FakeFrame *>(ptr);
  fs->Deallocate(ff, fs->stack_size_log(), class_id, real_stack);
  SetShadow(ptr, size, class_id, kMagic8);
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
