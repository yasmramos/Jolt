#include "native/runtime.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "native/dtoa.h"

namespace {

// Shorthand for the registers the runtime uses, so the emit helpers read like
// assembly instead of `X64Reg::Rax` everywhere. C++17 has no `using enum`, so
// they are plain constants.
constexpr X64Reg Rax = X64Reg::Rax;
constexpr X64Reg Rcx = X64Reg::Rcx;
constexpr X64Reg Rdx = X64Reg::Rdx;
constexpr X64Reg Rbx = X64Reg::Rbx;
constexpr X64Reg Rsi = X64Reg::Rsi;
constexpr X64Reg Rdi = X64Reg::Rdi;
constexpr X64Reg R8 = X64Reg::R8;
constexpr X64Reg R9 = X64Reg::R9;
constexpr X64Reg R10 = X64Reg::R10;
constexpr X64Reg R11 = X64Reg::R11;
constexpr X64Reg R12 = X64Reg::R12;
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsp = X64Reg::Rsp;

constexpr std::int64_t kTagNil = static_cast<std::int64_t>(AotTag::Nil);
constexpr std::int64_t kTagBool = static_cast<std::int64_t>(AotTag::Bool);
constexpr std::int64_t kTagInt = static_cast<std::int64_t>(AotTag::Int);
constexpr std::int64_t kTagFloat = static_cast<std::int64_t>(AotTag::Float);
constexpr std::int64_t kTagDouble = static_cast<std::int64_t>(AotTag::Double);
constexpr std::int64_t kTagStr = static_cast<std::int64_t>(AotTag::Str);

// Byte offset of a string's first byte from its `AotString` header (see
// `native/value.h`): the length is an 8-byte word, the bytes follow.
constexpr std::int64_t kStringDataOffset = static_cast<std::int64_t>(kAotStringHeaderSize);

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

// Stack frame `jolt_print_value` builds for a Float/Double: the dtoa `Scratch`
// block followed by a text buffer. Both sizes are rounded so the frame is a
// multiple of 16, keeping rsp 16-byte aligned at the `call` to the formatter
// (the ABI requires it, and the formatter's own `call`s rely on it).
constexpr std::int64_t kDtoaScratchBytes = static_cast<std::int64_t>(dtoa::kScratchBytes);
constexpr std::int64_t kDtoaTextBytes = 400;
constexpr std::int64_t kDtoaFrameBytes = kDtoaScratchBytes + kDtoaTextBytes;

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

// `void jolt_print_value(AotValue v)`: prints the value followed by a newline.
// int, bool, nil and str render directly; a Float/Double is formatted with the
// embedded dtoa (`jolt_format_double`) into an arena buffer, then written.
void emitPrintValue(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                    CodeGen::Label fail, CodeGen::Label writeInt,
                    CodeGen::Label formatDouble, const DataBlob& badValue,
                    const DataBlob& textTrue, const DataBlob& textFalse,
                    const DataBlob& textNil, const DataBlob& textNewline) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
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
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  // The remaining tags are Float (3) and Double (4); everything else is not
  // printable.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  emitFailCall(cg, fail, badValue);

  cg.bind(asInt);
  cg.movReg(Rdi, Rsi);
  cg.callLabel(writeInt);
  cg.jmp(newline);

