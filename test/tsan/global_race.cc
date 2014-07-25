// RUN: %clangxx_tsan -O1 %s -o %t && %deflake %run %t | FileCheck %s
#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>

int GlobalData[10];

void *Thread(void *a) {
  sleep(1);
  GlobalData[2] = 42;
  return 0;
}

int main() {
  fprintf(stderr, "addr=%p\n", GlobalData);
  pthread_t t;
  pthread_create(&t, 0, Thread, 0);
  GlobalData[2] = 43;
  pthread_join(t, 0);
}

// CHECK: addr=[[ADDR:0x[0-9,a-f]+]]
// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: Location is global 'GlobalData' of size 40 at [[ADDR]] ({{.*}}+0x{{[0-9,a-f]+}})

