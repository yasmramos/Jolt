#include "native/dtoa.h"

#include <cstddef>

// Emitter for the freestanding float-to-text routine (PHASE 7c.1d.3).
//
// Register conventions. Every routine below clobbers rax, rcx, rdx, rsi, rdi
// and r8-r11, and preserves rbx, rbp and r12-r15 (rbx is the Scratch base,
// r12-r15 hold the entry prelude's arena cursor). Persistent state therefore
// lives in Scratch fields, reloaded after each helper call, never in a
// caller-saved register across a call.
//
// `formatDouble` is the only routine that calls the helpers; `generate` runs
// the Dragon4 loop; the `bn_*` helpers are pure bignum primitives, exposed as
// handles so a test can exercise each one in isolation.
namespace dtoa {
namespace {

using X64Reg = ::X64Reg;
using Jcc = ::Jcc;

constexpr X64Reg Rax = X64Reg::Rax;
constexpr X64Reg Rcx = X64Reg::Rcx;
constexpr X64Reg Rdx = X64Reg::Rdx;
constexpr X64Reg Rbx = X64Reg::Rbx;
constexpr X64Reg Rsp = X64Reg::Rsp;
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsi = X64Reg::Rsi;
constexpr X64Reg Rdi = X64Reg::Rdi;
constexpr X64Reg R8 = X64Reg::R8;
constexpr X64Reg R9 = X64Reg::R9;
constexpr X64Reg R10 = X64Reg::R10;
constexpr X64Reg R11 = X64Reg::R11;
constexpr X64Reg R12 = X64Reg::R12;
constexpr X64Reg R13 = X64Reg::R13;
constexpr int Xmm0 = kXmmBase + 0;

constexpr std::int64_t kStride = 8;
constexpr std::int64_t kWO = static_cast<std::int64_t>(offsetof(Bignum, w));

#define O(name) static_cast<std::int64_t>(offsetof(Scratch, name))
constexpr std::int64_t offR = O(r);
constexpr std::int64_t offS = O(s);
constexpr std::int64_t offMp = O(mp);
constexpr std::int64_t offMm = O(mm);
constexpr std::int64_t offT = O(t);
constexpr std::int64_t offDigits = O(digits);
constexpr std::int64_t offText = O(text);
constexpr std::int64_t offText2 = O(text2);
constexpr std::int64_t offNd = O(nd);
constexpr std::int64_t offK = O(k);
constexpr std::int64_t offF = O(f);
constexpr std::int64_t offE = O(e);
constexpr std::int64_t offP = O(p);
constexpr std::int64_t offP2 = O(p2);
constexpr std::int64_t offMinE = O(minE);
constexpr std::int64_t offSign = O(sign);
constexpr std::int64_t offBoundary = O(boundary);
constexpr std::int64_t offOddM = O(oddM);
constexpr std::int64_t offLow = O(low);
constexpr std::int64_t offHigh = O(high);
constexpr std::int64_t offOutLen = O(outLen);
constexpr std::int64_t offD = O(d);
constexpr std::int64_t offPos = O(pos);
constexpr std::int64_t offTmp = O(tmp);
constexpr std::int64_t offOutp = O(outp);
constexpr std::int64_t kScratchBytes = static_cast<std::int64_t>(sizeof(Scratch));
constexpr std::int64_t kScratchAligned = (kScratchBytes + 15) & ~static_cast<std::int64_t>(15);
#undef O

// Plain `ret` for the bignum helpers, which save no frame (the generator and
// the formatters build their own prologue and epilogue).
void ret(CodeGen& cg) { cg.retInsn(); }

// rdi = rbx + off (address of a Scratch field or a bignum).
void setRdi(CodeGen& cg, std::int64_t off) {
  cg.movReg(Rdi, Rbx);
  if (off != 0) cg.addImm32(Rdi, static_cast<std::int32_t>(off));
}

void setRsi(CodeGen& cg, std::int64_t off) {
  cg.movReg(Rsi, Rbx);
  if (off != 0) cg.addImm32(Rsi, static_cast<std::int32_t>(off));
}

void callTwo(CodeGen& cg, CodeGen::Label fn, std::int64_t offA, std::int64_t offB) {
  setRdi(cg, offA);
  setRsi(cg, offB);
  cg.callLabel(fn);
}

void callImm(CodeGen& cg, CodeGen::Label fn, std::int64_t offA, std::int64_t value) {
  setRdi(cg, offA);
  cg.movImm(Rsi, value);
  cg.callLabel(fn);
}

// rax = digits[rax] (rax is the digit index on entry and the digit on exit).
// `movzxFromMem` takes a base and a displacement only, so the address is
// materialised in rdx first.
void loadDigit(CodeGen& cg) {
  cg.movReg(Rdx, Rax);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movzxFromMem(Rax, Rdx, 0);
}

// rax = '0' + digits[rax]: the generator stores raw digit values (0-9), so the
// formatters convert to ASCII when they write to the text buffers.
void loadDigitAscii(CodeGen& cg) {
  loadDigit(cg);
  cg.addImm32(Rax, '0');
}

// digits[rcx] = rax (low byte).
void storeDigit(CodeGen& cg) {
  cg.movReg(Rdx, Rcx);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offDigits));
  cg.movByteToMem(Rdx, 0, Rax);
}

// --- Bignum helpers -------------------------------------------------------

// void bn_set_u64(Bignum* bn, long value)
void emitBnSetU64(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label setOne = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movImmToMem(Rdi, 0, 0);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, done);
  cg.movReg32(Rax, Rsi);
  cg.movToMem32(Rdi, kWO, Rax);
  cg.movReg(Rcx, Rsi);
  cg.shrImm8(Rcx, 32);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, setOne);
  cg.movToMem32(Rdi, kWO + kStride, Rcx);
  cg.movImmToMem(Rdi, 0, 2);
  cg.jmp(done);
  cg.bind(setOne);
  cg.movImmToMem(Rdi, 0, 1);
  cg.bind(done);
  ret(cg);
}

// void bn_set_u32(Bignum* bn, long value)
void emitBnSetU32(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movImmToMem(Rdi, 0, 0);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, done);
  cg.movToMem32(Rdi, kWO, Rsi);
  cg.movImmToMem(Rdi, 0, 1);
  cg.bind(done);
  ret(cg);
}

// void bn_copy(Bignum* dst, const Bignum* src)
void emitBnCopy(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movFromMem(Rax, Rsi, 0);
  cg.movToMem(Rdi, 0, Rax);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kWO));
  cg.addImm32(Rdi, static_cast<std::int32_t>(kWO));
  cg.xorReg(Rcx, Rcx);
  cg.bind(loop);
  cg.cmpReg(Rcx, Rax);
  cg.jcc(Jcc::Ae, done);
  cg.movFromMem32(R8, Rsi, 0);
  cg.movToMem32(Rdi, 0, R8);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rcx, 1);
  cg.jmp(loop);
  cg.bind(done);
  ret(cg);
}

