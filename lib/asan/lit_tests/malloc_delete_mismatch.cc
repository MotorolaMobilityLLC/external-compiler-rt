// Check that we detect malloc/delete mismatch only if the approptiate flag
// is set.

// RUN: %clangxx_asan -g %s -o %t 2>&1
// RUN: ASAN_OPTIONS=alloc_dealloc_mismatch=1 %t 2>&1 | \
// RUN: %symbolize | FileCheck %s

// No error here.
// RUN: ASAN_OPTIONS=alloc_dealloc_mismatch=0 %t
#include <stdlib.h>

static volatile char *x;

int main() {
  x = (char*)malloc(10);
  x[0] = 0;
  delete x;
}
// CHECK: ERROR: AddressSanitizer: alloc-dealloc-mismatch (malloc vs operator delete) on 0x
// CHECK-NEXT: #0{{.*}}operator delete
// CHECK-NEXT: #1{{.*}}main
// CHECK: is located 0 bytes inside of 10-byte region
// CHECK-NEXT: allocated by thread T0 here:
// CHECK-NEXT: #0{{.*}}malloc
// CHECK-NEXT: #1{{.*}}main
// CHECK: HINT: if you don't care about these warnings you may set ASAN_OPTIONS=alloc_dealloc_mismatch=0
