#include "native/runtime.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

// Shorthand for the registers the runtime uses, so the emit helpers read like
// assembly instead of `X64Reg::Rax` everywhere. C++17 has no `using enum`, so
// they are plain constants.
constexpr X64Reg Rax = X64Reg::Rax;
constexpr X64Reg Rcx = X64Reg::Rcx;
constexpr X64Reg Rdx = X64Reg::Rdx;
constexpr X64Reg Rsi = X64Reg::Rsi;
constexpr X64Reg Rdi = X64Reg::Rdi;
constexpr X64Reg R8 = X64Reg::R8;
constexpr X64Reg R9 = X64Reg::R9;
constexpr X64Reg R10 = X64Reg::R10;
constexpr X64Reg R11 = X64Reg::R11;
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsp = X64Reg::Rsp;

constexpr std::int64_t kTagNil = static_cast<std::int64_t>(AotTag::Nil);
constexpr std::int64_t kTagBool = static_cast<std::int64_t>(AotTag::Bool);
constexpr std::int64_t kTagInt = static_cast<std::int64_t>(AotTag::Int);
constexpr std::int64_t kTagFloat = static_cast<std::int64_t>(AotTag::Float);
constexpr std::int64_t kTagDouble = static_cast<std::int64_t>(AotTag::Double);

// The sign bit of an IEEE-754 binary64, used to negate a float by flipping it.
constexpr std::int64_t kDoubleSignBit = static_cast<std::int64_t>(0x8000000000000000ULL);
// INT64_MIN as a bit pattern, for the `idiv` overflow special case.
constexpr std::int64_t kInt64Min = static_cast<std::int64_t>(0x8000000000000000ULL);

// SSE registers are numbered from `kXmmBase` in the emitter API (see
// `native/codegen.h`), not from zero, so the runtime names its scratch xmms
// through that base.
constexpr int Xmm0 = kXmmBase + 0;
constexpr int Xmm1 = kXmmBase + 1;
constexpr int Xmm2 = kXmmBase + 2;

// Exit code of an uncaught runtime error, matching the development engine
// (`main.cpp` returns 70 after a `RuntimeError`).
constexpr std::int64_t kRuntimeErrorExit = 70;

// Every helper keeps the System V stack invariant: it is entered with
// rsp % 16 == 8 (the `call` pushed a return address), it pushes rbp so rsp is
// 16-byte aligned inside, and any `call` it makes therefore leaves its callee
// aligned. rbp is callee-saved, so preserving it also protects the caller's
// register file, whose base lives in rbp. `prologue`/`epilogue` make that
// uniform; a helper that never returns omits the epilogue.
void prologue(CodeGen& cg) {
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
}

void epilogue(CodeGen& cg) {
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
}

// A block of constant text emitted after all the code, addressed through a
// label. The runtime needs the error messages as bytes in the text section; a
// data island with a `lea`-resolved address is enough and keeps the whole
// thing inside the single code buffer (no .rodata section, no relocations).
struct DataBlob {
  CodeGen::Label label;
  std::string text;
};

// Emits `jolt_write(1, rdi, rsi)`: writes `rsi` bytes from `rdi` to stdout.
// Clobbers rdi, rsi, rdx and rax (the syscall's return).
void inlineWriteStdout(CodeGen& cg, CodeGen::Label writeLabel) {
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movImm(Rdi, 1);
  cg.callLabel(writeLabel);
}

// Emits `call fail(message)`: loads the message address and length, then calls
// the shared fail helper. Never returns.
void emitFailCall(CodeGen& cg, CodeGen::Label fail, const DataBlob& message) {
  cg.leaRipLabel(Rdi, message.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(message.text.size()));
  cg.callLabel(fail);
  cg.ud2();
}

// `void fail(rdi = text, rsi = length)`: writes the text to stderr and exits
// with the runtime-error code. Never returns, so it has no epilogue.
void emitFail(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
              CodeGen::Label exitLabel) {
  cg.bind(label);
  prologue(cg);
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movImm(Rdi, 2);  // stderr
  cg.callLabel(writeLabel);
  cg.movImm(Rdi, kRuntimeErrorExit);
  cg.callLabel(exitLabel);
  cg.ud2();
}