// long bn_cmp(const Bignum* a, const Bignum* b) -> -1 / 0 / 1 in rax
void emitBnCmp(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label sameLen = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label greater = cg.newLabel();
  CodeGen::Label less = cg.newLabel();
  CodeGen::Label equal = cg.newLabel();
  cg.bind(label);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movFromMem(Rcx, Rsi, 0);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Eq, sameLen);
  cg.jcc(Jcc::A, greater);
  cg.movImm(Rax, -1);
  ret(cg);
  cg.bind(greater);
  cg.movImm(Rax, 1);
  ret(cg);
  cg.bind(sameLen);
  cg.movReg(R8, Rax);
  cg.bind(loop);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, equal);
  cg.subRegImm32(R8, 1);
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 3);
  cg.addImm32(R9, static_cast<std::int32_t>(kWO));
  cg.addReg(R9, Rdi);
  cg.movReg(R10, R8);
  cg.shlImm8(R10, 3);
  cg.addImm32(R10, static_cast<std::int32_t>(kWO));
  cg.addReg(R10, Rsi);
  cg.movFromMem32(R11, R9, 0);
  cg.movFromMem32(Rdx, R10, 0);
  cg.cmpReg(R11, Rdx);
  cg.jcc(Jcc::A, greater);
  cg.jcc(Jcc::B, less);
  cg.jmp(loop);
  cg.bind(less);
  cg.movImm(Rax, -1);
  ret(cg);
  cg.bind(equal);
  cg.xorReg(Rax, Rax);
  ret(cg);
}

// void bn_mul_small(Bignum* bn, long m): bn *= m, m in 32 bits.
void emitBnMulSmall(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label tail = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  cg.movReg(R11, Rdi);
  cg.movFromMem(R10, R11, 0);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kWO));
  cg.xorReg(Rcx, Rcx);
  cg.xorReg(R9, R9);
  cg.bind(loop);
  cg.cmpReg(Rcx, R10);
  cg.jcc(Jcc::Ae, tail);
  cg.movFromMem32(R8, Rdi, 0);
  cg.imulReg(R8, Rsi);
  cg.addReg(R8, R9);
  cg.movToMem32(Rdi, 0, R8);
  cg.movReg(R9, R8);
  cg.shrImm8(R9, 32);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rcx, 1);
  cg.jmp(loop);
  cg.bind(tail);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, done);
  cg.movToMem32(Rdi, 0, R9);
  cg.shrImm8(R9, 32);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStride));
  cg.addImm32(R10, 1);
  cg.jmp(tail);
  cg.bind(done);
  cg.movToMem(R11, 0, R10);
  ret(cg);
}

// void bn_shl(Bignum* bn, long bits): bn <<= bits, bits >= 0.
void emitBnShl(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label retL = cg.newLabel();
  CodeGen::Label moveLoop = cg.newLabel();
  CodeGen::Label moveDone = cg.newLabel();
  CodeGen::Label zeroLoop = cg.newLabel();
  CodeGen::Label zeroDone = cg.newLabel();
  CodeGen::Label remLoop = cg.newLabel();
  CodeGen::Label remDone = cg.newLabel();
  cg.bind(label);
  cg.movFromMem(Rax, Rdi, 0);  // n
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, retL);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, retL);
  cg.movReg(R8, Rsi);
  cg.shrImm8(R8, 5);  // limbs
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 5);
  cg.movReg(R10, Rsi);
  cg.subReg(R10, R9);  // rem
  // r11 = &w[0]; rdi stays the base.
  cg.movReg(R11, Rdi);
  cg.addImm32(R11, static_cast<std::int32_t>(kWO));
  cg.movReg(Rcx, Rax);
  cg.bind(moveLoop);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, moveDone);
  cg.subRegImm32(Rcx, 1);
  cg.movReg(Rdx, Rcx);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R11);  // src
  cg.movReg(R9, R8);
  cg.shlImm8(R9, 3);
  cg.addReg(R9, Rdx);  // dst
  cg.movFromMem32(Rsi, Rdx, 0);
  cg.movToMem32(R9, 0, Rsi);
  cg.jmp(moveLoop);
  cg.bind(moveDone);
  cg.addReg(Rax, R8);
  cg.movToMem(Rdi, 0, Rax);  // n += limbs
  cg.xorReg(Rcx, Rcx);
  cg.bind(zeroLoop);
  cg.cmpReg(Rcx, R8);
  cg.jcc(Jcc::Ae, zeroDone);
  cg.movReg(Rdx, Rcx);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R11);
  cg.movImmToMem(Rdx, 0, 0);
  cg.addImm32(Rcx, 1);
  cg.jmp(zeroLoop);
  cg.bind(zeroDone);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, retL);
  // w[i] = (w[i] << rem) | carry; carry = w[i] >> (32 - rem). `rem` is in r9
  // (reloaded from r10 each pass is unnecessary: nothing here clobbers r10).
  cg.movReg(R9, R10);  // r9 = rem (kept; r9 was only a scratch in move/zero)
  cg.xorReg(R8, R8);   // i
  cg.xorReg(Rsi, Rsi); // carry
  cg.bind(remLoop);
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpReg(R8, Rax);
  cg.jcc(Jcc::Ae, remDone);
  cg.movReg(Rdx, R8);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R11);           // &w[i]
  cg.movFromMem32(Rax, Rdx, 0);  // limb
  cg.movReg(R10, Rax);           // save limb
  cg.movReg(Rcx, R9);            // cl = rem
  cg.shlCl(Rax);                 // value = limb << rem
  cg.orReg(Rax, Rsi);            // | carry
  cg.movToMem32(Rdx, 0, Rax);
  cg.movImm(Rax, 32);
  cg.subReg(Rax, R9);  // 32 - rem
  cg.movReg(Rcx, Rax);
  cg.movReg(Rsi, R10);  // carry = limb
  cg.shrCl(Rsi);
  cg.addImm32(R8, 1);
  cg.jmp(remLoop);
  cg.bind(remDone);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, retL);
  cg.movFromMem(Rax, Rdi, 0);
  cg.movReg(Rdx, Rax);
  cg.shlImm8(Rdx, 3);
  cg.addReg(Rdx, R11);
  cg.movToMem32(Rdx, 0, Rsi);  // w[n] = carry
  cg.addImm32(Rax, 1);
  cg.movToMem(Rdi, 0, Rax);
  cg.bind(retL);
  ret(cg);
}

