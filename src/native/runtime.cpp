#include "native/runtime.h"

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
constexpr X64Reg Rbp = X64Reg::Rbp;
constexpr X64Reg Rsp = X64Reg::Rsp;

constexpr std::int64_t kTagNil = static_cast<std::int64_t>(AotTag::Nil);
constexpr std::int64_t kTagBool = static_cast<std::int64_t>(AotTag::Bool);
constexpr std::int64_t kTagInt = static_cast<std::int64_t>(AotTag::Int);

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
// newline. Floats and strings are not representable yet (PHASE 7c.1b).
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

// Emits `AotValue name(AotValue a, AotValue b)` for `+`, `-` and `*`, which
// are int-only for now. On any other pair it reports the same class of error
// the hosted `ops::binary` reports and exits.
void emitBinaryInt(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                   const DataBlob& message,
                   void (CodeGen::*op)(X64Reg, X64Reg)) {
  CodeGen::Label bad = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  (cg.*op)(Rsi, Rcx);  // rsi is a.payload, rcx is b.payload
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, Rsi);
  epilogue(cg);
  cg.bind(bad);
  emitFailCall(cg, fail, message);
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

// `AotValue jolt_negate(AotValue a)` / `jolt_bit_not(AotValue a)`: int-only
// unary operations. Both share the tag check.
void emitUnaryInt(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                  const DataBlob& message, void (CodeGen::*op)(X64Reg)) {
  CodeGen::Label bad = cg.newLabel();
  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, bad);
  cg.movReg(Rdx, Rsi);
  (cg.*op)(Rdx);
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
// pair, ordering for int/int and bool/bool. The result is always a bool. The
// op selector is the low nibble of the matching x86-64 condition code.
void emitCompare(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                 const DataBlob& message) {
  CodeGen::Label eq = cg.newLabel();
  CodeGen::Label ne = cg.newLabel();
  CodeGen::Label order = cg.newLabel();
  CodeGen::Label intOrder = cg.newLabel();
  CodeGen::Label boolOrder = cg.newLabel();
  CodeGen::Label badCompare = cg.newLabel();
  CodeGen::Label result = cg.newLabel();

  // Equality: same tag and same payload. Numbers are all ints here, so no
  // cross-type numeric promotion is needed yet.
  auto emitEquality = [&](CodeGen::Label target, bool negate) {
    CodeGen::Label different = cg.newLabel();
    CodeGen::Label finish = cg.newLabel();
    cg.bind(target);
    cg.xorReg(Rax, Rax);
    cg.cmpReg(Rdi, Rdx);
    cg.jcc(Jcc::Ne, different);
    cg.cmpReg(Rsi, Rcx);
    cg.jcc(Jcc::Ne, different);
    if (negate) {
      cg.jmp(finish);  // equal: Ne is false, leave rax = 0
    } else {
      cg.movImm(Rax, 1);
      cg.jmp(finish);
    }
    cg.bind(different);
    if (negate) cg.movImm(Rax, 1);
    cg.bind(finish);
    cg.movReg(Rdx, Rax);
    cg.movImm(Rax, kTagBool);
    epilogue(cg);
  };

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Eq));
  cg.jcc(Jcc::Eq, eq);
  cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ne));
  cg.jcc(Jcc::Eq, ne);
  cg.jmp(order);

  emitEquality(eq, false);
  emitEquality(ne, true);

  // Ordering: both operands must share a tag, and that tag must be int or
  // bool. Anything else is the hosted engine's "cannot compare" error.
  cg.bind(order);
  cg.cmpReg(Rdi, Rdx);
  cg.jcc(Jcc::Ne, badCompare);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, intOrder);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, boolOrder);
  cg.jmp(badCompare);

  // int/int: signed compare, then select the boolean for the requested op.
  {
    CodeGen::Label lt = cg.newLabel();
    CodeGen::Label gt = cg.newLabel();
    CodeGen::Label le = cg.newLabel();
    CodeGen::Label ge = cg.newLabel();
    cg.bind(intOrder);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Lt));
    cg.jcc(Jcc::Eq, lt);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Gt));
    cg.jcc(Jcc::Eq, gt);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Le));
    cg.jcc(Jcc::Eq, le);
    cg.cmpImm32(R8, static_cast<std::int32_t>(AotCompareOp::Ge));
    cg.jcc(Jcc::Eq, ge);
    cg.jmp(badCompare);
    auto select = [&](CodeGen::Label target, Jcc cc) {
      cg.bind(target);
      cg.cmpReg(Rsi, Rcx);
      cg.setcc(cc, Rax);
      cg.movzx(Rax, Rax);
      cg.jmp(result);
    };
    select(lt, Jcc::Lt);
    select(gt, Jcc::Gt);
    select(le, Jcc::Le);
    select(ge, Jcc::Ge);
  }

  // bool/bool: the hosted engine orders `false < true` through `!a && b`.
  {
    CodeGen::Label ltDone = cg.newLabel();
    CodeGen::Label gtDone = cg.newLabel();
    CodeGen::Label selLt = cg.newLabel();
    CodeGen::Label selGt = cg.newLabel();
    CodeGen::Label selLe = cg.newLabel();
    CodeGen::Label selGe = cg.newLabel();
    cg.bind(boolOrder);
    // r9 = (!a && b), the "a < b" result.
    cg.xorReg(R9, R9);
    cg.testReg(Rsi);
    cg.jcc(Jcc::Ne, ltDone);
    cg.testReg(Rcx);
    cg.jcc(Jcc::Eq, ltDone);
    cg.movImm(R9, 1);
    cg.bind(ltDone);
    // r10 = (!b && a), the "a > b" result.
    cg.xorReg(R10, R10);
    cg.testReg(Rcx);
    cg.jcc(Jcc::Ne, gtDone);
    cg.testReg(Rsi);
    cg.jcc(Jcc::Eq, gtDone);
    cg.movImm(R10, 1);
    cg.bind(gtDone);
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
    cg.movReg(Rax, R9);
    cg.jmp(result);
    cg.bind(selGt);
    cg.movReg(Rax, R10);
    cg.jmp(result);
    cg.bind(selLe);  // !(a > b)
    cg.xorReg(Rax, Rax);
    cg.testReg(R10);
    cg.setcc(Jcc::Eq, Rax);
    cg.movzx(Rax, Rax);
    cg.jmp(result);
    cg.bind(selGe);  // !(a < b)
    cg.xorReg(Rax, Rax);
    cg.testReg(R9);
    cg.setcc(Jcc::Eq, Rax);
    cg.movzx(Rax, Rax);
    cg.jmp(result);
  }

  cg.bind(result);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagBool);
  epilogue(cg);

  cg.bind(badCompare);
  emitFailCall(cg, fail, message);
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
  DataBlob msgAdd = addText("jolt: unsupported operand types for '+'\n");
  DataBlob msgSub = addText("jolt: unsupported operand types for '-'\n");
  DataBlob msgMul = addText("jolt: unsupported operand types for '*'\n");
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
  emitBinaryInt(cg, handles.addValues, fail, msgAdd, &CodeGen::addReg);
  emitBinaryInt(cg, handles.subValues, fail, msgSub, &CodeGen::subReg);
  emitBinaryInt(cg, handles.mulValues, fail, msgMul, &CodeGen::imulReg);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit);
  emitUnaryInt(cg, handles.negateValue, fail, msgNeg, &CodeGen::negReg);
  emitUnaryInt(cg, handles.bitNotValue, fail, msgBitNot, &CodeGen::notReg);
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
