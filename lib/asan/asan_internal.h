//===-- asan_internal.h -----------------------------------------*- C++ -*-===//
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
// ASan-private header which defines various general utilities.
//===----------------------------------------------------------------------===//
#ifndef ASAN_INTERNAL_H
#define ASAN_INTERNAL_H

#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)
# error "This operating system is not supported by AddressSanitizer"
#endif

#include <stdlib.h>  // for size_t, uintptr_t, etc.

#if defined(_WIN32)
// There's no <stdint.h> in Visual Studio 9, so we have to define [u]int*_t.
typedef unsigned __int8  uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
typedef __int8           int8_t;
typedef __int16          int16_t;
typedef __int32          int32_t;
typedef __int64          int64_t;

# define ALIAS(x)   // TODO(timurrrr): do we need this on Windows?
# define ALIGNED(x) __declspec(align(x))
# define NOINLINE __declspec(noinline)

# define ASAN_INTERFACE_ATTRIBUTE  // TODO(timurrrr): do we need this on Win?
#else  // defined(_WIN32)
# include <stdint.h>  // for __WORDSIZE

# define ALIAS(x) __attribute__((alias(x)))
# define ALIGNED(x) __attribute__((aligned(x)))
# define NOINLINE __attribute__((noinline))

# define ASAN_INTERFACE_ATTRIBUTE __attribute__((visibility("default")))
#endif  // defined(_WIN32)

// If __WORDSIZE was undefined by the platform, define it in terms of the
// compiler built-ins __LP64__ and _WIN64.
#ifndef __WORDSIZE
#if __LP64__ || defined(_WIN64)
#define __WORDSIZE 64
#else
#define __WORDSIZE 32
#endif
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if defined(__has_feature) && __has_feature(address_sanitizer)
# error "The AddressSanitizer run-time should not be"
        " instrumented by AddressSanitizer"
#endif

// Build-time configuration options.

// If set, asan will install its own SEGV signal handler.
#ifndef ASAN_NEEDS_SEGV
# define ASAN_NEEDS_SEGV 1
#endif

// If set, asan will intercept C++ exception api call(s).
#ifndef ASAN_HAS_EXCEPTIONS
# define ASAN_HAS_EXCEPTIONS 1
#endif

// If set, asan uses the values of SHADOW_SCALE and SHADOW_OFFSET
// provided by the instrumented objects. Otherwise constants are used.
#ifndef ASAN_FLEXIBLE_MAPPING_AND_OFFSET
# define ASAN_FLEXIBLE_MAPPING_AND_OFFSET 0
#endif

// All internal functions in asan reside inside the __asan namespace
// to avoid namespace collisions with the user programs.
// Seperate namespace also makes it simpler to distinguish the asan run-time
// functions from the instrumented user code in a profile.
namespace __asan {

class AsanThread;
struct AsanStackTrace;

// asan_rtl.cc
void CheckFailed(const char *cond, const char *file, int line);
void ShowStatsAndAbort();

// asan_globals.cc
bool DescribeAddrIfGlobal(uintptr_t addr);

// asan_malloc_linux.cc / asan_malloc_mac.cc
void ReplaceSystemMalloc();

void OutOfMemoryMessageAndDie(const char *mem_type, size_t size);

// asan_linux.cc / asan_mac.cc
void *AsanDoesNotSupportStaticLinkage();
int AsanOpenReadonly(const char* filename);
const char *AsanGetEnv(const char *name);

void *AsanMmapFixedNoReserve(uintptr_t fixed_addr, size_t size);
void *AsanMmapFixedReserve(uintptr_t fixed_addr, size_t size);
void *AsanMprotect(uintptr_t fixed_addr, size_t size);
void *AsanMmapSomewhereOrDie(size_t size, const char *where);
void AsanUnmapOrDie(void *ptr, size_t size);

void AsanDisableCoreDumper();
void GetPcSpBp(void *context, uintptr_t *pc, uintptr_t *sp, uintptr_t *bp);

size_t AsanRead(int fd, void *buf, size_t count);
size_t AsanWrite(int fd, const void *buf, size_t count);
int AsanClose(int fd);

bool AsanInterceptsSignal(int signum);
void InstallSignalHandlers();
int GetPid();
uintptr_t GetThreadSelf();
int AtomicInc(int *a);

// Wrapper for TLS/TSD.
void AsanTSDInit();
void *AsanTSDGet();
void AsanTSDSet(void *tsd);

// Opens the file 'file_name" and reads up to 'max_len' bytes.
// The resulting buffer is mmaped and stored in '*buff'.
// The size of the mmaped region is stored in '*buff_size',
// Returns the number of read bytes or 0 if file can not be opened.
size_t ReadFileToBuffer(const char *file_name, char **buff,
                        size_t *buff_size, size_t max_len);

// asan_printf.cc
void RawWrite(const char *buffer);
int SNPrint(char *buffer, size_t length, const char *format, ...);
void Printf(const char *format, ...);
int SScanf(const char *str, const char *format, ...);
void Report(const char *format, ...);

// Don't use std::min and std::max, to minimize dependency on libstdc++.
template<class T> T Min(T a, T b) { return a < b ? a : b; }
template<class T> T Max(T a, T b) { return a > b ? a : b; }

// asan_poisoning.cc
// Poisons the shadow memory for "size" bytes starting from "addr".
void PoisonShadow(uintptr_t addr, size_t size, uint8_t value);
// Poisons the shadow memory for "redzone_size" bytes starting from
// "addr + size".
void PoisonShadowPartialRightRedzone(uintptr_t addr,
                                     uintptr_t size,
                                     uintptr_t redzone_size,
                                     uint8_t value);

extern size_t FLAG_quarantine_size;
extern int    FLAG_demangle;
extern bool   FLAG_symbolize;
extern int    FLAG_v;
extern size_t FLAG_redzone;
extern int    FLAG_debug;
extern bool   FLAG_poison_shadow;
extern int    FLAG_report_globals;
extern size_t FLAG_malloc_context_size;
extern bool   FLAG_replace_str;
extern bool   FLAG_replace_intrin;
extern bool   FLAG_replace_cfallocator;
extern bool   FLAG_fast_unwind;
extern bool   FLAG_use_fake_stack;
extern size_t FLAG_max_malloc_fill_size;
extern int    FLAG_exitcode;
extern bool   FLAG_allow_user_poisoning;
extern int    FLAG_sleep_before_dying;
extern bool   FLAG_handle_segv;

extern int asan_inited;
// Used to avoid infinite recursion in __asan_init().
extern bool asan_init_is_running;

enum LinkerInitialized { LINKER_INITIALIZED = 0 };

void AsanDie();

#define CHECK(cond) do { if (!(cond)) { \
  CheckFailed(#cond, __FILE__, __LINE__); \
}}while(0)