// Emits `rax = 1` if `tag` is a number (int, float or double), else `rax = 0`.
// The three tags are contiguous (Int=2, Float=3, Double=4), so the check is a
// range test. Clobbers rax and the flags.
void emitIsNumberFlag(CodeGen& cg, X64Reg tag) {
  CodeGen::Label notNumber = cg.newLabel();
  cg.xorReg(Rax, Rax);
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::B, notNumber);  // tag < Int
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::A, notNumber);  // tag > Double
  cg.movImm(Rax, 1);
  cg.bind(notNumber);
}

// Emits `xmm = (double)value`, where `value` is `tag`:`payload`. An int is
// converted with `cvtsi2sd`; a float/double payload already *is* the bit
// pattern of a binary64 value, so it moves with `movq`. A float is stored
// widened, exactly as the hosted `Value` stores it, so the conversion is
// lossless. Clobbers rax.
void emitToDouble(CodeGen& cg, X64Reg tag, X64Reg payload, int xmm) {
  CodeGen::Label isInt = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.cmpImm32(tag, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, isInt);
  cg.movqToXmm(xmm, payload);
  cg.jmp(done);
  cg.bind(isInt);
  cg.cvtsi2sd(xmm, payload);
  cg.bind(done);
}

// `long jolt_truthy(AotValue a)`: 1 when the value is truthy, else 0. Mirrors
// `Value::truthy`: a bool is itself, nil is false, everything else is true.
void emitTruthy(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label isBool = cg.newLabel();
  CodeGen::Label isNil = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, isBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, isNil);
  cg.movImm(Rax, 1);  // every other value is truthy
  epilogue(cg);
  cg.bind(isBool);
  cg.movReg(Rax, Rsi);
  epilogue(cg);
  cg.bind(isNil);
  cg.xorReg(Rax, Rax);
  epilogue(cg);
}

// `void jolt_print_value(AotValue v)`: prints int/bool/nil followed by a
// newline. Floats and strings are not representable yet (PHASE 7c.1c).
void emitPrintValue(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                    CodeGen::Label fail, CodeGen::Label writeInt,
                    const DataBlob& badValue, const DataBlob& textTrue,
                    const DataBlob& textFalse, const DataBlob& textNil,
                    const DataBlob& textNewline) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label newline = cg.newLabel();

  cg.bind(label);
  prologue(cg);

  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  emitFailCall(cg, fail, badValue);

  cg.bind(asInt);
  cg.movReg(Rdi, Rsi);
  cg.callLabel(writeInt);
  cg.jmp(newline);

  cg.bind(asBool);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.leaRipLabel(Rdi, textTrue.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textTrue.text.size()));
  inlineWriteStdout(cg, writeLabel);
  cg.jmp(newline);
  cg.bind(boolFalse);
  cg.leaRipLabel(Rdi, textFalse.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textFalse.text.size()));
  inlineWriteStdout(cg, writeLabel);
  cg.jmp(newline);

  cg.bind(asNil);
  cg.leaRipLabel(Rdi, textNil.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textNil.text.size()));
  inlineWriteStdout(cg, writeLabel);

  cg.bind(newline);
  cg.leaRipLabel(Rdi, textNewline.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textNewline.text.size()));
  inlineWriteStdout(cg, writeLabel);

  epilogue(cg);
}

