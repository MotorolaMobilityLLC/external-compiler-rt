//===-- asan_stack.h --------------------------------------------*- C++ -*-===//
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
// ASan-private header for asan_stack.cc.
//===----------------------------------------------------------------------===//
#ifndef ASAN_STACK_H
#define ASAN_STACK_H

#include "asan_internal.h"

namespace __asan {

static const uptr kStackTraceMax = 64;

struct StackTrace {
  typedef bool (*SymbolizeCallback)(const void *pc, char *out_buffer,
                                     int out_size);
  uptr size;
  uptr max_size;
  uptr trace[kStackTraceMax];
  static void PrintStack(uptr *addr, uptr size,
                         bool symbolize, const char *strip_file_prefix,
                         SymbolizeCallback symbolize_callback);
  void CopyTo(uptr *dst, uptr dst_size) {
    for (uptr i = 0; i < size && i < dst_size; i++)
      dst[i] = trace[i];
    for (uptr i = size; i < dst_size; i++)
      dst[i] = 0;
  }

  void CopyFrom(uptr *src, uptr src_size) {
    size = src_size;
    if (size > kStackTraceMax) size = kStackTraceMax;
    for (uptr i = 0; i < size; i++) {
      trace[i] = src[i];
    }
  }

  void FastUnwindStack(uptr pc, uptr bp, uptr stack_top, uptr stack_bottom);

  static uptr GetCurrentPc();

  static uptr CompressStack(StackTrace *stack,
                            u32 *compressed, uptr size);
  static void UncompressStack(StackTrace *stack,
                              u32 *compressed, uptr size);
};

void GetStackTrace(StackTrace *stack, uptr max_s, uptr pc, uptr bp);
void PrintStack(StackTrace *stack);



}  // namespace __asan

// Use this macro if you want to print stack trace with the caller
// of the current function in the top frame.
#define GET_CALLER_PC_BP_SP \
  uptr bp = GET_CURRENT_FRAME();              \
  uptr pc = GET_CALLER_PC();                  \
  uptr local_stack;                           \
  uptr sp = (uptr)&local_stack

// Use this macro if you want to print stack trace with the current
// function in the top frame.
#define GET_CURRENT_PC_BP_SP \
  uptr bp = GET_CURRENT_FRAME();              \
  uptr pc = StackTrace::GetCurrentPc();   \
  uptr local_stack;                           \
  uptr sp = (uptr)&local_stack

// Get the stack trace with the given pc and bp.
// The pc will be in the position 0 of the resulting stack trace.
// The bp may refer to the current frame or to the caller's frame.
// fast_unwind is currently unused.
#define GET_STACK_TRACE_WITH_PC_AND_BP(max_s, pc, bp)               \
  StackTrace stack;                                             \
  GetStackTrace(&stack, max_s, pc, bp)

// NOTE: A Rule of thumb is to retrieve stack trace in the interceptors
// as early as possible (in functions exposed to the user), as we generally
// don't want stack trace to contain functions from ASan internals.

#define GET_STACK_TRACE_HERE(max_size)                        \
  GET_STACK_TRACE_WITH_PC_AND_BP(max_size,                    \
      StackTrace::GetCurrentPc(), GET_CURRENT_FRAME())

#define GET_STACK_TRACE_HERE_FOR_MALLOC                             \
  GET_STACK_TRACE_HERE(flags()->malloc_context_size)

#define GET_STACK_TRACE_HERE_FOR_FREE(ptr)                          \
  GET_STACK_TRACE_HERE(flags()->malloc_context_size)

#define PRINT_CURRENT_STACK()                    \
  {                                              \
    GET_STACK_TRACE_HERE(kStackTraceMax);        \
    PrintStack(&stack);                          \
  }

#endif  // ASAN_STACK_H