// void bn_sub(Bignum* a, const Bignum* b): a -= b, requires a >= b.
void emitBnSub(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label noBorrow = cg.newLabel();
  CodeGen::Label trim = cg.newLabel();
  CodeGen::Label checkTop = cg.newLabel();
  CodeGen::Label storeN = cg.newLabel();
  cg.bind(label);
  cg.movReg(R11, Rdi);         // a base
  cg.movFromMem(R10, R11, 0);  // na
  cg.movFromMem(R9, Rsi, 0);   // nb
  cg.addImm32(Rdi, static_cast<std::int32_t>(kWO));
  cg.addImm32(Rsi, static_cast<std::int32_t>(kWO));
  cg.xorReg(Rcx, Rcx);
  cg.xorReg(R8, R8);  // borrow
  cg.bind(loop);
  cg.cmpReg(Rcx, R10);
  cg.jcc(Jcc::Ae, trim);
  cg.movFromMem32(Rax, Rdi, 0);
  cg.xorReg(Rdx, Rdx);
  cg.cmpReg(Rcx, R9);
  cg.jcc(Jcc::Ae, noBorrow);
  cg.movFromMem32(Rdx, Rsi, 0);
  cg.bind(noBorrow);
  cg.subReg(Rax, Rdx);
  cg.subReg(Rax, R8);
  cg.movToMem32(Rdi, 0, Rax);
  cg.movReg(R8, Rax);
  cg.shrImm8(R8, 63);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rcx, 1);
  cg.jmp(loop);
  cg.bind(trim);
  cg.bind(checkTop);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, storeN);
  cg.movReg(Rdx, R10);
  cg.shlImm8(Rdx, 3);
  cg.subRegImm32(Rdx, static_cast<std::int32_t>(kStride));
  cg.addReg(Rdx, R11);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kWO));
  cg.movFromMem32(Rax, Rdx, 0);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, storeN);
  cg.subRegImm32(R10, 1);
  cg.jmp(checkTop);
  cg.bind(storeN);
  cg.movToMem(R11, 0, R10);
  ret(cg);
}

// void bn_add(Bignum* a, const Bignum* b): a += b.
void emitBnAdd(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label loopDone = cg.newLabel();
  CodeGen::Label aDone = cg.newLabel();
  CodeGen::Label bDone = cg.newLabel();
  CodeGen::Label haveN = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  cg.bind(label);
  cg.push(Rbx);                // rbx is the formatter's scratch base
  cg.movReg(R11, Rdi);
  cg.movFromMem(R10, R11, 0);  // na
  cg.movFromMem(R9, Rsi, 0);   // nb
  cg.movReg(R8, R10);          // n = max(na, nb)
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, haveN);
  cg.movReg(R8, R9);
  cg.bind(haveN);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kWO));
  cg.addImm32(Rsi, static_cast<std::int32_t>(kWO));
  cg.xorReg(Rcx, Rcx);         // i
  cg.xorReg(Rbx, Rbx);         // carry
  cg.bind(loop);
  cg.cmpReg(Rcx, R8);
  cg.jcc(Jcc::Ae, loopDone);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rcx, R10);
  cg.jcc(Jcc::Ae, aDone);
  cg.movFromMem32(Rax, Rdi, 0);
  cg.bind(aDone);
  cg.xorReg(Rdx, Rdx);
  cg.cmpReg(Rcx, R9);
  cg.jcc(Jcc::Ae, bDone);
  cg.movFromMem32(Rdx, Rsi, 0);
  cg.bind(bDone);
  cg.addReg(Rax, Rdx);
  cg.addReg(Rax, Rbx);
  cg.movToMem32(Rdi, 0, Rax);
  cg.movReg(Rbx, Rax);
  cg.shrImm8(Rbx, 32);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStride));
  cg.addImm32(Rcx, 1);
  cg.jmp(loop);
  cg.bind(loopDone);
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, store);
  cg.movToMem32(Rdi, 0, Rbx);
  cg.addImm32(R8, 1);
  cg.bind(store);
  cg.movToMem(R11, 0, R8);
  cg.pop(Rbx);
  ret(cg);
}

// void bn_mul_pow10(Bignum* bn, long k): bn *= 10^k, k >= 0.
void emitBnMulPow10(CodeGen& cg, CodeGen::Label label, CodeGen::Label mulSmall) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label small = cg.newLabel();
  CodeGen::Label smallLoop = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  // `bn_mul_small` clobbers r11, so `k` lives in the callee-saved r12 (saved
  // and restored here) and `bn` in rbx.
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);   // bn
  cg.movReg(R12, Rsi);   // k
  cg.bind(loop);
  cg.cmpImm32(R12, 9);
  cg.jcc(Jcc::Lt, small);
  cg.movReg(Rdi, Rbx);
  cg.movImm(Rsi, 1000000000);
  cg.callLabel(mulSmall);
  cg.subRegImm32(R12, 9);
  cg.jmp(loop);
  cg.bind(small);
  cg.bind(smallLoop);
  cg.testReg(R12);
  cg.jcc(Jcc::Eq, done);
  cg.movReg(Rdi, Rbx);
  cg.movImm(Rsi, 10);
  cg.callLabel(mulSmall);
  cg.subRegImm32(R12, 1);
  cg.jmp(smallLoop);
  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  ret(cg);
}

// long bn_divmod_small(Bignum* bn, long divisor): bn = bn / divisor, returns
// the remainder (< divisor <= 1e9) in rax.
void emitBnDivMod(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label trim = cg.newLabel();
  CodeGen::Label checkTop = cg.newLabel();
  CodeGen::Label storeN = cg.newLabel();
  cg.bind(label);
  cg.movReg(R11, Rdi);         // bn base
  cg.movFromMem(R10, R11, 0);  // n
  cg.movReg(R9, Rsi);          // divisor
  cg.addImm32(Rdi, static_cast<std::int32_t>(kWO));
  cg.movReg(Rcx, R10);
  cg.xorReg(Rdx, Rdx);  // remainder
  cg.bind(loop);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, trim);
  cg.subRegImm32(Rcx, 1);
  cg.movReg(R8, Rcx);
  cg.shlImm8(R8, 3);
  cg.addReg(R8, Rdi);  // &w[i]
  cg.movReg(Rsi, R8);  // save address
  cg.movFromMem32(Rax, R8, 0);
  // cur = (remainder << 32) | limb, then a single 64-bit divide. Using
  // `rdx:rax` directly would put the remainder in the high *64* bits.
  cg.shlImm8(Rdx, 32);
  cg.orReg(Rdx, Rax);
  cg.movReg(Rax, Rdx);
  cg.xorReg(Rdx, Rdx);
  cg.divRaw(R9);  // rax = quotient limb, rdx = new remainder
  cg.movToMem32(Rsi, 0, Rax);
  cg.jmp(loop);
  cg.bind(trim);
  cg.bind(checkTop);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, storeN);
  cg.movReg(Rax, R10);
  cg.shlImm8(Rax, 3);
  cg.subRegImm32(Rax, static_cast<std::int32_t>(kStride));
  cg.addReg(Rax, Rdi);
  cg.movFromMem32(R8, Rax, 0);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, storeN);
  cg.subRegImm32(R10, 1);
  cg.jmp(checkTop);
  cg.bind(storeN);
  cg.movToMem(R11, 0, R10);
  cg.movReg(Rax, Rdx);  // remainder
  ret(cg);
}