// `void write_int(long value)`: prints a signed 64-bit integer in decimal
// without libc. The digits are produced least-significant first into a stack
// buffer and written in one syscall; `INT64_MIN` is handled by taking the
// magnitude in unsigned arithmetic (`neg` on the two's-complement pattern is
// exact for every value, including the most negative one).
void emitWriteInt(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel) {
  constexpr std::int64_t kBuffer = 48;  // 20 digits + sign, rounded up

  CodeGen::Label positive = cg.newLabel();
  CodeGen::Label convert = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label emit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kBuffer));

  cg.movReg(R8, Rsp);
  cg.addImm32(R8, static_cast<std::int32_t>(kBuffer));  // cursor = end of buffer
  cg.movReg(Rax, Rdi);

  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, positive);
  cg.movImm(R9, 1);  // remember the sign
  cg.negReg(Rax);    // magnitude
  cg.jmp(convert);

  cg.bind(positive);
  cg.xorReg(R9, R9);

  cg.bind(convert);
  cg.movImm(Rcx, 10);
  cg.bind(loop);
  cg.divUnsigned(Rcx);  // rax = rax / 10, rdx = rax % 10 (rdx pre-zeroed)
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(R8, 1);
  cg.movByteToMem(R8, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);

  cg.testReg(R9);
  cg.jcc(Jcc::Eq, emit);
  cg.subRegImm32(R8, 1);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('-'));

  cg.bind(emit);
  cg.movReg(Rsi, Rsp);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kBuffer));
  cg.subReg(Rsi, R8);  // length = end - cursor
  cg.movReg(Rdi, R8);
  inlineWriteStdout(cg, writeLabel);

  epilogue(cg);
}

