//===-- lib/floatunssidf.c - uint -> double-precision conversion --*- C -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements unsigned integer to double-precision conversion for the
// compiler-rt library in the IEEE-754 default round-to-nearest, ties-to-even
// mode.
//
//===----------------------------------------------------------------------===//

#define DOUBLE_PRECISION
#include "fp_lib.h"

fp_t __floatunssidf(unsigned int a) {
    
    const int aWidth = sizeof a * CHAR_BIT;
    
    // Handle zero as a special case to protect clz
    if (a == 0) return fromRep(0);
    
    // Exponent of (fp_t)a is the width of abs(a).
    const int exponent = (aWidth - 1) - __builtin_clz(a);
    rep_t result;
    
    // Shift a into the significand field, rounding if it is a right-shift
    if (exponent <= significandBits) {
        const int shift = significandBits - exponent;
        result = (rep_t)a << shift ^ implicitBit;
    } else {
        const int shift = exponent - significandBits;
        result = (rep_t)a >> shift ^ implicitBit;
        rep_t round = (rep_t)a << (typeWidth - shift);
        if (round > signBit) result++;
        if (round == signBit) result += result & 1;
    }
    
    // Insert the exponent
    result += (rep_t)(exponent + exponentBias) << significandBits;
    return fromRep(result);
}