  // A Float/Double: format into an arena buffer with the embedded dtoa, then
  // write the returned length. The `Scratch` block and the text buffer live on
  // the stack for the call; `kDtoaScratchBytes` is padded to a multiple of 16
  // so rsp stays 16-byte aligned (the ABI requirement at every `call`).
  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movReg(Rcx, Rsp);                        // scratch = rsp
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out = rsp + scratch
  cg.callLabel(formatDouble);                 // rdi=tag, rsi=payload (unchanged)
  cg.movReg(Rsi, Rax);                        // length
  cg.movReg(Rdi, Rsp);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kDtoaScratchBytes));
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  inlineWriteStdout(cg, writeLabel);
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
  cg.jmp(newline);

  // A string is its bytes verbatim: print writes the payload's `length` bytes
  // starting right after the header. No quoting, matching `Value::toString`.
  // `inlineWriteStdout` takes (rdi = buffer, rsi = length).
  cg.bind(asStr);
  cg.movFromMem(Rcx, Rsi, 0);  // length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rdi, Rsi);         // buffer = payload + 8
  cg.movReg(Rsi, Rcx);         // length
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
// exactly like the hosted engine. `Add` on two strings concatenates (7c.1c);
// `%` on floats uses x87 `fprem` (7c.1d.1).
void emitArith(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
               const DataBlob& msgTypes, const DataBlob& msgDivZero,
               CodeGen::Label concatLabel) {
  CodeGen::Label bad = cg.newLabel();
  CodeGen::Label intPath = cg.newLabel();
  CodeGen::Label floatPath = cg.newLabel();
  CodeGen::Label arithNumbers = cg.newLabel();
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
  CodeGen::Label floatMod = cg.newLabel();
  CodeGen::Label floatModGo = cg.newLabel();
  CodeGen::Label floatModLoop = cg.newLabel();
  CodeGen::Label floatDivZero = cg.newLabel();
  CodeGen::Label floatNotZero = cg.newLabel();
  CodeGen::Label floatDone = cg.newLabel();
  CodeGen::Label useFloat = cg.newLabel();
  CodeGen::Label useDouble = cg.newLabel();

  cg.bind(label);
  prologue(cg);

  // Both operands numbers -> numeric arithmetic (below). Both strings with `+`
  // -> concatenation. Anything else is an unsupported operand pair.
  emitIsNumberFlag(cg, Rdi);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, arithNumbers);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Add));
  cg.jcc(Jcc::Ne, bad);
  cg.callLabel(concatLabel);  // returns an AotValue in rax:rdx
  epilogue(cg);

  cg.bind(arithNumbers);
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
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotArithOp::Mod));
  cg.jcc(Jcc::Eq, floatMod);
  cg.jmp(bad);  // float `**` needs a correctly rounded pow (7c.1d.2)

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

  // float/double `%`: the truncated remainder, matching `std::fmod` (the sign
  // follows the dividend). SSE has no remainder instruction, so the operands
  // cross to the x87 unit through the stack. `fprem` produces a partial
  // remainder and sets C2 while the result is not yet complete; the loop runs
  // until C2 is clear. Dividing by zero is the same error as `/` (the hosted
  // engine checks `right.number() == 0` before `fmod`). The x87 stack is
  // drained on every path, so the rest of the backend keeps seeing a clean FPU.
  cg.bind(floatMod);
  cg.xorps(Xmm2, Xmm2);  // xmm2 = 0.0
  cg.ucomisd(Xmm1, Xmm2);
  cg.jcc(Jcc::P, floatModGo);  // unordered (NaN) divisor is not zero
  cg.jcc(Jcc::Eq, floatDivZero);
  cg.bind(floatModGo);
  cg.subRegImm32(Rsp, 16);         // scratch for st(1), kept 16-byte aligned
  cg.movsdFromXmm(Rsp, 0, Xmm0);   // [rsp] = a
  cg.movsdFromXmm(Rsp, 8, Xmm1);   // [rsp+8] = b
  cg.fldM64(Rsp, 8);               // st0 = b
  cg.fldM64(Rsp, 0);               // st0 = a, st1 = b
  cg.bind(floatModLoop);
  cg.fprem();                      // st0 = partial remainder
  cg.fnstswAx();
  cg.testAhImm8(0x04);             // C2 -> incomplete, keep reducing
  cg.jcc(Jcc::Ne, floatModLoop);
  cg.fstpM64(Rsp, 0);              // [rsp] = result, pop st0
  cg.fstpSt0();                    // pop st1 (the divisor)
  cg.movsdToXmm(Xmm0, Rsp, 0);
  cg.addImm32(Rsp, 16);
  cg.jmp(floatDone);

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
  CodeGen::Label orderNonNumeric = cg.newLabel();
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
  CodeGen::Label eqIdentity = cg.newLabel();
  CodeGen::Label eqFloat = cg.newLabel();
  CodeGen::Label eqReturn = cg.newLabel();
  CodeGen::Label strEquals = cg.newLabel();
  CodeGen::Label strEqualsLoop = cg.newLabel();
  CodeGen::Label strEqualsSame = cg.newLabel();
  CodeGen::Label strEqualsDone = cg.newLabel();
  CodeGen::Label strOrder = cg.newLabel();
  CodeGen::Label strOrderLoop = cg.newLabel();
  CodeGen::Label strOrderByteDiffers = cg.newLabel();
  CodeGen::Label strOrderAExhausted = cg.newLabel();
  CodeGen::Label strOrderBExhausted = cg.newLabel();

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
  cg.jcc(Jcc::Eq, orderNonNumeric);
  emitIsNumberFlag(cg, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, orderNonNumeric);
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

  // --- non-numeric ordering: strings compare lexicographically, bools by
  // value; any other pair cannot be ordered. ---
  cg.bind(orderNonNumeric);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, orderBool);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, badCompare);
  cg.callLabel(strOrder);
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
  // Two strings compare by content, not by pointer; every other non-numeric
  // pair is identity (nil == nil, bool == bool, same object).
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, eqIdentity);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, eqIdentity);
  cg.callLabel(strEquals);
  cg.jmp(eqReturn);
  cg.bind(eqIdentity);
  cg.xorReg(Rax, Rax);
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, eqReturn);
  cg.cmpReg(Rsi, Rcx);
  cg.jcc(Jcc::Ne, eqReturn);
  cg.movImm(Rax, 1);
  cg.bind(eqReturn);
  cg.retInsn();

  // --- internal subroutine: rax = (a == b) for two strings ---
  // rsi/rcx hold the two `AotString` pointers. Equal lengths and equal bytes.
  cg.bind(strEquals);
  cg.movFromMem(R8, Rsi, 0);   // lenA
  cg.movFromMem(R9, Rcx, 0);   // lenB
  cg.xorReg(Rax, Rax);         // result = 0
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, strEqualsDone);
  cg.xorReg(R10, R10);         // index
  cg.bind(strEqualsLoop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, strEqualsSame);  // index >= len -> equal
  cg.movReg(R11, R10);
  cg.addReg(R11, Rsi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.movReg(R9, R10);
  cg.addReg(R9, Rcx);
  cg.movzxFromMem(R9, R9, kStringDataOffset);    // b byte
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ne, strEqualsDone);  // a byte differs -> not equal
  cg.addImm32(R10, 1);
  cg.jmp(strEqualsLoop);
  cg.bind(strEqualsSame);
  cg.movImm(Rax, 1);
  cg.bind(strEqualsDone);
  cg.retInsn();

  // --- internal subroutine: rax = (a < b), r11 = (b < a) for two strings ---
  // Lexicographic over unsigned bytes; a proper prefix is smaller. rsi/rcx hold
  // the two `AotString` pointers. The selector in r8 must survive (orderSelect
  // reads it), so this uses only r9/rdi/rdx/r10/r11 as scratch.
  cg.bind(strOrder);
  cg.movFromMem(R9, Rsi, 0);   // lenA
  cg.movFromMem(Rdi, Rcx, 0);  // lenB
  cg.xorReg(R10, R10);         // index
  cg.bind(strOrderLoop);
  cg.cmpReg(R10, R9);
  cg.jcc(Jcc::Ae, strOrderAExhausted);  // index >= lenA
  cg.cmpReg(R10, Rdi);
  cg.jcc(Jcc::Ae, strOrderBExhausted);  // index >= lenB
  cg.movReg(Rdx, R10);
  cg.addReg(Rdx, Rcx);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);  // b byte
  cg.movReg(R11, R10);
  cg.addReg(R11, Rsi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.cmpReg(R11, Rdx);
  cg.jcc(Jcc::Ne, strOrderByteDiffers);
  cg.addImm32(R10, 1);
  cg.jmp(strOrderLoop);
  cg.bind(strOrderByteDiffers);
  // A byte at `index` differs (the loop only reaches here on inequality), so
  // the bytes are ordered one way or the other. Set `ab` from the unsigned
  // comparison, then `ba = !ab`. `setcc` reads the flags, so nothing may
  // clobber them between the `cmp` and the `setcc`.
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R11, Rdx);
  cg.setcc(Jcc::B, Rax);   // ab = aByte < bByte
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.testReg(Rax);
  cg.setcc(Jcc::Eq, R11);  // ba = !ab (the bytes differ)
  cg.movzx(R11, R11);
  cg.retInsn();
  cg.bind(strOrderAExhausted);
  // a ran out first: a < b iff b has bytes left, i.e. lenA < lenB.
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R9, Rdi);
  cg.setcc(Jcc::B, Rax);   // ab = lenA < lenB
  cg.movzx(Rax, Rax);
  cg.xorReg(R11, R11);
  cg.retInsn();
  cg.bind(strOrderBExhausted);
  // b ran out first (and a has bytes left): b < a.
  cg.xorReg(Rax, Rax);
  cg.movImm(R11, 1);
  cg.retInsn();
}