// --- Dragon4 generator ----------------------------------------------------

// void generate(Scratch* s): fills `digits`, `nd` and `k` from `f`, `e`,
// `p`, `minE` and `oddM`, the validated Steele & White free-format loop.
void emitGenerate(CodeGen& cg, CodeGen::Label label, const DtoaHandles& h) {
  CodeGen::Label skipShl1 = cg.newLabel();
  CodeGen::Label bSetup = cg.newLabel();
  CodeGen::Label negE = cg.newLabel();
  CodeGen::Label negB = cg.newLabel();
  CodeGen::Label estDone = cg.newLabel();
  CodeGen::Label estAdj = cg.newLabel();
  CodeGen::Label scaleNeg = cg.newLabel();
  CodeGen::Label scaleDone = cg.newLabel();
  CodeGen::Label fixHi = cg.newLabel();
  CodeGen::Label fixLo = cg.newLabel();
  CodeGen::Label fixLoDone = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label divDone = cg.newLabel();
  CodeGen::Label emitDigit = cg.newLabel();
  CodeGen::Label lowTrue = cg.newLabel();
  CodeGen::Label lowFalse = cg.newLabel();
  CodeGen::Label lowEven = cg.newLabel();
  CodeGen::Label lowDone = cg.newLabel();
  CodeGen::Label highTrue = cg.newLabel();
  CodeGen::Label highFalse = cg.newLabel();
  CodeGen::Label highEven = cg.newLabel();
  CodeGen::Label highDone = cg.newLabel();
  CodeGen::Label doRound = cg.newLabel();
  CodeGen::Label carryLoop = cg.newLabel();
  CodeGen::Label carryDone = cg.newLabel();
  CodeGen::Label newDigit = cg.newLabel();
  CodeGen::Label shiftLoop = cg.newLabel();
  CodeGen::Label shiftDone = cg.newLabel();
  CodeGen::Label afterRound = cg.newLabel();
  CodeGen::Label stripLoop = cg.newLabel();
  CodeGen::Label stripDone = cg.newLabel();
  CodeGen::Label roundDown = cg.newLabel();
  CodeGen::Label roundUp = cg.newLabel();
  CodeGen::Label tieEven = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // rbx = Scratch*

  // boundary = (e > minE) && (f == p2)
  cg.movFromMem(Rax, Rbx, offF);
  cg.movFromMem(Rcx, Rbx, offP2);
  cg.cmpReg(Rax, Rcx);
  cg.setcc(Jcc::Eq, R8);
  cg.movzx(R8, R8);
  cg.movFromMem(Rax, Rbx, offE);
  cg.movFromMem(Rcx, Rbx, offMinE);
  cg.cmpReg(Rax, Rcx);
  cg.setcc(Jcc::Gt, R9);
  cg.movzx(R9, R9);
  cg.andReg(R8, R9);
  cg.movToMem(Rbx, offBoundary, R8);

  cg.movFromMem(Rax, Rbx, offE);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, negE);

  // e >= 0: r = f << (boundary ? e+2 : e+1)
  setRdi(cg, offR);
  cg.movFromMem(Rsi, Rbx, offF);
  cg.callLabel(h.bnSetU64);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.addImm32(Rsi, 1);
  cg.movFromMem(Rax, Rbx, offBoundary);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, skipShl1);
  cg.addImm32(Rsi, 1);
  cg.bind(skipShl1);
  setRdi(cg, offR);
  cg.callLabel(h.bnShl);
  cg.movFromMem(Rax, Rbx, offBoundary);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, bSetup);
  callImm(cg, h.bnSetU32, offS, 2);
  callImm(cg, h.bnSetU32, offMp, 1);
  setRdi(cg, offMp);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.callLabel(h.bnShl);
  callTwo(cg, h.bnCopy, offMm, offMp);
  cg.jmp(estDone);
  cg.bind(bSetup);
  callImm(cg, h.bnSetU32, offS, 4);
  callImm(cg, h.bnSetU32, offMp, 1);
  setRdi(cg, offMp);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.addImm32(Rsi, 1);
  cg.callLabel(h.bnShl);
  callImm(cg, h.bnSetU32, offMm, 1);
  setRdi(cg, offMm);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.callLabel(h.bnShl);
  cg.jmp(estDone);

  cg.bind(negE);
  // r = f << (boundary ? 2 : 1)
  setRdi(cg, offR);
  cg.movFromMem(Rsi, Rbx, offF);
  cg.callLabel(h.bnSetU64);
  cg.movFromMem(Rsi, Rbx, offBoundary);
  cg.addImm32(Rsi, 1);
  setRdi(cg, offR);
  cg.callLabel(h.bnShl);
  cg.movFromMem(Rax, Rbx, offBoundary);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, negB);
  callImm(cg, h.bnSetU32, offS, 1);
  setRdi(cg, offS);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.negReg(Rsi);
  cg.addImm32(Rsi, 1);
  cg.callLabel(h.bnShl);
  callImm(cg, h.bnSetU32, offMp, 1);
  callImm(cg, h.bnSetU32, offMm, 1);
  cg.jmp(estDone);
  cg.bind(negB);
  callImm(cg, h.bnSetU32, offS, 1);
  setRdi(cg, offS);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.negReg(Rsi);
  cg.addImm32(Rsi, 2);
  cg.callLabel(h.bnShl);
  callImm(cg, h.bnSetU32, offMp, 2);
  callImm(cg, h.bnSetU32, offMm, 1);

  cg.bind(estDone);
  // m = e + bsr(f); q = floor(m * 30103 / 100000); k = q + 1
  cg.movFromMem(Rax, Rbx, offF);
  cg.bsrReg(Rax, Rax);
  cg.movFromMem(Rcx, Rbx, offE);
  cg.addReg(Rcx, Rax);
  cg.imulImm32(Rax, Rcx, 30103);
  cg.movImm(R9, 100000);
  cg.cdq();
  cg.idiv(R9);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Eq, estAdj);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Ge, estAdj);
  cg.subRegImm32(Rax, 1);
  cg.bind(estAdj);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offK, Rax);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, scaleNeg);
  setRdi(cg, offS);
  cg.movFromMem(Rsi, Rbx, offK);
  cg.callLabel(h.bnMulPow10);
  cg.jmp(scaleDone);
  cg.bind(scaleNeg);
  cg.movFromMem(Rsi, Rbx, offK);
  cg.negReg(Rsi);
  setRdi(cg, offR);
  cg.callLabel(h.bnMulPow10);
  cg.movFromMem(Rsi, Rbx, offK);
  cg.negReg(Rsi);
  setRdi(cg, offMp);
  cg.callLabel(h.bnMulPow10);
  cg.movFromMem(Rsi, Rbx, offK);
  cg.negReg(Rsi);
  setRdi(cg, offMm);
  cg.callLabel(h.bnMulPow10);
  cg.bind(scaleDone);

  // while (r >= s) { s *= 10; k++ }
  cg.bind(fixHi);
  callTwo(cg, h.bnCmp, offR, offS);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Lt, fixLo);
  callImm(cg, h.bnMulSmall, offS, 10);
  cg.movFromMem(Rax, Rbx, offK);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offK, Rax);
  cg.jmp(fixHi);

  // while (r*10 < s) { r,mp,mm *= 10; k-- }
  cg.bind(fixLo);
  callTwo(cg, h.bnCopy, offT, offR);
  callImm(cg, h.bnMulSmall, offT, 10);
  callTwo(cg, h.bnCmp, offT, offS);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Ge, fixLoDone);
  callImm(cg, h.bnMulSmall, offR, 10);
  callImm(cg, h.bnMulSmall, offMp, 10);
  callImm(cg, h.bnMulSmall, offMm, 10);
  cg.movFromMem(Rax, Rbx, offK);
  cg.subRegImm32(Rax, 1);
  cg.movToMem(Rbx, offK, Rax);
  cg.jmp(fixLo);
  cg.bind(fixLoDone);

  cg.movImmToMem(Rbx, offNd, 0);
  cg.bind(loop);
  callImm(cg, h.bnMulSmall, offR, 10);
  callImm(cg, h.bnMulSmall, offMp, 10);
  callImm(cg, h.bnMulSmall, offMm, 10);
  cg.movImmToMem(Rbx, offD, 0);
  cg.bind(divDone);
  callTwo(cg, h.bnCmp, offR, offS);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Lt, emitDigit);
  callTwo(cg, h.bnSub, offR, offS);
  cg.movFromMem(Rax, Rbx, offD);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offD, Rax);
  cg.jmp(divDone);

  cg.bind(emitDigit);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.movFromMem(Rax, Rbx, offD);
  storeDigit(cg);
  cg.movFromMem(Rax, Rbx, offNd);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offNd, Rax);

  // low = oddM ? (r < mm) : (r <= mm)
  callTwo(cg, h.bnCmp, offR, offMm);
  cg.movFromMem(Rcx, Rbx, offOddM);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, lowEven);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Lt, lowTrue);
  cg.jmp(lowFalse);
  cg.bind(lowEven);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Le, lowTrue);
  cg.bind(lowFalse);
  cg.movImmToMem(Rbx, offLow, 0);
  cg.jmp(lowDone);
  cg.bind(lowTrue);
  cg.movImmToMem(Rbx, offLow, 1);
  cg.bind(lowDone);

  // high = oddM ? (r+mp > s) : (r+mp >= s)
  callTwo(cg, h.bnCopy, offT, offR);
  callTwo(cg, h.bnAdd, offT, offMp);
  callTwo(cg, h.bnCmp, offT, offS);
  cg.movFromMem(Rcx, Rbx, offOddM);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, highEven);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Gt, highTrue);
  cg.jmp(highFalse);
  cg.bind(highEven);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Ge, highTrue);
  cg.bind(highFalse);
  cg.movImmToMem(Rbx, offHigh, 0);
  cg.jmp(highDone);
  cg.bind(highTrue);
  cg.movImmToMem(Rbx, offHigh, 1);
  cg.bind(highDone);

  cg.movFromMem(Rax, Rbx, offLow);
  cg.movFromMem(Rcx, Rbx, offHigh);
  cg.orReg(Rax, Rcx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, loop);

  // Rounding decision: both margins -> 2r vs s; one -> that one; tie -> even.
  cg.movFromMem(Rax, Rbx, offLow);
  cg.movFromMem(Rcx, Rbx, offHigh);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Eq, doRound);
  cg.movFromMem(Rax, Rbx, offLow);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, roundDown);
  cg.jmp(roundUp);
  cg.bind(doRound);
  callTwo(cg, h.bnCopy, offT, offR);
  callTwo(cg, h.bnAdd, offT, offR);
  callTwo(cg, h.bnCmp, offT, offS);
  cg.cmpImm32(Rax, 0);
  cg.jcc(Jcc::Gt, roundUp);
  cg.jcc(Jcc::Lt, roundDown);
  cg.bind(tieEven);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.subRegImm32(Rcx, 1);
  cg.movReg(Rax, Rcx);
  loadDigit(cg);
  cg.andImm32(Rax, 1);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, roundUp);
  cg.jmp(roundDown);

  cg.bind(roundDown);
  cg.jmp(afterRound);

  cg.bind(roundUp);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.subRegImm32(Rcx, 1);
  cg.bind(carryLoop);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Lt, newDigit);
  cg.movReg(R8, Rcx);
  cg.movReg(Rax, Rcx);
  loadDigit(cg);
  cg.movReg(Rcx, R8);
  cg.cmpImm32(Rax, 9);
  cg.jcc(Jcc::Ne, carryDone);
  cg.movImm(Rax, 0);
  storeDigit(cg);
  cg.subRegImm32(Rcx, 1);
  cg.jmp(carryLoop);
  cg.bind(carryDone);
  cg.addImm32(Rax, 1);
  storeDigit(cg);
  cg.jmp(afterRound);

  cg.bind(newDigit);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.bind(shiftLoop);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, shiftDone);
  cg.movReg(R8, Rcx);
  cg.subRegImm32(R8, 1);
  cg.movReg(Rax, R8);
  loadDigit(cg);
  storeDigit(cg);
  cg.subRegImm32(Rcx, 1);
  cg.jmp(shiftLoop);
  cg.bind(shiftDone);
  cg.movImm(Rax, 1);
  cg.xorReg(Rcx, Rcx);
  storeDigit(cg);
  cg.movFromMem(Rax, Rbx, offNd);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offNd, Rax);
  cg.movFromMem(Rax, Rbx, offK);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offK, Rax);

  cg.bind(afterRound);
  // Strip trailing zeros produced by a 9->10 carry.
  cg.bind(stripLoop);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.cmpImm32(Rcx, 1);
  cg.jcc(Jcc::Le, stripDone);
  cg.subRegImm32(Rcx, 1);
  cg.movReg(Rax, Rcx);
  loadDigit(cg);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, stripDone);
  cg.movFromMem(Rax, Rbx, offNd);
  cg.subRegImm32(Rax, 1);
  cg.movToMem(Rbx, offNd, Rax);
  cg.jmp(stripLoop);
  cg.bind(stripDone);
  cg.addImm32(Rsp, 16);
  cg.pop(Rbx);
  cg.pop(Rbp);
  cg.retInsn();
}


