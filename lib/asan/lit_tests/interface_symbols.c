// Check the presense of interface symbols in compiled file.

// RUN: %clang -fsanitize=address -dead_strip -O2 %s -o %t.exe
// RUN: nm %t.exe | grep " T " | sed "s/.* T //" \
// RUN:    | grep "__asan_" | sed "s/___asan_/__asan_/" \
// RUN:    | grep -v "__asan_malloc_hook" \
// RUN:    | grep -v "__asan_free_hook" \
// RUN:    | grep -v "__asan_symbolize" \
// RUN:    | grep -v "__asan_default_options" \
// RUN:    | grep -v "__asan_on_error" > %t.symbols
// RUN: cat %p/../../../include/sanitizer/asan_interface.h \
// RUN:    | sed "s/\/\/.*//" | sed "s/typedef.*//" \
// RUN:    | grep -v "OPTIONAL" \
// RUN:    | grep "__asan_.*(" | sed "s/.* __asan_/__asan_/;s/(.*//" \
// RUN:    > %t.interface
// RUN: echo __asan_report_load1 >> %t.interface
// RUN: echo __asan_report_load2 >> %t.interface
// RUN: echo __asan_report_load4 >> %t.interface
// RUN: echo __asan_report_load8 >> %t.interface
// RUN: echo __asan_report_load16 >> %t.interface
// RUN: echo __asan_report_store1 >> %t.interface
// RUN: echo __asan_report_store2 >> %t.interface
// RUN: echo __asan_report_store4 >> %t.interface
// RUN: echo __asan_report_store8 >> %t.interface
// RUN: echo __asan_report_store16 >> %t.interface
// RUN: cat %t.interface | sort -u | diff %t.symbols -

int main() { return 0; }
