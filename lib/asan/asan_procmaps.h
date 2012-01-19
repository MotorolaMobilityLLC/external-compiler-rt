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
#if defined __APPLE__
#include <mach-o/loader.h>
#endif

namespace __asan {

class AsanProcMaps {
 public:
  AsanProcMaps();
  bool Next(uintptr_t *start, uintptr_t *end, uintptr_t *offset,
            char filename[], size_t filename_size);
  void Reset();
  // Gets the object file name and the offset in that object for a given
  // address 'addr'. Returns true on success.
  bool GetObjectNameAndOffset(uintptr_t addr, uintptr_t *offset,
                              char filename[], size_t filename_size);
  ~AsanProcMaps();
 private:
#if defined __APPLE__
  template<uint32_t kLCSegment, typename SegmentCommand>
  bool NextSegmentLoad(uintptr_t *start, uintptr_t *end, uintptr_t *offset,
                       char filename[], size_t filename_size);
#endif
  // Default implementation of GetObjectNameAndOffset.
  // Quite slow, because it iterates through the whole process map for each
  // lookup.
  bool IterateForObjectNameAndOffset(uintptr_t addr, uintptr_t *offset,
                                     char filename[], size_t filename_size) {
    Reset();
    uintptr_t start, end, file_offset;
    while (Next(&start, &end, &file_offset,
                filename, filename_size)) {
      if (addr >= start && addr < end) {
        *offset = (addr - start) + file_offset;
        return true;
      }
    }
    if (filename_size)
      filename[0] = '\0';
    return false;
  }

#if defined __linux__
  char *proc_self_maps_buff_;
  size_t proc_self_maps_buff_mmaped_size_;
  size_t proc_self_maps_buff_len_;
  char *current_;
#elif defined __APPLE__
  int current_image_;
  int current_load_cmd_count_;
  char *current_load_cmd_addr_;
#endif
};

}  // namespace __asan

#endif  // ASAN_PROCMAPS_H