// `AotValue jolt_arith(AotValue a, AotValue b, long op)`: `+`, `-`, `*`, `/`
// and `%` with the int/float/double promotion of `ops::binary`. int/int uses
// 64-bit wrapping arithmetic; anything with a float promotes to double, and a
// float/float result rounds back to binary32 so `0.1f + 0.2f == 0.3f` holds,
// exactly like the hosted engine. `%` on floats needs `fmod` (libm) and is
// deferred to PHASE 7c.1c.
void emitArith(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgTypes, const DataBlob& msgDivZero) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label intPath = cg.newLabel();
  CodeGen::Label floatPath = cg.newLabel();
  CodeGen::Label intDone = cg.newLabel();
  CodeGen::Label intAdd = cg.newLabel();
  CodeGen::Label intSub = cg.newLabel();
  CodeGen::Label intMul = cg.newLabel();
  CodeGen::Label intDiv = cg.newLabel();
  CodeGen::Label intMod = cg.newLabel();
  CodeGen::Label intDivCommon = cg.newLabel();
  CodeGen::Label intDivZero = cg.newLabel();
  CodeGen::Label intNormalDiv = cg.newLabel();
  CodeGen::Label intSpecial = cg.newLabel();
  CodeGen::Label intSpecialMod = cg.newLabel();
  CodeGen::Label floatAdd = cg.newLabel();
  CodeGen::Label floatSub = cg.newLabel();
  CodeGen::Label floatMul = cg.newLabel();
  CodeGen::Label floatDiv = cg.newLabel();
  CodeGen::Label floatDivZero = cg.newLabel();
  CodeGen::Label floatNotZero = cg.newLabel();
  CodeGen::Label floatDone = cg.newLabel();
  CodeGen::Label useFloat = cg.newLabel();
  CodeGen::Label useDouble = cg.newLabel();

  cg.bind(label);
  prologue(cg);

  // Both operands must be numbers.
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, bad);

  // int/int takes the wrapping integer path; everything else promotes.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, floatPath);
  cg.jmp(intPath);

  // --- integer path ---
  cg.bind(intPath);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Eq, intAdd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Sub));
  cg.jcc(Jcc::Eq, intSub);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mul));
  cg.jcc(Jcc::Eq, intMul);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Div));
  cg.jcc(Jcc::Eq, intDiv);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mod));
  cg.jcc(Jcc::Eq, intMod);
  cg.jmp(bad);

  cg.bind(intAdd);
  cg.addReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);
  cg.bind(intSub);
  cg.subReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);
  cg.bind(intMul);
  cg.imulReg(Rsi, Rcx);
  cg.movReg(Rax, Rsi);
  cg.jmp(intDone);

  cg.bind(intDiv);
  cg.movImm(R9, 1);  // want the quotient
  cg.jmp(intDivCommon);
  cg.bind(intMod);
  cg.xorReg(R9, R9);  // want the remainder

  cg.bind(intDivCommon);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, intDivZero);
  // INT64_MIN / -1 overflows in `idiv`; the hosted engine returns `a` for `/`
  // and 0 for `%`, so the runtime special-cases it instead of faulting.
  cg.movImm(R10, kInt64Min);
  cg.cmpReg(Rsi, R10);
  cg.jcc(Jcc::Ne, intNormalDiv);
  cg.cmpImm32(Rcx, -1);
  cg.jcc(Jcc::Ne, intNormalDiv);
  cg.jmp(intSpecial);
  cg.bind(intSpecial);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, intSpecialMod);
  cg.movReg(Rax, Rsi);  // quotient = a (INT64_MIN)
  cg.jmp(intDone);
  cg.bind(intSpecialMod);
  cg.xorReg(Rax, Rax);  // remainder = 0
  cg.jmp(intDone);
  cg.bind(intNormalDiv);
  cg.movReg(Rax, Rsi);
  cg.divReg(Rcx);  // rax = quotient, rdx = remainder
  cg.testReg(R9);
  cg.jcc(Jcc::Ne, intDone);
  cg.movReg(Rax, Rdx);
  cg.jmp(intDone);

  cg.bind(intDone);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(intDivZero);
  emitFailCall(cg, fail, msgDivZero);

  // --- float/double path ---
  cg.bind(floatPath);
  emitToDouble(cg, Rdi, Rsi, Xmm0);  // xmm0 = a
  emitToDouble(cg, Rdx, Rcx, Xmm1);  // xmm1 = b
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Eq, floatAdd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Sub));
  cg.jcc(Jcc::Eq, floatSub);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mul));
  cg.jcc(Jcc::Eq, floatMul);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Div));
  cg.jcc(Jcc::Eq, floatDiv);
  cg.jmp(bad);  // float `%` needs fmod (7c.1c)

  cg.bind(floatAdd);
  cg.addsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatSub);
  cg.subsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatMul);
  cg.mulsd(Xmm0, Xmm1);
  cg.jmp(floatDone);

  cg.bind(floatDiv);
  cg.xorps(Xmm2, Xmm2);  // xmm2 = 0.0
  cg.ucomisd(Xmm1, Xmm2);
  cg.jcc(Jcc::P, floatNotZero);  // unordered (NaN) is not zero
  cg.jcc(Jcc::Eq, floatDivZero);
  cg.bind(floatNotZero);
  cg.divsd(Xmm0, Xmm1);
  cg.jmp(floatDone);
  cg.bind(floatDivZero);
  emitFailCall(cg, fail, msgDivZero);

  cg.bind(floatDone);
  // The result is a double unless neither operand is one, in which case it
  // rounds back to binary32.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, useDouble);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, useDouble);
  cg.jmp(useFloat);
  cg.bind(useFloat);
  cg.cvtsd2ss(Xmm0, Xmm0);
  cg.cvtss2sd(Xmm0, Xmm0);  // store the binary32 value widened, as the hosted Value does
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagFloat);
  epilogue(cg);
  cg.bind(useDouble);
  cg.movqFromXmm(Rdx, Xmm0);
  cg.movImm(Rax, kTagDouble);
  epilogue(cg);

  cg.bind(bad);
  emitFailCall(cg, fail, msgTypes);
}

// `AotValue jolt_pow(AotValue a, AotValue b)`: integer `**` for int/int with a
// non-negative exponent, replicating `intPow` (wrapping multiply, square and
// multiply). Every other case needs `pow` from libm and is deferred.
void emitPow(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
             const DataBlob& msgPow) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label skipMul = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.testReg(Rcx);
  cg.jcc(Jcc::S, bad);  // negative exponent -> std::pow (7c.1c)

  cg.movReg(R8, Rcx);  // exp = b
  cg.movReg(R9, Rsi);  // factor = a
  cg.movImm(Rax, 1);   // result = 1
  cg.movImm(Rcx, 1);   // cl = 1, the shift amount for the loop
  cg.bind(loop);
  cg.testImm32(R8, 1);
  cg.jcc(Jcc::Eq, skipMul);
  cg.imulReg(Rax, R9);
  cg.bind(skipMul);
  cg.imulReg(R9, R9);
  cg.shrCl(R8);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, loop);

  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(bad);
  emitFailCall(cg, fail, msgPow);
}

