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
constexpr std::int64_t kTagFunc = static_cast<std::int64_t>(AotTag::Func);
constexpr std::int64_t kTagArray = static_cast<std::int64_t>(AotTag::Array);
constexpr std::int64_t kTagMap = static_cast<std::int64_t>(AotTag::Map);
constexpr std::int64_t kTagRange = static_cast<std::int64_t>(AotTag::Range);

// Field offsets of the collection headers (see `native/value.h`). Arrays and
// maps share the same three-word header (length, capacity, block), so one set
// of offsets serves both; a map entry is a key pointer followed by a value.
constexpr std::int64_t kTableLengthOffset = 0;
constexpr std::int64_t kTableCapacityOffset = 8;
constexpr std::int64_t kTableBlockOffset = 16;
constexpr std::int64_t kArrayItemSize = static_cast<std::int64_t>(sizeof(AotValue));
constexpr std::int64_t kMapEntrySize = static_cast<std::int64_t>(kAotMapEntrySize);
constexpr std::int64_t kMapEntryKeyOffset = 0;
constexpr std::int64_t kMapEntryValueOffset = 8;

// Field offsets of a range header (see `native/value.h`).
constexpr std::int64_t kRangeStartOffset = 0;
constexpr std::int64_t kRangeEndOffset = 8;
constexpr std::int64_t kRangeInclusiveOffset = 16;

// Byte offset of a string's first byte from its `AotString` header (see
// `native/value.h`): the length is an 8-byte word, the bytes follow.
constexpr std::int64_t kStringDataOffset = static_cast<std::int64_t>(kAotStringHeaderSize);

// Field offsets of a string builder header (see `native/value.h`). It shares the
// array/map three-word shape (length, capacity, block), so the array offsets
// above serve it too; these aliases name the intent.
constexpr std::int64_t kSbLengthOffset = 0;
constexpr std::int64_t kSbCapacityOffset = 8;
constexpr std::int64_t kSbDataOffset = 16;
// Initial capacity of a string builder, in bytes. A rendered scalar or a small
// collection fits without a regrow; the arena never frees, so the block is
// sized to cover the common case rather than the worst one.
constexpr std::int64_t kSbInitialCapacity = 64;
// The depth at which the recursive repr prints `...` instead of descending, so
// a self-referential structure terminates. Matches the hosted `writeValue`.
constexpr std::int64_t kReprMaxDepth = 8;

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

// Everything the collection helpers (PHASE 7c.3a) need from the surrounding
// runtime: the labels of the helpers they call and the error/format text they
// reference. Bundled so the emitters take one argument instead of twenty.
struct CollectionEnv {
  CodeGen::Label alloc;
  CodeGen::Label fail;
  CodeGen::Label write;
  CodeGen::Label writeInt;
  CodeGen::Label formatDouble;
  CodeGen::Label strEq;        // long jolt_str_eq(AotString* a, AotString* b)
  CodeGen::Label mapFind;      // long jolt_map_find(AotMap* m, AotString* k)
  CodeGen::Label blockEnsure;  // long jolt_block_ensure(void* h, long n, long itemSize)
  CodeGen::Label utf8Len;      // long jolt_utf8_len(AotString* s)
  CodeGen::Label utf8CharAt;   // (AotString*, long) -> char* in rax, length in rdx
  CodeGen::Label compare;      // AotValue jolt_compare(AotValue, AotValue, long)
  CodeGen::Label rawPrint;     // void jolt_print_value_raw(AotValue)
  CodeGen::Label printRepr;    // void jolt_print_repr(AotValue, long depth)
  CodeGen::Label printStrEsc;  // void jolt_print_str_escaped(AotString*)
  CodeGen::Label lengthConst;  // data: the string "length"
  const DataBlob* msgOom;
  const DataBlob* msgArrayIndexType;
  const DataBlob* msgIndexRange;
  const DataBlob* msgMapKeyType;
  const DataBlob* msgBadIndex;
  const DataBlob* msgNoMember;
  const DataBlob* msgImmutable;
  const DataBlob* msgBadAssign;
  const DataBlob* msgBadField;
  const DataBlob* msgBadIn;
  const DataBlob* msgMapValue;
  const DataBlob* textTrue;
  const DataBlob* textFalse;
  const DataBlob* textNil;
  const DataBlob* textFunc;
  const DataBlob* textComma;
  const DataBlob* textColon;
  const DataBlob* textOpenBracket;
  const DataBlob* textCloseBracket;
  const DataBlob* textOpenBrace;
  const DataBlob* textCloseBrace;
  const DataBlob* textEllipsis;
  const DataBlob* textQuote;
  const DataBlob* textEscQuote;
  const DataBlob* textEscBackslash;
  const DataBlob* textEscNL;
  const DataBlob* textEscTab;
  const DataBlob* textEscCR;
  const DataBlob* textHexPrefix;
};

// Emits `jolt_write(1, rdi, rsi)`: writes `rsi` bytes from `rdi` to stdout.
// Clobbers rdi, rsi, rdx and rax (the syscall's return).
void inlineWriteStdout(CodeGen& cg, CodeGen::Label writeLabel) {
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movImm(Rdi, 1);
  cg.callLabel(writeLabel);
}

// Emits `jolt_write(fdReg, rdi, rsi)`: writes `rsi` bytes from `rdi` to `fdReg`.
// `fdReg` must not be rdi, rsi or rdx (they are the syscall's other arguments).
// Clobbers rdi, rsi, rdx and rax.
void inlineWriteFd(CodeGen& cg, CodeGen::Label writeLabel, X64Reg fdReg) {
  cg.movReg(Rdx, Rsi);
  cg.movReg(Rsi, Rdi);
  cg.movReg(Rdi, fdReg);
  cg.callLabel(writeLabel);
}

// Emits a write of `rsi` bytes from `rdi` to a *constant* fd (1 for stdout, 2
// for stderr). fd 1 uses the tight `inlineWriteStdout`; any other fd loads r10
// (a scratch the helpers own) and goes through `inlineWriteFd`. Clobbers rdi,
// rsi, rdx, r10 and rax.
void emitWriteToFd(CodeGen& cg, CodeGen::Label writeLabel, std::int64_t fd) {
  if (fd == 1) {
    inlineWriteStdout(cg, writeLabel);
  } else {
    cg.movImm(R10, fd);
    inlineWriteFd(cg, writeLabel, R10);
  }
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

// The shared body of the printers: renders one value to file descriptor `fd`.
// `newline` appends a trailing '\n' (the `print` builtin and the standalone
// `jolt_print_value`), which the raw variants omit so they can join arguments.
// int, bool, nil and str render directly; a Float/Double is formatted with the
// embedded dtoa (`jolt_format_double`) into an arena buffer, then written.
void emitPrintValueCore(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                        CodeGen::Label fail, CodeGen::Label writeInt,
                        CodeGen::Label formatDouble, CodeGen::Label printRepr,
                        const DataBlob& badValue,
                        const DataBlob& textTrue, const DataBlob& textFalse,
                        const DataBlob& textNil, const DataBlob& textNewline,
                        std::int64_t fd, bool newline) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

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
  // Functions and collections are rendered by the recursive repr helper, which
  // quotes nested strings, prints arrays as `[a, b]` and maps as `{k: v}`.
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRepr);
  emitFailCall(cg, fail, badValue);

  cg.bind(asRepr);
  cg.xorReg(Rdx, Rdx);          // depth = 0
  cg.movImm(Rcx, fd);           // target fd for the nested render
  cg.callLabel(printRepr);
  cg.jmp(done);

  cg.bind(asInt);
  cg.movReg(Rdi, Rsi);
  cg.movImm(R10, fd);  // `jolt_write_int` writes to the fd in r10
  cg.callLabel(writeInt);
  cg.jmp(done);

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
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  cg.bind(asBool);
  cg.testReg(Rsi);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.leaRipLabel(Rdi, textTrue.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textTrue.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);
  cg.bind(boolFalse);
  cg.leaRipLabel(Rdi, textFalse.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textFalse.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  cg.bind(asNil);
  cg.leaRipLabel(Rdi, textNil.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textNil.text.size()));
  emitWriteToFd(cg, writeLabel, fd);
  cg.jmp(done);

  // A string is its bytes verbatim: print writes the payload's `length` bytes
  // starting right after the header. No quoting, matching `Value::toString`.
  // The write helpers take (rdi = buffer, rsi = length).
  cg.bind(asStr);
  cg.movFromMem(Rcx, Rsi, 0);  // length
  cg.addImm32(Rsi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rdi, Rsi);         // buffer = payload + 8
  cg.movReg(Rsi, Rcx);         // length
  emitWriteToFd(cg, writeLabel, fd);

  cg.bind(done);
  if (newline) {
    cg.leaRipLabel(Rdi, textNewline.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(textNewline.text.size()));
    emitWriteToFd(cg, writeLabel, fd);
  }
  epilogue(cg);
}

// `void jolt_print_value(AotValue v)`: prints the value followed by a newline.
void emitPrintValue(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                    CodeGen::Label fail, CodeGen::Label writeInt,
                    CodeGen::Label formatDouble, CodeGen::Label printRepr,
                    const DataBlob& badValue,
                    const DataBlob& textTrue, const DataBlob& textFalse,
                    const DataBlob& textNil, const DataBlob& textNewline) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/1,
                     /*newline=*/true);
}

// `void jolt_print_value_raw(AotValue v)`: prints the value with no trailing
// newline, to stdout, for the `print`/`write` builtins.
void emitPrintValueRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                       CodeGen::Label fail, CodeGen::Label writeInt,
                       CodeGen::Label formatDouble, CodeGen::Label printRepr,
                       const DataBlob& badValue,
                       const DataBlob& textTrue, const DataBlob& textFalse,
                       const DataBlob& textNil, const DataBlob& textNewline) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/1,
                     /*newline=*/false);
}

// `void jolt_print_value_err_raw(AotValue v)`: prints the value with no trailing
// newline, to stderr, for the `eprint` builtin.
void emitPrintValueErrRaw(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                          CodeGen::Label fail, CodeGen::Label writeInt,
                          CodeGen::Label formatDouble, CodeGen::Label printRepr,
                          const DataBlob& badValue,
                          const DataBlob& textTrue, const DataBlob& textFalse,
                          const DataBlob& textNil, const DataBlob& textNewline) {
  emitPrintValueCore(cg, label, writeLabel, fail, writeInt, formatDouble, printRepr,
                     badValue, textTrue, textFalse, textNil, textNewline, /*fd=*/2,
                     /*newline=*/false);
}

// One of the `print`/`write`/`eprint` builtins, as a machine-code function with
// the same convention as a bytecode proto: `AotValue entry(AotValue* args, long
// argc, AotClosure* self)`. `args` points at the caller's `R[a+1]` and the
// caller's register file grows down, so `args[i]` sits 16 bytes *below*
// `args[i-1]` -- the same layout the proto prologue reads. Arguments are joined
// with a single space; `rawPrint` renders each one (no newline) and `writeLabel`
// writes the separators. `newline` appends a trailing '\n' (print/eprint), which
// `write` omits. The result is always nil.
void emitPrintBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label rawPrint,
                      CodeGen::Label writeLabel, const DataBlob& textSpace,
                      const DataBlob& textNewline, std::int64_t fd, bool newline) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label body = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);          // scratch for the index (keeps rsp aligned)
  cg.movReg(Rbx, Rdi);             // rbx = args
  cg.movReg(R12, Rsi);             // r12 = argc
  cg.movImmToMem(Rbp, -24, 0);     // index = 0

  cg.bind(loop);
  cg.movFromMem(Rax, Rbp, -24);    // index
  cg.cmpReg(Rax, R12);
  cg.jcc(Jcc::Ge, done);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, body);           // index == 0: no separator
  cg.leaRipLabel(Rdi, textSpace.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textSpace.text.size()));
  emitWriteToFd(cg, writeLabel, fd);

  cg.bind(body);
  cg.movFromMem(Rax, Rbp, -24);
  cg.shlImm8(Rax, 4);              // index * 16
  cg.movReg(Rdi, Rbx);
  cg.subReg(Rdi, Rax);             // rdi = &args[index]
  cg.movFromMem(Rsi, Rdi, 8);      // payload
  cg.movFromMem(Rdi, Rdi, 0);      // tag
  cg.callLabel(rawPrint);
  cg.movFromMem(Rax, Rbp, -24);
  cg.addImm32(Rax, 1);
  cg.movToMem(Rbp, -24, Rax);      // index++
  cg.jmp(loop);

  cg.bind(done);
  if (newline) {
    cg.leaRipLabel(Rdi, textNewline.label);
    cg.movImm(Rsi, static_cast<std::int64_t>(textNewline.text.size()));
    emitWriteToFd(cg, writeLabel, fd);
  }
  cg.xorReg(Rax, Rax);             // nil result
  cg.xorReg(Rdx, Rdx);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();
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
  inlineWriteFd(cg, writeLabel, R10);  // fd is in r10

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

