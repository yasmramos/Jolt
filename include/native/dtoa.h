#pragma once

#include <cstddef>
#include <cstdint>

#include "native/codegen.h"

// Freestanding float-to-text for the AOT runtime (PHASE 7c.1d.3).
//
// The hosted engine prints a float/double with the shortest decimal text that
// round-trips through the value's type (`std::to_chars`), and the AOT runtime
// must reproduce it exactly, because `print` is observable output and the two
// engines share semantics (see `native/value.h`). That is a "shortest
// round-trip" dtoa: 17 digits always suffice, but the *shortest* text needs the
// exact value, so this is a Dragon4 (Steele & White free-format) built on a
// small fixed bignum -- no libc, no `printf`, no table of powers of ten.
//
// The algorithm was prototyped and validated in C++ against `std::to_chars`
// over millions of random values (both binary64 and binary32) before being
// emitted, so the emitted code only has to match a known-correct reference.
namespace dtoa {

// A bignum used by the digit generator: an 8-byte limb count followed by
// 32-bit limbs, each stored in an 8-byte slot, so a limb is always a full
// machine word and the multiply can use a 64-bit `imul` whose low half is the
// exact 32x32 product. 48 limbs is comfortably above the 34 the largest double
// needs, leaving room for the intermediate `r*10` and the fix-up scaling.
constexpr int kLimbs = 48;

struct Bignum {
  std::int64_t n;
  std::uint64_t w[kLimbs];
};

// Scratch block for one conversion. Declared as a struct so the emitter uses
// `offsetof` for every field instead of hand-counted byte offsets, which is
// how a layout bug would otherwise slip in silently.
struct Scratch {
  Bignum r, s, mp, mm, t;  // Dragon4 state: value, scale, upper/lower margins
  char digits[64];         // significant digits, ASCII '0'..'9'
  char text[400];          // the fixed form is built here
  char text2[400];         // the scientific form / exact digits are built here
  std::int64_t nd;         // number of significant digits
  std::int64_t k;          // decimal exponent: value = 0.d0d1.. * 10^k
  std::int64_t f;          // input mantissa (> 0)
  std::int64_t e;          // input binary exponent
  std::int64_t p;          // input precision in bits (53 or 24)
  std::int64_t p2;         // 1 << (p-1): the boundary mantissa for `boundary`
  std::int64_t minE;       // input minimum exponent (-1074 or -149)
  std::int64_t sign;       // 1 when the value is negative
  std::int64_t boundary;   // 1 when the mantissa sits on a power-of-two boundary
  std::int64_t oddM;       // 1 when the mantissa is odd (strict rounding)
  std::int64_t low;        // Dragon4 stop test: remainder below the lower margin
  std::int64_t high;       // Dragon4 stop test: remainder above the upper margin
  std::int64_t outLen;     // length of the chosen body, before the sign
  std::int64_t d;          // current digit / loop counter
  std::int64_t pos;        // output cursor while building a form
  std::int64_t tmp;        // generic temporary (rounding decision, etc.)
  void* outp;              // caller's output buffer (kept across helper calls)
};

// Size of one Scratch block. A caller (the runtime's `print`, or a test) owns
// the block; `formatDouble` receives it as its fourth argument.
constexpr std::size_t kScratchBytes = sizeof(Scratch);

// Labels for the bignum subroutines and the generator, so a test can exercise
// each one in isolation (see `tests/dtoa_test.cpp`) instead of only through
// the formatter. All helpers take their arguments in rdi/rsi and clobber
// rax, rcx, rdx, rsi, rdi, r8-r11; rbx (the scratch base) and r12-r15 (the
// arena registers) are preserved.
struct DtoaHandles {
  CodeGen::Label bnSetU64;    // void bn_set_u64(Bignum* bn, long value)
  CodeGen::Label bnSetU32;    // void bn_set_u32(Bignum* bn, long value)
  CodeGen::Label bnCopy;      // void bn_copy(Bignum* dst, const Bignum* src)
  CodeGen::Label bnCmp;       // long bn_cmp(const Bignum* a, const Bignum* b)
  CodeGen::Label bnMulSmall;  // void bn_mul_small(Bignum* bn, long m)
  CodeGen::Label bnShl;       // void bn_shl(Bignum* bn, long bits)
  CodeGen::Label bnSub;       // void bn_sub(Bignum* a, const Bignum* b)
  CodeGen::Label bnAdd;       // void bn_add(Bignum* a, const Bignum* b)
  CodeGen::Label bnMulPow10;  // void bn_mul_pow10(Bignum* bn, long k)
  CodeGen::Label bnDivMod;    // long bn_divmod_small(Bignum* bn, long divisor)
  CodeGen::Label generate;    // void generate(Scratch* s): fills digits/nd/k
  CodeGen::Label formatDouble;  // long jolt_format_double(long tag, long payload, char* out)
};

inline DtoaHandles newDtoaHandles(CodeGen& code) {
  DtoaHandles h;
  h.bnSetU64 = code.newLabel();
  h.bnSetU32 = code.newLabel();
  h.bnCopy = code.newLabel();
  h.bnCmp = code.newLabel();
  h.bnMulSmall = code.newLabel();
  h.bnShl = code.newLabel();
  h.bnSub = code.newLabel();
  h.bnAdd = code.newLabel();
  h.bnMulPow10 = code.newLabel();
  h.bnDivMod = code.newLabel();
  h.generate = code.newLabel();
  h.formatDouble = code.newLabel();
  return h;
}

// Emits every bignum helper plus the Dragon4 generator and the formatter, and
// binds `handles`. `formatDouble` has the signature
//
//   long jolt_format_double(long tag, long payload, char* out)
//
// `tag`/`payload` are an `AotValue` (a Float or Double tag with the IEEE-754
// bit pattern as payload). The text is written to `out` without a NUL
// terminator and the byte length is returned, so the caller writes it with one
// `jolt_write`. The function makes calls, so it preserves rbx, rbp and r12-r15.
void emitDtoa(CodeGen& code, const DtoaHandles& handles);

}  // namespace dtoa
