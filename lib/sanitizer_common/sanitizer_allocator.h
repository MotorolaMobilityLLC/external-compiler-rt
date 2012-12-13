//===-- sanitizer_allocator.h -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Specialized memory allocator for ThreadSanitizer, MemorySanitizer, etc.
//
//===----------------------------------------------------------------------===//

#ifndef SANITIZER_ALLOCATOR_H
#define SANITIZER_ALLOCATOR_H

#include "sanitizer_internal_defs.h"
#include "sanitizer_common.h"
#include "sanitizer_libc.h"
#include "sanitizer_list.h"
#include "sanitizer_mutex.h"

namespace __sanitizer {

// Maps size class id to size and back.
template <uptr l0, uptr l1, uptr l2, uptr l3, uptr l4, uptr l5,
          uptr s0, uptr s1, uptr s2, uptr s3, uptr s4,
          uptr c0, uptr c1, uptr c2, uptr c3, uptr c4>
class SplineSizeClassMap {
 private:
  // Here we use a spline composed of 5 polynomials of oder 1.
  // The first size class is l0, then the classes go with step s0
  // untill they reach l1, after which they go with step s1 and so on.
  // Steps should be powers of two for cheap division.
  // The size of the last size class should be a power of two.
  // There should be at most 256 size classes.
  static const uptr u0 = 0  + (l1 - l0) / s0;
  static const uptr u1 = u0 + (l2 - l1) / s1;
  static const uptr u2 = u1 + (l3 - l2) / s2;
  static const uptr u3 = u2 + (l4 - l3) / s3;
  static const uptr u4 = u3 + (l5 - l4) / s4;

 public:
  // The number of size classes should be a power of two for fast division.
  static const uptr kNumClasses = u4 + 1;
  static const uptr kMaxSize = l5;
  static const uptr kMinSize = l0;

  COMPILER_CHECK(kNumClasses <= 256);
  COMPILER_CHECK((kNumClasses & (kNumClasses - 1)) == 0);
  COMPILER_CHECK((kMaxSize & (kMaxSize - 1)) == 0);

  static uptr Size(uptr class_id) {
    if (class_id <= u0) return l0 + s0 * (class_id - 0);
    if (class_id <= u1) return l1 + s1 * (class_id - u0);
    if (class_id <= u2) return l2 + s2 * (class_id - u1);
    if (class_id <= u3) return l3 + s3 * (class_id - u2);
    if (class_id <= u4) return l4 + s4 * (class_id - u3);
    return 0;
  }
  static uptr ClassID(uptr size) {
    if (size <= l1) return 0  + (size - l0 + s0 - 1) / s0;
    if (size <= l2) return u0 + (size - l1 + s1 - 1) / s1;
    if (size <= l3) return u1 + (size - l2 + s2 - 1) / s2;
    if (size <= l4) return u2 + (size - l3 + s3 - 1) / s3;
    if (size <= l5) return u3 + (size - l4 + s4 - 1) / s4;
    return 0;
  }

