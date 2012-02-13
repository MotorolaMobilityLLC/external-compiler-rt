#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  char *x = (char*)malloc(10 * sizeof(char));
  memset(x, 0, 10);
  int res = x[argc * 10];  // BOOOM
  free(x);
  return res;
}

// CHECK: {{READ of size 1 at 0x.* thread T0}}
// CHECK: {{    #0 0x.* in main .*heap-overflow.cc:6}}
// CHECK: {{0x.* is located 0 bytes to the right of 10-byte region}}
// CHECK: {{allocated by thread T0 here:}}
// CHECK: {{    #0 0x.* in malloc}}
// CHECK: {{    #1 0x.* in main .*heap-overflow.cc:[45]}}

// Darwin: {{READ of size 1 at 0x.* thread T0}}
// Darwin: {{    #0 0x.* in main .*heap-overflow.cc:6}}
// Darwin: {{0x.* is located 0 bytes to the right of 10-byte region}}
// Darwin: {{allocated by thread T0 here:}}
// Darwin: {{    #0 0x.* in .*mz_malloc.*}}
// Darwin: {{    #1 0x.* in malloc_zone_malloc.*}}
// Darwin: {{    #2 0x.* in malloc.*}}
// Darwin: {{    #3 0x.* in main heap-overflow.cc:[45]}}
