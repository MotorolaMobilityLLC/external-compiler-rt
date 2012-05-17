//===-- tsan_rtl.cc ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Main file (entry points) for the TSan run-time.
//===----------------------------------------------------------------------===//

#include "tsan_defs.h"
#include "tsan_platform.h"
#include "tsan_rtl.h"
#include "tsan_interface.h"
#include "tsan_atomic.h"
#include "tsan_mman.h"
#include "tsan_placement_new.h"
#include "tsan_suppressions.h"

volatile int __tsan_stop = 0;

extern "C" void __tsan_resume() {
  __tsan_stop = 0;
}

namespace __tsan {

THREADLOCAL char cur_thread_placeholder[sizeof(ThreadState)] ALIGN(64);
static char ctx_placeholder[sizeof(Context)] ALIGN(64);

static Context *ctx;
Context *CTX() {
  return ctx;
}

Context::Context()
  : initialized()
  , report_mtx(MutexTypeReport, StatMtxReport)
  , nreported()
  , nmissed_expected()
  , thread_mtx(MutexTypeThreads, StatMtxThreads)
  , racy_stacks(MBlockRacyStacks)
  , racy_addresses(MBlockRacyAddresses) {
}

// The objects are allocated in TLS, so one may rely on zero-initialization.
ThreadState::ThreadState(Context *ctx, int tid, u64 epoch,
                         uptr stk_addr, uptr stk_size,
                         uptr tls_addr, uptr tls_size)
  : fast_state(tid, epoch)
  // Do not touch these, rely on zero initialization,
  // they may be accessed before the ctor.
  // , fast_ignore_reads()
  // , fast_ignore_writes()
  // , in_rtl()
  , shadow_stack_pos(&shadow_stack[0])
  , tid(tid)
  , func_call_count()
  , stk_addr(stk_addr)
  , stk_size(stk_size)
  , tls_addr(tls_addr)
  , tls_size(tls_size) {
}

ThreadContext::ThreadContext(int tid)
  : tid(tid)
  , unique_id()
  , user_id()
  , thr()
  , status(ThreadStatusInvalid)
  , detached()
  , reuse_count()
  , epoch0()
  , epoch1()
  , dead_next() {
}

void Initialize(ThreadState *thr) {
  // Thread safe because done before all threads exist.
  static bool is_initialized = false;
  if (is_initialized)
    return;
  is_initialized = true;
  ScopedInRtl in_rtl;
  InitializeInterceptors();
  const char *env = InitializePlatform();
  InitializeMutex();
  InitializeDynamicAnnotations();
  ctx = new(ctx_placeholder) Context;
  InitializeShadowMemory();
  ctx->dead_list_size = 0;
  ctx->dead_list_head = 0;
  ctx->dead_list_tail = 0;
  InitializeFlags(&ctx->flags, env);
  InitializeSuppressions();

  if (ctx->flags.verbosity)
    Printf("***** Running under ThreadSanitizer v2 (pid=%d) *****\n", GetPid());

  // Initialize thread 0.
  ctx->thread_seq = 0;
  int tid = ThreadCreate(thr, 0, 0, true);
  CHECK_EQ(tid, 0);
  ThreadStart(thr, tid);
  CHECK_EQ(thr->in_rtl, 1);
  ctx->initialized = true;

  if (__tsan_stop) {
    Printf("ThreadSanitizer is suspended at startup.\n");
    while (__tsan_stop);
  }
}

int Finalize(ThreadState *thr) {
  ScopedInRtl in_rtl;
  Context *ctx = __tsan::ctx;
  bool failed = false;

  // Be very careful beyond that point.
  // All bets are off. Everything is destroyed.
  ThreadFinish(thr);
  ThreadFinalize(thr);
  FinalizeFlags(&ctx->flags);

  if (ctx->nreported) {
    failed = true;
    Printf("ThreadSanitizer: reported %d warnings\n", ctx->nreported);
  }

  if (ctx->nmissed_expected) {
    failed = true;
    Printf("ThreadSanitizer: missed %d expected races\n",
        ctx->nmissed_expected);
  }

  StatOutput(ctx->stat);
  FinalizeSuppressions();
  FinalizePlatform();

  const int exitcode = failed ? flags()->exitcode : 0;
  const int log_fileno = flags()->log_fileno;
  __tsan::ctx->~Context();
  __tsan::ctx = 0;

  InternalAllocStatAggregate(ctx, thr);

  for (int i = 0; i < (int)MBlockTypeCount; i++) {
    if (ctx->int_alloc_cnt[i] == 0 && ctx->int_alloc_siz[i] == 0)
      continue;
    InternalScopedBuf<char> tmp(1024);
    Snprintf(tmp, tmp.Size(), "ThreadSanitizer: Internal memory leak: "
        "type=%d count=%lld size=%lld\n",
        (int)i, ctx->int_alloc_cnt[i], ctx->int_alloc_siz[i]);
    internal_write(log_fileno, tmp, internal_strlen(tmp));
  }

  return exitcode;
}

static void TraceSwitch(ThreadState *thr) {
  ScopedInRtl in_rtl;
  Lock l(&thr->trace.mtx);
  unsigned trace = (thr->fast_state.epoch() / kTracePartSize) % kTraceParts;
  TraceHeader *hdr = &thr->trace.headers[trace];
  hdr->epoch0 = thr->fast_state.epoch();
  hdr->stack0.ObtainCurrent(thr, 0);
}

extern "C" void __tsan_trace_switch() {
  TraceSwitch(cur_thread());
}

extern "C" void __tsan_report_race() {
  ReportRace(cur_thread());
}

ALWAYS_INLINE
static Shadow LoadShadow(u64 *p) {
  u64 raw = atomic_load((atomic_uint64_t*)p, memory_order_relaxed);
  return Shadow(raw);
}

ALWAYS_INLINE
static void StoreShadow(u64 *sp, u64 s) {
  atomic_store((atomic_uint64_t*)sp, s, memory_order_relaxed);
}

ALWAYS_INLINE
static void StoreIfNotYetStored(u64 *sp, u64 *s) {
  StoreShadow(sp, *s);
  *s = 0;
}

static inline void HandleRace(ThreadState *thr, u64 *shadow_mem,
                              Shadow cur, Shadow old) {
  thr->racy_state[0] = cur.raw();
  thr->racy_state[1] = old.raw();
  thr->racy_shadow_addr = shadow_mem;
  HACKY_CALL(__tsan_report_race);
}

static inline bool BothReads(Shadow s, int kAccessIsWrite) {
  return !kAccessIsWrite && !s.is_write();
}

static inline bool OldIsRWStronger(Shadow old, int kAccessIsWrite) {
  return old.is_write() || !kAccessIsWrite;
}

static inline bool OldIsRWWeaker(Shadow old, int kAccessIsWrite) {
  return !old.is_write() || kAccessIsWrite;
}

static inline bool OldIsInSameSynchEpoch(Shadow old, ThreadState *thr) {
  return old.epoch() >= thr->fast_synch_epoch;
}

static inline bool HappensBefore(Shadow old, ThreadState *thr) {
  return thr->clock.get(old.tid()) >= old.epoch();
}

ALWAYS_INLINE
void MemoryAccessImpl(ThreadState *thr, uptr addr,
    int kAccessSizeLog, bool kAccessIsWrite, FastState fast_state,
    u64 *shadow_mem, Shadow cur) {
  StatInc(thr, StatMop);
  StatInc(thr, kAccessIsWrite ? StatMopWrite : StatMopRead);
  StatInc(thr, (StatType)(StatMop1 + kAccessSizeLog));

  // This potentially can live in an MMX/SSE scratch register.
  // The required intrinsics are:
  // __m128i _mm_move_epi64(__m128i*);
  // _mm_storel_epi64(u64*, __m128i);
  u64 store_word = cur.raw();

  // scan all the shadow values and dispatch to 4 categories:
  // same, replace, candidate and race (see comments below).
  // we consider only 3 cases regarding access sizes:
  // equal, intersect and not intersect. initially I considered
  // larger and smaller as well, it allowed to replace some
  // 'candidates' with 'same' or 'replace', but I think
  // it's just not worth it (performance- and complexity-wise).

  Shadow old(0);
  if (kShadowCnt == 1) {
    int idx = 0;
#include "tsan_update_shadow_word_inl.h"
  } else if (kShadowCnt == 2) {
    int idx = 0;
#include "tsan_update_shadow_word_inl.h"
    idx = 1;
#include "tsan_update_shadow_word_inl.h"
  } else if (kShadowCnt == 4) {
    int idx = 0;
#include "tsan_update_shadow_word_inl.h"
    idx = 1;
#include "tsan_update_shadow_word_inl.h"
    idx = 2;
#include "tsan_update_shadow_word_inl.h"
    idx = 3;
#include "tsan_update_shadow_word_inl.h"
  } else if (kShadowCnt == 8) {
    int idx = 0;
#include "tsan_update_shadow_word_inl.h"
    idx = 1;
#include "tsan_update_shadow_word_inl.h"
    idx = 2;
#include "tsan_update_shadow_word_inl.h"
    idx = 3;
#include "tsan_update_shadow_word_inl.h"
    idx = 4;
#include "tsan_update_shadow_word_inl.h"
    idx = 5;
#include "tsan_update_shadow_word_inl.h"
    idx = 6;
#include "tsan_update_shadow_word_inl.h"
    idx = 7;
#include "tsan_update_shadow_word_inl.h"
  } else {
    CHECK(false);
  }

  // we did not find any races and had already stored
  // the current access info, so we are done
  if (LIKELY(store_word == 0))
    return;
  // choose a random candidate slot and replace it
  StoreShadow(shadow_mem + (cur.epoch() % kShadowCnt), store_word);
  StatInc(thr, StatShadowReplace);
  return;
 RACE:
  HandleRace(thr, shadow_mem, cur, old);
  return;
}

ALWAYS_INLINE
void MemoryAccess(ThreadState *thr, uptr pc, uptr addr,
    int kAccessSizeLog, bool kAccessIsWrite) {
  u64 *shadow_mem = (u64*)MemToShadow(addr);
  DPrintf2("#%d: tsan::OnMemoryAccess: @%p %p size=%d"
      " is_write=%d shadow_mem=%p {%llx, %llx, %llx, %llx}\n",
      (int)thr->fast_state.tid(), (void*)pc, (void*)addr,
      (int)(1 << kAccessSizeLog), kAccessIsWrite, shadow_mem,
      shadow_mem[0], shadow_mem[1], shadow_mem[2], shadow_mem[3]);
#if TSAN_DEBUG
  if (!IsAppMem(addr)) {
    Printf("Access to non app mem %lx\n", addr);
    DCHECK(IsAppMem(addr));
  }
  if (!IsShadowMem((uptr)shadow_mem)) {
    Printf("Bad shadow addr %p (%lx)\n", shadow_mem, addr);
    DCHECK(IsShadowMem((uptr)shadow_mem));
  }
#endif

  FastState fast_state = thr->fast_state;
  if (fast_state.GetIgnoreBit())
    return;
  fast_state.IncrementEpoch();
  thr->fast_state = fast_state;
  Shadow cur(fast_state);
  cur.SetAddr0AndSizeLog(addr & 7, kAccessSizeLog);
  cur.SetWrite(kAccessIsWrite);

  // We must not store to the trace if we do not store to the shadow.
  // That is, this call must be moved somewhere below.
  TraceAddEvent(thr, fast_state.epoch(), EventTypeMop, pc);

  MemoryAccessImpl(thr, addr, kAccessSizeLog, kAccessIsWrite, fast_state,
      shadow_mem, cur);
}

static void MemoryRangeSet(ThreadState *thr, uptr pc, uptr addr, uptr size,
                           u64 val) {
  if (size == 0)
    return;
  // FIXME: fix me.
  uptr offset = addr % kShadowCell;
  if (offset) {
    offset = kShadowCell - offset;
    if (size <= offset)
      return;
    addr += offset;
    size -= offset;
  }
  CHECK_EQ(addr % 8, 0);
  CHECK(IsAppMem(addr));
  CHECK(IsAppMem(addr + size - 1));
  (void)thr;
  (void)pc;
  // Some programs mmap like hundreds of GBs but actually used a small part.
  // So, it's better to report a false positive on the memory
  // then to hang here senselessly.
  const uptr kMaxResetSize = 1024*1024*1024;
  if (size > kMaxResetSize)
    size = kMaxResetSize;
  size = (size + 7) & ~7;
  u64 *p = (u64*)MemToShadow(addr);
  CHECK(IsShadowMem((uptr)p));
  CHECK(IsShadowMem((uptr)(p + size * kShadowCnt / kShadowCell - 1)));
  // FIXME: may overwrite a part outside the region
  for (uptr i = 0; i < size * kShadowCnt / kShadowCell; i++)
    p[i] = val;
}

void MemoryResetRange(ThreadState *thr, uptr pc, uptr addr, uptr size) {
  MemoryRangeSet(thr, pc, addr, size, 0);
}

void MemoryRangeFreed(ThreadState *thr, uptr pc, uptr addr, uptr size) {
  MemoryAccessRange(thr, pc, addr, size, true);
  Shadow s(thr->fast_state);
  s.MarkAsFreed();
  s.SetWrite(true);
  s.SetAddr0AndSizeLog(0, 3);
  MemoryRangeSet(thr, pc, addr, size, s.raw());
}

void FuncEntry(ThreadState *thr, uptr pc) {
  DCHECK_EQ(thr->in_rtl, 0);
  StatInc(thr, StatFuncEnter);
  DPrintf2("#%d: tsan::FuncEntry %p\n", (int)thr->fast_state.tid(), (void*)pc);
  thr->fast_state.IncrementEpoch();
  TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeFuncEnter, pc);