  static uptr MaxCached(uptr class_id) {
    if (class_id <= u0) return c0;
    if (class_id <= u1) return c1;
    if (class_id <= u2) return c2;
    if (class_id <= u3) return c3;
    if (class_id <= u4) return c4;
    return 0;
  }
};

class DefaultSizeClassMap: public SplineSizeClassMap<
  /* l: */1 << 4, 1 << 9,  1 << 12, 1 << 15, 1 << 18, 1 << 21,
  /* s: */1 << 4, 1 << 6,  1 << 9,  1 << 12, 1 << 15,
  /* c: */256,    64,      16,      4,       1> {
 private:
  COMPILER_CHECK(kNumClasses == 256);
};

class CompactSizeClassMap: public SplineSizeClassMap<
  /* l: */1 << 3, 1 << 4,  1 << 7, 1 << 8, 1 << 12, 1 << 15,
  /* s: */1 << 3, 1 << 4,  1 << 7, 1 << 8, 1 << 12,
  /* c: */256,    64,      16,      4,       1> {
 private:
  COMPILER_CHECK(kNumClasses <= 32);
};

struct AllocatorListNode {
  AllocatorListNode *next;
};

typedef IntrusiveList<AllocatorListNode> AllocatorFreeList;

// Move at most max_count chunks from allocate_from to allocate_to.
// This function is better be a method of AllocatorFreeList, but we can't
// inherit it from IntrusiveList as the ancient gcc complains about non-PODness.
static inline void BulkMove(uptr max_count,
                            AllocatorFreeList *allocate_from,
                            AllocatorFreeList *allocate_to) {
  CHECK(!allocate_from->empty());
  CHECK(allocate_to->empty());
  if (allocate_from->size() <= max_count) {
    allocate_to->append_front(allocate_from);
    CHECK(allocate_from->empty());
  } else {
    for (uptr i = 0; i < max_count; i++) {
      AllocatorListNode *node = allocate_from->front();
      allocate_from->pop_front();
      allocate_to->push_front(node);
    }
    CHECK(!allocate_from->empty());
  }
  CHECK(!allocate_to->empty());
}

// Allocators call these callbacks on mmap/munmap.
struct NoOpMapUnmapCallback {
  void OnMap(uptr p, uptr size) const { }
  void OnUnmap(uptr p, uptr size) const { }
};

// SizeClassAllocator64 -- allocator for 64-bit address space.
//
// Space: a portion of address space of kSpaceSize bytes starting at
// a fixed address (kSpaceBeg). Both constants are powers of two and
// kSpaceBeg is kSpaceSize-aligned.
// At the beginning the entire space is mprotect-ed, then small parts of it
// are mapped on demand.
//
// Region: a part of Space dedicated to a single size class.
// There are kNumClasses Regions of equal size.
//
// UserChunk: a piece of memory returned to user.
// MetaChunk: kMetadataSize bytes of metadata associated with a UserChunk.
//
// A Region looks like this:
// UserChunk1 ... UserChunkN <gap> MetaChunkN ... MetaChunk1
template <const uptr kSpaceBeg, const uptr kSpaceSize,
          const uptr kMetadataSize, class SizeClassMap,
          class MapUnmapCallback = NoOpMapUnmapCallback>
class SizeClassAllocator64 {
 public:
  void Init() {
    CHECK_EQ(kSpaceBeg,
             reinterpret_cast<uptr>(Mprotect(kSpaceBeg, kSpaceSize)));
    MapWithCallback(kSpaceEnd, AdditionalSize());
  }

  void MapWithCallback(uptr beg, uptr size) {
    CHECK_EQ(beg, reinterpret_cast<uptr>(MmapFixedNoReserve(beg, size)));
    MapUnmapCallback().OnMap(beg, size);
  }

  void UnmapWithCallback(uptr beg, uptr size) {
    MapUnmapCallback().OnUnmap(beg, size);
    UnmapOrDie(reinterpret_cast<void *>(beg), size);
  }

  bool CanAllocate(uptr size, uptr alignment) {
    return size <= SizeClassMap::kMaxSize &&
      alignment <= SizeClassMap::kMaxSize;
  }

  void *Allocate(uptr size, uptr alignment) {
    if (size < alignment) size = alignment;
    CHECK(CanAllocate(size, alignment));
    return AllocateBySizeClass(ClassID(size));
  }

  void Deallocate(void *p) {
    CHECK(PointerIsMine(p));
    DeallocateBySizeClass(p, GetSizeClass(p));
  }

  // Allocate several chunks of the given class_id.
  void BulkAllocate(uptr class_id, AllocatorFreeList *free_list) {
    CHECK_LT(class_id, kNumClasses);
    RegionInfo *region = GetRegionInfo(class_id);
    SpinMutexLock l(&region->mutex);
    if (region->free_list.empty()) {
      PopulateFreeList(class_id, region);
    }
    BulkMove(SizeClassMap::MaxCached(class_id), &region->free_list, free_list);
  }