// `void jolt_init_globals(long count)`: allocate `count` `AotValue` slots
// (16 bytes each) from the arena, clear them to nil, and keep the base in r15
// (`kAotGlobalBase`). Called once by the chunk entry, before the program runs,
// so every global reads as nil until it is assigned -- the same "unset" state
// the hosted engine's `GETGLOBAL` checks. Reports "out of memory" on a null
// allocation.
void emitInitGlobals(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                     CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label clear = cg.newLabel();
  CodeGen::Label clearDone = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  cg.push(Rbp);
  cg.movReg(Rbp, Rsp);
  cg.push(Rbx);

  cg.movReg(Rbx, Rdi);                 // rbx = count
  cg.shlImm8(Rdi, 4);                  // count * 16 bytes
  cg.callLabel(allocLabel);            // rax = base (or null)
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(kAotGlobalBase, Rax);

  // Zero every slot: tag = Nil (0), payload = 0. `rep stosq` would need rdi
  // and rcx; a short explicit loop keeps the register use obvious and the
  // count is small (one slot per distinct global in the program).
  cg.movReg(R10, Rax);
  cg.movReg(R11, Rbx);
  cg.shlImm8(R11, 4);                  // byte count again
  cg.testReg(R11);
  cg.jcc(Jcc::Eq, clearDone);
  cg.bind(clear);
  cg.movImmToMem(R10, 0, 0);
  cg.movImmToMem(R10, 8, 0);
  cg.addImm32(R10, 16);
  cg.subRegImm32(R11, 16);
  cg.jcc(Jcc::Ne, clear);
  cg.bind(clearDone);

  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void jolt_global_set(long slot, AotValue v)`: store `v` (rsi:rdx) into
// slot `rdi` of the global table at r15. One multiply and two stores; no
// hashing, no strings.
void emitGlobalSet(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movToMem(Rdi, 0, Rsi);            // tag
  cg.movToMem(Rdi, 8, Rdx);            // payload
  cg.retInsn();
}

// `AotValue jolt_global_get(long slot)`: load slot `rdi` of the global table
// into rax:rdx. The slot is nil until assigned, matching the hosted engine's
// "unset" state; the lowerer rejects a read of a global that is never assigned
// in the chunk, so a program never observes that nil by accident.
void emitGlobalGet(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.shlImm8(Rdi, 4);                  // slot * 16
  cg.addReg(Rdi, kAotGlobalBase);
  cg.movFromMem(Rax, Rdi, 0);          // tag
  cg.movFromMem(Rdx, Rdi, 8);          // payload
  cg.retInsn();
}

// `AotValue jolt_closure(void* code, long arity, long upvalueCount)`: allocate
// an `AotClosure` (code, arity, upvalueCount) followed by `upvalueCount` cell
// pointers in the arena and return it as a `Func` value (rax:rdx). The slot
// area is left for the lowering to fill: it knows which cells the child
// captures and writes each pointer with a plain store, so the helper does not
// need the descriptor table.
void emitClosure(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                 CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);  // scratch slot for the capture count (keeps rsp aligned)
  cg.movReg(Rbx, Rdi);      // code address
  cg.movReg(R12, Rsi);      // arity
  cg.movToMem(Rbp, -24, Rdx);  // stash upvalueCount
  cg.movReg(Rdi, Rdx);
  cg.shlImm8(Rdi, 3);       // count * 8 bytes of cell pointers
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotClosureHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);

  cg.movToMem(Rax, 0, Rbx);             // code
  cg.movToMem(Rax, 8, R12);             // arity
  cg.movFromMem(R11, Rbp, -24);
  cg.movToMem(Rax, 16, R11);            // upvalueCount
  cg.movReg(Rdx, Rax);                  // payload
  cg.movImm(Rax, kTagFunc);             // tag
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue* jolt_cell_new(AotValue v)`: allocate a 16-byte cell in the arena
// holding `v` (rdi:rsi) and return its address (rax). A captured local lives in
// such a cell, shared with every closure that captures it, so a write through
// the closure and a read in the defining frame observe the same value -- the
// `Cell` the hosted engine uses (PHASE 7c.2c.2).
void emitCellNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                 CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);   // tag
  cg.movReg(R12, Rsi);   // payload
  cg.movImm(Rdi, static_cast<std::int64_t>(sizeof(AotValue)));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, Rbx);
  cg.movToMem(Rax, 8, R12);
  cg.pop(R12);
  cg.pop(Rbx);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `AotValue jolt_call(AotValue callee, long argc, AotValue* args)`: the callee
// arrives in rdi:rsi, the argument count in rdx and the argument window in rcx.
// If the callee is a function, call its code with the lowering's function
// convention -- rdi = args, rsi = argc, rdx = the `AotClosure*` -- and return
// the result (rax:rdx) untouched. A non-function callee is a fatal error, the
// runtime counterpart of the hosted engine's "cannot call a value of type".
void emitCall(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
              const DataBlob& msgNonFunc) {
  CodeGen::Label bad = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Ne, bad);
  // Load the code address before clobbering rsi, then reorder the arguments for
  // the function convention (args, argc, self) through r11 as scratch:
  //   in:  rdi = tag, rsi = closure, rdx = argc, rcx = args
  //   out: rdi = args, rsi = argc, rdx = closure, r10 = code
  cg.movFromMem(R10, Rsi, 0);   // code = closure->code
  cg.movReg(R11, Rsi);          // r11 = closure
  cg.movReg(Rsi, Rdx);          // rsi = argc
  cg.movReg(Rdx, R11);          // rdx = closure
  cg.movReg(Rdi, Rcx);          // rdi = args
  cg.callReg(R10);
  cg.movReg(Rsp, Rbp);
  cg.pop(Rbp);
  cg.retInsn();

  cg.bind(bad);
  emitFailCall(cg, fail, msgNonFunc);
}

// `void jolt_arity_error(void)`: report the wrong-number-of-arguments error and
// exit. A function's prologue calls it when `argc` does not match its arity. It
// pushes rbp only to keep the stack 16-byte aligned at the `fail` call, exactly
// like every other helper; it never returns, so it has no epilogue.
void emitArityError(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                    const DataBlob& message) {
  cg.bind(label);
  prologue(cg);
  emitFailCall(cg, fail, message);
}

// `long jolt_str_eq(AotString* a, AotString* b)`: 1 when the two strings have
// equal length and equal bytes. Map keys are compared by content, so this is
// the primitive the map helpers build on. A leaf: it calls nothing and uses no
// callee-saved register.
void emitStrEq(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label same = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);  // lenA
  cg.movFromMem(R9, Rsi, 0);  // lenB
  cg.xorReg(Rax, Rax);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ne, done);
  cg.xorReg(R10, R10);  // index
  cg.bind(loop);
  cg.cmpReg(R10, R8);
  cg.jcc(Jcc::Ae, same);
  cg.movReg(R11, R10);
  cg.addReg(R11, Rdi);
  cg.movzxFromMem(R11, R11, kStringDataOffset);  // a byte
  cg.movReg(Rcx, R10);
  cg.addReg(Rcx, Rsi);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);  // b byte
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::Ne, done);
  cg.addImm32(R10, 1);
  cg.jmp(loop);
  cg.bind(same);
  cg.movImm(Rax, 1);
  cg.bind(done);
  cg.retInsn();
}

// `long jolt_map_find(AotMap* m, AotString* key)`: the index of `key` in the
// map's entry array, or -1. A linear scan keeps the layout simple; a hash index
// can be added later without changing the contract. The loop state survives the
// `strEq` call on the stack, since that call clobbers r8-r11.
void emitMapFind(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // m
  cg.movReg(R12, Rsi);  // key
  cg.movFromMem(Rax, Rbx, kTableLengthOffset);
  cg.movToMem(Rbp, -24, Rax);  // len
  cg.movImmToMem(Rbp, -32, 0); // i
  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -32);  // i
  cg.movFromMem(R9, Rbp, -24);  // len
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, notFound);
  cg.movReg(R10, R8);
  cg.imulImm32(R10, R10, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R11, Rbx, kTableBlockOffset);
  cg.addReg(R10, R11);              // entry = entries + i*24
  cg.movFromMem(Rdi, R10, kMapEntryKeyOffset);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, found);
  cg.movFromMem(R8, Rbp, -32);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -32, R8);  // i++
  cg.jmp(loop);
  cg.bind(found);
  cg.movFromMem(Rax, Rbp, -32);  // index
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(notFound);
  cg.movImm(Rax, -1);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long jolt_block_ensure(void* header, long needed, long itemSize)`: returns a
// block with room for `needed` items of `itemSize` bytes. The existing block is
// returned when its capacity already suffices; otherwise a fresh block of the
// next power-of-two capacity is allocated from the arena, the live items are
// copied, and the header's block/capacity are updated. Returns null on
// exhaustion. `header` is the 3-word array/map header (length, capacity, block).
void emitBlockEnsure(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label growLoop = cg.newLabel();
  CodeGen::Label growLoopDone = cg.newLabel();
  CodeGen::Label haveCapacity = cg.newLabel();
  CodeGen::Label noCopy = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);  // header
  cg.movReg(R12, Rsi);  // needed
  cg.movToMem(Rbp, -24, Rdx);  // itemSize
  cg.movFromMem(R8, Rbx, kTableCapacityOffset);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ae, haveCapacity);
  // newCap = cap == 0 ? 8 : cap * 2; while (newCap < needed) newCap *= 2.
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, grow);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(grow);
  cg.movImm(R8, 8);
  cg.bind(growLoop);
  cg.cmpReg(R8, R12);
  cg.jcc(Jcc::Ae, growLoopDone);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(growLoopDone);
  cg.movToMem(Rbp, -32, R8);  // newCap
  // size = newCap * itemSize
  cg.movReg(Rdi, R8);
  cg.movFromMem(Rax, Rbp, -24);
  cg.imulReg(Rdi, Rax);
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);  // new block
  // bytes = length * itemSize
  cg.movFromMem(Rcx, Rbx, kTableLengthOffset);
  cg.movFromMem(Rax, Rbp, -24);
  cg.imulReg(Rcx, Rax);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, noCopy);
  cg.movReg(Rdi, R10);
  cg.movFromMem(Rsi, Rbx, kTableBlockOffset);
  cg.repMovsb();
  cg.bind(noCopy);
  cg.movToMem(Rbx, kTableBlockOffset, R10);
  cg.movFromMem(Rax, Rbp, -32);
  cg.movToMem(Rbx, kTableCapacityOffset, Rax);
  cg.movReg(Rax, R10);
  cg.jmp(done);
  cg.bind(haveCapacity);
  cg.movFromMem(Rax, Rbx, kTableBlockOffset);
  cg.jmp(done);
  cg.bind(oom);
  cg.xorReg(Rax, Rax);
  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `long jolt_utf8_len(AotString* s)`: the number of UTF-8 code points, i.e. the
// count of bytes that are not continuation bytes (0b10xxxxxx). A leaf.
void emitUtf8Len(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label skip = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);  // length in bytes
  cg.xorReg(Rax, Rax);        // code-point count
  cg.xorReg(R9, R9);          // index
  cg.bind(loop);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ae, done);
  cg.movReg(R10, R9);
  cg.addReg(R10, Rdi);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.andImm32(R10, 0xC0);
  cg.cmpImm32(R10, 0x80);
  cg.jcc(Jcc::Eq, skip);
  cg.addImm32(Rax, 1);
  cg.bind(skip);
  cg.addImm32(R9, 1);
  cg.jmp(loop);
  cg.bind(done);
  cg.retInsn();
}

// Emits the length of the UTF-8 character whose lead byte is in `lead` (r8) into
// `len` (r9). 1/2/3/4 from the high bits of the lead byte. Clobbers r10.
void emitUtf8LeadLen(CodeGen& cg, X64Reg lead, X64Reg len, X64Reg scratch) {
  CodeGen::Label one = cg.newLabel();
  CodeGen::Label two = cg.newLabel();
  CodeGen::Label three = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0x80);
  cg.testReg(scratch);
  cg.jcc(Jcc::Eq, one);  // < 0x80
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0xE0);
  cg.cmpImm32(scratch, 0xC0);
  cg.jcc(Jcc::Eq, two);
  cg.movReg(scratch, lead);
  cg.andImm32(scratch, 0xF0);
  cg.cmpImm32(scratch, 0xE0);
  cg.jcc(Jcc::Eq, three);
  cg.movImm(len, 4);
  cg.jmp(done);
  cg.bind(one);
  cg.movImm(len, 1);
  cg.jmp(done);
  cg.bind(two);
  cg.movImm(len, 2);
  cg.jmp(done);
  cg.bind(three);
  cg.movImm(len, 3);
  cg.bind(done);
}

// `(AotString*, long index) -> (char* in rax, byte length in rdx)`: the bytes of
// the `index`-th UTF-8 code point, or rax = 0 when the index is past the end.
// A leaf used by string indexing and string iteration.
void emitUtf8CharAt(CodeGen& cg, CodeGen::Label label) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label found = cg.newLabel();
  CodeGen::Label notFound = cg.newLabel();

  cg.bind(label);
  cg.movFromMem(R8, Rdi, 0);           // total bytes
  cg.movReg(R9, Rdi);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));  // cursor
  cg.movReg(R10, R9);
  cg.addReg(R10, R8);                  // end
  cg.xorReg(R11, R11);                 // code-point index
  cg.bind(loop);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, notFound);
  cg.movzxFromMem(Rcx, R9, 0);         // lead byte
  emitUtf8LeadLen(cg, Rcx, Rdx, R8);
  cg.cmpReg(R11, Rsi);
  cg.jcc(Jcc::Eq, found);
  cg.addReg(R9, Rdx);
  cg.addImm32(R11, 1);
  cg.jmp(loop);
  cg.bind(found);
  cg.movReg(Rax, R9);
  cg.retInsn();
  cg.bind(notFound);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.retInsn();
}

// `AotValue jolt_table_new(long kind)`: allocate a zeroed array (kind 0) or map
// (kind 1) header in the arena. The items/entries block is allocated lazily by
// `jolt_block_ensure` on the first store, so an empty collection costs one
// allocation.
void emitTableNew(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // kind
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotArrayHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movImmToMem(Rax, kTableLengthOffset, 0);
  cg.movImmToMem(Rax, kTableCapacityOffset, 0);
  cg.movImmToMem(Rax, kTableBlockOffset, 0);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagArray);
  cg.testReg(Rbx);
  cg.jcc(Jcc::Eq, done);
  cg.movImm(Rax, kTagMap);
  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue jolt_table_get(AotValue obj, AotValue index)`: `obj[index]` for an