// `AotValue jolt_shift(AotValue a, AotValue b, long op)`: integer `<<`/`>>`.
// The count must be an int in 0..63 (the hosted range check); `>>` is
// arithmetic, so it preserves the sign.
void emitShift(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgShift) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label doShl = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.testReg(Rcx);
  cg.jcc(Jcc::S, bad);  // negative count
  cg.cmpImm32(Rcx, 63);
  cg.jcc(Jcc::Gt, bad);
  cg.movReg(Rax, Rsi);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotShiftOp::Shl));
  cg.jcc(Jcc::Eq, doShl);
  cg.sarCl(Rax);
  cg.jmp(done);
  cg.bind(doShl);
  cg.shlCl(Rax);
  cg.bind(done);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(bad);
  emitFailCall(cg, fail, msgShift);
}

// `AotValue jolt_bitwise(AotValue a, AotValue b, long op)`: int-only `&`, `|`
// and `^`, selected by `op` in r8.
void emitBitwise(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label isAnd = cg.newLabel();
  CodeGen::Label isOr = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::And));
  cg.jcc(Jcc::Eq, isAnd);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotBitwiseOp::Or));
  cg.jcc(Jcc::Eq, isOr);
  cg.xorReg(Rsi, Rcx);  // Xor is the fall-through case
  cg.jmp(done);
  cg.bind(isAnd);
  cg.andReg(Rsi, Rcx);
  cg.jmp(done);
  cg.bind(isOr);
  cg.orReg(Rsi, Rcx);
  cg.bind(done);
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, Rsi);
  epilogue(cg);
  cg.bind(bad);
  emitFailCall(cg, fail, message);
}

// `AotValue jolt_negate(AotValue a)`: arithmetic negation of an int (wrapping)
// or a float/double (sign-bit flip, exact for every value including NaN).
void emitNegate(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                const DataBlob& message) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asFloat = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asFloat);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asFloat);
  cg.jmp(bad);

  cg.bind(asInt);
  cg.movReg(Rdx, Rsi);
  cg.negReg(Rdx);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);

  cg.bind(asFloat);
  cg.movReg(R10, Rsi);
  cg.movImm(R11, kDoubleSignBit);
  cg.xorReg(R10, R11);  // flip the sign bit
  cg.movReg(Rdx, R10);
  cg.movReg(Rax, Rdi);  // the tag is unchanged (Float or Double)
  epilogue(cg);

  cg.bind(bad);
  emitFailCall(cg, fail, message);
}

// `AotValue jolt_bit_not(AotValue a)`: int-only `~`.
void emitBitNot(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                const DataBlob& message) {
  CodeGen::Label bad = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.movReg(Rdx, Rsi);
  cg.notReg(Rdx);
  cg.movImm(Rax, kTagInt);
  epilogue(cg);
  cg.bind(bad);
  emitFailCall(cg, fail, message);
}

// `AotValue jolt_not(AotValue a)`: logical not of the value's truthiness,
// always a bool.
void emitNot(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label isBool = cg.newLabel();
  CodeGen::Label isNil = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, isBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, isNil);
  cg.xorReg(Rdx, Rdx);  // any other value is truthy, so not(x) is false
  cg.jmp(done);
  cg.bind(isBool);
  cg.xorReg(Rdx, Rdx);
  cg.testReg(Rsi);
  cg.setcc(Jcc::Eq, Rdx);  // payload == 0 (false) -> not is true
  cg.movzx(Rdx, Rdx);
  cg.jmp(done);
  cg.bind(isNil);
  cg.movImm(Rdx, 1);
  cg.bind(done);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);
}