  // Swallow the entire free_list for the given class_id.
  void BulkDeallocate(uptr class_id, AllocatorFreeList *free_list) {
    CHECK_LT(class_id, kNumClasses);
    RegionInfo *region = GetRegionInfo(class_id);
    SpinMutexLock l(&region->mutex);
    region->free_list.append_front(free_list);
  }

  static bool PointerIsMine(void *p) {
    return reinterpret_cast<uptr>(p) / kSpaceSize == kSpaceBeg / kSpaceSize;
  }

  static uptr GetSizeClass(void *p) {
    return (reinterpret_cast<uptr>(p) / kRegionSize) % kNumClasses;
  }

  static void *GetBlockBegin(void *p) {
    uptr class_id = GetSizeClass(p);
    uptr size = SizeClassMap::Size(class_id);
    uptr chunk_idx = GetChunkIdx((uptr)p, size);
    uptr reg_beg = (uptr)p & ~(kRegionSize - 1);
    uptr begin = reg_beg + chunk_idx * size;
    return reinterpret_cast<void*>(begin);
  }

  static uptr GetActuallyAllocatedSize(void *p) {
    CHECK(PointerIsMine(p));
    return SizeClassMap::Size(GetSizeClass(p));
  }

  uptr ClassID(uptr size) { return SizeClassMap::ClassID(size); }

  void *GetMetaData(void *p) {
    uptr class_id = GetSizeClass(p);
    uptr size = SizeClassMap::Size(class_id);
    uptr chunk_idx = GetChunkIdx(reinterpret_cast<uptr>(p), size);
    return reinterpret_cast<void*>(kSpaceBeg + (kRegionSize * (class_id + 1)) -
                                   (1 + chunk_idx) * kMetadataSize);
  }

  uptr TotalMemoryUsed() {
    uptr res = 0;
    for (uptr i = 0; i < kNumClasses; i++)
      res += GetRegionInfo(i)->allocated_user;
    return res;
  }

  // Test-only.
  void TestOnlyUnmap() {
    UnmapWithCallback(kSpaceBeg, kSpaceSize + AdditionalSize());
  }

  typedef SizeClassMap SizeClassMapT;
  static const uptr kNumClasses = SizeClassMap::kNumClasses;  // 2^k <= 256

 private:
  static const uptr kRegionSize = kSpaceSize / kNumClasses;
  static const uptr kSpaceEnd = kSpaceBeg + kSpaceSize;
  COMPILER_CHECK(kSpaceBeg % kSpaceSize == 0);
  // kRegionSize must be >= 2^32.
  COMPILER_CHECK((kRegionSize) >= (1ULL << (SANITIZER_WORDSIZE / 2)));
  // Populate the free list with at most this number of bytes at once
  // or with one element if its size is greater.
  static const uptr kPopulateSize = 1 << 18;
  // Call mmap for user memory with this size.
  static const uptr kUserMapSize = 1 << 22;
  // Call mmap for metadata memory with this size.
  static const uptr kMetaMapSize = 1 << 20;

  struct RegionInfo {
    SpinMutex mutex;
    AllocatorFreeList free_list;
    uptr allocated_user;  // Bytes allocated for user memory.
    uptr allocated_meta;  // Bytes allocated for metadata.
    uptr mapped_user;  // Bytes mapped for user memory.
    uptr mapped_meta;  // Bytes mapped for metadata.
  };
  COMPILER_CHECK(sizeof(RegionInfo) >= kCacheLineSize);

  static uptr AdditionalSize() {
    uptr PageSize = GetPageSizeCached();
    uptr res = Max(sizeof(RegionInfo) * kNumClasses, PageSize);
    CHECK_EQ(res % PageSize, 0);
    return res;
  }

  RegionInfo *GetRegionInfo(uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    RegionInfo *regions = reinterpret_cast<RegionInfo*>(kSpaceBeg + kSpaceSize);
    return &regions[class_id];
  }

