//===-- asan_interface.h ----------------------------------------*- C++ -*-===//
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
// This header can be included by the instrumented program to fetch
// data (mostly allocator statistics) from ASan runtime library.
//===----------------------------------------------------------------------===//
#ifndef ASAN_INTERFACE_H
#define ASAN_INTERFACE_H

// ----------- ATTENTION -------------
// This header should NOT include any other headers from ASan runtime.
// All functions in this header are extern "C" and start with __asan_.

#if !defined(_WIN32)
#include <stdint.h>  // for uintptr_t
#define ASAN_INTERFACE_FUNCTION_ATTRIBUTE __attribute__((visibility("default")))
#else
// TODO(timurrrr): find out what we need on Windows. __declspec(dllexport) ?
#define ASAN_INTERFACE_FUNCTION_ATTRIBUTE
#endif
#include <stddef.h>  // for size_t

extern "C" {
  // This function should be called at the very beginning of the process,
  // before any instrumented code is executed and before any call to malloc.
  void __asan_init() ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // This function should be called by the instrumented code.
  // 'addr' is the address of a global variable called 'name' of 'size' bytes.
  void __asan_register_global(uintptr_t addr, size_t size, const char *name)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // This structure describes an instrumented global variable.
  struct __asan_global {
    size_t beg;                // The address of the global.
    size_t size;               // The original size of the global.
    size_t size_with_redzone;  // The size with the redzone.
    const char *name;          // Name as a C string.
  };

  // These two functions should be called by the instrumented code.
  // 'globals' is an array of structures describing 'n' globals.
  void __asan_register_globals(__asan_global *globals, size_t n)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  void __asan_unregister_globals(__asan_global *globals, size_t n)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // These two functions are used by the instrumented code in the
  // use-after-return mode. __asan_stack_malloc allocates size bytes of
  // fake stack and __asan_stack_free poisons it. real_stack is a pointer to
  // the real stack region.
  size_t __asan_stack_malloc(size_t size, size_t real_stack)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  void __asan_stack_free(size_t ptr, size_t size, size_t real_stack)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // Marks memory region [addr, addr+size) as unaddressable.
  // This memory must be previously allocated by the user program. Accessing
  // addresses in this region from instrumented code is forbidden until
  // this region is unpoisoned. This function is not guaranteed to poison
  // the whole region - it may poison only subregion of [addr, addr+size) due
  // to ASan alignment restrictions.
  // Method is NOT thread-safe in the sense that no two threads can
  // (un)poison memory in the same memory region simultaneously.
  void __asan_poison_memory_region(void const volatile *addr, size_t size)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Marks memory region [addr, addr+size) as addressable.
  // This memory must be previously allocated by the user program. Accessing
  // addresses in this region is allowed until this region is poisoned again.
  // This function may unpoison a superregion of [addr, addr+size) due to
  // ASan alignment restrictions.
  // Method is NOT thread-safe in the sense that no two threads can
  // (un)poison memory in the same memory region simultaneously.
  void __asan_unpoison_memory_region(void const volatile *addr, size_t size)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // Performs cleanup before a NoReturn function. Must be called before things
  // like _exit and execl to avoid false positives on stack.
  void __asan_handle_no_return() ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

// User code should use macro instead of functions.
#if defined(__has_feature) && __has_feature(address_sanitizer)
#define ASAN_POISON_MEMORY_REGION(addr, size) \
  __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) \
  __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) \
  ((void)(addr), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) \
  ((void)(addr), (void)(size))
#endif

  // Returns true iff addr is poisoned (i.e. 1-byte read/write access to this
  // address will result in error report from AddressSanitizer).
  bool __asan_address_is_poisoned(void const volatile *addr)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // This is an internal function that is called to report an error.
  // However it is still a part of the interface because users may want to
  // set a breakpoint on this function in a debugger.
  void __asan_report_error(uintptr_t pc, uintptr_t bp, uintptr_t sp,
                           uintptr_t addr, bool is_write, size_t access_size)
    ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // Sets the exit code to use when reporting an error.
  // Returns the old value.
  int __asan_set_error_exit_code(int exit_code)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // Sets the callback to be called right before death on error.
  // Passing NULL will unset the callback.
  void __asan_set_death_callback(void (*callback)(void))
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;

  // Returns the estimated number of bytes that will be reserved by allocator
  // for request of "size" bytes. If ASan allocator can't allocate that much
  // memory, returns the maximal possible allocation size, otherwise returns
  // "size".
  size_t __asan_get_estimated_allocated_size(size_t size)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Returns true if p was returned by the ASan allocator and
  // is not yet freed.
  bool __asan_get_ownership(const void *p)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Returns the number of bytes reserved for the pointer p.
  // Requires (get_ownership(p) == true) or (p == NULL).
  size_t __asan_get_allocated_size(const void *p)
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Number of bytes, allocated and not yet freed by the application.
  size_t __asan_get_current_allocated_bytes()
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Number of bytes, mmaped by asan allocator to fulfill allocation requests.
  // Generally, for request of X bytes, allocator can reserve and add to free
  // lists a large number of chunks of size X to use them for future requests.
  // All these chunks count toward the heap size. Currently, allocator never
  // releases memory to OS (instead, it just puts freed chunks to free lists).
  size_t __asan_get_heap_size()
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Number of bytes, mmaped by asan allocator, which can be used to fulfill
  // allocation requests. When a user program frees memory chunk, it can first
  // fall into quarantine and will count toward __asan_get_free_bytes() later.
  size_t __asan_get_free_bytes()
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Number of bytes in unmapped pages, that are released to OS. Currently,
  // always returns 0.
  size_t __asan_get_unmapped_bytes()
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
  // Prints accumulated stats to stderr. Used for debugging.
  void __asan_print_accumulated_stats()
      ASAN_INTERFACE_FUNCTION_ATTRIBUTE;
}  // namespace

#undef ASAN_INTERFACE_FUNCTION_ATTRIBUTE
#endif  // ASAN_INTERFACE_H
