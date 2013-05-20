//=-- lsan_common.cc ------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of LeakSanitizer.
// Implementation of common leak checking functionality.
//
//===----------------------------------------------------------------------===//

#include "lsan_common.h"

#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_stackdepot.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "sanitizer_common/sanitizer_stoptheworld.h"

namespace __lsan {

Flags lsan_flags;

static void InitializeFlags() {
  Flags *f = flags();
  // Default values.
  f->sources = kSourceAllAligned;
  f->report_blocks = false;
  f->resolution = 0;
  f->max_leaks = 0;
  f->log_pointers = false;
  f->log_threads = false;

  const char *options = GetEnv("LSAN_OPTIONS");
  if (options) {
    bool aligned = true;
    ParseFlag(options, &aligned, "aligned");
    if (!aligned) f->sources |= kSourceUnaligned;
    ParseFlag(options, &f->report_blocks, "report_blocks");
    ParseFlag(options, &f->resolution, "resolution");
    CHECK_GE(&f->resolution, 0);
    ParseFlag(options, &f->max_leaks, "max_leaks");
    CHECK_GE(&f->max_leaks, 0);
    ParseFlag(options, &f->log_pointers, "log_pointers");
    ParseFlag(options, &f->log_threads, "log_threads");
  }
}

void InitCommonLsan() {
  InitializeFlags();
  InitializePlatformSpecificModules();
}

static inline bool CanBeAHeapPointer(uptr p) {
  // Since our heap is located in mmap-ed memory, we can assume a sensible lower
  // boundary on heap addresses.
  const uptr kMinAddress = 4 * 4096;
  if (p < kMinAddress) return false;
#ifdef __x86_64__
  // Accept only canonical form user-space addresses.
  return ((p >> 47) == 0);
#else
  return true;
#endif
}

// Scan the memory range, looking for byte patterns that point into allocator
// chunks. Mark those chunks with tag and add them to the frontier.
// There are two usage modes for this function: finding non-leaked chunks
// (tag = kReachable) and finding indirectly leaked chunks
// (tag = kIndirectlyLeaked). In the second case, there's no flood fill,
// so frontier = 0.
void ScanRangeForPointers(uptr begin, uptr end, InternalVector<uptr> *frontier,
                          const char *region_type, ChunkTag tag) {
  const uptr alignment = flags()->pointer_alignment();
  if (flags()->log_pointers)
    Report("Scanning %s range %p-%p.\n", region_type, begin, end);
  uptr pp = begin;
  if (pp % alignment)
    pp = pp + alignment - pp % alignment;
  for (; pp + sizeof(uptr) <= end; pp += alignment) {
    void *p = *reinterpret_cast<void**>(pp);
    if (!CanBeAHeapPointer(reinterpret_cast<uptr>(p))) continue;
    // FIXME: PointsIntoChunk is SLOW because GetBlockBegin() in
    // LargeMmapAllocator involves a lock and a linear search.
    void *chunk = PointsIntoChunk(p);
    if (!chunk) continue;
    LsanMetadata m(chunk);
    if (m.tag() == kReachable) continue;
    m.set_tag(tag);
    if (flags()->log_pointers)
      Report("%p: found %p pointing into chunk %p-%p of size %llu.\n", pp, p,
             chunk, reinterpret_cast<uptr>(chunk) + m.requested_size(),
             m.requested_size());
    if (frontier)
      frontier->push_back(reinterpret_cast<uptr>(chunk));
  }
}

// Scan thread data (stacks and TLS) for heap pointers.
static void ProcessThreads(SuspendedThreadsList const &suspended_threads,
                           InternalVector<uptr> *frontier) {
  InternalScopedBuffer<uptr> registers(SuspendedThreadsList::RegisterCount());
  uptr registers_begin = reinterpret_cast<uptr>(registers.data());
  uptr registers_end = registers_begin + registers.size();
  for (uptr i = 0; i < suspended_threads.thread_count(); i++) {
    uptr os_id = static_cast<uptr>(suspended_threads.GetThreadID(i));
    if (flags()->log_threads) Report("Processing thread %d.\n", os_id);
    uptr stack_begin, stack_end, tls_begin, tls_end, cache_begin, cache_end;
    bool thread_found = GetThreadRangesLocked(os_id, &stack_begin, &stack_end,
                                              &tls_begin, &tls_end,
                                              &cache_begin, &cache_end);
    if (!thread_found) {
      // If a thread can't be found in the thread registry, it's probably in the
      // process of destruction. Log this event and move on.
      if (flags()->log_threads)
        Report("Thread %d not found in registry.\n", os_id);
      continue;
    }
    uptr sp;
    bool have_registers =
        (suspended_threads.GetRegistersAndSP(i, registers.data(), &sp) == 0);
    if (!have_registers) {
      Report("Unable to get registers from thread %d.\n");
      // If unable to get SP, consider the entire stack to be reachable.
      sp = stack_begin;
    }

    if (flags()->use_registers() && have_registers)
      ScanRangeForPointers(registers_begin, registers_end, frontier,
                           "REGISTERS", kReachable);

    if (flags()->use_stacks()) {
      if (flags()->log_threads)
        Report("Stack at %p-%p, SP = %p.\n", stack_begin, stack_end, sp);
      if (sp < stack_begin || sp >= stack_end) {
        // SP is outside the recorded stack range (e.g. the thread is running a
        // signal handler on alternate stack). Again, consider the entire stack
        // range to be reachable.
        if (flags()->log_threads)
          Report("WARNING: stack_pointer not in stack_range.\n");
      } else {
        // Shrink the stack range to ignore out-of-scope values.
        stack_begin = sp;
      }
      ScanRangeForPointers(stack_begin, stack_end, frontier, "STACK",
                           kReachable);
    }

    if (flags()->use_tls()) {
      if (flags()->log_threads) Report("TLS at %p-%p.\n", tls_begin, tls_end);
      // Because LSan should not be loaded with dlopen(), we can assume
      // that allocator cache will be part of static TLS image.
      CHECK_LE(tls_begin, cache_begin);
      CHECK_GE(tls_end, cache_end);
      if (tls_begin < cache_begin)
        ScanRangeForPointers(tls_begin, cache_begin, frontier, "TLS",
                             kReachable);
      if (tls_end > cache_end)
        ScanRangeForPointers(cache_end, tls_end, frontier, "TLS", kReachable);
    }
  }
}

static void FloodFillReachable(InternalVector<uptr> *frontier) {
  while (frontier->size()) {
    uptr next_chunk = frontier->back();
    frontier->pop_back();
    LsanMetadata m(reinterpret_cast<void *>(next_chunk));
    ScanRangeForPointers(next_chunk, next_chunk + m.requested_size(), frontier,
                         "HEAP", kReachable);
  }
}

// Mark leaked chunks which are reachable from other leaked chunks.
void MarkIndirectlyLeakedCb::operator()(void *p) const {
  LsanMetadata m(p);
  if (m.allocated() && m.tag() != kReachable) {
    ScanRangeForPointers(reinterpret_cast<uptr>(p),
                         reinterpret_cast<uptr>(p) + m.requested_size(),
                         /* frontier */ 0, "HEAP", kIndirectlyLeaked);
  }
}

// Set the appropriate tag on each chunk.
static void ClassifyAllChunks(SuspendedThreadsList const &suspended_threads) {
  // Holds the flood fill frontier.
  InternalVector<uptr> frontier(GetPageSizeCached());

  if (flags()->use_globals())
    ProcessGlobalRegions(&frontier);
  ProcessThreads(suspended_threads, &frontier);
  FloodFillReachable(&frontier);
  ProcessPlatformSpecificAllocations(&frontier);
  FloodFillReachable(&frontier);

  // Now all reachable chunks are marked. Iterate over leaked chunks and mark
  // those that are reachable from other leaked chunks.
  if (flags()->log_pointers)
    Report("Now scanning leaked blocks for pointers.\n");
  ForEachChunk(MarkIndirectlyLeakedCb());
}

void ClearTagCb::operator()(void *p) const {
  LsanMetadata m(p);
  m.set_tag(kDirectlyLeaked);
}

static void PrintStackTraceById(u32 stack_trace_id) {
  CHECK(stack_trace_id);
  uptr size = 0;
  const uptr *trace = StackDepotGet(stack_trace_id, &size);
  StackTrace::PrintStack(trace, size, common_flags()->symbolize,
                         common_flags()->strip_path_prefix, 0);
}

static void LockAndSuspendThreads(StopTheWorldCallback callback, void *arg) {
  LockThreadRegistry();
  LockAllocator();
  StopTheWorld(callback, arg);
  // Allocator must be unlocked by the callback.
  UnlockThreadRegistry();
}

///// Normal leak checking. /////

void CollectLeaksCb::operator()(void *p) const {
  LsanMetadata m(p);
  if (!m.allocated()) return;
  if (m.tag() != kReachable) {
    uptr resolution = flags()->resolution;
    if (resolution > 0) {
      uptr size = 0;
      const uptr *trace = StackDepotGet(m.stack_trace_id(), &size);
      size = Min(size, resolution);
      leak_report_->Add(StackDepotPut(trace, size), m.requested_size(),
                        m.tag());
    } else {
      leak_report_->Add(m.stack_trace_id(), m.requested_size(), m.tag());
    }
  }
}

static void CollectLeaks(LeakReport *leak_report) {
  ForEachChunk(CollectLeaksCb(leak_report));
}

void PrintLeakedCb::operator()(void *p) const {
  LsanMetadata m(p);
  if (!m.allocated()) return;
  if (m.tag() != kReachable) {
    CHECK(m.tag() == kDirectlyLeaked || m.tag() == kIndirectlyLeaked);
    Printf("%s leaked %llu byte block at %p\n",
           m.tag() == kDirectlyLeaked ? "Directly" : "Indirectly",
           m.requested_size(), p);
  }
}

static void PrintLeaked() {
  Printf("\nReporting individual blocks:\n");
  ForEachChunk(PrintLeakedCb());
}

static void DoLeakCheckCallback(const SuspendedThreadsList &suspended_threads,
                                void *arg) {
  // Allocator must not be locked when we call GetRegionBegin().
  UnlockAllocator();
  bool *success = reinterpret_cast<bool *>(arg);
  ClassifyAllChunks(suspended_threads);
  LeakReport leak_report;
  CollectLeaks(&leak_report);
  if (!leak_report.IsEmpty()) {
    leak_report.PrintLargest(flags()->max_leaks);
    if (flags()->report_blocks)
      PrintLeaked();
  }
  ForEachChunk(ClearTagCb());
  *success = true;
}

void DoLeakCheck() {
  bool success = false;
  LockAndSuspendThreads(DoLeakCheckCallback, &success);
  if (!success)
    Report("Leak check failed!\n");
}

///// Reporting of leaked blocks' addresses (for testing). /////

void ReportLeakedCb::operator()(void *p) const {
  LsanMetadata m(p);
  if (m.allocated() && m.tag() != kReachable)
    leaked_->push_back(p);
}

struct ReportLeakedParam {
  InternalVector<void *> *leaked;
  uptr sources;
  bool success;
};

static void ReportLeakedCallback(const SuspendedThreadsList &suspended_threads,
                                 void *arg) {
  // Allocator must not be locked when we call GetRegionBegin().
  UnlockAllocator();
  ReportLeakedParam *param = reinterpret_cast<ReportLeakedParam *>(arg);
  flags()->sources = param->sources;
  ClassifyAllChunks(suspended_threads);
  ForEachChunk(ReportLeakedCb(param->leaked));
  ForEachChunk(ClearTagCb());
  param->success = true;
}

void ReportLeaked(InternalVector<void *> *leaked, uptr sources) {
  CHECK_EQ(0, leaked->size());
  ReportLeakedParam param;
  param.leaked = leaked;
  param.success = false;
  param.sources = sources;
  LockAndSuspendThreads(ReportLeakedCallback, &param);
  CHECK(param.success);
}

///// LeakReport implementation. /////

// A hard limit on the number of distinct leaks, to avoid quadratic complexity
// in LeakReport::Add(). We don't expect to ever see this many leaks in
// real-world applications.
// FIXME: Get rid of this limit by changing the implementation of LeakReport to
// use a hash table.
const uptr kMaxLeaksConsidered = 1000;

void LeakReport::Add(u32 stack_trace_id, uptr leaked_size, ChunkTag tag) {
  CHECK(tag == kDirectlyLeaked || tag == kIndirectlyLeaked);
  bool is_directly_leaked = (tag == kDirectlyLeaked);
  for (uptr i = 0; i < leaks_.size(); i++)
    if (leaks_[i].stack_trace_id == stack_trace_id &&
        leaks_[i].is_directly_leaked == is_directly_leaked) {
      leaks_[i].hit_count++;
      leaks_[i].total_size += leaked_size;
      return;
    }
  if (leaks_.size() == kMaxLeaksConsidered) return;
  Leak leak = { /* hit_count */ 1, leaked_size, stack_trace_id,
                is_directly_leaked };
  leaks_.push_back(leak);
}

static bool IsLarger(const Leak &leak1, const Leak &leak2) {
  return leak1.total_size > leak2.total_size;
}

void LeakReport::PrintLargest(uptr max_leaks) {
  CHECK(leaks_.size() <= kMaxLeaksConsidered);
  Printf("\n");
  if (leaks_.size() == kMaxLeaksConsidered)
    Printf("Too many leaks! Only the first %llu leaks encountered will be "
           "reported.\n",
           kMaxLeaksConsidered);
  if (max_leaks > 0 && max_leaks < leaks_.size())
    Printf("The %llu largest leak%s:\n", max_leaks, max_leaks > 1 ? "s" : "");
  InternalSort(&leaks_, leaks_.size(), IsLarger);
  max_leaks = max_leaks > 0 ? Min(max_leaks, leaks_.size()) : leaks_.size();
  for (uptr i = 0; i < max_leaks; i++) {
    Printf("\n%s leak of %llu bytes in %llu objects allocated from:\n",
           leaks_[i].is_directly_leaked ? "Direct" : "Indirect",
           leaks_[i].total_size, leaks_[i].hit_count);
    PrintStackTraceById(leaks_[i].stack_trace_id);
  }
  if (max_leaks < leaks_.size()) {
    uptr remaining = leaks_.size() - max_leaks;
    Printf("\nOmitting %llu more leak%s.\n", remaining,
           remaining > 1 ? "s" : "");
  }
}

}  // namespace __lsan