  static uptr GetChunkIdx(uptr chunk, uptr size) {
    u32 offset = chunk % kRegionSize;
    // Here we divide by a non-constant. This is costly.
    // We require that kRegionSize is at least 2^32 so that offset is 32-bit.
    // We save 2x by using 32-bit div, but may need to use a 256-way switch.
    return offset / (u32)size;
  }

  void PopulateFreeList(uptr class_id, RegionInfo *region) {
    CHECK(region->free_list.empty());
    uptr size = SizeClassMap::Size(class_id);
    uptr beg_idx = region->allocated_user;
    uptr end_idx = beg_idx + kPopulateSize;
    uptr region_beg = kSpaceBeg + kRegionSize * class_id;
    if (end_idx > region->mapped_user) {
      // Do the mmap for the user memory.
      CHECK_GT(region->mapped_user + kUserMapSize, end_idx);
      MapWithCallback(region_beg + region->mapped_user, kUserMapSize);
      region->mapped_user += kUserMapSize;
    }
    uptr idx = beg_idx;
    uptr i = 0;
    do {  // do-while loop because we need to put at least one item.
      uptr p = region_beg + idx;
      region->free_list.push_front(reinterpret_cast<AllocatorListNode*>(p));
      idx += size;
      i++;
    } while (idx < end_idx);
    region->allocated_user += idx - beg_idx;
    region->allocated_meta += i * kMetadataSize;
    if (region->allocated_meta > region->mapped_meta) {
      // Do the mmap for the metadata.
      CHECK_GT(region->mapped_meta + kMetaMapSize, region->allocated_meta);
      MapWithCallback(region_beg + kRegionSize -
                      region->mapped_meta - kMetaMapSize, kMetaMapSize);
      region->mapped_meta += kMetaMapSize;
    }
    if (region->allocated_user + region->allocated_meta > kRegionSize) {
      Printf("Out of memory. Dying.\n");
      Printf("The process has exhausted %zuMB for size class %zu.\n",
          kRegionSize / 1024 / 1024, size);
      Die();
    }
  }

  void *AllocateBySizeClass(uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    RegionInfo *region = GetRegionInfo(class_id);
    SpinMutexLock l(&region->mutex);
    if (region->free_list.empty()) {
      PopulateFreeList(class_id, region);
    }
    CHECK(!region->free_list.empty());
    AllocatorListNode *node = region->free_list.front();
    region->free_list.pop_front();
    return reinterpret_cast<void*>(node);
  }

  void DeallocateBySizeClass(void *p, uptr class_id) {
    RegionInfo *region = GetRegionInfo(class_id);
    SpinMutexLock l(&region->mutex);
    region->free_list.push_front(reinterpret_cast<AllocatorListNode*>(p));
  }
};

// SizeClassAllocator32 -- allocator for 32-bit address space.
// This allocator can theoretically be used on 64-bit arch, but there it is less
// efficient than SizeClassAllocator64.
//
// [kSpaceBeg, kSpaceBeg + kSpaceSize) is the range of addresses which can
// be returned by MmapOrDie().
//
// Region:
//   a result of a single call to MmapAlignedOrDie(kRegionSize, kRegionSize).
// Since the regions are aligned by kRegionSize, there are exactly
// kNumPossibleRegions possible regions in the address space and so we keep
// an u8 array possible_regions[kNumPossibleRegions] to store the size classes.
// 0 size class means the region is not used by the allocator.
//
// One Region is used to allocate chunks of a single size class.
// A Region looks like this:
// UserChunk1 .. UserChunkN <gap> MetaChunkN .. MetaChunk1
//
// In order to avoid false sharing the objects of this class should be
// chache-line aligned.
template <const uptr kSpaceBeg, const u64 kSpaceSize,
          const uptr kMetadataSize, class SizeClassMap,
          class MapUnmapCallback = NoOpMapUnmapCallback>
class SizeClassAllocator32 {
 public:
  void Init() {
    state_ = reinterpret_cast<State *>(MapWithCallback(sizeof(State)));
  }

