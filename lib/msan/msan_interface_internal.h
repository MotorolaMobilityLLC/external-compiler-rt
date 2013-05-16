//===-- msan_interface_internal.h -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of MemorySanitizer.
//
// Private MSan interface header.
//===----------------------------------------------------------------------===//

#ifndef MSAN_INTERFACE_INTERNAL_H
#define MSAN_INTERFACE_INTERNAL_H

#include "sanitizer_common/sanitizer_internal_defs.h"

extern "C" {
// FIXME: document all interface functions.

SANITIZER_INTERFACE_ATTRIBUTE
int __msan_get_track_origins();

SANITIZER_INTERFACE_ATTRIBUTE
void __msan_init();

// Print a warning and maybe return.
// This function can die based on flags()->exit_code.
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_warning();

// Print a warning and die.
// Intrumentation inserts calls to this function when building in "fast" mode
// (i.e. -mllvm -msan-keep-going)
SANITIZER_INTERFACE_ATTRIBUTE __attribute__((noreturn))
void __msan_warning_noreturn();

SANITIZER_INTERFACE_ATTRIBUTE
void __msan_unpoison(const void *a, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_clear_and_unpoison(void *a, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void* __msan_memcpy(void *dst, const void *src, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void* __msan_memset(void *s, int c, uptr n);
SANITIZER_INTERFACE_ATTRIBUTE
void* __msan_memmove(void* dest, const void* src, uptr n);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_copy_poison(void *dst, const void *src, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_copy_origin(void *dst, const void *src, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_move_poison(void *dst, const void *src, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_poison(const void *a, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_poison_stack(void *a, uptr size);

// Copy size bytes from src to dst and unpoison the result.
// Useful to implement unsafe loads.
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_load_unpoisoned(void *src, uptr size, void *dst);

// Returns the offset of the first (at least partially) poisoned byte,
// or -1 if the whole range is good.
SANITIZER_INTERFACE_ATTRIBUTE
sptr __msan_test_shadow(const void *x, uptr size);

SANITIZER_INTERFACE_ATTRIBUTE
void __msan_set_origin(const void *a, uptr size, u32 origin);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_set_alloca_origin(void *a, uptr size, const char *descr);
SANITIZER_INTERFACE_ATTRIBUTE
u32 __msan_get_origin(const void *a);

SANITIZER_INTERFACE_ATTRIBUTE
void __msan_clear_on_return();

// Default: -1 (don't exit on error).
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_set_exit_code(int exit_code);

SANITIZER_INTERFACE_ATTRIBUTE
int __msan_set_poison_in_malloc(int do_poison);

SANITIZER_WEAK_ATTRIBUTE SANITIZER_INTERFACE_ATTRIBUTE
/* OPTIONAL */ const char* __msan_default_options();

// For testing.
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_set_expect_umr(int expect_umr);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_print_shadow(const void *x, uptr size);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_print_param_shadow();
SANITIZER_INTERFACE_ATTRIBUTE
int  __msan_has_dynamic_component();

// Returns x such that %fs:x is the first byte of __msan_retval_tls.
SANITIZER_INTERFACE_ATTRIBUTE
int __msan_get_retval_tls_offset();
SANITIZER_INTERFACE_ATTRIBUTE
int __msan_get_param_tls_offset();

// For intercepting mmap from ld.so in msandr.
SANITIZER_INTERFACE_ATTRIBUTE
bool __msan_is_in_loader();

// For testing.
SANITIZER_INTERFACE_ATTRIBUTE
u32 __msan_get_umr_origin();
SANITIZER_INTERFACE_ATTRIBUTE
const char *__msan_get_origin_descr_if_stack(u32 id);
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_partial_poison(const void* data, void* shadow, uptr size);

// Tell MSan about newly allocated memory (ex.: custom allocator).
// Memory will be marked uninitialized, with origin at the call site.
SANITIZER_INTERFACE_ATTRIBUTE
void __msan_allocated_memory(const void* data, uptr size);
}  // extern "C"

// Unpoison first n function arguments.
void __msan_unpoison_param(uptr n);

#endif  // MSAN_INTERFACE_INTERNAL_H
