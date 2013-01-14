//===-- sanitizer_allocator64_testlib.cc ----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Malloc replacement library based on CombinedAllocator.
// The primary purpose of this file is an end-to-end integration test
// for CombinedAllocator.
//===----------------------------------------------------------------------===//
/* Usage:
clang++ -fno-exceptions  -g -fPIC -I. -I../include -Isanitizer \
 sanitizer_common/tests/sanitizer_allocator64_testlib.cc \
 sanitizer_common/sanitizer_*.cc -shared -o testmalloc.so
LD_PRELOAD=`pwd`/testmalloc.so /your/app
*/
#include "sanitizer_common/sanitizer_allocator.h"
#include "sanitizer_common/sanitizer_common.h"
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

namespace {
static const uptr kAllocatorSpace = 0x600000000000ULL;
static const uptr kAllocatorSize = 0x10000000000;  // 1T.

typedef SizeClassAllocator64<kAllocatorSpace, kAllocatorSize, 0,
  CompactSizeClassMap> PrimaryAllocator;
typedef SizeClassAllocatorLocalCache<PrimaryAllocator> AllocatorCache;
typedef LargeMmapAllocator<> SecondaryAllocator;
typedef CombinedAllocator<PrimaryAllocator, AllocatorCache,
          SecondaryAllocator> Allocator;

static AllocatorCache cache;
static Allocator allocator;

static int inited = 0;

__attribute__((constructor))
static void Init() {
  if (inited) return;
  inited = true;  // this must happen before any threads are created.
  allocator.Init();
}

}  // namespace

#if 1
extern "C" {
void *malloc(size_t size) {
  Init();
  assert(inited);
  return allocator.Allocate(&cache, size, 8);
}

void free(void *p) {
  if (!inited) return;
  // assert(inited);
  allocator.Deallocate(&cache, p);
}

void *calloc(size_t nmemb, size_t size) {
  Init();
  assert(inited);
  return allocator.Allocate(&cache, nmemb * size, 8, /*cleared=*/true);
}

void *realloc(void *p, size_t new_size) {
  Init();
  assert(inited);
  return allocator.Reallocate(&cache, p, new_size, 8);
}

void *memalign(size_t boundary, size_t size) {
  Init();
  return allocator.Allocate(&cache, size, boundary);
}
void *__libc_memalign(size_t boundary, size_t size) {
  Init();
  return allocator.Allocate(&cache, size, boundary);
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  Init();
  *memptr = allocator.Allocate(&cache, size, alignment);
  CHECK_EQ(((uptr)*memptr & (alignment - 1)), 0);
  return 0;
}

void *valloc(size_t size) {
  Init();
  assert(inited);
  return allocator.Allocate(&cache, size, GetPageSizeCached());
}

void *pvalloc(size_t size) {
  Init();
  assert(inited);
  if (size == 0) size = GetPageSizeCached();
  return allocator.Allocate(&cache, size, GetPageSizeCached());
}

void malloc_usable_size() { }
void mallinfo() { }
void mallopt() { }
}
#endif