// array (int index, negative from the end), a string (int code-point index) or
// a map (string key, missing key yields nil), mirroring `ops::indexGet`.
void emitTableGet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label arrNonNeg = cg.newLabel();
  CodeGen::Label mapMiss = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failArrayType = cg.newLabel();
  CodeGen::Label failStrType = cg.newLabel();
  CodeGen::Label failMapType = cg.newLabel();
  CodeGen::Label failRange = cg.newLabel();
  CodeGen::Label failIndex = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rdx);       // index tag
  cg.movToMem(Rbp, -32, Rcx);       // index payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.jmp(failIndex);

  // --- array[index] ---
  cg.bind(arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failArrayType);
  cg.movFromMem(R8, Rbp, -32);      // i
  cg.movFromMem(R9, R12, kTableLengthOffset);  // len
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, arrNonNeg);
  cg.addReg(R8, R9);                // negative index from the end
  cg.bind(arrNonNeg);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, failRange);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, failRange);
  cg.shlImm8(R8, 4);                // i * 16
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, R8);
  cg.movFromMem(Rax, R10, 0);
  cg.movFromMem(Rdx, R10, 8);
  cg.jmp(done);

  // --- string[index] ---
  cg.bind(str);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failStrType);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.utf8CharAt);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failRange);
  cg.movToMem(Rbp, -40, Rax);       // char pointer
  cg.movToMem(Rbp, -48, Rdx);       // char byte length
  cg.movReg(Rdi, Rdx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.movToMem(Rax, 0, Rcx);         // length word
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rsi, Rbp, -40);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  // --- map[index] ---
  cg.bind(map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failMapType);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapMiss);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(done);
  cg.bind(mapMiss);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(failArrayType);
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failStrType);
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failMapType);
  emitFailCall(cg, env.fail, *env.msgMapKeyType);
  cg.bind(failRange);
  emitFailCall(cg, env.fail, *env.msgIndexRange);
  cg.bind(failIndex);
  emitFailCall(cg, env.fail, *env.msgBadIndex);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue jolt_member_get(AotValue obj, AotValue name)`: `obj.name` for a map
// (the entry, else `length`, else nil), an array (`.length`) or a string
// (`.length`). Methods are not bound yet, so a miss on an array/string is the
// "no member" error, exactly as `ops::memberGet` reports it.
void emitMemberGet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeSat = cg.newLabel();
  CodeGen::Label rangeUse = cg.newLabel();
  CodeGen::Label rangeMax = cg.newLabel();
  CodeGen::Label rangeHave = cg.newLabel();
  CodeGen::Label mapMiss = cg.newLabel();
  CodeGen::Label isLength = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failMember = cg.newLabel();
  CodeGen::Label failIndex = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(Rbx, Rdi);          // obj tag
  cg.movReg(R12, Rsi);          // obj payload
  cg.movToMem(Rbp, -24, Rcx);   // name payload (AotString*)
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.jmp(failMember);

  cg.bind(map);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapMiss);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(done);
  cg.bind(mapMiss);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, isLength);
  cg.xorReg(Rax, Rax);          // no such member -> nil (methods deferred)
  cg.xorReg(Rdx, Rdx);
  cg.jmp(done);

  cg.bind(arr);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failMember);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(str);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failMember);
  cg.movReg(Rdi, R12);
  cg.callLabel(env.utf8Len);
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  cg.bind(isLength);
  cg.movFromMem(Rdx, R12, kTableLengthOffset);
  cg.movImm(Rax, kTagInt);
  cg.jmp(done);

  // A range's `.length`: the distance between the bounds, plus one when
  // inclusive, saturated at INT64_MAX so an overflowing span stays defined.
  cg.bind(range);
  cg.movFromMem(Rdi, Rbp, -24);
  cg.leaRipLabel(Rsi, env.lengthConst);
  cg.callLabel(env.strEq);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, failMember);
  cg.movFromMem(R8, R12, kRangeStartOffset);
  cg.movFromMem(R9, R12, kRangeEndOffset);
  cg.cmpReg(R9, R8);
  cg.jcc(Jcc::Ge, rangeAsc);
  cg.movReg(R10, R8);
  cg.subReg(R10, R9);              // descending: start - end
  cg.jmp(rangeSat);
  cg.bind(rangeAsc);
  cg.movReg(R10, R9);
  cg.subReg(R10, R8);              // ascending: end - start
  cg.bind(rangeSat);
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, rangeUse);
  cg.movImm(R11, -1);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Eq, rangeMax);       // distance == UINT64_MAX: saturate
  cg.addImm32(R10, 1);
  cg.bind(rangeMax);
  cg.movImm(R11, -1);
  cg.shrImm8(R11, 1);              // INT64_MAX
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Le, rangeUse);
  cg.movReg(R10, R11);             // saturate
  cg.bind(rangeUse);
  cg.movReg(Rdx, R10);
  cg.movImm(Rax, kTagInt);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failMember);
  emitFailCall(cg, env.fail, *env.msgNoMember);
  cg.bind(failIndex);
  emitFailCall(cg, env.fail, *env.msgBadIndex);
}

// `void jolt_table_set(AotValue obj, AotValue index, AotValue value)`:
// `obj[index] = value`. An array index equal to the length appends; a map key
// that exists is overwritten, otherwise appended. A string is immutable,
// mirroring `ops::indexSet`.
void emitTableSet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label arrAppend = cg.newLabel();
  CodeGen::Label arrStore = cg.newLabel();
  CodeGen::Label arrNonNeg = cg.newLabel();
  CodeGen::Label arrRange = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label mapAppend = cg.newLabel();
  CodeGen::Label mapStore = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failArrType = cg.newLabel();
  CodeGen::Label failMapType = cg.newLabel();
  CodeGen::Label failImmutable = cg.newLabel();
  CodeGen::Label failAssign = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rdx);       // index tag
  cg.movToMem(Rbp, -32, Rcx);       // index payload
  cg.movToMem(Rbp, -40, R8);        // value tag
  cg.movToMem(Rbp, -48, R9);        // value payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, failImmutable);
  cg.jmp(failAssign);

  cg.bind(arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failArrType);
  cg.movFromMem(R8, Rbp, -32);                 // i
  cg.movFromMem(R9, R12, kTableLengthOffset);  // len
  cg.testReg(R8);
  cg.jcc(Jcc::Ge, arrNonNeg);
  cg.addReg(R8, R9);
  cg.bind(arrNonNeg);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Eq, arrAppend);
  cg.testReg(R8);
  cg.jcc(Jcc::Lt, arrRange);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ge, arrRange);
  cg.jmp(arrStore);
  cg.bind(arrAppend);
  // Grow to len + 1, then store at index len and bump the length.
  cg.movReg(Rdi, R12);
  cg.movReg(Rsi, R9);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kArrayItemSize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R10, Rbp, -48);
  cg.movReg(R11, R8);
  cg.shlImm8(R11, 4);
  cg.addReg(R11, Rax);
  cg.movToMem(R11, 0, R9);
  cg.movToMem(R11, 8, R10);
  cg.addImm32(R8, 1);
  cg.movToMem(R12, kTableLengthOffset, R8);
  cg.jmp(done);
  cg.bind(arrStore);
  cg.shlImm8(R8, 4);
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, R8);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.jmp(done);

  cg.bind(map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failMapType);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, mapAppend);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(mapStore);
  cg.bind(mapAppend);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -32);
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(R12, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  cg.bind(mapStore);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.jmp(done);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failArrType);
  emitFailCall(cg, env.fail, *env.msgArrayIndexType);
  cg.bind(failMapType);
  emitFailCall(cg, env.fail, *env.msgMapKeyType);
  cg.bind(failImmutable);
  emitFailCall(cg, env.fail, *env.msgImmutable);
  cg.bind(failAssign);
  emitFailCall(cg, env.fail, *env.msgBadAssign);
  cg.bind(arrRange);
  emitFailCall(cg, env.fail, *env.msgIndexRange);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `void jolt_member_set(AotValue obj, AotValue name, AotValue value)`:
// `obj.name = value`, valid only on a map (mirrors `ops::memberSet`).
void emitMemberSet(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label append = cg.newLabel();
  CodeGen::Label store = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failField = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // obj tag
  cg.movReg(R12, Rsi);              // obj payload
  cg.movToMem(Rbp, -24, Rcx);       // name payload (AotString*)
  cg.movToMem(Rbp, -40, R8);        // value tag
  cg.movToMem(Rbp, -48, R9);        // value payload
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.jmp(failField);

  cg.bind(map);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, append);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.addImm32(R10, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.jmp(store);
  cg.bind(append);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, R12, kTableLengthOffset);
  cg.addImm32(Rsi, 1);
  cg.movImm(Rdx, kMapEntrySize);
  cg.callLabel(env.blockEnsure);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(R8, R12, kTableLengthOffset);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.addReg(R8, Rax);
  cg.movFromMem(R9, Rbp, -24);
  cg.movToMem(R8, kMapEntryKeyOffset, R9);
  cg.addImm32(R8, static_cast<std::int32_t>(kMapEntryValueOffset));
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.addImm32(R9, 1);
  cg.movToMem(R12, kTableLengthOffset, R9);
  cg.movReg(R10, R8);
  cg.bind(store);
  cg.movFromMem(R9, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.movToMem(R10, 0, R9);
  cg.movToMem(R10, 8, R11);
  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failField);
  emitFailCall(cg, env.fail, *env.msgBadField);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// `AotValue jolt_contains(AotValue needle, AotValue haystack)`: the `in`
// operator for arrays (value equality via `jolt_compare`), maps (string key)
// and strings (substring). Ranges are not lowered yet.
void emitContains(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label arrLoop = cg.newLabel();
  CodeGen::Label arrFound = cg.newLabel();
  CodeGen::Label arrMiss = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label mapFound = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label strLoop = cg.newLabel();
  CodeGen::Label strOuter = cg.newLabel();
  CodeGen::Label strNext = cg.newLabel();
  CodeGen::Label strFound = cg.newLabel();
  CodeGen::Label strMiss = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeAscIncl = cg.newLabel();
  CodeGen::Label rangeDescIncl = cg.newLabel();
  CodeGen::Label trueDone = cg.newLabel();
  CodeGen::Label falseDone = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failType = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 48);
  cg.movReg(Rbx, Rdi);              // needle tag
  cg.movReg(R12, Rsi);              // needle payload
  cg.movToMem(Rbp, -24, Rdx);       // haystack tag
  cg.movToMem(Rbp, -32, Rcx);       // haystack payload
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.jmp(failType);

  cg.bind(arr);
  cg.movFromMem(R8, Rbp, -32);      // array ptr
  cg.movImmToMem(Rbp, -40, 0);      // index
  cg.movFromMem(R9, R8, kTableLengthOffset);
  cg.movToMem(Rbp, -48, R9);        // len
  cg.bind(arrLoop);
  cg.movFromMem(R10, Rbp, -40);
  cg.movFromMem(R11, Rbp, -48);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ae, arrMiss);
  cg.movReg(R11, R10);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R9, R8, kTableBlockOffset);
  cg.addReg(R11, R9);
  cg.movFromMem(Rdi, R11, 0);       // item tag
  cg.movFromMem(Rsi, R11, 8);       // item payload
  cg.movReg(Rdx, Rbx);
  cg.movReg(Rcx, R12);
  cg.movImm(R8, static_cast<std::int64_t>(AotCompareOp::Eq));
  cg.callLabel(env.compare);
  cg.testReg(Rdx);
  cg.jcc(Jcc::Ne, arrFound);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.movFromMem(R8, Rbp, -32);
  cg.jmp(arrLoop);

  cg.bind(map);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failType);
  cg.movFromMem(Rdi, Rbp, -32);
  cg.movReg(Rsi, R12);
  cg.callLabel(env.mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, mapFound);
  cg.jmp(falseDone);

  cg.bind(str);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Ne, failType);
  // Substring search: for each start in the haystack where the needle's first
  // byte matches, compare the following bytes.
  cg.movFromMem(R8, Rbp, -32);       // haystack AotString*
  cg.movFromMem(R9, R12, 0);         // needle byte length
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, strFound);         // an empty needle is always contained
  cg.movImmToMem(Rbp, -40, 0);       // outer index
  cg.bind(strOuter);
  cg.movFromMem(R10, Rbp, -40);      // i
  cg.movFromMem(R11, R8, 0);         // haystack length
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Ae, strMiss);
  // Compare needle[0] with haystack[i]; if equal, compare the rest.
  cg.movReg(Rcx, R12);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.movReg(Rdx, R10);
  cg.addReg(Rdx, R8);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);
  cg.cmpReg(Rcx, Rdx);
  cg.jcc(Jcc::Ne, strNext);
  // Check that needle fits and every byte matches.
  cg.movReg(R11, R10);
  cg.addReg(R11, R9);
  cg.movFromMem(Rcx, R8, 0);
  cg.cmpReg(R11, Rcx);
  cg.jcc(Jcc::A, strMiss);           // does not fit
  cg.movImmToMem(Rbp, -48, 0);       // j
  cg.bind(strLoop);
  cg.movFromMem(R11, Rbp, -48);
  cg.cmpReg(R11, R9);
  cg.jcc(Jcc::Ae, strFound);
  cg.movReg(Rcx, R11);
  cg.addReg(Rcx, R12);
  cg.movzxFromMem(Rcx, Rcx, kStringDataOffset);
  cg.movReg(Rdx, R11);
  cg.addReg(Rdx, R10);
  cg.addReg(Rdx, R8);
  cg.movzxFromMem(Rdx, Rdx, kStringDataOffset);
  cg.cmpReg(Rcx, Rdx);
  cg.jcc(Jcc::Ne, strNext);
  cg.movFromMem(R11, Rbp, -48);
  cg.addImm32(R11, 1);
  cg.movToMem(Rbp, -48, R11);
  cg.jmp(strLoop);
  cg.bind(strNext);
  cg.movFromMem(R10, Rbp, -40);
  cg.addImm32(R10, 1);
  cg.movToMem(Rbp, -40, R10);
  cg.jmp(strOuter);

  cg.bind(arrFound);
  cg.bind(mapFound);
  cg.bind(strFound);
  cg.jmp(trueDone);
  cg.bind(arrMiss);
  cg.bind(strMiss);
  cg.jmp(falseDone);

  // A range contains an integer when it falls inside the bounds. An ascending
  // range (start <= end) checks `start <= value < end` (or `<= end` inclusive);
  // a descending one checks the mirror, and a descending exclusive range
  // matches neither bound (`5 in 5..0` is false), matching `ops::inRange`.
  cg.bind(range);
  cg.cmpImm32(Rbx, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failType);
  cg.movFromMem(R11, Rbp, -32);        // range header
  cg.movFromMem(R10, R11, kRangeStartOffset);
  cg.movFromMem(R9, R11, kRangeEndOffset);
  cg.cmpReg(R10, R9);
  cg.jcc(Jcc::Le, rangeAsc);
  cg.movFromMem(R8, R11, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeDescIncl);
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Le, falseDone);          // value <= end: outside
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Ge, falseDone);          // value >= start: outside
  cg.jmp(trueDone);
  cg.bind(rangeDescIncl);
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Lt, falseDone);          // value < end: outside
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Gt, falseDone);          // value > start: outside
  cg.jmp(trueDone);
  cg.bind(rangeAsc);
  cg.movFromMem(R8, R11, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeAscIncl);
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Lt, falseDone);          // value < start: outside
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Ge, falseDone);          // value >= end: outside
  cg.jmp(trueDone);
  cg.bind(rangeAscIncl);
  cg.cmpReg(R12, R10);
  cg.jcc(Jcc::Lt, falseDone);          // value < start: outside
  cg.cmpReg(R12, R9);
  cg.jcc(Jcc::Gt, falseDone);          // value > end: outside
  cg.jmp(trueDone);

  cg.bind(trueDone);
  cg.movImm(Rax, kTagBool);
  cg.movImm(Rdx, 1);
  cg.jmp(done);
  cg.bind(falseDone);
  cg.movImm(Rax, kTagBool);
  cg.xorReg(Rdx, Rdx);
  cg.bind(done);
  cg.addImm32(Rsp, 48);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failType);
  emitFailCall(cg, env.fail, *env.msgBadIn);
}

// `AotValue jolt_iter_next(AotValue* cursor, AotValue iterable)`: advance a
// `for-in` cursor and return the item (rax:rdx) with the done flag in r8. The
// cursor starts nil; it holds an int index afterwards. Arrays yield items, maps
// yield keys and strings yield one code point, mirroring the VM's `ITER`.
void emitIterNextRange(CodeGen& cg, CodeGen::Label range, CodeGen::Label rangeAsc,
                       CodeGen::Label rangeAscIncl, CodeGen::Label rangeDescIncl,
                       CodeGen::Label rangeEmit, CodeGen::Label rangeDescNext,
                       CodeGen::Label rangeStoreCursor, CodeGen::Label doneTrue,
                       CodeGen::Label done);

// `AotValue jolt_iter_next(AotValue* cursor, long tag, long payload)`: advance
// the `for-in` cursor and return the item (rax:rdx) with the done flag in r8.
void emitIterNext(CodeGen& cg, CodeGen::Label label, const CollectionEnv& env) {
  CodeGen::Label zero = cg.newLabel();
  CodeGen::Label arr = cg.newLabel();
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label str = cg.newLabel();
  CodeGen::Label range = cg.newLabel();
  CodeGen::Label rangeAsc = cg.newLabel();
  CodeGen::Label rangeAscIncl = cg.newLabel();
  CodeGen::Label rangeDescIncl = cg.newLabel();
  CodeGen::Label rangeEmit = cg.newLabel();
  CodeGen::Label rangeDescNext = cg.newLabel();
  CodeGen::Label rangeStoreCursor = cg.newLabel();
  CodeGen::Label dispatch = cg.newLabel();
  CodeGen::Label storeCursor = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label doneTrue = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label failType = cg.newLabel();
  CodeGen::Label failState = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);              // cursor pointer
  cg.movReg(R12, Rdx);              // iterable payload
  cg.movToMem(Rbp, -24, Rsi);       // iterable tag
  cg.movFromMem(R8, Rbx, 0);        // cursor tag
  cg.movFromMem(R9, Rbx, 8);        // cursor payload
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, zero);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, failState);
  cg.jmp(dispatch);
  cg.bind(zero);
  // A range's cursor is the current value, which starts at `start`; every
  // other iterable uses a 0-based index.
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Ne, dispatch);
  cg.movFromMem(R9, R12, kRangeStartOffset);
  cg.bind(dispatch);
  cg.movToMem(Rbp, -32, R9);        // index / range value
  cg.movFromMem(R8, Rbp, -24);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, arr);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, str);
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, range);
  cg.jmp(failType);

  cg.bind(arr);
  cg.movFromMem(R9, Rbp, -32);
  cg.movFromMem(R10, R12, kTableLengthOffset);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, doneTrue);
  cg.movReg(R11, R9);
  cg.shlImm8(R11, 4);
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R11, R10);
  cg.movFromMem(Rax, R11, 0);
  cg.movFromMem(Rdx, R11, 8);
  cg.jmp(storeCursor);

  cg.bind(map);
  cg.movFromMem(R9, Rbp, -32);
  cg.movFromMem(R10, R12, kTableLengthOffset);
  cg.cmpReg(R9, R10);
  cg.jcc(Jcc::Ae, doneTrue);
  cg.imulImm32(R9, R9, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R9, R10);
  cg.movImm(Rax, kTagStr);
  cg.movFromMem(Rdx, R9, kMapEntryKeyOffset);
  cg.jmp(storeCursor);

  cg.bind(str);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -32);
  cg.callLabel(env.utf8CharAt);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, doneTrue);
  cg.movToMem(Rbp, -40, Rax);       // char pointer
  cg.movToMem(Rbp, -48, Rdx);       // char byte length
  cg.movReg(Rdi, Rdx);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.callLabel(env.alloc);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.movToMem(Rax, 0, Rcx);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kAotStringHeaderSize));
  cg.movFromMem(Rsi, Rbp, -40);
  cg.movFromMem(Rcx, Rbp, -48);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);

  emitIterNextRange(cg, range, rangeAsc, rangeAscIncl, rangeDescIncl, rangeEmit,
                    rangeDescNext, rangeStoreCursor, doneTrue, done);

  cg.bind(storeCursor);
  cg.movFromMem(R9, Rbp, -32);
  cg.addImm32(R9, 1);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);                // done = false
  cg.jmp(done);

  cg.bind(doneTrue);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);
  cg.movImm(R8, 1);

  cg.bind(done);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
  cg.bind(failType);
  emitFailCall(cg, env.fail, *env.msgBadIn);
  cg.bind(failState);
  emitFailCall(cg, env.fail, *env.msgBadIn);
  cg.bind(oom);
  emitFailCall(cg, env.fail, *env.msgOom);
}

// The `for-in` body for a range: `R9` holds the current value, `R12` the range
// header. Ascending ranges step +1, descending ones step -1 (unsigned, so
// INT64_MIN/MAX bounds stay defined). The next value goes to `rangeStoreCursor`.
void emitIterNextRange(CodeGen& cg, CodeGen::Label range, CodeGen::Label rangeAsc,
                       CodeGen::Label rangeAscIncl, CodeGen::Label rangeDescIncl,
                       CodeGen::Label rangeEmit, CodeGen::Label rangeDescNext,
                       CodeGen::Label rangeStoreCursor, CodeGen::Label doneTrue,
                       CodeGen::Label done) {
  cg.bind(range);
  cg.movFromMem(R9, Rbp, -32);          // current value
  cg.movFromMem(R10, R12, kRangeStartOffset);
  cg.movFromMem(R11, R12, kRangeEndOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Le, rangeAsc);

  // Descending: start > end. Stop when the current value passes `end`.
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeDescIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Le, doneTrue);            // exclusive: value > end required
  cg.jmp(rangeEmit);
  cg.bind(rangeDescIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Lt, doneTrue);            // inclusive: value >= end required
  cg.jmp(rangeEmit);

  // Ascending: start <= end. Stop when the current value passes `end`.
  cg.bind(rangeAsc);
  cg.movFromMem(R8, R12, kRangeInclusiveOffset);
  cg.testReg(R8);
  cg.jcc(Jcc::Ne, rangeAscIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Ge, doneTrue);            // exclusive: value < end required
  cg.jmp(rangeEmit);
  cg.bind(rangeAscIncl);
  cg.cmpReg(R9, R11);
  cg.jcc(Jcc::Gt, doneTrue);            // inclusive: value <= end required

  cg.bind(rangeEmit);
  cg.movImm(Rax, kTagInt);
  cg.movReg(Rdx, R9);
  // Decide the direction once more to step the cursor the right way.
  cg.movFromMem(R10, R12, kRangeStartOffset);
  cg.movFromMem(R11, R12, kRangeEndOffset);
  cg.cmpReg(R10, R11);
  cg.jcc(Jcc::Gt, rangeDescNext);
  cg.addImm32(R9, 1);                   // ascending: next = current + 1
  cg.jmp(rangeStoreCursor);
  cg.bind(rangeDescNext);
  cg.subRegImm32(R9, 1);                // descending: next = current - 1

  cg.bind(rangeStoreCursor);
  cg.movImmToMem(Rbx, 0, kTagInt);
  cg.movToMem(Rbx, 8, R9);
  cg.xorReg(R8, R8);                    // done = false
  cg.jmp(done);
}

// `AotValue jolt_builtin_map_value(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$mapValue` helper the compiler calls for `for k, v in map`. It
// returns the value paired with the key `args[0]`, so the key/value loop form
// is an ordinary call. A non-map iterable is rejected, matching the hosted
// `$mapValue` (`for k, v in [1, 2, 3]` is a runtime error).
void emitMapValueBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label mapFind,
                         const CollectionEnv& env) {
  CodeGen::Label map = cg.newLabel();
  CodeGen::Label missing = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);          // args
  // args[0] is the map, args[1] the key. The argument window grows *down* from
  // `args`, so args[1] sits 16 bytes below args[0].
  cg.movFromMem(R8, Rbx, 0);    // map tag
  cg.movFromMem(R12, Rbx, 8);   // map payload
  cg.cmpImm32(R8, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, map);
  emitFailCall(cg, env.fail, *env.msgMapValue);

  cg.bind(map);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbx, -8);  // args[1] payload (the key)
  cg.callLabel(mapFind);
  cg.testReg(Rax);
  cg.jcc(Jcc::Lt, missing);
  cg.imulImm32(Rax, Rax, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R10, R12, kTableBlockOffset);
  cg.addReg(R10, Rax);
  cg.movFromMem(Rax, R10, kMapEntryValueOffset);
  cg.movFromMem(Rdx, R10, kMapEntryValueOffset + 8);
  cg.jmp(done);

  cg.bind(missing);
  cg.xorReg(Rax, Rax);
  cg.xorReg(Rdx, Rdx);

  cg.bind(done);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue jolt_builtin_is_error(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$isError` helper that `try` and `x catch y` compile to. The AOT
// runtime has no error values yet, so a value is never an error: the helper
// returns `false` and the success path of the construct runs. A real runtime
// failure still exits with code 70 before any `try` can observe it.
void emitIsErrorBuiltin(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  prologue(cg);
  cg.movImm(Rax, kTagBool);
  cg.xorReg(Rdx, Rdx);  // false
  epilogue(cg);
}

// `AotValue jolt_builtin_non_null(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$nonNull` helper for `a!!`. It returns the operand unchanged, or
// fails when it is nil, matching the hosted helper.
void emitNonNullBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label fail,
                        const DataBlob& msgNonNull) {
  CodeGen::Label ok = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  // args[0] is the operand: the argument window grows *down* from `args`, so
  // args[0] sits at [rdi + 0] (tag) and [rdi + 8] (payload).
  cg.movFromMem(Rax, Rdi, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Ne, ok);
  emitFailCall(cg, fail, msgNonNull);

  cg.bind(ok);
  cg.movFromMem(Rax, Rdi, 0);   // tag
  cg.movFromMem(Rdx, Rdi, 8);   // payload
  epilogue(cg);
}

