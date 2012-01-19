//===-- asan_stack.cc -------------------------------------------*- C++ -*-===//
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
// Code for ASan stack trace.
//===----------------------------------------------------------------------===//
#include "asan_interceptors.h"
#include "asan_lock.h"
#include "asan_procmaps.h"
#include "asan_stack.h"
#include "asan_thread.h"
#include "asan_thread_registry.h"

#ifdef ASAN_USE_EXTERNAL_SYMBOLIZER
extern bool
ASAN_USE_EXTERNAL_SYMBOLIZER(const void *pc, char *out, int out_size);
#endif

namespace __asan {

// ----------------------- AsanStackTrace ----------------------------- {{{1
#if defined(ASAN_USE_EXTERNAL_SYMBOLIZER)
void AsanStackTrace::PrintStack(uintptr_t *addr, size_t size) {
  for (size_t i = 0; i < size && addr[i]; i++) {
    uintptr_t pc = addr[i];
    char buff[4096];
    ASAN_USE_EXTERNAL_SYMBOLIZER((void*)pc, buff, sizeof(buff));
    Printf("  #%ld 0x%lx %s\n", i, pc, buff);
  }
}

#else  // ASAN_USE_EXTERNAL_SYMBOLIZER
void AsanStackTrace::PrintStack(uintptr_t *addr, size_t size) {
  AsanProcMaps proc_maps;
  for (size_t i = 0; i < size && addr[i]; i++) {
    proc_maps.Reset();
    uintptr_t pc = addr[i];
    uintptr_t offset;
    char filename[4096];
    if (proc_maps.GetObjectNameAndOffset(pc, &offset,
                                         filename, sizeof(filename))) {
      Printf("    #%ld 0x%lx (%s+0x%lx)\n", i, pc, filename, offset);
    } else {
      Printf("    #%ld 0x%lx\n", i, pc);
    }
  }
}
#endif  // ASAN_USE_EXTERNAL_SYMBOLIZER

uintptr_t AsanStackTrace::GetCurrentPc() {
  return GET_CALLER_PC();
}

void AsanStackTrace::FastUnwindStack(uintptr_t pc, uintptr_t bp) {
  CHECK(size == 0 && trace[0] == pc);
  size = 1;
  if (!asan_inited) return;
  AsanThread *t = asanThreadRegistry().GetCurrent();
  if (!t) return;
  uintptr_t *frame = (uintptr_t*)bp;
  uintptr_t *prev_frame = frame;
  uintptr_t *top = (uintptr_t*)t->stack_top();
  uintptr_t *bottom = (uintptr_t*)t->stack_bottom();
  while (frame >= prev_frame &&
         frame < top &&
         frame > bottom &&
         size < max_size) {
    uintptr_t pc1 = frame[1];
    if (pc1 != pc) {
      trace[size++] = pc1;
    }
    prev_frame = frame;
    frame = (uintptr_t*)frame[0];
  }
}

// On 32-bits we don't compress stack traces.
// On 64-bits we compress stack traces: if a given pc differes slightly from
// the previous one, we record a 31-bit offset instead of the full pc.
size_t AsanStackTrace::CompressStack(AsanStackTrace *stack,
                                   uint32_t *compressed, size_t size) {
#if __WORDSIZE == 32
  // Don't compress, just copy.
  size_t res = 0;
  for (size_t i = 0; i < stack->size && i < size; i++) {
    compressed[i] = stack->trace[i];
    res++;
  }
  if (stack->size < size)
    compressed[stack->size] = 0;
#else  // 64 bits, compress.
  uintptr_t prev_pc = 0;
  const uintptr_t kMaxOffset = (1ULL << 30) - 1;
  uintptr_t c_index = 0;
  size_t res = 0;
  for (size_t i = 0, n = stack->size; i < n; i++) {
    uintptr_t pc = stack->trace[i];
    if (!pc) break;
    if ((int64_t)pc < 0) break;
    // Printf("C pc[%ld] %lx\n", i, pc);
    if (prev_pc - pc < kMaxOffset || pc - prev_pc < kMaxOffset) {
      uintptr_t offset = (int64_t)(pc - prev_pc);
      offset |= (1U << 31);
      if (c_index >= size) break;
      // Printf("C co[%ld] offset %lx\n", i, offset);
      compressed[c_index++] = offset;
    } else {
      uintptr_t hi = pc >> 32;
      uintptr_t lo = (pc << 32) >> 32;
      CHECK((hi & (1 << 31)) == 0);
      if (c_index + 1 >= size) break;
      // Printf("C co[%ld] hi/lo: %lx %lx\n", c_index, hi, lo);
      compressed[c_index++] = hi;
      compressed[c_index++] = lo;
    }
    res++;
    prev_pc = pc;
  }
  if (c_index < size)
    compressed[c_index] = 0;
  if (c_index + 1 < size)
    compressed[c_index + 1] = 0;
#endif  // __WORDSIZE

  // debug-only code
#if 0
  AsanStackTrace check_stack;
  UncompressStack(&check_stack, compressed, size);
  if (res < check_stack.size) {
    Printf("res %ld check_stack.size %ld; c_size %ld\n", res,
           check_stack.size, size);
  }
  // |res| may be greater than check_stack.size, because
  // UncompressStack(CompressStack(stack)) eliminates the 0x0 frames.
  CHECK(res >= check_stack.size);
  CHECK(0 == real_memcmp(check_stack.trace, stack->trace,
                         check_stack.size * sizeof(uintptr_t)));
#endif

  return res;
}

void AsanStackTrace::UncompressStack(AsanStackTrace *stack,
                                     uint32_t *compressed, size_t size) {
#if __WORDSIZE == 32
  // Don't uncompress, just copy.
  stack->size = 0;
  for (size_t i = 0; i < size && i < kStackTraceMax; i++) {
    if (!compressed[i]) break;
    stack->size++;
    stack->trace[i] = compressed[i];
  }
#else  // 64 bits, uncompress
  uintptr_t prev_pc = 0;
  stack->size = 0;
  for (size_t i = 0; i < size && stack->size < kStackTraceMax; i++) {
    uint32_t x = compressed[i];
    uintptr_t pc = 0;
    if (x & (1U << 31)) {
      // Printf("U co[%ld] offset: %x\n", i, x);
      // this is an offset
      int32_t offset = x;
      offset = (offset << 1) >> 1;  // remove the 31-byte and sign-extend.
      pc = prev_pc + offset;
      CHECK(pc);
    } else {
      // CHECK(i + 1 < size);
      if (i + 1 >= size) break;
      uintptr_t hi = x;
      uintptr_t lo = compressed[i+1];
      // Printf("U co[%ld] hi/lo: %lx %lx\n", i, hi, lo);
      i++;
      pc = (hi << 32) | lo;
      if (!pc) break;
    }
    // Printf("U pc[%ld] %lx\n", stack->size, pc);
    stack->trace[stack->size++] = pc;
    prev_pc = pc;
  }
#endif  // __WORDSIZE
}

}  // namespace __asan
