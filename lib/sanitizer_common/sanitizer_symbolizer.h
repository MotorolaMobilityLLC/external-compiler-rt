//===-- sanitizer_symbolizer.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Symbolizer is intended to be used by both
// AddressSanitizer and ThreadSanitizer to symbolize a given
// address. It is an analogue of addr2line utility and allows to map
// instruction address to a location in source code at run-time.
//
// Symbolizer is planned to use debug information (in DWARF format)
// in a binary via interface defined in "llvm/DebugInfo/DIContext.h"
//
// Symbolizer code should be called from the run-time library of
// dynamic tools, and generally should not call memory allocation
// routines or other system library functions intercepted by those tools.
// Instead, Symbolizer code should use their replacements, defined in
// "compiler-rt/lib/sanitizer_common/sanitizer_libc.h".
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_SYMBOLIZER_H
#define SANITIZER_SYMBOLIZER_H

#include "sanitizer_internal_defs.h"
#include "sanitizer_libc.h"
// WARNING: Do not include system headers here. See details above.

namespace __sanitizer {

struct AddressInfo {
  uptr address;
  char *module;
  uptr module_offset;
  char *function;
  char *file;
  int line;
  int column;

  AddressInfo() {
    internal_memset(this, 0, sizeof(AddressInfo));
  }
  // Deletes all strings and sets all fields to zero.
  void Clear();
};

// Fills at most "max_frames" elements of "frames" with descriptions
// for a given address (in all inlined functions). Returns the number
// of descriptions actually filled.
// This function should NOT be called from two threads simultaneously.
uptr SymbolizeCode(uptr address, AddressInfo *frames, uptr max_frames);

// Debug info routines
struct DWARFSection {
  const char *data;
  uptr size;
};
// Returns true on success.
bool FindDWARFSection(uptr object_file_addr, const char *section_name,
                      DWARFSection *section);
bool IsFullNameOfDWARFSection(const char *full_name, const char *short_name);

class ModuleDIContext {
 public:
  explicit ModuleDIContext(const char *module_name);
  void addAddressRange(uptr beg, uptr end);
  bool containsAddress(uptr address) const;
  void getAddressInfo(AddressInfo *info);

  const char *full_name() const { return full_name_; }

 private:
  void CreateDIContext();

  struct AddressRange {
    uptr beg;
    uptr end;
  };
  char *full_name_;
  char *short_name_;
  uptr base_address_;
  static const uptr kMaxNumberOfAddressRanges = 16;
  AddressRange ranges_[kMaxNumberOfAddressRanges];
  uptr n_ranges_;
  uptr mapped_addr_;
  uptr mapped_size_;
};

// OS-dependent function that gets the linked list of all loaded modules.
uptr GetListOfModules(ModuleDIContext *modules, uptr max_modules);

}  // namespace __sanitizer

#endif  // SANITIZER_SYMBOLIZER_H