// Renders the integer in `rax` as decimal digits, written *backwards* from the
// cursor in `cursor` (r8), which the helper advances downward. A leading '-'
// is written for a negative value, matching `std::to_string`. Clobbers rax,
// rcx, rdx and r9.
void emitIntDigitsBackwards(CodeGen& cg, X64Reg cursor, CodeGen::Label positive,
                            CodeGen::Label convert, CodeGen::Label loop,
                            CodeGen::Label emit) {
  cg.testReg(Rax);
  cg.jcc(Jcc::Ge, positive);
  cg.movImm(R9, 1);          // negative
  cg.negReg(Rax);
  cg.jmp(convert);
  cg.bind(positive);
  cg.xorReg(R9, R9);
  cg.bind(convert);
  cg.movImm(Rcx, 10);
  cg.bind(loop);
  cg.divUnsigned(Rcx);       // rax /= 10, rdx = rax % 10
  cg.addImm32(Rdx, static_cast<std::int32_t>('0'));
  cg.subRegImm32(cursor, 1);
  cg.movByteToMem(cursor, 0, Rdx);
  cg.testReg(Rax);
  cg.jcc(Jcc::Ne, loop);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, emit);
  cg.subRegImm32(cursor, 1);
  cg.movImmByteToMem(cursor, 0, static_cast<std::uint8_t>('-'));
  cg.bind(emit);
}