  void *MapWithCallback(uptr size) {
    size = RoundUpTo(size, GetPageSizeCached());
    void *res = MmapOrDie(size, "SizeClassAllocator32");
    MapUnmapCallback().OnMap((uptr)res, size);
    return res;
  }
  void UnmapWithCallback(uptr beg, uptr size) {
    MapUnmapCallback().OnUnmap(beg, size);
    UnmapOrDie(reinterpret_cast<void *>(beg), size);
  }

  bool CanAllocate(uptr size, uptr alignment) {
    return size <= SizeClassMap::kMaxSize &&
      alignment <= SizeClassMap::kMaxSize;
  }

  void *Allocate(uptr size, uptr alignment) {
    if (size < alignment) size = alignment;
    CHECK(CanAllocate(size, alignment));
    return AllocateBySizeClass(ClassID(size));
  }

  void Deallocate(void *p) {
    CHECK(PointerIsMine(p));
    DeallocateBySizeClass(p, GetSizeClass(p));
  }

  void *GetMetaData(void *p) {
    CHECK(PointerIsMine(p));
    uptr mem = reinterpret_cast<uptr>(p);
    uptr beg = ComputeRegionBeg(mem);
    uptr size = SizeClassMap::Size(GetSizeClass(p));
    u32 offset = mem - beg;
    uptr n = offset / (u32)size;  // 32-bit division
    uptr meta = (beg + kRegionSize) - (n + 1) * kMetadataSize;
    return reinterpret_cast<void*>(meta);
  }

  // Allocate several chunks of the given class_id.
  void BulkAllocate(uptr class_id, AllocatorFreeList *free_list) {
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    EnsureSizeClassHasAvailableChunks(sci, class_id);
    CHECK(!sci->free_list.empty());
    BulkMove(SizeClassMap::MaxCached(class_id), &sci->free_list, free_list);
  }

  // Swallow the entire free_list for the given class_id.
  void BulkDeallocate(uptr class_id, AllocatorFreeList *free_list) {
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    sci->free_list.append_front(free_list);
  }

  bool PointerIsMine(void *p) {
    return
      state_->possible_regions[ComputeRegionId(reinterpret_cast<uptr>(p))] != 0;
  }

  uptr GetSizeClass(void *p) {
    return
      state_->possible_regions[ComputeRegionId(reinterpret_cast<uptr>(p))] - 1;
  }

  void *GetBlockBegin(void *p) {
    CHECK(PointerIsMine(p));
    uptr mem = reinterpret_cast<uptr>(p);
    uptr beg = ComputeRegionBeg(mem);
    uptr size = SizeClassMap::Size(GetSizeClass(p));
    u32 offset = mem - beg;
    u32 n = offset / (u32)size;  // 32-bit division
    uptr res = beg + (n * (u32)size);
    return reinterpret_cast<void*>(res);
  }

  uptr GetActuallyAllocatedSize(void *p) {
    CHECK(PointerIsMine(p));
    return SizeClassMap::Size(GetSizeClass(p));
  }

  uptr ClassID(uptr size) { return SizeClassMap::ClassID(size); }

  uptr TotalMemoryUsed() {
    // No need to lock here.
    uptr res = 0;
    for (uptr i = 0; i < kNumPossibleRegions; i++)
      if (state_->possible_regions[i])
        res += kRegionSize;
    return res;
  }

  void TestOnlyUnmap() {
    for (uptr i = 0; i < kNumPossibleRegions; i++)
      if (state_->possible_regions[i])
        UnmapWithCallback((i * kRegionSize), kRegionSize);
    UnmapWithCallback(reinterpret_cast<uptr>(state_), sizeof(State));
  }

  typedef SizeClassMap SizeClassMapT;
  static const uptr kNumClasses = SizeClassMap::kNumClasses;  // 2^k <= 128

 private:
  static const uptr kRegionSizeLog = SANITIZER_WORDSIZE == 64 ? 24 : 20;
  static const uptr kRegionSize = 1 << kRegionSizeLog;
  static const uptr kNumPossibleRegions = kSpaceSize / kRegionSize;
  COMPILER_CHECK(kNumClasses <= 128);