// store rax's low byte at [rbx + off + rcx]; rcx is the cursor.
void storeAt(CodeGen& cg, std::int64_t off) {
  cg.movReg(Rdx, Rcx);
  cg.addReg(Rdx, Rbx);
  if (off != 0) cg.addImm32(Rdx, static_cast<std::int32_t>(off));
  cg.movByteToMem(Rdx, 0, Rax);
}

// store rax's low byte at [rdi + rcx]; rcx is the output cursor.
void storeOut(CodeGen& cg) {
  cg.movReg(Rdx, Rdi);
  cg.addReg(Rdx, Rcx);
  cg.movByteToMem(Rdx, 0, Rax);
}

// void build_fixed(Scratch* s): fixed body into `text`, its length into `tmp`.
void emitBuildFixed(CodeGen& cg, CodeGen::Label label, const DtoaHandles& h) {
  CodeGen::Label notExact = cg.newLabel();
  CodeGen::Label exact = cg.newLabel();
  CodeGen::Label kGeNd = cg.newLabel();
  CodeGen::Label middle = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label zLoop = cg.newLabel();
  CodeGen::Label zDone = cg.newLabel();
  CodeGen::Label dLoop = cg.newLabel();
  CodeGen::Label dDone = cg.newLabel();
  CodeGen::Label gLoop = cg.newLabel();
  CodeGen::Label gDone = cg.newLabel();
  CodeGen::Label z2Loop = cg.newLabel();
  CodeGen::Label z2Done = cg.newLabel();
  CodeGen::Label mLoop = cg.newLabel();
  CodeGen::Label mDot = cg.newLabel();
  CodeGen::Label mLoop2 = cg.newLabel();
  CodeGen::Label mDone = cg.newLabel();
  CodeGen::Label eLoop = cg.newLabel();
  CodeGen::Label eRev = cg.newLabel();
  CodeGen::Label eRevDone = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);

  // Exact path: k >= nd && 0 <= e <= 74 (f << e is then exact decimal).
  cg.movFromMem(Rax, Rbx, offK);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Lt, notExact);
  cg.movFromMem(Rcx, Rbx, offE);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Lt, notExact);
  cg.cmpImm32(Rcx, 74);
  cg.jcc(Jcc::Gt, notExact);
  cg.jmp(exact);

  cg.bind(notExact);
  cg.movFromMem(Rax, Rbx, offK);
  cg.testReg(Rax);
  cg.jcc(Jcc::Gt, kGeNd);
  // k <= 0: "0." + (-k) zeros + digits
  cg.xorReg(Rcx, Rcx);
  cg.movImm(Rax, '0'); storeAt(cg, offText); cg.addImm32(Rcx, 1);
  cg.movImm(Rax, '.'); storeAt(cg, offText); cg.addImm32(Rcx, 1);
  cg.movFromMem(R8, Rbx, offK);
  cg.negReg(R8);
  cg.xorReg(R9, R9);
  cg.bind(zLoop);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, zDone);
  cg.movImm(Rax, '0'); storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(zLoop);
  cg.bind(zDone);
  cg.xorReg(R9, R9);
  cg.bind(dLoop);
  cg.movFromMem(R8, Rbx, offNd);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, dDone);
  cg.movReg(Rax, R9);
  loadDigitAscii(cg);
  storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(dLoop);
  cg.bind(dDone);
  cg.movToMem(Rbx, offTmp, Rcx);
  cg.jmp(done);

  cg.bind(kGeNd);
  cg.movFromMem(Rax, Rbx, offK);
  cg.movFromMem(Rcx, Rbx, offNd);
  cg.cmpReg(Rax, Rcx);
  cg.jcc(Jcc::Lt, middle);
  // k >= nd: digits + (k-nd) zeros
  cg.xorReg(Rcx, Rcx);
  cg.xorReg(R9, R9);
  cg.bind(gLoop);
  cg.movFromMem(R8, Rbx, offNd);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, gDone);
  cg.movReg(Rax, R9);
  loadDigitAscii(cg);
  storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(gLoop);
  cg.bind(gDone);
  cg.movFromMem(R8, Rbx, offK);
  cg.subReg(R8, R9);
  cg.xorReg(R9, R9);
  cg.bind(z2Loop);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, z2Done);
  cg.movImm(Rax, '0'); storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(z2Loop);
  cg.bind(z2Done);
  cg.movToMem(Rbx, offTmp, Rcx);
  cg.jmp(done);

  cg.bind(middle);
  // 0 < k < nd: digits[0..k-1] + "." + digits[k..nd-1]
  cg.xorReg(Rcx, Rcx);
  cg.xorReg(R9, R9);
  cg.bind(mLoop);
  cg.movFromMem(R8, Rbx, offK);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, mDot);
  cg.movReg(Rax, R9);
  loadDigitAscii(cg);
  storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(mLoop);
  cg.bind(mDot);
  cg.movImm(Rax, '.'); storeAt(cg, offText); cg.addImm32(Rcx, 1);
  cg.bind(mLoop2);
  cg.movFromMem(R8, Rbx, offNd);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, mDone);
  cg.movReg(Rax, R9);
  loadDigitAscii(cg);
  storeAt(cg, offText); cg.addImm32(Rcx, 1); cg.addImm32(R9, 1);
  cg.jmp(mLoop2);
  cg.bind(mDone);
  cg.movToMem(Rbx, offTmp, Rcx);
  cg.jmp(done);

  cg.bind(exact);
  setRdi(cg, offT);
  cg.movFromMem(Rsi, Rbx, offF);
  cg.callLabel(h.bnSetU64);
  setRdi(cg, offT);
  cg.movFromMem(Rsi, Rbx, offE);
  cg.callLabel(h.bnShl);
  cg.movImmToMem(Rbx, offD, 0);  // pos
  cg.bind(eLoop);
  setRdi(cg, offT);
  cg.movImm(Rsi, 10);
  cg.callLabel(h.bnDivMod);
  cg.addImm32(Rax, '0');
  cg.movFromMem(Rdx, Rbx, offD);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offText2));
  cg.movByteToMem(Rdx, 0, Rax);
  cg.movFromMem(Rax, Rbx, offD);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbx, offD, Rax);
  cg.movFromMem(Rax, Rbx, offT);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, eLoop);
  // Reverse text2[0..pos) into text.
  cg.xorReg(R9, R9);
  cg.bind(eRev);
  cg.movFromMem(Rcx, Rbx, offD);
  cg.cmpReg(R9, Rcx);
  cg.jcc(Jcc::Ge, eRevDone);
  cg.movReg(R8, Rcx);
  cg.subRegImm32(R8, 1);
  cg.subReg(R8, R9);
  cg.movReg(Rdx, R8);
  cg.addReg(Rdx, Rbx);
  cg.addImm32(Rdx, static_cast<std::int32_t>(offText2));
  cg.movzxFromMem(Rax, Rdx, 0);
  cg.movReg(Rcx, R9);
  storeAt(cg, offText);
  cg.addImm32(R9, 1);
  cg.jmp(eRev);
  cg.bind(eRevDone);
  cg.movToMem(Rbx, offTmp, R9);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(Rbx);
  cg.pop(Rbp);
  cg.retInsn();
}