#define RAW_CHECK_MSG(expr, msg) do { \
  if (!(expr)) { \
    RawWrite(msg); \
    AsanDie(); \
  } \
} while (0)

#define RAW_CHECK(expr) RAW_CHECK_MSG(expr, #expr)

#define UNIMPLEMENTED() CHECK("unimplemented" && 0)

#define ASAN_ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))

const size_t kWordSize = __WORDSIZE / 8;
const size_t kWordSizeInBits = 8 * kWordSize;
const size_t kPageSizeBits = 12;
const size_t kPageSize = 1UL << kPageSizeBits;

#ifndef _WIN32
#define GET_CALLER_PC() (uintptr_t)__builtin_return_address(0)
#define GET_CURRENT_FRAME() (uintptr_t)__builtin_frame_address(0)
#else
// TODO(timurrrr): implement.
#define GET_CALLER_PC() (uintptr_t)0
#define GET_CURRENT_FRAME() (uintptr_t)0
#endif

#define GET_BP_PC_SP \
  uintptr_t bp = GET_CURRENT_FRAME();              \
  uintptr_t pc = GET_CALLER_PC();                  \
  uintptr_t local_stack;                           \
  uintptr_t sp = (uintptr_t)&local_stack;

// These magic values are written to shadow for better error reporting.
const int kAsanHeapLeftRedzoneMagic = 0xfa;
const int kAsanHeapRightRedzoneMagic = 0xfb;
const int kAsanHeapFreeMagic = 0xfd;
const int kAsanStackLeftRedzoneMagic = 0xf1;
const int kAsanStackMidRedzoneMagic = 0xf2;
const int kAsanStackRightRedzoneMagic = 0xf3;
const int kAsanStackPartialRedzoneMagic = 0xf4;
const int kAsanStackAfterReturnMagic = 0xf5;
const int kAsanUserPoisonedMemoryMagic = 0xf7;
const int kAsanGlobalRedzoneMagic = 0xf9;
const int kAsanInternalHeapMagic = 0xfe;

static const uintptr_t kCurrentStackFrameMagic = 0x41B58AB3;
static const uintptr_t kRetiredStackFrameMagic = 0x45E0360E;

// --------------------------- Bit twiddling ------- {{{1
inline bool IsPowerOfTwo(size_t x) {
  return (x & (x - 1)) == 0;
}

inline size_t RoundUpTo(size_t size, size_t boundary) {
  CHECK(IsPowerOfTwo(boundary));
  return (size + boundary - 1) & ~(boundary - 1);
}

// -------------------------- LowLevelAllocator ----- {{{1
// A simple low-level memory allocator for internal use.
class LowLevelAllocator {
 public:
  explicit LowLevelAllocator(LinkerInitialized) {}
  // 'size' must be a power of two.
  // Requires an external lock.
  void *Allocate(size_t size);
 private:
  char *allocated_end_;
  char *allocated_current_;
};

}  // namespace __asan

#endif  // ASAN_INTERNAL_H
