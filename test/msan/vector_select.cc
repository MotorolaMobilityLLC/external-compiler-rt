// RUN: %clangxx_msan -m64 -O0 %s -c -o %t
// RUN: %clangxx_msan -m64 -O3 %s -c -o %t

// Regression test for MemorySanitizer instrumentation of a select instruction
// with vector arguments.

#if defined(__x86_64__)
#include <emmintrin.h>

__m128d select(bool b, __m128d c, __m128d d)
{
  return b ? c : d;
}
#elif defined (__mips64)
typedef double __w64d __attribute__ ((vector_size(16)));

__w64d select(bool b, __w64d c, __w64d d)
{
  return b ? c : d;
}
#endif