// void build_sci(Scratch* s): scientific body into `text2`, length into `tmp`.
void emitBuildSci(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label noFrac = cg.newLabel();
  CodeGen::Label fracLoop = cg.newLabel();
  CodeGen::Label expPos = cg.newLabel();
  CodeGen::Label expAbs = cg.newLabel();
  CodeGen::Label cntLoop = cg.newLabel();
  CodeGen::Label cntDone = cg.newLabel();
  CodeGen::Label wDone = cg.newLabel();
  CodeGen::Label powLoop = cg.newLabel();
  CodeGen::Label powDone = cg.newLabel();
  CodeGen::Label emitLoop = cg.newLabel();
  CodeGen::Label emitDone = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);
  cg.xorReg(Rcx, Rcx);

  cg.xorReg(Rax, Rax);
  loadDigitAscii(cg);
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.movFromMem(R8, Rbx, offNd);
  cg.cmpImm32(R8, 1);
  cg.jcc(Jcc::Le, noFrac);
  cg.movImm(Rax, '.');
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.movImm(R9, 1);
  cg.bind(fracLoop);
  cg.movFromMem(R8, Rbx, offNd);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, noFrac);
  cg.movReg(Rax, R9);
  loadDigitAscii(cg);
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.addImm32(R9, 1);
  cg.jmp(fracLoop);
  cg.bind(noFrac);
  cg.movImm(Rax, 'e');
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.movFromMem(R8, Rbx, offK);
  cg.subRegImm32(R8, 1);
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, expPos);
  cg.movImm(Rax, '-');
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.negReg(R8);
  cg.jmp(expAbs);
  cg.bind(expPos);
  cg.movImm(Rax, '+');
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.bind(expAbs);
  // Count the exponent's decimal digits (at least 2).
  cg.movReg(R9, R8);
  cg.movImm(R10, 1);
  cg.bind(cntLoop);
  cg.cmpImm32(R9, 10);
  cg.jcc(Jcc::Lt, cntDone);
  cg.movReg(Rax, R9);
  cg.movImm(R9, 10);
  cg.divReg(R9);
  cg.movReg(R9, Rax);
  cg.addImm32(R10, 1);
  cg.jmp(cntLoop);
  cg.bind(cntDone);
  cg.cmpImm32(R10, 2);
  cg.jcc(Jcc::Ge, wDone);
  cg.movImm(R10, 2);
  cg.bind(wDone);
  // divisor = 10^(width-1)
  cg.movImm(R11, 1);
  cg.movReg(R9, R10);
  cg.subRegImm32(R9, 1);
  cg.bind(powLoop);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, powDone);
  cg.movImm(Rax, 10);
  cg.imulReg(R11, Rax);
  cg.subRegImm32(R9, 1);
  cg.jmp(powLoop);
  cg.bind(powDone);
  cg.bind(emitLoop);
  cg.testReg(R10);
  cg.jcc(Jcc::Eq, emitDone);
  cg.movReg(Rax, R8);
  cg.divReg(R11);
  cg.movReg(R9, Rdx);  // remainder
  cg.addImm32(Rax, '0');
  storeAt(cg, offText2);
  cg.addImm32(Rcx, 1);
  cg.movReg(R8, R9);
  cg.movReg(Rax, R11);
  cg.movImm(R9, 10);
  cg.divReg(R9);
  cg.movReg(R11, Rax);
  cg.subRegImm32(R10, 1);
  cg.jmp(emitLoop);
  cg.bind(emitDone);
  cg.movToMem(Rbx, offTmp, Rcx);
  cg.addImm32(Rsp, 16);
  cg.pop(Rbx);
  cg.pop(Rbp);
  cg.retInsn();
}

