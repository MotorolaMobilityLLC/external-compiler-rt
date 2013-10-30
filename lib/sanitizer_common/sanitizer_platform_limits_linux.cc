//===-- sanitizer_platform_limits_linux.cc --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of Sanitizer common code.
//
// Sizes and layouts of linux kernel data structures.
//===----------------------------------------------------------------------===//

// This is a separate compilation unit for linux headers that conflict with
// userspace headers.
// Most "normal" includes go in sanitizer_platform_limits_posix.cc

#include "sanitizer_platform.h"
#if SANITIZER_LINUX

// This header seems to contain the definitions of _kernel_ stat* structs.
#include <asm/stat.h>
#include <linux/aio_abi.h>

#if SANITIZER_ANDROID
#include <asm/statfs.h>
#else
#include <sys/statfs.h>
#endif

#if !SANITIZER_ANDROID
#include <linux/perf_event.h>
#endif

namespace __sanitizer {
  unsigned struct___old_kernel_stat_sz = sizeof(struct __old_kernel_stat);
  unsigned struct_kernel_stat_sz = sizeof(struct stat);
  unsigned struct_io_event_sz = sizeof(struct io_event);
  unsigned struct_iocb_sz = sizeof(struct iocb);
  unsigned struct_statfs64_sz = sizeof(struct statfs64);

#ifndef _LP64
  unsigned struct_kernel_stat64_sz = sizeof(struct stat64);
#else
  unsigned struct_kernel_stat64_sz = 0;
#endif

#if !SANITIZER_ANDROID
  unsigned struct_perf_event_attr_sz = sizeof(struct perf_event_attr);
#endif
}  // namespace __sanitizer

#endif  // SANITIZER_LINUX