  struct SizeClassInfo {
    SpinMutex mutex;
    AllocatorFreeList free_list;
    char padding[kCacheLineSize - sizeof(uptr) - sizeof(AllocatorFreeList)];
  };
  COMPILER_CHECK(sizeof(SizeClassInfo) == kCacheLineSize);

  uptr ComputeRegionId(uptr mem) {
    uptr res = mem >> kRegionSizeLog;
    CHECK_LT(res, kNumPossibleRegions);
    return res;
  }

  uptr ComputeRegionBeg(uptr mem) {
    return mem & ~(kRegionSize - 1);
  }

  uptr AllocateRegion(uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    uptr res = reinterpret_cast<uptr>(MmapAlignedOrDie(kRegionSize, kRegionSize,
                                      "SizeClassAllocator32"));
    MapUnmapCallback().OnMap(res, kRegionSize);
    CHECK_EQ(0U, (res & (kRegionSize - 1)));
    CHECK_EQ(0U, state_->possible_regions[ComputeRegionId(res)]);
    state_->possible_regions[ComputeRegionId(res)] = class_id + 1;
    return res;
  }

  SizeClassInfo *GetSizeClassInfo(uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    return &state_->size_class_info_array[class_id];
  }

  void EnsureSizeClassHasAvailableChunks(SizeClassInfo *sci, uptr class_id) {
    if (!sci->free_list.empty()) return;
    uptr size = SizeClassMap::Size(class_id);
    uptr reg = AllocateRegion(class_id);
    uptr n_chunks = kRegionSize / (size + kMetadataSize);
    for (uptr i = reg; i < reg + n_chunks * size; i += size)
      sci->free_list.push_back(reinterpret_cast<AllocatorListNode*>(i));
  }

  void *AllocateBySizeClass(uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    EnsureSizeClassHasAvailableChunks(sci, class_id);
    CHECK(!sci->free_list.empty());
    AllocatorListNode *node = sci->free_list.front();
    sci->free_list.pop_front();
    return reinterpret_cast<void*>(node);
  }

  void DeallocateBySizeClass(void *p, uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    SizeClassInfo *sci = GetSizeClassInfo(class_id);
    SpinMutexLock l(&sci->mutex);
    sci->free_list.push_front(reinterpret_cast<AllocatorListNode*>(p));
  }

  struct State {
    u8 possible_regions[kNumPossibleRegions];
    SizeClassInfo size_class_info_array[kNumClasses];
  };
  State *state_;
};

// Objects of this type should be used as local caches for SizeClassAllocator64.
// Since the typical use of this class is to have one object per thread in TLS,
// is has to be POD.
template<class SizeClassAllocator>
struct SizeClassAllocatorLocalCache {
  typedef SizeClassAllocator Allocator;
  static const uptr kNumClasses = SizeClassAllocator::kNumClasses;
  // Don't need to call Init if the object is a global (i.e. zero-initialized).
  void Init() {
    internal_memset(this, 0, sizeof(*this));
  }

  void *Allocate(SizeClassAllocator *allocator, uptr class_id) {
    CHECK_LT(class_id, kNumClasses);
    AllocatorFreeList *free_list = &free_lists_[class_id];
    if (free_list->empty())
      allocator->BulkAllocate(class_id, free_list);
    CHECK(!free_list->empty());
    void *res = free_list->front();
    free_list->pop_front();
    return res;
  }

  void Deallocate(SizeClassAllocator *allocator, uptr class_id, void *p) {
    CHECK_LT(class_id, kNumClasses);
    AllocatorFreeList *free_list = &free_lists_[class_id];
    free_list->push_front(reinterpret_cast<AllocatorListNode*>(p));
    if (free_list->size() >= 2 * SizeClassMap::MaxCached(class_id))
      DrainHalf(allocator, class_id);
  }

  void Drain(SizeClassAllocator *allocator) {
    for (uptr i = 0; i < kNumClasses; i++) {
      allocator->BulkDeallocate(i, &free_lists_[i]);
      CHECK(free_lists_[i].empty());
    }
  }

