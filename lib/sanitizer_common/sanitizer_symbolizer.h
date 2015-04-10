//===-- sanitizer_symbolizer.h ----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Symbolizer is used by sanitizers to map instruction address to a location in
// source code at run-time. Symbolizer either uses __sanitizer_symbolize_*
// defined in the program, or (if they are missing) tries to find and
// launch "llvm-symbolizer" commandline tool in a separate process and
// communicate with it.
//
// Generally we should try to avoid calling system library functions during
// symbolization (and use their replacements from sanitizer_libc.h instead).
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_SYMBOLIZER_H
#define SANITIZER_SYMBOLIZER_H

#include "sanitizer_common.h"
#include "sanitizer_mutex.h"

namespace __sanitizer {

struct AddressInfo {
  // Owns all the string members. Storage for them is
  // (de)allocated using sanitizer internal allocator.
  uptr address;

  char *module;
  uptr module_offset;

  static const uptr kUnknown = ~(uptr)0;
  char *function;
  uptr function_offset;

  char *file;
  int line;
  int column;

  AddressInfo();
  // Deletes all strings and resets all fields.
  void Clear();
  void FillModuleInfo(const char *mod_name, uptr mod_offset);
};

// Linked list of symbolized frames (each frame is described by AddressInfo).
struct SymbolizedStack {
  SymbolizedStack *next;
  AddressInfo info;
  static SymbolizedStack *New(uptr addr);
  // Deletes current, and all subsequent frames in the linked list.
  // The object cannot be accessed after the call to this function.
  void ClearAll();

 private:
  SymbolizedStack();
};

// For now, DataInfo is used to describe global variable.
struct DataInfo {
  // Owns all the string members. Storage for them is
  // (de)allocated using sanitizer internal allocator.
  char *module;
  uptr module_offset;
  char *name;
  uptr start;
  uptr size;

  DataInfo();
  void Clear();
};

class SymbolizerTool;

class Symbolizer {
 public:
  /// Initialize and return platform-specific implementation of symbolizer
  /// (if it wasn't already initialized).
  static Symbolizer *GetOrInit();
  // Returns a list of symbolized frames for a given address (containing
  // all inlined functions, if necessary).
  SymbolizedStack *SymbolizePC(uptr address);
  bool SymbolizeData(uptr address, DataInfo *info);
  bool GetModuleNameAndOffsetForPC(uptr pc, const char **module_name,
                                   uptr *module_address);
  const char *GetModuleNameForPc(uptr pc) {
    const char *module_name = 0;
    uptr unused;
    if (GetModuleNameAndOffsetForPC(pc, &module_name, &unused))
      return module_name;
    return nullptr;
  }
  // Release internal caches (if any).
  void Flush();
  // Attempts to demangle the provided C++ mangled name.
  const char *Demangle(const char *name);
  void PrepareForSandboxing();

  // Allow user to install hooks that would be called before/after Symbolizer
  // does the actual file/line info fetching. Specific sanitizers may need this
  // to distinguish system library calls made in user code from calls made
  // during in-process symbolization.
  typedef void (*StartSymbolizationHook)();
  typedef void (*EndSymbolizationHook)();
  // May be called at most once.
  void AddHooks(StartSymbolizationHook start_hook,
                EndSymbolizationHook end_hook);

 private:
  /// Platform-specific function for creating a Symbolizer object.
  static Symbolizer *PlatformInit();

  virtual bool PlatformFindModuleNameAndOffsetForAddress(
      uptr address, const char **module_name, uptr *module_offset) {
    UNIMPLEMENTED();
  }
  // Platform-specific default demangler, must not return nullptr.
  virtual const char *PlatformDemangle(const char *name) { UNIMPLEMENTED(); }
  virtual void PlatformPrepareForSandboxing() { UNIMPLEMENTED(); }

  static Symbolizer *symbolizer_;
  static StaticSpinMutex init_mu_;

  // Mutex locked from public methods of |Symbolizer|, so that the internals
  // (including individual symbolizer tools and platform-specific methods) are
  // always synchronized.
  BlockingMutex mu_;

  typedef IntrusiveList<SymbolizerTool>::Iterator Iterator;
  IntrusiveList<SymbolizerTool> tools_;

 protected:
  explicit Symbolizer(IntrusiveList<SymbolizerTool> tools);

  static LowLevelAllocator symbolizer_allocator_;

  StartSymbolizationHook start_hook_;
  EndSymbolizationHook end_hook_;
  class SymbolizerScope {
   public:
    explicit SymbolizerScope(const Symbolizer *sym);
    ~SymbolizerScope();
   private:
    const Symbolizer *sym_;
  };
};

}  // namespace __sanitizer

#endif  // SANITIZER_SYMBOLIZER_H