// `AotValue jolt_builtin_str(AotValue* args, long argc, AotClosure* self)`: the
// hidden `$str` helper the compiler emits for string interpolation (`"${x}"`)
// and computed map keys. It renders the operand to an `AotString` in the arena,
// byte-identical to the hosted `Value::toString` for nil, booleans, integers,
// floats, doubles and strings (a string is returned unchanged, so a string
// interpolates without quotes). A collection or function operand renders with
// the recursive repr (`jolt_repr_to_string`), so `"${[1, 2]}"` reads exactly
// like `print([1, 2])` (7c.4b.2).
void emitStrBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                    CodeGen::Label fail, CodeGen::Label formatDouble,
                    CodeGen::Label reprToString, const DataBlob& msgOom,
                    const DataBlob& textTrue, const DataBlob& textFalse,
                    const DataBlob& textNil, const DataBlob& msgStrBad) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asRepr = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label rangeIncl = cg.newLabel();
  CodeGen::Label rangeAfterSep = cg.newLabel();
  CodeGen::Label intPos = cg.newLabel();
  CodeGen::Label intConv = cg.newLabel();
  CodeGen::Label intLoop = cg.newLabel();
  CodeGen::Label intEmit = cg.newLabel();
  CodeGen::Label rangeEndPos = cg.newLabel();
  CodeGen::Label rangeEndConv = cg.newLabel();
  CodeGen::Label rangeEndLoop = cg.newLabel();
  CodeGen::Label rangeEndEmit = cg.newLabel();
  CodeGen::Label rangeStartPos = cg.newLabel();
  CodeGen::Label rangeStartConv = cg.newLabel();
  CodeGen::Label rangeStartLoop = cg.newLabel();
  CodeGen::Label rangeStartEmit = cg.newLabel();
  CodeGen::Label buildConst = cg.newLabel();
  CodeGen::Label numOom = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 64);   // digit buffer (int, or range bounds + separator)
  cg.movReg(Rbx, Rdi);       // rbx = args

  // args[0] is the operand: tag at [rbx + 0], payload at [rbx + 8].
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asRepr);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asRepr);
  emitFailCall(cg, fail, msgStrBad);

  // A collection or function renders with the recursive repr, so `"${[1, 2]}"`
  // reads exactly like `print([1, 2])` (nested strings quoted, depth-capped).
  cg.bind(asRepr);
  cg.movFromMem(Rdi, Rbx, 0);
  cg.movFromMem(Rsi, Rbx, 8);
  cg.callLabel(reprToString);
  cg.jmp(done);

  // A string renders as itself.
  cg.bind(asStr);
  cg.movFromMem(Rax, Rbx, 0);
  cg.movFromMem(Rdx, Rbx, 8);
  cg.jmp(done);

  cg.bind(asNil);
  cg.leaRipLabel(R8, textNil.label);
  cg.movImm(R12, static_cast<std::int64_t>(textNil.text.size()));
  cg.jmp(buildConst);

  cg.bind(asBool);
  cg.movFromMem(R9, Rbx, 8);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.leaRipLabel(R8, textTrue.label);
  cg.movImm(R12, static_cast<std::int64_t>(textTrue.text.size()));
  cg.jmp(buildConst);
  cg.bind(boolFalse);
  cg.leaRipLabel(R8, textFalse.label);
  cg.movImm(R12, static_cast<std::int64_t>(textFalse.text.size()));
  cg.jmp(buildConst);

  // An integer: decimal digits built backwards into the stack buffer, then a
  // leading '-' when negative, matching `std::to_string`.
  cg.bind(asInt);
  cg.movFromMem(Rax, Rbx, 8);
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);       // cursor = end of buffer
  emitIntDigitsBackwards(cg, R8, intPos, intConv, intLoop, intEmit);
  cg.movReg(R12, Rsp);
  cg.addImm32(R12, 64);
  cg.subReg(R12, R8);        // length = end - start
  cg.jmp(buildConst);

  // A range: `<start>..<end>` (exclusive) or `<start>..=<end>` (inclusive),
  // built backwards into the buffer (end, separator, then start).
  cg.bind(asRange);
  cg.movFromMem(R12, Rbx, 8);            // range header
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);                   // cursor = end of buffer
  cg.movFromMem(Rax, R12, kRangeEndOffset);
  emitIntDigitsBackwards(cg, R8, rangeEndPos, rangeEndConv, rangeEndLoop, rangeEndEmit);
  cg.movFromMem(R9, R12, kRangeInclusiveOffset);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, rangeIncl);
  cg.subRegImm32(R8, 3);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 2, static_cast<std::uint8_t>('='));
  cg.jmp(rangeAfterSep);
  cg.bind(rangeIncl);
  cg.subRegImm32(R8, 2);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.bind(rangeAfterSep);
  cg.movFromMem(Rax, R12, kRangeStartOffset);
  emitIntDigitsBackwards(cg, R8, rangeStartPos, rangeStartConv, rangeStartLoop,
                         rangeStartEmit);
  cg.movReg(R12, Rsp);
  cg.addImm32(R12, 64);
  cg.subReg(R12, R8);                    // length = end - start
  cg.jmp(buildConst);

  // A Float/Double: format with the embedded dtoa into a stack frame, then copy
  // the text into the arena string. The frame keeps rsp 16-byte aligned at the
  // `call`, as the ABI and the formatter's own calls require.
  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movFromMem(Rdi, Rbx, 0);                 // tag
  cg.movFromMem(Rsi, Rbx, 8);                 // payload
  cg.movReg(Rcx, Rsp);                        // scratch = rsp
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out
  cg.callLabel(formatDouble);
  cg.movReg(R12, Rax);                        // length
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, static_cast<std::int32_t>(kDtoaScratchBytes));   // text start
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, numOom);
  cg.movToMem(Rax, 0, R12);                   // store the length
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R8);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(done);

  cg.bind(numOom);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(oom);

  // Build an `AotString` from a constant/data region: r8 = address, r12 = length.
  cg.bind(buildConst);
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rsi, R8);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  cg.addImm32(Rsp, 64);
  cg.leaRipLabel(Rdi, msgOom.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(msgOom.text.size()));
  cg.callLabel(fail);
  cg.ud2();

  cg.bind(done);
  cg.addImm32(Rsp, 64);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue jolt_builtin_range(AotValue* args, long argc, AotClosure* self)`:
// the hidden `$range` helper for the range literals `a..b` (inclusive = false)
// and `a..=b` (inclusive = true). Both bounds must be integers, matching the
// hosted helper. The range header lives in the arena.
void emitRangeBuiltin(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                      CodeGen::Label fail, const DataBlob& msgOom,
                      const DataBlob& msgRangeBounds) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label badBounds = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.movReg(Rbx, Rdi);  // args
  // args[0] and args[1] are the bounds (16 bytes apart, growing down); args[2]
  // is the inclusive flag. Both bounds must be Int.
  cg.movFromMem(Rax, Rbx, 0);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badBounds);
  cg.movFromMem(Rax, Rbx, -16);
  cg.cmpImm32(Rax, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Ne, badBounds);

  cg.movImm(Rdi, static_cast<std::int64_t>(kAotRangeHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R12, Rax);
  cg.movFromMem(R8, Rbx, 8);        // start
  cg.movToMem(R12, kRangeStartOffset, R8);
  cg.movFromMem(R8, Rbx, -8);       // end
  cg.movToMem(R12, kRangeEndOffset, R8);
  cg.movFromMem(R8, Rbx, -24);      // inclusive flag (bool payload)
  cg.movToMem(R12, kRangeInclusiveOffset, R8);
  cg.movReg(Rdx, R12);
  cg.movImm(Rax, kTagRange);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(badBounds);
  emitFailCall(cg, fail, msgRangeBounds);
  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void jolt_sb_init(AotStringBuilder* sb, char* data, long capacity)`: start a
// builder over a caller-owned buffer (`data`/`capacity`) with `length` zero. A
// caller that renders a `print` value passes a stack buffer, so a small render
// never touches the arena; `jolt_sb_new` passes an arena block. Growth copies
// the bytes to a fresh arena block, so the initial buffer may be on the stack.
void emitSbInit(CodeGen& cg, CodeGen::Label label) {
  cg.bind(label);
  cg.movImmToMem(Rdi, kSbLengthOffset, 0);
  cg.movToMem(Rdi, kSbCapacityOffset, Rdx);
  cg.movToMem(Rdi, kSbDataOffset, Rsi);
  cg.retInsn();
}