  // private:
  typedef typename SizeClassAllocator::SizeClassMapT SizeClassMap;
  AllocatorFreeList free_lists_[kNumClasses];

  void DrainHalf(SizeClassAllocator *allocator, uptr class_id) {
    AllocatorFreeList *free_list = &free_lists_[class_id];
    AllocatorFreeList half;
    half.clear();
    const uptr count = free_list->size() / 2;
    for (uptr i = 0; i < count; i++) {
      AllocatorListNode *node = free_list->front();
      free_list->pop_front();
      half.push_front(node);
    }
    allocator->BulkDeallocate(class_id, &half);
  }
};

// This class can (de)allocate only large chunks of memory using mmap/unmap.
// The main purpose of this allocator is to cover large and rare allocation
// sizes not covered by more efficient allocators (e.g. SizeClassAllocator64).
template <class MapUnmapCallback = NoOpMapUnmapCallback>
class LargeMmapAllocator {
 public:
  void Init() {
    internal_memset(this, 0, sizeof(*this));
    page_size_ = GetPageSizeCached();
  }
  void *Allocate(uptr size, uptr alignment) {
    CHECK(IsPowerOfTwo(alignment));
    uptr map_size = RoundUpMapSize(size);
    if (alignment > page_size_)
      map_size += alignment;
    if (map_size < size) return 0;  // Overflow.
    uptr map_beg = reinterpret_cast<uptr>(
        MmapOrDie(map_size, "LargeMmapAllocator"));
    MapUnmapCallback().OnMap(map_beg, map_size);
    uptr map_end = map_beg + map_size;
    uptr res = map_beg + page_size_;
    if (res & (alignment - 1))  // Align.
      res += alignment - (res & (alignment - 1));
    CHECK_EQ(0, res & (alignment - 1));
    CHECK_LE(res + size, map_end);
    Header *h = GetHeader(res);
    h->size = size;
    h->map_beg = map_beg;
    h->map_size = map_size;
    {
      SpinMutexLock l(&mutex_);
      h->next = list_;
      h->prev = 0;
      if (list_)
        list_->prev = h;
      list_ = h;
    }
    return reinterpret_cast<void*>(res);
  }

  void Deallocate(void *p) {
    Header *h = GetHeader(p);
    {
      SpinMutexLock l(&mutex_);
      Header *prev = h->prev;
      Header *next = h->next;
      if (prev)
        prev->next = next;
      if (next)
        next->prev = prev;
      if (h == list_)
        list_ = next;
    }
    MapUnmapCallback().OnUnmap(h->map_beg, h->map_size);
    UnmapOrDie(reinterpret_cast<void*>(h->map_beg), h->map_size);
  }

  uptr TotalMemoryUsed() {
    SpinMutexLock l(&mutex_);
    uptr res = 0;
    for (Header *l = list_; l; l = l->next) {
      res += RoundUpMapSize(l->size);
    }
    return res;
  }

  bool PointerIsMine(void *p) {
    // Fast check.
    if ((reinterpret_cast<uptr>(p) & (page_size_ - 1))) return false;
    SpinMutexLock l(&mutex_);
    for (Header *l = list_; l; l = l->next) {
      if (GetUser(l) == p) return true;
    }
    return false;
  }

  uptr GetActuallyAllocatedSize(void *p) {
    return RoundUpMapSize(GetHeader(p)->size) - page_size_;
  }

  // At least page_size_/2 metadata bytes is available.
  void *GetMetaData(void *p) {
    return GetHeader(p) + 1;
  }

  void *GetBlockBegin(void *p) {
    SpinMutexLock l(&mutex_);
    for (Header *l = list_; l; l = l->next) {
      void *b = GetUser(l);
      if (p >= b && p < (u8*)b + l->size)
        return b;
    }
    return 0;
  }

 private:
  struct Header {
    uptr map_beg;
    uptr map_size;
    uptr size;
    Header *next;
    Header *prev;
  };

