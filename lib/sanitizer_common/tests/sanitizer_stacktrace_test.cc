//===-- sanitizer_stacktrace_test.cc --------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer/AddressSanitizer runtime.
//
//===----------------------------------------------------------------------===//

#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "gtest/gtest.h"

namespace __sanitizer {

class FastUnwindTest : public ::testing::Test {
 protected:
  virtual void SetUp();

  uptr fake_stack[10];
  uptr start_pc;
  uptr fake_top;
  uptr fake_bottom;
  StackTrace trace;
};

void FastUnwindTest::SetUp() {
  // Fill an array of pointers with fake fp+retaddr pairs.  Frame pointers have
  // even indices.
  for (int i = 0; i+1 < ARRAY_SIZE(fake_stack); i += 2) {
    fake_stack[i] = (uptr)&fake_stack[i+2];  // fp
    fake_stack[i+1] = i+1; // retaddr
  }
  // Mark the last fp as zero to terminate the stack trace.
  fake_stack[RoundDownTo(ARRAY_SIZE(fake_stack) - 1, 2)] = 0;

  // Top is two slots past the end because FastUnwindStack subtracts two.
  fake_top = (uptr)&fake_stack[ARRAY_SIZE(fake_stack) + 2];
  // Bottom is one slot before the start because FastUnwindStack uses >.
  fake_bottom = (uptr)&fake_stack[-1];
  start_pc = 0;

  // This is common setup done by __asan::GetStackTrace().
  trace.size = 0;
  trace.max_size = ARRAY_SIZE(fake_stack);
  trace.trace[0] = start_pc;
}

TEST_F(FastUnwindTest, Basic) {
  trace.FastUnwindStack(start_pc, (uptr)&fake_stack[0],
                        fake_top, fake_bottom);
  // Should get all on-stack retaddrs and start_pc.
  EXPECT_EQ(6, trace.size);
  EXPECT_EQ(start_pc, trace.trace[0]);
  for (int i = 1; i <= 5; i++) {
    EXPECT_EQ(i*2 - 1, trace.trace[i]);
  }
}

// From: http://code.google.com/p/address-sanitizer/issues/detail?id=162
TEST_F(FastUnwindTest, FramePointerLoop) {
  // Make one fp point to itself.
  fake_stack[4] = (uptr)&fake_stack[4];
  trace.FastUnwindStack(start_pc, (uptr)&fake_stack[0],
                        fake_top, fake_bottom);
  // Should get all on-stack retaddrs up to the 4th slot and start_pc.
  EXPECT_EQ(4, trace.size);
  EXPECT_EQ(start_pc, trace.trace[0]);
  for (int i = 1; i <= 3; i++) {
    EXPECT_EQ(i*2 - 1, trace.trace[i]);
  }
}

}  // namespace __sanitizer