// `void jolt_sb_append(AotStringBuilder* sb, const char* ptr, long len)`:
// append `len` bytes from `ptr`, growing the block when the capacity is short.
// Growth doubles the capacity until it fits, allocates a fresh arena block and
// copies the bytes already written. Preserves rbx/r12 (the builder and the
// source pointer), which are callee-saved.
void emitSbAppend(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
                  CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label grow = cg.newLabel();
  CodeGen::Label growLoop = cg.newLabel();
  CodeGen::Label growDone = cg.newLabel();
  CodeGen::Label noCopy = cg.newLabel();
  CodeGen::Label copy = cg.newLabel();
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);            // sb
  cg.movReg(R12, Rsi);            // ptr
  cg.movToMem(Rbp, -24, Rdx);     // len
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.addReg(Rax, Rdx);            // needed = length + len
  cg.movToMem(Rbp, -32, Rax);
  cg.movFromMem(R8, Rbx, kSbCapacityOffset);
  cg.cmpReg(Rax, R8);
  cg.jcc(Jcc::A, grow);
  cg.jmp(copy);

  cg.bind(grow);
  cg.movFromMem(R8, Rbx, kSbCapacityOffset);
  cg.movFromMem(R9, Rbp, -32);
  cg.bind(growLoop);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, growDone);
  cg.addReg(R8, R8);
  cg.jmp(growLoop);
  cg.bind(growDone);
  cg.movToMem(Rbp, -32, R8);      // newCap
  cg.movReg(Rdi, R8);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(R10, Rax);            // new block
  cg.movFromMem(Rcx, Rbx, kSbLengthOffset);
  cg.testReg(Rcx);
  cg.jcc(Jcc::Eq, noCopy);
  cg.movReg(Rdi, R10);
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.repMovsb();
  cg.bind(noCopy);
  cg.movToMem(Rbx, kSbDataOffset, R10);
  cg.movFromMem(R8, Rbp, -32);
  cg.movToMem(Rbx, kSbCapacityOffset, R8);

  cg.bind(copy);
  cg.movFromMem(Rdi, Rbx, kSbDataOffset);
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.addReg(Rdi, Rax);            // dst = data + length
  cg.movReg(Rsi, R12);
  cg.movFromMem(Rcx, Rbp, -24);
  cg.repMovsb();
  cg.movFromMem(Rax, Rbx, kSbLengthOffset);
  cg.movFromMem(R9, Rbp, -24);
  cg.addReg(Rax, R9);
  cg.movToMem(Rbx, kSbLengthOffset, Rax);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void jolt_sb_append_byte(AotStringBuilder* sb, long byte)`: append one byte.
// A thin wrapper over `jolt_sb_append` through a one-byte stack slot.
void emitSbAppendByte(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend) {
  cg.bind(label);
  prologue(cg);
  cg.subRegImm32(Rsp, 16);
  cg.movByteToMem(Rsp, 0, Rsi);
  cg.movReg(Rsi, Rsp);
  cg.movImm(Rdx, 1);
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, 16);
  epilogue(cg);
}

// `AotStringBuilder* jolt_sb_new(void)`: allocate a builder header and its
// initial block from the arena, and return the header address. Reports "out of
// memory" on a null allocation.
void emitSbNew(CodeGen& cg, CodeGen::Label label, CodeGen::Label allocLabel,
               CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.subRegImm32(Rsp, 16);
  cg.movImm(Rdi, static_cast<std::int64_t>(kAotSbHeaderSize));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movReg(Rbx, Rax);
  cg.movImm(Rdi, kSbInitialCapacity);
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rbx, kSbDataOffset, Rax);
  cg.movImmToMem(Rbx, kSbLengthOffset, 0);
  cg.movImmToMem(Rbx, kSbCapacityOffset, kSbInitialCapacity);
  cg.movReg(Rax, Rbx);
  cg.addImm32(Rsp, 16);
  cg.pop(Rbx);
  epilogue(cg);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);
}

// `void jolt_sb_append_escaped(AotStringBuilder* sb, AotString* s)`: append `s`
// between double quotes with the same escapes as `quoteString`: `\"`, `\\`,
// `\n`, `\t`, `\r`, and `\xHH` for any other control byte. This is the
// builder-sink counterpart of `jolt_print_str_escaped`, so a quoted string in a
// rendered collection reads identically on `print` and `$str`.
void emitSbAppendEscaped(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend,
                         CodeGen::Label sbAppendByte, const DataBlob& textQuote,
                         const DataBlob& textEscQuote, const DataBlob& textEscBackslash,
                         const DataBlob& textEscNL, const DataBlob& textEscTab,
                         const DataBlob& textEscCR) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label plain = cg.newLabel();
  CodeGen::Label hex = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label loopDone = cg.newLabel();
  CodeGen::Label escQuote = cg.newLabel();
  CodeGen::Label escBack = cg.newLabel();
  CodeGen::Label escNL = cg.newLabel();
  CodeGen::Label escTab = cg.newLabel();
  CodeGen::Label escCR = cg.newLabel();
  CodeGen::Label hexLow = cg.newLabel();
  CodeGen::Label hexHigh = cg.newLabel();
  CodeGen::Label hexLowDone = cg.newLabel();
  CodeGen::Label hexHighDone = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.push(X64Reg::R13);
  cg.subRegImm32(Rsp, 40);
  cg.movReg(Rbx, Rdi);  // sb
  cg.movReg(R12, Rsi);  // string
  cg.movImmToMem(Rbp, -40, 0);  // index

  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textQuote.text.size()));
  cg.callLabel(sbAppend);

  cg.bind(loop);
  cg.movFromMem(X64Reg::R13, Rbp, -40);
  cg.movFromMem(R9, R12, 0);
  cg.cmpReg(X64Reg::R13, R9);
  cg.jcc(Jcc::Ae, loopDone);

  cg.movReg(R10, X64Reg::R13);
  cg.addReg(R10, R12);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.cmpImm32(R10, '"');
  cg.jcc(Jcc::Eq, escQuote);
  cg.cmpImm32(R10, '\\');
  cg.jcc(Jcc::Eq, escBack);
  cg.cmpImm32(R10, '\n');
  cg.jcc(Jcc::Eq, escNL);
  cg.cmpImm32(R10, '\t');
  cg.jcc(Jcc::Eq, escTab);
  cg.cmpImm32(R10, '\r');
  cg.jcc(Jcc::Eq, escCR);
  cg.cmpImm32(R10, 0x20);
  cg.jcc(Jcc::B, hex);
  cg.jmp(plain);

  cg.bind(escQuote);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscQuote.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escBack);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscBackslash.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscBackslash.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escNL);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscNL.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscNL.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escTab);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscTab.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscTab.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);
  cg.bind(escCR);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEscCR.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEscCR.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(next);

  cg.bind(hex);
  cg.movImmByteToMem(Rbp, -48, '\\');
  cg.movImmByteToMem(Rbp, -47, 'x');
  cg.movFromMem(R10, Rbp, -40);
  cg.addReg(R10, R12);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.movReg(R9, R10);
  cg.shrImm8(R9, 4);
  cg.andImm32(R9, 0x0F);
  cg.movReg(R8, R9);
  cg.cmpImm32(R8, 10);
  cg.jcc(Jcc::B, hexHigh);
  cg.addImm32(R8, 'a' - 10);
  cg.jmp(hexHighDone);
  cg.bind(hexHigh);
  cg.addImm32(R8, '0');
  cg.bind(hexHighDone);
  cg.movByteToMem(Rbp, -46, R8);
  cg.andImm32(R10, 0x0F);
  cg.cmpImm32(R10, 10);
  cg.jcc(Jcc::B, hexLow);
  cg.addImm32(R10, 'a' - 10);
  cg.jmp(hexLowDone);
  cg.bind(hexLow);
  cg.addImm32(R10, '0');
  cg.bind(hexLowDone);
  cg.movByteToMem(Rbp, -45, R10);
  cg.movReg(Rdi, Rbx);
  cg.leaMem(Rsi, Rbp, -48);
  cg.movImm(Rdx, 4);
  cg.callLabel(sbAppend);
  cg.jmp(next);

  cg.bind(plain);
  cg.movReg(Rdi, Rbx);
  cg.movReg(Rsi, R10);
  cg.callLabel(sbAppendByte);

  cg.bind(next);
  cg.movFromMem(X64Reg::R13, Rbp, -40);
  cg.addImm32(X64Reg::R13, 1);
  cg.movToMem(Rbp, -40, X64Reg::R13);
  cg.jmp(loop);

  cg.bind(loopDone);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textQuote.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textQuote.text.size()));
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, 40);
  cg.pop(X64Reg::R13);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void jolt_repr_into(AotValue v, long depth, AotStringBuilder* sb)`: append
// the representation of `v` to `sb`, the builder-sink counterpart of the hosted
// `Value::repr` (`[1, 2]`, `{"k": 1}`, quoted strings, `<fn>`). `depth` caps the
// recursion at 8, where `...` is appended instead, so a self-referential
// structure terminates. Every `print` and every `$str` of a collection goes
// through here, so the two agree byte for byte.
void emitReprInto(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbAppend,
                  CodeGen::Label sbAppendEscaped, CodeGen::Label formatDouble,
                  const DataBlob& textTrue, const DataBlob& textFalse,
                  const DataBlob& textNil, const DataBlob& textFunc,
                  const DataBlob& textComma, const DataBlob& textColon,
                  const DataBlob& textOpenBracket, const DataBlob& textCloseBracket,
                  const DataBlob& textOpenBrace, const DataBlob& textCloseBrace,
                  const DataBlob& textEllipsis) {
  CodeGen::Label asInt = cg.newLabel();
  CodeGen::Label asBool = cg.newLabel();
  CodeGen::Label boolFalse = cg.newLabel();
  CodeGen::Label asNil = cg.newLabel();
  CodeGen::Label asStr = cg.newLabel();
  CodeGen::Label asNum = cg.newLabel();
  CodeGen::Label asFunc = cg.newLabel();
  CodeGen::Label asArray = cg.newLabel();
  CodeGen::Label asMap = cg.newLabel();
  CodeGen::Label asRange = cg.newLabel();
  CodeGen::Label rangeIncl = cg.newLabel();
  CodeGen::Label rangeAfterSep = cg.newLabel();
  CodeGen::Label arrayLoop = cg.newLabel();
  CodeGen::Label arrayBody = cg.newLabel();
  CodeGen::Label arrayEnd = cg.newLabel();
  CodeGen::Label mapLoop = cg.newLabel();
  CodeGen::Label mapBody = cg.newLabel();
  CodeGen::Label mapEnd = cg.newLabel();
  CodeGen::Label tooDeep = cg.newLabel();
  CodeGen::Label done = cg.newLabel();
  CodeGen::Label intPos = cg.newLabel();
  CodeGen::Label intConv = cg.newLabel();
  CodeGen::Label intLoop = cg.newLabel();
  CodeGen::Label intEmit = cg.newLabel();
  CodeGen::Label rangeEndPos = cg.newLabel();
  CodeGen::Label rangeEndConv = cg.newLabel();
  CodeGen::Label rangeEndLoop = cg.newLabel();
  CodeGen::Label rangeEndEmit = cg.newLabel();
  CodeGen::Label rangeStartPos = cg.newLabel();
  CodeGen::Label rangeStartConv = cg.newLabel();
  CodeGen::Label rangeStartLoop = cg.newLabel();
  CodeGen::Label rangeStartEmit = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 128);   // digit buffer (rbp-144..rbp-80) + locals
  cg.movReg(Rbx, Rcx);        // sb
  cg.movToMem(Rbp, -24, Rdx); // depth
  cg.movToMem(Rbp, -32, Rdi); // tag
  cg.movToMem(Rbp, -40, Rsi); // payload

  cg.cmpImm32(Rdx, static_cast<std::int32_t>(kReprMaxDepth));
  cg.jcc(Jcc::Gt, tooDeep);

  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagInt));
  cg.jcc(Jcc::Eq, asInt);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagBool));
  cg.jcc(Jcc::Eq, asBool);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagNil));
  cg.jcc(Jcc::Eq, asNil);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagStr));
  cg.jcc(Jcc::Eq, asStr);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFloat));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagDouble));
  cg.jcc(Jcc::Eq, asNum);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagFunc));
  cg.jcc(Jcc::Eq, asFunc);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagArray));
  cg.jcc(Jcc::Eq, asArray);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagMap));
  cg.jcc(Jcc::Eq, asMap);
  cg.cmpImm32(Rdi, static_cast<std::int32_t>(kTagRange));
  cg.jcc(Jcc::Eq, asRange);
  cg.jmp(done);

  cg.bind(asInt);
  cg.movFromMem(Rax, Rbp, -40);
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);        // cursor = end of digit buffer
  emitIntDigitsBackwards(cg, R8, intPos, intConv, intLoop, intEmit);
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, 64);
  cg.subReg(Rdx, R8);         // len = end - cursor
  cg.movReg(Rsi, R8);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asBool);
  cg.movFromMem(R8, Rbp, -40);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, boolFalse);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textTrue.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textTrue.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);
  cg.bind(boolFalse);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textFalse.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textFalse.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asNil);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textNil.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textNil.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asStr);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, Rbp, -40);
  cg.callLabel(sbAppendEscaped);
  cg.jmp(done);

  cg.bind(asNum);
  cg.subRegImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.movFromMem(Rdi, Rbp, -32);               // tag
  cg.movFromMem(Rsi, Rbp, -40);               // payload
  cg.movReg(Rcx, Rsp);                        // scratch
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, static_cast<std::int32_t>(kDtoaScratchBytes));  // out
  cg.callLabel(formatDouble);
  cg.movReg(Rdx, Rax);                        // len
  cg.movReg(Rsi, Rsp);
  cg.addImm32(Rsi, static_cast<std::int32_t>(kDtoaScratchBytes));
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.addImm32(Rsp, static_cast<std::int32_t>(kDtoaFrameBytes));
  cg.jmp(done);

  cg.bind(asFunc);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textFunc.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textFunc.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asArray);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textOpenBracket.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textOpenBracket.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -40);        // array header
  cg.movImmToMem(Rbp, -48, 0);         // index
  cg.bind(arrayLoop);
  cg.movFromMem(R8, Rbp, -48);
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, arrayEnd);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, arrayBody);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textComma.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textComma.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(arrayBody);
  cg.movFromMem(R8, Rbp, -48);
  cg.shlImm8(R8, 4);
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movFromMem(Rdi, R9, 0);
  cg.movFromMem(Rsi, R9, 8);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.addImm32(Rdx, 1);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(label);
  cg.movFromMem(R8, Rbp, -48);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -48, R8);
  cg.jmp(arrayLoop);
  cg.bind(arrayEnd);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseBracket.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseBracket.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asMap);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textOpenBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textOpenBrace.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R12, Rbp, -40);        // map header
  cg.movImmToMem(Rbp, -48, 0);         // index
  cg.bind(mapLoop);
  cg.movFromMem(R8, Rbp, -48);
  cg.movFromMem(R9, R12, kTableLengthOffset);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, mapEnd);
  cg.testReg(R8);
  cg.jcc(Jcc::Eq, mapBody);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textComma.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textComma.text.size()));
  cg.callLabel(sbAppend);
  cg.bind(mapBody);
  cg.movFromMem(R8, Rbp, -48);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movReg(Rdi, Rbx);
  cg.movFromMem(Rsi, R9, kMapEntryKeyOffset);
  cg.callLabel(sbAppendEscaped);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textColon.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textColon.text.size()));
  cg.callLabel(sbAppend);
  cg.movFromMem(R8, Rbp, -48);
  cg.imulImm32(R8, R8, static_cast<std::int32_t>(kMapEntrySize));
  cg.movFromMem(R9, R12, kTableBlockOffset);
  cg.addReg(R9, R8);
  cg.movFromMem(Rdi, R9, kMapEntryValueOffset);
  cg.movFromMem(Rsi, R9, kMapEntryValueOffset + 8);
  cg.movFromMem(Rdx, Rbp, -24);
  cg.addImm32(Rdx, 1);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(label);
  cg.movFromMem(R8, Rbp, -48);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -48, R8);
  cg.jmp(mapLoop);
  cg.bind(mapEnd);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textCloseBrace.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textCloseBrace.text.size()));
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(asRange);
  cg.movFromMem(R12, Rbp, -40);        // range header
  cg.movReg(R8, Rsp);
  cg.addImm32(R8, 64);                 // cursor = end of digit buffer
  cg.movFromMem(Rax, R12, kRangeEndOffset);
  emitIntDigitsBackwards(cg, R8, rangeEndPos, rangeEndConv, rangeEndLoop, rangeEndEmit);
  cg.movFromMem(R9, R12, kRangeInclusiveOffset);
  cg.testReg(R9);
  cg.jcc(Jcc::Eq, rangeIncl);
  cg.subRegImm32(R8, 3);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 2, static_cast<std::uint8_t>('='));
  cg.jmp(rangeAfterSep);
  cg.bind(rangeIncl);
  cg.subRegImm32(R8, 2);
  cg.movImmByteToMem(R8, 0, static_cast<std::uint8_t>('.'));
  cg.movImmByteToMem(R8, 1, static_cast<std::uint8_t>('.'));
  cg.bind(rangeAfterSep);
  cg.movFromMem(Rax, R12, kRangeStartOffset);
  emitIntDigitsBackwards(cg, R8, rangeStartPos, rangeStartConv, rangeStartLoop,
                         rangeStartEmit);
  cg.movReg(Rdx, Rsp);
  cg.addImm32(Rdx, 64);
  cg.subReg(Rdx, R8);
  cg.movReg(Rsi, R8);
  cg.movReg(Rdi, Rbx);
  cg.callLabel(sbAppend);
  cg.jmp(done);

  cg.bind(tooDeep);
  cg.movReg(Rdi, Rbx);
  cg.leaRipLabel(Rsi, textEllipsis.label);
  cg.movImm(Rdx, static_cast<std::int64_t>(textEllipsis.text.size()));
  cg.callLabel(sbAppend);

  cg.bind(done);
  cg.addImm32(Rsp, 128);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `AotValue jolt_repr_to_string(AotValue v)`: render `v` with the recursive repr
// (`jolt_repr_into`) into a fresh arena `AotString` and return it as a `Str`
// value. The `$str` helper uses it for a collection, function or range operand,
// so `"${[1, 2]}"` reads exactly like `print([1, 2])`.
void emitReprToString(CodeGen& cg, CodeGen::Label label, CodeGen::Label sbNew,
                      CodeGen::Label reprInto, CodeGen::Label allocLabel,
                      CodeGen::Label fail, const DataBlob& msgOom) {
  CodeGen::Label oom = cg.newLabel();
  CodeGen::Label done = cg.newLabel();

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 16);
  cg.movReg(R12, Rdi);          // tag
  cg.movToMem(Rbp, -24, Rsi);   // payload
  cg.callLabel(sbNew);          // rax = builder
  cg.movReg(Rbx, Rax);
  cg.movReg(Rdi, R12);
  cg.movFromMem(Rsi, Rbp, -24);
  cg.xorReg(Rdx, Rdx);
  cg.movReg(Rcx, Rbx);
  cg.callLabel(reprInto);
  cg.movFromMem(R12, Rbx, kSbLengthOffset);   // length
  cg.movReg(Rdi, R12);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.callLabel(allocLabel);
  cg.testReg(Rax);
  cg.jcc(Jcc::Eq, oom);
  cg.movToMem(Rax, 0, R12);
  cg.movReg(Rdi, Rax);
  cg.addImm32(Rdi, static_cast<std::int32_t>(kStringDataOffset));
  cg.movFromMem(Rsi, Rbx, kSbDataOffset);
  cg.movReg(Rcx, R12);
  cg.repMovsb();
  cg.movReg(Rdx, Rax);
  cg.movImm(Rax, kTagStr);
  cg.jmp(done);

  cg.bind(oom);
  emitFailCall(cg, fail, msgOom);

  cg.bind(done);
  cg.addImm32(Rsp, 16);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void jolt_print_str_escaped(AotString* s, long fd)`: writes `s` between
