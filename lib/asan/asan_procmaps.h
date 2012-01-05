//===-- asan_process.h ------------------------------------------*- C++ -*-===//
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
// Information about the process mappings.
//===----------------------------------------------------------------------===//
#ifndef ASAN_PROCMAPS_H
#define ASAN_PROCMAPS_H

#include "asan_internal.h"

namespace __asan {

class AsanProcMaps {
 public:
  AsanProcMaps();
  bool Next(uint64_t *start, uint64_t *end, uint64_t *offset,
            char filename[], size_t filename_size);
  void Reset();
  ~AsanProcMaps();
 private:
#if defined __linux__
  char *proc_self_maps_buff_;
  size_t proc_self_maps_buff_mmaped_size_;
  size_t proc_self_maps_buff_len_;
  char *current_;
#elif defined __APPLE__
// FIXME: Mac code goes here
#endif
};

}  // namespace __asan

#endif  // ASAN_PROCMAPS_H