// long format_double(long tag, long payload, char* out, Scratch* scratch):
// the entry point. `scratch` is a caller-provided block of at least
// `kScratchBytes` bytes; the routine keeps the arena (r12-r15) untouched.
void emitFormatDouble(CodeGen& cg, CodeGen::Label label, CodeGen::Label generate,
                      CodeGen::Label buildFixed, CodeGen::Label buildSci) {
  CodeGen::Label floatPath = cg.newLabel();
  CodeGen::Label dSub = cg.newLabel();
  CodeGen::Label commonD = cg.newLabel();
  CodeGen::Label fSub = cg.newLabel();
  CodeGen::Label fCommon = cg.newLabel();
  CodeGen::Label special = cg.newLabel();
  CodeGen::Label nanPath = cg.newLabel();
  CodeGen::Label infNoSign = cg.newLabel();
  CodeGen::Label zero = cg.newLabel();
  CodeGen::Label zeroNoSign = cg.newLabel();
  CodeGen::Label common = cg.newLabel();
  CodeGen::Label useSci = cg.newLabel();
  CodeGen::Label copy = cg.newLabel();
  CodeGen::Label cpLoop = cg.newLabel();
  CodeGen::Label finish = cg.newLabel();
  CodeGen::Label noSign = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.movReg(Rbx, Rcx);  // scratch block (4th argument)
  cg.movToMem(Rbx, offOutp, Rdx);

  cg.movReg(Rax, Rsi);
  cg.movReg(R10, Rax);
  cg.shrImm8(R10, 63);
  cg.movToMem(Rbx, offSign, R10);
  cg.movReg(R8, Rax);
  cg.shrImm8(R8, 52);
  cg.andImm32(R8, 0x7FF);
  cg.movReg(R9, Rax);
  cg.shlImm8(R9, 12);
  cg.shrImm8(R9, 12);

  cg.cmpImm32(Rdi, 3);  // AotTag::Float
  cg.jcc(Jcc::Eq, floatPath);

  // double
  cg.cmpImm32(R8, 0x7FF);
  cg.jcc(Jcc::Eq, special);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, dSub);
  cg.movImm(Rcx, 0x10000000000000);
  cg.orReg(R9, Rcx);
  cg.movReg(Rcx, R8);
  cg.subRegImm32(Rcx, 1075);
  cg.jmp(commonD);

  cg.bind(dSub);
  cg.movImm(Rcx, -1074);
  cg.bind(commonD);
  cg.movReg(Rax, R9);
  cg.movImm(Rdx, 53);
  cg.movImm(Rsi, -1074);
  cg.jmp(common);

  cg.bind(floatPath);
  // The payload is the binary64 pattern of a binary32 value. `r8` is the double
  // biased exponent (0x7FF for inf/nan) and `r9` the double fraction. For a
  // normal float be23 = be_double - 896 and frac23 = frac52 >> 29. A float
  // subnormal widens to a *normal* double, so its implicit bit is set and the
  // original 23-bit fraction must be recovered as (2^52 + frac52) >> (926 - be).
  cg.cmpImm32(R8, 0x7FF);
  cg.jcc(Jcc::Eq, special);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, zero);
  cg.cmpImm32(R8, 896);
  cg.jcc(Jcc::Le, fSub);
  cg.shrImm8(R9, 29);          // frac23
  cg.movImm(R11, 0x800000);
  cg.orReg(R9, R11);
  cg.movReg(Rcx, R8);
  cg.subRegImm32(Rcx, 1046);
  cg.jmp(fCommon);
  cg.bind(fSub);
  // Float subnormal: value = frac23 * 2^-149, a uniform ULP, so no boundary
  // case and no normalisation (which would shrink the margin). Rebuild frac23
  // from the double significand: frac23 = (2^52 + frac52) >> (926 - be).
  cg.movImm(R11, 0x10000000000000);
  cg.addReg(R9, R11);
  cg.movImm(Rcx, 926);
  cg.subReg(Rcx, R8);
  cg.shrCl(R9);
  cg.movImm(Rcx, -149);
  cg.bind(fCommon);
  cg.movReg(Rax, R9);
  cg.movImm(Rdx, 24);
  cg.movImm(Rsi, -149);

  cg.bind(common);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, zero);
  cg.movToMem(Rbx, offF, Rax);
  cg.movToMem(Rbx, offE, Rcx);
  cg.movToMem(Rbx, offP, Rdx);
  cg.movToMem(Rbx, offMinE, Rsi);
  cg.movImm(Rax, 1);
  cg.movReg(Rcx, Rdx);
  cg.subRegImm32(Rcx, 1);
  cg.shlCl(Rax);
  cg.movToMem(Rbx, offP2, Rax);
  cg.movFromMem(Rax, Rbx, offF);
  cg.andImm32(Rax, 1);
  cg.movToMem(Rbx, offOddM, Rax);

  cg.movReg(Rdi, Rbx);
  cg.callLabel(generate);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(buildFixed);
  cg.movFromMem(Rax, Rbx, offTmp);
  cg.movToMem(Rbx, offPos, Rax);  // fixed length
  cg.movReg(Rdi, Rbx);
  cg.callLabel(buildSci);
  cg.movFromMem(Rcx, Rbx, offTmp);  // sci length
  cg.movFromMem(R8, Rbx, offPos);   // fixed length
  cg.cmpReg(Rcx, R8);
  cg.jcc(Jcc::Lt, useSci);
  cg.movReg(Rsi, Rbx);
  cg.addImm32(Rsi, static_cast<std::int32_t>(offText));
  cg.movReg(R10, R8);
  cg.jmp(copy);
  cg.bind(useSci);
  cg.movReg(Rsi, Rbx);
  cg.addImm32(Rsi, static_cast<std::int32_t>(offText2));
  cg.movReg(R10, Rcx);

  cg.bind(copy);
  cg.movFromMem(Rdi, Rbx, offOutp);
  cg.xorReg(Rcx, Rcx);
  cg.movFromMem(Rax, Rbx, offSign);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, noSign);
  cg.movImm(Rax, '-');
  storeOut(cg);
  cg.addImm32(Rcx, 1);
  cg.bind(noSign);
  cg.xorReg(R9, R9);
  cg.bind(cpLoop);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ge, finish);
  cg.movReg(Rdx, R9);
  cg.addReg(Rdx, Rsi);
  cg.movzxFromMem(Rax, Rdx, 0);
  storeOut(cg);
  cg.addImm32(R9, 1);
  cg.addImm32(Rcx, 1);
  cg.jmp(cpLoop);

  cg.bind(special);
  cg.movFromMem(Rdi, Rbx, offOutp);
  cg.xorReg(Rcx, Rcx);
  cg.testReg(R9);
  cg.jcc(Jcc::Ne, nanPath);
  cg.movFromMem(Rax, Rbx, offSign);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, infNoSign);
  cg.movImm(Rax, '-');
  storeOut(cg);
  cg.addImm32(Rcx, 1);
  cg.bind(infNoSign);
  cg.movImm(Rax, 'i'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.movImm(Rax, 'n'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.movImm(Rax, 'f'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.jmp(finish);
  cg.bind(nanPath);
  cg.movImm(Rax, 'n'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.movImm(Rax, 'a'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.movImm(Rax, 'n'); storeOut(cg); cg.addImm32(Rcx, 1);
  cg.jmp(finish);

  cg.bind(zero);
  cg.movFromMem(Rdi, Rbx, offOutp);
  cg.xorReg(Rcx, Rcx);
  cg.movFromMem(Rax, Rbx, offSign);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, zeroNoSign);
  cg.movImm(Rax, '-');
  storeOut(cg);
  cg.addImm32(Rcx, 1);
  cg.bind(zeroNoSign);
  cg.movImm(Rax, '0'); storeOut(cg); cg.addImm32(Rcx, 1);

  cg.bind(finish);
  cg.movReg(Rax, Rcx);
  cg.pop(Rbx);
  cg.pop(Rbp);
  cg.retInsn();
}

}  // namespace

void emitDtoa(CodeGen& code, const DtoaHandles& handles) {
  emitBnSetU64(code, handles.bnSetU64);
  emitBnSetU32(code, handles.bnSetU32);
  emitBnCopy(code, handles.bnCopy);
  emitBnCmp(code, handles.bnCmp);
  emitBnMulSmall(code, handles.bnMulSmall);
  emitBnShl(code, handles.bnShl);
  emitBnSub(code, handles.bnSub);
  emitBnAdd(code, handles.bnAdd);
  emitBnMulPow10(code, handles.bnMulPow10, handles.bnMulSmall);
  emitBnDivMod(code, handles.bnDivMod);
  emitGenerate(code, handles.generate, handles);
  CodeGen::Label buildFixed = code.newLabel();
  CodeGen::Label buildSci = code.newLabel();
  emitBuildFixed(code, buildFixed, handles);
  emitBuildSci(code, buildSci);
  emitFormatDouble(code, handles.formatDouble, handles.generate, buildFixed, buildSci);
}

}  // namespace dtoa