// `AotValue jolt_compare(AotValue a, AotValue b, long op)`: equality for any
// pair and ordering for int/int, float/double and bool/bool. Equality of two
// numbers is numeric (`1 == 1.0` is true), matching `Value::equals`. The result
// is always a bool. The op selector is the low nibble of the x86-64 condition
// code of the matching comparison.
void emitCompare(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message) {
  CodeGen::Label eqBranch = cg.newLabel();
  CodeGen::Label neBranch = cg.newLabel();
  CodeGen::Label order = cg.newLabel();
  CodeGen::Label boolResult = cg.newLabel();
  CodeGen::Label orderInt = cg.newLabel();
  CodeGen::Label orderMixed = cg.newLabel();
  CodeGen::Label orderBool = cg.newLabel();
  CodeGen::Label orderSelect = cg.newLabel();
  CodeGen::Label selLt = cg.newLabel();
  CodeGen::Label selGt = cg.newLabel();
  CodeGen::Label selLe = cg.newLabel();
  CodeGen::Label selGe = cg.newLabel();
  CodeGen::Label orderFinish = cg.newLabel();
  CodeGen::Label badCompare = cg.newLabel();
  CodeGen::Label abDone = cg.newLabel();
  CodeGen::Label baDone = cg.newLabel();
  CodeGen::Label abDoneBool = cg.newLabel();
  CodeGen::Label baDoneBool = cg.newLabel();
  CodeGen::Label eqHelper = cg.newLabel();
  CodeGen::Label eqNonNumeric = cg.newLabel();
  CodeGen::Label eqFloat = cg.newLabel();
  CodeGen::Label eqReturn = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Eq));
  cg.jcc(Jcc::Eq, eqBranch);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ne));
  cg.jcc(Jcc::Eq, neBranch);
  cg.jmp(order);

  cg.bind(eqBranch);
  cg.callLabel(eqHelper);
  cg.jmp(boolResult);
  cg.bind(neBranch);
  cg.callLabel(eqHelper);
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(boolResult);
  cg.bind(boolResult);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);

  // --- ordering ---
  cg.bind(order);
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, orderBool);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, orderBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, orderMixed);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, orderMixed);
  cg.jmp(orderInt);

  cg.bind(orderInt);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rsi, Rcx);
  cg.setcc(Jcc::Lt, Rax);  // ab = a < b
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.cmpReg(Rcx, Rsi);
  cg.setcc(Jcc::Lt, R11);  // ba = b < a
  cg.movzx(R11, R11);
  cg.jmp(orderSelect);

  cg.bind(orderMixed);
  emitToDouble(cg, Rdi, Rsi, Xmm0);
  emitToDouble(cg, Rdx, Rcx, Xmm1);
  cg.xorReg(Rax, Rax);
  cg.ucomisd(Xmm0, Xmm1);
  cg.jcc(Jcc::P, abDone);   // unordered -> ab = 0
  cg.jcc(Jcc::Ae, abDone);  // CF=0 -> not less
  cg.movImm(Rax, 1);
  cg.bind(abDone);
  cg.xorReg(R11, R11);
  cg.ucomisd(1, 0);
  cg.jcc(Jcc::P, baDone);
  cg.jcc(Jcc::Ae, baDone);
  cg.movImm(R11, 1);
  cg.bind(baDone);
  cg.jmp(orderSelect);

  cg.bind(orderBool);
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, badCompare);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Ne, badCompare);
  cg.xorReg(Rax, Rax);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Ne, abDoneBool);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, abDoneBool);
  cg.movImm(Rax, 1);  // ab = !a && b
  cg.bind(abDoneBool);
  cg.xorReg(R11, R11);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Ne, baDoneBool);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, baDoneBool);
  cg.movImm(R11, 1);  // ba = !b && a
  cg.bind(baDoneBool);
  cg.jmp(orderSelect);

  cg.bind(orderSelect);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Lt));
  cg.jcc(Jcc::Eq, selLt);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Gt));
  cg.jcc(Jcc::Eq, selGt);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Le));
  cg.jcc(Jcc::Eq, selLe);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ge));
  cg.jcc(Jcc::Eq, selGe);
  cg.jmp(badCompare);
  cg.bind(selLt);
  cg.jmp(orderFinish);  // rax already holds ab
  cg.bind(selGt);
  cg.movReg(Rax, R11);
  cg.jmp(orderFinish);
  cg.bind(selLe);  // !ba
  cg.testReg(R11);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(orderFinish);
  cg.bind(selGe);  // !ab
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.bind(orderFinish);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);

  cg.bind(badCompare);
  emitFailCall(cg, fail, message);

  // --- internal subroutine: rax = (a == b) ---
  // Not reached by fall-through: every branch above ends in `ret` or a call to
  // `fail` that never returns. Called with the operands still in rdi:rsi/rdx:rcx.
  cg.bind(eqHelper);
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqNonNumeric);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, eqNonNumeric);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, eqFloat);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, eqFloat);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rsi, Rcx);
  cg.setcc(Jcc::Eq, Rax);
  cg.movzx(Rax, Rax);
  cg.jmp(eqReturn);
  cg.bind(eqFloat);
  emitToDouble(cg, Rdi, Rsi, Xmm0);
  emitToDouble(cg, Rdx, Rcx, Xmm1);
  cg.xorReg(Rax, Rax);
  cg.ucomisd(Xmm0, Xmm1);
  cg.jcc(Jcc::P, eqReturn);   // unordered -> not equal
  cg.jcc(Jcc::Ne, eqReturn);  // ZF=0 -> not equal
  cg.movImm(Rax, 1);
  cg.jmp(eqReturn);
  cg.bind(eqNonNumeric);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, eqReturn);
  cg.cmpReg(Rsi, Rcx);
  cg.jcc(Jcc::Ne, eqReturn);
  cg.movImm(Rax, 1);
  cg.bind(eqReturn);
  cg.retInsn();
}

}  // namespace