// double quotes with the same escapes as `quoteString`: `\"`, `\\`, `\n`,
// `\t`, `\r`, and `\xHH` for any other control byte.
void emitPrintStrEsc(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                     const DataBlob& textQuote, const DataBlob& textEscQuote,
                     const DataBlob& textEscBackslash, const DataBlob& textEscNL,
                     const DataBlob& textEscTab, const DataBlob& textEscCR,
                     const DataBlob& textHexPrefix) {
  CodeGen::Label loop = cg.newLabel();
  CodeGen::Label plain = cg.newLabel();
  CodeGen::Label hex = cg.newLabel();
  CodeGen::Label next = cg.newLabel();
  CodeGen::Label loopDone = cg.newLabel();
  CodeGen::Label escQuote = cg.newLabel();
  CodeGen::Label escBack = cg.newLabel();
  CodeGen::Label escNL = cg.newLabel();
  CodeGen::Label escTab = cg.newLabel();
  CodeGen::Label escCR = cg.newLabel();
  CodeGen::Label hexLow = cg.newLabel();
  CodeGen::Label hexHigh = cg.newLabel();
  CodeGen::Label hexLowDone = cg.newLabel();
  CodeGen::Label hexHighDone = cg.newLabel();
  (void)textHexPrefix;

  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);
  cg.push(R12);
  cg.subRegImm32(Rsp, 32);
  cg.movReg(Rbx, Rdi);  // string
  cg.movReg(R12, Rsi);  // fd
  cg.movImmToMem(Rbp, -24, 0);  // index

  cg.leaRipLabel(Rdi, textQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);

  cg.bind(loop);
  cg.movFromMem(R8, Rbp, -24);
  cg.movFromMem(R9, Rbx, 0);
  cg.cmpReg(R8, R9);
  cg.jcc(Jcc::Ae, loopDone);

  cg.movReg(R10, R8);
  cg.addReg(R10, Rbx);
  cg.movzxFromMem(R10, R10, kStringDataOffset);  // byte
  cg.cmpImm32(R10, '"');
  cg.jcc(Jcc::Eq, escQuote);
  cg.cmpImm32(R10, '\\');
  cg.jcc(Jcc::Eq, escBack);
  cg.cmpImm32(R10, '\n');
  cg.jcc(Jcc::Eq, escNL);
  cg.cmpImm32(R10, '\t');
  cg.jcc(Jcc::Eq, escTab);
  cg.cmpImm32(R10, '\r');
  cg.jcc(Jcc::Eq, escCR);
  cg.cmpImm32(R10, 0x20);
  cg.jcc(Jcc::B, hex);
  cg.jmp(plain);

  cg.bind(escQuote);
  cg.leaRipLabel(Rdi, textEscQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escBack);
  cg.leaRipLabel(Rdi, textEscBackslash.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscBackslash.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escNL);
  cg.leaRipLabel(Rdi, textEscNL.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscNL.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escTab);
  cg.leaRipLabel(Rdi, textEscTab.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscTab.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);
  cg.bind(escCR);
  cg.leaRipLabel(Rdi, textEscCR.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textEscCR.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);

  cg.bind(hex);
  // Build "\xHH" in a 4-byte stack buffer and write it in one syscall.
  cg.movImmByteToMem(Rbp, -40, '\\');
  cg.movImmByteToMem(Rbp, -39, 'x');
  cg.movFromMem(R10, Rbp, -24);
  cg.addReg(R10, Rbx);
  cg.movzxFromMem(R10, R10, kStringDataOffset);
  cg.movReg(R9, R10);
  cg.shrImm8(R9, 4);
  cg.andImm32(R9, 0x0F);
  cg.movReg(R8, R9);
  cg.cmpImm32(R8, 10);
  cg.jcc(Jcc::B, hexHigh);
  cg.addImm32(R8, 'a' - 10);
  cg.jmp(hexHighDone);
  cg.bind(hexHigh);
  cg.addImm32(R8, '0');
  cg.bind(hexHighDone);
  cg.movByteToMem(Rbp, -38, R8);
  cg.andImm32(R10, 0x0F);
  cg.cmpImm32(R10, 10);
  cg.jcc(Jcc::B, hexLow);
  cg.addImm32(R10, 'a' - 10);
  cg.jmp(hexLowDone);
  cg.bind(hexLow);
  cg.addImm32(R10, '0');
  cg.bind(hexLowDone);
  cg.movByteToMem(Rbp, -37, R10);
  cg.leaMem(Rdi, Rbp, -40);
  cg.movImm(Rsi, 4);
  inlineWriteFd(cg, writeLabel, R12);
  cg.jmp(next);

  cg.bind(plain);
  cg.movFromMem(R8, Rbp, -24);
  cg.movReg(R9, R8);
  cg.addReg(R9, Rbx);
  cg.addImm32(R9, static_cast<std::int32_t>(kStringDataOffset));
  cg.movReg(Rdi, R9);
  cg.movImm(Rsi, 1);
  inlineWriteFd(cg, writeLabel, R12);

  cg.bind(next);
  cg.movFromMem(R8, Rbp, -24);
  cg.addImm32(R8, 1);
  cg.movToMem(Rbp, -24, R8);
  cg.jmp(loop);

  cg.bind(loopDone);
  cg.leaRipLabel(Rdi, textQuote.label);
  cg.movImm(Rsi, static_cast<std::int64_t>(textQuote.text.size()));
  inlineWriteFd(cg, writeLabel, R12);
  cg.addImm32(Rsp, 32);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
}