  // Shadow stack maintenance can be replaced with
  // stack unwinding during trace switch (which presumably must be faster).
  DCHECK(thr->shadow_stack_pos >= &thr->shadow_stack[0]);
  DCHECK(thr->shadow_stack_pos < &thr->shadow_stack[kShadowStackSize]);
  thr->shadow_stack_pos[0] = pc;
  thr->shadow_stack_pos++;
}

void FuncExit(ThreadState *thr) {
  DCHECK_EQ(thr->in_rtl, 0);
  StatInc(thr, StatFuncExit);
  DPrintf2("#%d: tsan::FuncExit\n", (int)thr->fast_state.tid());
  thr->fast_state.IncrementEpoch();
  TraceAddEvent(thr, thr->fast_state.epoch(), EventTypeFuncExit, 0);

  DCHECK(thr->shadow_stack_pos > &thr->shadow_stack[0]);
  DCHECK(thr->shadow_stack_pos < &thr->shadow_stack[kShadowStackSize]);
  thr->shadow_stack_pos--;
}

void IgnoreCtl(ThreadState *thr, bool write, bool begin) {
  DPrintf("#%d: IgnoreCtl(%d, %d)\n", thr->tid, write, begin);
  thr->ignore_reads_and_writes += begin ? 1 : -1;
  CHECK_GE(thr->ignore_reads_and_writes, 0);
  if (thr->ignore_reads_and_writes)
    thr->fast_state.SetIgnoreBit();
  else
    thr->fast_state.ClearIgnoreBit();
}

void InternalAllocStatAggregate(Context *ctx, ThreadState *thr) {
  for (int i = 0; i < (int)MBlockTypeCount; i++) {
    ctx->int_alloc_cnt[i] += thr->int_alloc_cnt[i];
    ctx->int_alloc_siz[i] += thr->int_alloc_siz[i];
    thr->int_alloc_cnt[i] = 0;
    thr->int_alloc_siz[i] = 0;
  }
}

#if TSAN_DEBUG
void build_consistency_debug() {}
#else
void build_consistency_release() {}
#endif

#if TSAN_COLLECT_STATS
void build_consistency_stats() {}
#else
void build_consistency_nostats() {}
#endif

#if TSAN_SHADOW_COUNT == 1
void build_consistency_shadow1() {}
#elif TSAN_SHADOW_COUNT == 2
void build_consistency_shadow2() {}
#elif TSAN_SHADOW_COUNT == 4
void build_consistency_shadow4() {}
#else
void build_consistency_shadow8() {}
#endif

}  // namespace __tsan

// Must be included in this file to make sure everything is inlined.
#include "tsan_interface_inl.h"