void emitAotRuntime(CodeGen& cg, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry) {
  // Constant text, bound after all code so nothing falls through into it.
  std::vector<DataBlob> data;
  auto addText = [&](const char* text) {
    DataBlob blob{cg.newLabel(), std::string(text)};
    data.push_back(std::move(blob));
    return data.back();
  };
  // Copy the blobs out so later `data.push_back` reallocations cannot
  // invalidate a reference held by an emit helper below.
  DataBlob msgTypes = addText("jolt: unsupported operand types\n");
  DataBlob msgPow = addText("jolt: unsupported operand types for '**'\n");
  DataBlob msgDivZero = addText("jolt: division by zero\n");
  DataBlob msgShift = addText("jolt: shift count must be between 0 and 63\n");
  DataBlob msgBit = addText("jolt: unsupported operand types for a bitwise operator\n");
  DataBlob msgNeg = addText("jolt: unsupported operand type for unary '-'\n");
  DataBlob msgBitNot = addText("jolt: unsupported operand type for unary '~'\n");
  DataBlob msgCompare = addText("jolt: cannot compare these values\n");
  DataBlob msgPrint = addText("jolt: cannot print this value\n");
  DataBlob textTrue = addText("true");
  DataBlob textFalse = addText("false");
  DataBlob textNil = addText("nil");
  DataBlob textNewline = addText("\n");

  CodeGen::Label fail = cg.newLabel();
  CodeGen::Label writeInt = cg.newLabel();

  emitFail(cg, fail, entry.write, entry.exit);
  emitWriteInt(cg, writeInt, entry.write);
  emitPrintValue(cg, handles.printValue, entry.write, fail, writeInt, msgPrint,
                 textTrue, textFalse, textNil, textNewline);
  emitTruthy(cg, handles.truthy);
  emitArith(cg, handles.arithValues, fail, msgTypes, msgDivZero);
  emitPow(cg, handles.powValues, fail, msgPow);
  emitShift(cg, handles.shiftValues, fail, msgShift);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit);
  emitNegate(cg, handles.negateValue, fail, msgNeg);
  emitBitNot(cg, handles.bitNotValue, fail, msgBitNot);
  emitNot(cg, handles.notValue);
  emitCompare(cg, handles.compareValues, fail, msgCompare);

  // Data islands last: every code path above ends in `ret` or `ud2`, so
  // control never reaches them, and their addresses are already referenced by
  // the `lea` fixups emitted above.
  for (const DataBlob& blob : data) {
    cg.bind(blob.label);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(blob.text.data()),
                 blob.text.size());
  }
}