  Header *GetHeader(uptr p) {
    CHECK_EQ(p % page_size_, 0);
    return reinterpret_cast<Header*>(p - page_size_);
  }
  Header *GetHeader(void *p) { return GetHeader(reinterpret_cast<uptr>(p)); }

  void *GetUser(Header *h) {
    CHECK_EQ((uptr)h % page_size_, 0);
    return reinterpret_cast<void*>(reinterpret_cast<uptr>(h) + page_size_);
  }

  uptr RoundUpMapSize(uptr size) {
    return RoundUpTo(size, page_size_) + page_size_;
  }

  uptr page_size_;
  Header *list_;
  SpinMutex mutex_;
};

// This class implements a complete memory allocator by using two
// internal allocators:
// PrimaryAllocator is efficient, but may not allocate some sizes (alignments).
//  When allocating 2^x bytes it should return 2^x aligned chunk.
// PrimaryAllocator is used via a local AllocatorCache.
// SecondaryAllocator can allocate anything, but is not efficient.
template <class PrimaryAllocator, class AllocatorCache,
          class SecondaryAllocator>  // NOLINT
class CombinedAllocator {
 public:
  void Init() {
    primary_.Init();
    secondary_.Init();
  }

  void *Allocate(AllocatorCache *cache, uptr size, uptr alignment,
                 bool cleared = false) {
    // Returning 0 on malloc(0) may break a lot of code.
    if (size == 0)
      size = 1;
    if (size + alignment < size)
      return 0;
    if (alignment > 8)
      size = RoundUpTo(size, alignment);
    void *res;
    if (primary_.CanAllocate(size, alignment))
      res = cache->Allocate(&primary_, primary_.ClassID(size));
    else
      res = secondary_.Allocate(size, alignment);
    if (alignment > 8)
      CHECK_EQ(reinterpret_cast<uptr>(res) & (alignment - 1), 0);
    if (cleared && res)
      internal_memset(res, 0, size);
    return res;
  }

  void Deallocate(AllocatorCache *cache, void *p) {
    if (!p) return;
    if (primary_.PointerIsMine(p))
      cache->Deallocate(&primary_, primary_.GetSizeClass(p), p);
    else
      secondary_.Deallocate(p);
  }

  void *Reallocate(AllocatorCache *cache, void *p, uptr new_size,
                   uptr alignment) {
    if (!p)
      return Allocate(cache, new_size, alignment);
    if (!new_size) {
      Deallocate(cache, p);
      return 0;
    }
    CHECK(PointerIsMine(p));
    uptr old_size = GetActuallyAllocatedSize(p);
    uptr memcpy_size = Min(new_size, old_size);
    void *new_p = Allocate(cache, new_size, alignment);
    if (new_p)
      internal_memcpy(new_p, p, memcpy_size);
    Deallocate(cache, p);
    return new_p;
  }

  bool PointerIsMine(void *p) {
    if (primary_.PointerIsMine(p))
      return true;
    return secondary_.PointerIsMine(p);
  }

  void *GetMetaData(void *p) {
    if (primary_.PointerIsMine(p))
      return primary_.GetMetaData(p);
    return secondary_.GetMetaData(p);
  }

  void *GetBlockBegin(void *p) {
    if (primary_.PointerIsMine(p))
      return primary_.GetBlockBegin(p);
    return secondary_.GetBlockBegin(p);
  }

  uptr GetActuallyAllocatedSize(void *p) {
    if (primary_.PointerIsMine(p))
      return primary_.GetActuallyAllocatedSize(p);
    return secondary_.GetActuallyAllocatedSize(p);
  }

  uptr TotalMemoryUsed() {
    return primary_.TotalMemoryUsed() + secondary_.TotalMemoryUsed();
  }

  void TestOnlyUnmap() { primary_.TestOnlyUnmap(); }

  void SwallowCache(AllocatorCache *cache) {
    cache->Drain(&primary_);
  }

 private:
  PrimaryAllocator primary_;
  SecondaryAllocator secondary_;
};

}  // namespace __sanitizer

#endif  // SANITIZER_ALLOCATOR_H

