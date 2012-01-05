//===-- asan_printf.cc ------------------------------------------*- C++ -*-===//
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
// Internal printf function, used inside ASan run-time library.
// We can't use libc printf because we intercept some of the functions used
// inside it.
//===----------------------------------------------------------------------===//

#include "asan_internal.h"
#include "asan_interceptors.h"

#include <stdarg.h>
#include <stdio.h>

namespace __asan {

void RawWrite(const char *buffer) {
  static const char *kRawWriteError = "RawWrite can't output requested buffer!";
  ssize_t length = (ssize_t)internal_strlen(buffer);
  if (length != AsanWrite(2, buffer, length)) {
    AsanWrite(2, kRawWriteError, internal_strlen(kRawWriteError));
    ASAN_DIE;
  }
}

static inline int AppendChar(char **buff, const char *buff_end, char c) {
  if (*buff < buff_end) {
    **buff = c;
    (*buff)++;
  }
  return 1;
}

// Appends number in a given base to buffer. If its length is less than
// "minimal_num_length", it is padded with leading zeroes.
static int AppendUnsigned(char **buff, const char *buff_end, uint64_t num,
                          uint8_t base, uint8_t minimal_num_length) {
  size_t const kMaxLen = 30;
  RAW_CHECK(base == 10 || base == 16);
  RAW_CHECK(minimal_num_length < kMaxLen);
  size_t num_buffer[kMaxLen];
  size_t pos = 0;
  do {
    RAW_CHECK_MSG(pos < kMaxLen, "appendNumber buffer overflow");
    num_buffer[pos++] = num % base;
    num /= base;
  } while (num > 0);
  while (pos < minimal_num_length) num_buffer[pos++] = 0;
  int result = 0;
  while (pos-- > 0) {
    size_t digit = num_buffer[pos];
    result += AppendChar(buff, buff_end, (digit < 10) ? '0' + digit
                                                      : 'a' + digit - 10);
  }
  return result;
}

static inline int AppendSignedDecimal(char **buff, const char *buff_end,
                                      int64_t num) {
  int result = 0;
  if (num < 0) {
    result += AppendChar(buff, buff_end, '-');
    num = -num;
  }
  result += AppendUnsigned(buff, buff_end, (uint64_t)num, 10, 0);
  return result;
}

static inline int AppendString(char **buff, const char *buff_end,
                               const char *s) {
  // Avoid library functions like stpcpy here.
  RAW_CHECK(s);
  int result = 0;
  for (; *s; s++) {
    result += AppendChar(buff, buff_end, *s);
  }
  return result;
}

static inline int AppendPointer(char **buff, const char *buff_end,
                                uint64_t ptr_value) {
  int result = 0;
  result += AppendString(buff, buff_end, "0x");
  result += AppendUnsigned(buff, buff_end, ptr_value, 16,
                           (__WORDSIZE == 64) ? 12 : 8);
  return result;
}

static int VSNPrintf(char *buff, int buff_length,
                     const char *format, va_list args) {
  static const char *kPrintfFormatsHelp = "Supported Printf formats: "
                                          "%%[l]{d,u,x}; %%p; %%s";
  RAW_CHECK(format);
  RAW_CHECK(buff_length > 0);
  const char *buff_end = &buff[buff_length - 1];
  const char *cur = format;
  int result = 0;
  for (; *cur; cur++) {
    if (*cur == '%') {
      cur++;
      bool have_l = (*cur == 'l');
      cur += have_l;
      int64_t dval;
      uint64_t uval, xval;
      switch (*cur) {
        case 'd': dval = have_l ? va_arg(args, intptr_t)
                                : va_arg(args, int);
                  result += AppendSignedDecimal(&buff, buff_end, dval);
                  break;
        case 'u': uval = have_l ? va_arg(args, uintptr_t)
                                : va_arg(args, unsigned int);
                  result += AppendUnsigned(&buff, buff_end, uval, 10, 0);
                  break;
        case 'x': xval = have_l ? va_arg(args, uintptr_t)
                                : va_arg(args, unsigned int);
                  result += AppendUnsigned(&buff, buff_end, xval, 16, 0);
                  break;
        case 'p': RAW_CHECK_MSG(!have_l, kPrintfFormatsHelp);
                  result += AppendPointer(&buff, buff_end,
                                          va_arg(args, uintptr_t));
                  break;
        case 's': RAW_CHECK_MSG(!have_l, kPrintfFormatsHelp);
                  result += AppendString(&buff, buff_end, va_arg(args, char*));
                  break;
        default:  RAW_CHECK_MSG(false, kPrintfFormatsHelp);
      }
    } else {
      result += AppendChar(&buff, buff_end, *cur);
    }
  }
  RAW_CHECK(buff <= buff_end);
  AppendChar(&buff, buff_end + 1, '\0');
  return result;
}

void Printf(const char *format, ...) {
  const int kLen = 1024 * 4;
  char buffer[kLen];
  va_list args;
  va_start(args, format);
  int needed_length = VSNPrintf(buffer, kLen, format, args);
  va_end(args);
  RAW_CHECK_MSG(needed_length < kLen, "Buffer in Printf is too short!\n");
  RawWrite(buffer);
}

// Writes at most "length" symbols to "buffer" (including trailing '\0').
// Returns the number of symbols that should have been written to buffer
// (not including trailing '\0'). Thus, the string is truncated
// iff return value is not less than "length".
int SNPrintf(char *buffer, size_t length, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int needed_length = VSNPrintf(buffer, length, format, args);
  va_end(args);
  return needed_length;
}

// Like Printf, but prints the current PID before the output string.
void Report(const char *format, ...) {
  const int kLen = 1024 * 4;
  char buffer[kLen];
  int needed_length = SNPrintf(buffer, kLen, "==%d== ", getpid());
  RAW_CHECK_MSG(needed_length < kLen, "Buffer in Report is too short!\n");
  va_list args;
  va_start(args, format);
  needed_length += VSNPrintf(buffer + needed_length, kLen - needed_length,
                             format, args);
  va_end(args);
  RAW_CHECK_MSG(needed_length < kLen, "Buffer in Report is too short!\n");
  RawWrite(buffer);
}

int SScanf(const char *str, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int res = vsscanf(str, format, args);
  va_end(args);
  return res;
}

}  // namespace __asan