// `void* jolt_alloc(long size)`: bump allocation from the arena seeded by
// `jolt_init_arena`. The cursor and end live in r13/r14 (see `native/entry.h`);
// this helper reads and advances the cursor only. Returns an 8-byte-aligned
// pointer, or null when the region is exhausted (mirroring `Arena::allocate`).
void emitAlloc(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label exhausted = cg.newLabel();

  cg.bind(label);
  cg.movReg(Rax, kAotArenaCursor);
  cg.addImm32(Rax, 7);
  cg.movImm(R10, -8);
  cg.andReg(Rax, R10);      // rax = cursor aligned up to 8
  cg.movReg(R11, Rax);
  cg.addReg(R11, Rdi);      // r11 = new cursor
  cg.jcc(Jcc::B, exhausted);        // wrapped: size was absurd
  cg.cmpReg(R11, kAotArenaEnd);
  cg.jcc(Jcc::A, exhausted);        // past the end
  cg.movReg(kAotArenaCursor, R11);
  cg.retInsn();
  cg.bind(exhausted);
  cg.xorReg(Rax, Rax);
  cg.retInsn();
}

// `AotValue jolt_concat(AotValue a, AotValue b)`: allocate lenA+lenB+8 bytes,
// write the combined length and the two byte runs, and return a Str value
// (payload = the new `AotString`). `a` is rdi:rsi, `b` is rdx:rcx. rbx and r12
// are callee-saved, so they are preserved across the helper.
void emitConcat(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);
  cg.push(R12);

  cg.movReg(Rbx, Rcx);            // rbx = b's AotString
  cg.movFromMem(R12, Rsi, 0);     // r12 = lenA
  cg.movFromMem(Rax, Rbx, 0);     // lenB
  cg.addReg(R12, Rax);            // r12 = lenA + lenB
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(allocLabel);       // rax = result pointer (or null)
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);       // store the combined length
  // Copy A's bytes: dst = ptr + 8, src = a's bytes, count = lenA.
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rcx, Rsi, 0);     // lenA
  cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.repMovsb();
  // Copy B's bytes: dst = current rdi, src = b's bytes, count = lenB.
  cg.movReg(Rsi, Rbx);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rcx, Rbx, 0);     // lenB
  cg.repMovsb();
  // The result value: tag Str, payload = the pointer (still in rax).
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
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
  DataBlob msgOom = addText("jolt: out of memory\n");
  DataBlob textTrue = addText("true");
  DataBlob textFalse = addText("false");
  DataBlob textNil = addText("nil");
  DataBlob textNewline = addText("\n");

  CodeGen::Label fail = cg.newLabel();
  CodeGen::Label writeInt = cg.newLabel();

  // The float/double formatter is part of the embedded runtime: bind its
  // handles and emit the whole dtoa routine so `jolt_print_value` can call it.
  dtoa::DtoaHandles dtoaHandles = dtoa::newDtoaHandles(cg);

  emitFail(cg, fail, entry.write, entry.exit);
  emitWriteInt(cg, writeInt, entry.write);
  emitAlloc(cg, handles.alloc);
  emitConcat(cg, handles.concat, handles.alloc, fail, msgOom);
  emitPrintValue(cg, handles.printValue, entry.write, fail, writeInt,
                 dtoaHandles.formatDouble, msgPrint, textTrue, textFalse, textNil,
                 textNewline);
  emitTruthy(cg, handles.truthy);
  emitArith(cg, handles.arithValues, fail, msgTypes, msgDivZero, handles.concat);
  emitPow(cg, handles.powValues, fail, msgPow);
  emitShift(cg, handles.shiftValues, fail, msgShift);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit);
  emitNegate(cg, handles.negateValue, fail, msgNeg);
  emitBitNot(cg, handles.bitNotValue, fail, msgBitNot);
  emitNot(cg, handles.notValue);
  emitCompare(cg, handles.compareValues, fail, msgCompare);
  dtoa::emitDtoa(cg, dtoaHandles);

  // Data islands last: every code path above ends in `ret` or `ud2`, so
  // control never reaches them, and their addresses are already referenced by
  // the `lea` fixups emitted above.
  for (const DataBlob& blob : data) {
    cg.bind(blob.label);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(blob.text.data()),
                 blob.text.size());
  }
}