// `void jolt_print_repr(AotValue v, long depth, long fd)`: the recursive
// container renderer, matching `Value::repr` (`[1, 2]`, `{"k": 1}`, quoted
// strings, `<fn>`). It renders into a stack string builder and writes the bytes
// in one syscall; `jolt_repr_into` does the traversal, so `print` and `$str`
// share one renderer.
void emitPrintRepr(CodeGen& cg, CodeGen::Label label, CodeGen::Label writeLabel,
                   CodeGen::Label sbInit, CodeGen::Label reprInto) {
  cg.bind(label);
  prologue(cg);
  cg.push(Rbx);                  // saved at [rbp-8]
  cg.push(R12);                  // saved at [rbp-16]
  cg.subRegImm32(Rsp, 640);      // builder header + 512-byte initial buffer
  // The builder header sits below the saved registers: [rbp-40] length,
  // [rbp-32] capacity, [rbp-24] data. The buffer runs [rbp-552, rbp-40).
  cg.movToMem(Rbp, -560, Rdi);   // value tag
  cg.movToMem(Rbp, -568, Rsi);   // value payload
  cg.movToMem(Rbp, -576, Rdx);   // depth
  cg.movReg(R12, Rcx);           // fd
  cg.leaMem(Rdi, Rbp, -40);      // builder header
  cg.leaMem(Rsi, Rbp, -552);     // initial buffer
  cg.movImm(Rdx, 512);
  cg.callLabel(sbInit);
  cg.leaMem(Rcx, Rbp, -40);      // sb = header
  cg.movFromMem(Rdi, Rbp, -560);
  cg.movFromMem(Rsi, Rbp, -568);
  cg.movFromMem(Rdx, Rbp, -576);
  cg.callLabel(reprInto);
  cg.movFromMem(Rsi, Rbp, -40);  // length
  cg.movFromMem(Rdi, Rbp, -24);  // data
  cg.movReg(R10, R12);
  inlineWriteFd(cg, writeLabel, R10);
  cg.addImm32(Rsp, 640);
  cg.pop(R12);
  cg.pop(Rbx);
  epilogue(cg);
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
  DataBlob msgCallNonFunc = addText("jolt: cannot call a non-function value\n");
  DataBlob msgArity = addText("jolt: wrong number of arguments\n");
  DataBlob msgArrayIndexType = addText("jolt: array index must be an integer\n");
  DataBlob msgIndexRange = addText("jolt: index out of range\n");
  DataBlob msgMapKeyType = addText("jolt: map key must be a string\n");
  DataBlob msgBadIndex = addText("jolt: value is not indexable\n");
  DataBlob msgNoMember = addText("jolt: value has no such member\n");
  DataBlob msgImmutable = addText("jolt: strings are immutable\n");
  DataBlob msgBadAssign = addText("jolt: cannot assign to this value\n");
  DataBlob msgBadField = addText("jolt: cannot assign a field on this value\n");
  DataBlob msgBadIn = addText("jolt: unsupported operands for 'in'\n");
  DataBlob msgMapValue = addText("jolt: key/value iteration ('for k, v in ...') requires a map\n");
  DataBlob msgNonNull = addText("jolt: unexpected nil (non-null assertion '!!' failed)\n");
  DataBlob msgStrBad = addText("jolt: cannot render this value to a string\n");
  DataBlob textTrue = addText("true");
  DataBlob textFalse = addText("false");
  DataBlob textNil = addText("nil");
  DataBlob textNewline = addText("\n");
  DataBlob textSpace = addText(" ");
  DataBlob textFunc = addText("<fn>");
  DataBlob textComma = addText(", ");
  DataBlob textColon = addText(": ");
  DataBlob textOpenBracket = addText("[");
  DataBlob textCloseBracket = addText("]");
  DataBlob textOpenBrace = addText("{");
  DataBlob textCloseBrace = addText("}");
  DataBlob textEllipsis = addText("...");
  DataBlob msgRangeBounds = addText("jolt: range bounds must be integers\n");
  DataBlob textQuote = addText("\"");
  DataBlob textEscQuote = addText("\\\"");
  DataBlob textEscBackslash = addText("\\\\");
  DataBlob textEscNL = addText("\\n");
  DataBlob textEscTab = addText("\\t");
  DataBlob textEscCR = addText("\\r");
  DataBlob textHexPrefix = addText("\\x");

  CodeGen::Label fail = cg.newLabel();
  CodeGen::Label writeInt = cg.newLabel();
  CodeGen::Label printRepr = cg.newLabel();
  CodeGen::Label printStrEsc = cg.newLabel();
  CodeGen::Label sbInit = cg.newLabel();
  CodeGen::Label sbAppend = cg.newLabel();
  CodeGen::Label sbAppendByte = cg.newLabel();
  CodeGen::Label sbNew = cg.newLabel();
  CodeGen::Label sbAppendEscaped = cg.newLabel();
  CodeGen::Label reprInto = cg.newLabel();
  CodeGen::Label reprToString = cg.newLabel();

  // The float/double formatter is part of the embedded runtime: bind its
  // handles and emit the whole dtoa routine so `jolt_print_value` can call it.
  dtoa::DtoaHandles dtoaHandles = dtoa::newDtoaHandles(cg);

  // Collection helpers reference one another and the printers, so the env is
  // built from the handles and the labels above before any of them is emitted.
  CollectionEnv env{};
  env.alloc = handles.alloc;
  env.fail = fail;
  env.write = entry.write;
  env.writeInt = writeInt;
  env.formatDouble = dtoaHandles.formatDouble;
  env.strEq = handles.strEq;
  env.mapFind = cg.newLabel();
  env.blockEnsure = cg.newLabel();
  env.utf8Len = cg.newLabel();
  env.utf8CharAt = cg.newLabel();
  env.compare = handles.compareValues;
  env.rawPrint = handles.printValueRaw;
  env.printRepr = printRepr;
  env.printStrEsc = printStrEsc;
  env.lengthConst = cg.newLabel();
  env.msgOom = &msgOom;
  env.msgArrayIndexType = &msgArrayIndexType;
  env.msgIndexRange = &msgIndexRange;
  env.msgMapKeyType = &msgMapKeyType;
  env.msgBadIndex = &msgBadIndex;
  env.msgNoMember = &msgNoMember;
  env.msgImmutable = &msgImmutable;
  env.msgBadAssign = &msgBadAssign;
  env.msgBadField = &msgBadField;
  env.msgBadIn = &msgBadIn;
  env.msgMapValue = &msgMapValue;
  env.textTrue = &textTrue;
  env.textFalse = &textFalse;
  env.textNil = &textNil;
  env.textFunc = &textFunc;
  env.textComma = &textComma;
  env.textColon = &textColon;
  env.textOpenBracket = &textOpenBracket;
  env.textCloseBracket = &textCloseBracket;
  env.textOpenBrace = &textOpenBrace;
  env.textCloseBrace = &textCloseBrace;
  env.textEllipsis = &textEllipsis;
  env.textQuote = &textQuote;
  env.textEscQuote = &textEscQuote;
  env.textEscBackslash = &textEscBackslash;
  env.textEscNL = &textEscNL;
  env.textEscTab = &textEscTab;
  env.textEscCR = &textEscCR;
  env.textHexPrefix = &textHexPrefix;

  emitFail(cg, fail, entry.write, entry.exit);
  emitWriteInt(cg, writeInt, entry.write);
  emitAlloc(cg, handles.alloc);
  emitConcat(cg, handles.concat, handles.alloc, fail, msgOom);
  emitInitGlobals(cg, handles.initGlobals, handles.alloc, fail, msgOom);
  emitGlobalSet(cg, handles.globalSet);
  emitGlobalGet(cg, handles.globalGet);
  emitClosure(cg, handles.closure, handles.alloc, fail, msgOom);
  emitCellNew(cg, handles.cellNew, handles.alloc, fail, msgOom);
  emitCall(cg, handles.call, fail, msgCallNonFunc);
  emitArityError(cg, handles.arityError, fail, msgArity);
  emitPrintStrEsc(cg, printStrEsc, entry.write, textQuote, textEscQuote,
                  textEscBackslash, textEscNL, textEscTab, textEscCR, textHexPrefix);
  // The string builder and the recursive repr are shared by `print` and `$str`:
  // `jolt_repr_into` traverses, `jolt_print_repr` renders into a stack builder
  // and writes, `jolt_repr_to_string` renders into an arena string.
  emitSbInit(cg, sbInit);
  emitSbAppend(cg, sbAppend, handles.alloc, fail, msgOom);
  emitSbAppendByte(cg, sbAppendByte, sbAppend);
  emitSbNew(cg, sbNew, handles.alloc, fail, msgOom);
  emitSbAppendEscaped(cg, sbAppendEscaped, sbAppend, sbAppendByte, textQuote,
                      textEscQuote, textEscBackslash, textEscNL, textEscTab,
                      textEscCR);
  emitReprInto(cg, reprInto, sbAppend, sbAppendEscaped, dtoaHandles.formatDouble,
               textTrue, textFalse, textNil, textFunc, textComma, textColon,
               textOpenBracket, textCloseBracket, textOpenBrace, textCloseBrace,
               textEllipsis);
  emitReprToString(cg, reprToString, sbNew, reprInto, handles.alloc, fail, msgOom);
  emitPrintRepr(cg, printRepr, entry.write, sbInit, reprInto);
  emitPrintValue(cg, handles.printValue, entry.write, fail, writeInt,
                 dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                 textNil, textNewline);
  emitPrintValueRaw(cg, handles.printValueRaw, entry.write, fail, writeInt,
                    dtoaHandles.formatDouble, printRepr, msgPrint, textTrue, textFalse,
                    textNil, textNewline);
  emitPrintValueErrRaw(cg, handles.printValueErrRaw, entry.write, fail, writeInt,
                       dtoaHandles.formatDouble, printRepr, msgPrint, textTrue,
                       textFalse, textNil, textNewline);
  emitPrintBuiltin(cg, handles.printBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/true);
  emitPrintBuiltin(cg, handles.writeBuiltin, handles.printValueRaw, entry.write,
                   textSpace, textNewline, /*fd=*/1, /*newline=*/false);
  emitPrintBuiltin(cg, handles.eprintBuiltin, handles.printValueErrRaw, entry.write,
                   textSpace, textNewline, /*fd=*/2, /*newline=*/true);
  emitTruthy(cg, handles.truthy);
  emitArith(cg, handles.arithValues, fail, msgTypes, msgDivZero, handles.concat);
  emitPow(cg, handles.powValues, fail, msgPow);
  emitShift(cg, handles.shiftValues, fail, msgShift);
  emitBitwise(cg, handles.bitwiseValues, fail, msgBit);
  emitNegate(cg, handles.negateValue, fail, msgNeg);
  emitBitNot(cg, handles.bitNotValue, fail, msgBitNot);
  emitNot(cg, handles.notValue);
  emitCompare(cg, handles.compareValues, fail, msgCompare);
  emitStrEq(cg, handles.strEq);
  emitMapFind(cg, env.mapFind, env);
  emitBlockEnsure(cg, env.blockEnsure, env);
  emitUtf8Len(cg, env.utf8Len);
  emitUtf8CharAt(cg, env.utf8CharAt);
  emitTableNew(cg, handles.tableNew, env);
  emitTableGet(cg, handles.tableGet, env);
  emitMemberGet(cg, handles.memberGet, env);
  emitTableSet(cg, handles.tableSet, env);
  emitMemberSet(cg, handles.memberSet, env);
  emitContains(cg, handles.contains, env);
  emitIterNext(cg, handles.iterNext, env);
  emitMapValueBuiltin(cg, handles.mapValueBuiltin, env.mapFind, env);
  emitIsErrorBuiltin(cg, handles.isErrorBuiltin);
  emitNonNullBuiltin(cg, handles.nonNullBuiltin, fail, msgNonNull);
  emitStrBuiltin(cg, handles.strBuiltin, handles.alloc, fail,
                 dtoaHandles.formatDouble, reprToString, msgOom, textTrue, textFalse,
                 textNil, msgStrBad);
  emitRangeBuiltin(cg, handles.rangeBuiltin, handles.alloc, fail, msgOom,
                   msgRangeBounds);
  dtoa::emitDtoa(cg, dtoaHandles);

  // The "length" member name is a raw `AotString` (8-byte length + bytes), laid
  // out with the other data islands so no code falls through into it.
  cg.bind(env.lengthConst);
  cg.emitBytes(reinterpret_cast<const std::uint8_t*>("\x06\x00\x00\x00\x00\x00\x00\x00length"), 14);

  // Data islands last: every code path above ends in `ret` or `ud2`, so
  // control never reaches them, and their addresses are already referenced by
  // the `lea` fixups emitted above.
  for (const DataBlob& blob : data) {
    cg.bind(blob.label);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(blob.text.data()),
                 blob.text.size());
  }
}
