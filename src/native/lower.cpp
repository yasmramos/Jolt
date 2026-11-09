#include "native/lower.h"

#include "native/codegen.h"
#include "native/entry.h"
#include "native/runtime.h"
#include "native/value.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// PHASE 7c.1 lowers the runtime-free opcodes over the boxed `AotValue`. A
// bytecode register is now a 16-byte slot in a register file on the stack, not
// a physical CPU register: a boxed value does not fit in one, and the file is
// what makes `print`, collections and calls (7c.2/7c.3) possible without a
// second value representation. The file grows down from rbp:
//
//   [rbp - 16 * (i + 1)] = tag(Ri), [rbp - 16 * (i + 1) + 8] = payload(Ri)
//
// so R0 sits highest, right below rbp. rbp is both the frame pointer and the
// file base, so the epilogue (`mov rsp, rbp; pop rbp; ret`) needs no extra
// bookkeeping.

// The physical registers the lowering uses for scratch. The register file
// lives on the stack, so the CPU registers are free to be temporaries.
constexpr X64Reg kFileBase = X64Reg::Rbp;  // &R0 is [rbp - 16]
constexpr X64Reg kTmpTag = X64Reg::Rax;
constexpr X64Reg kTmpPayload = X64Reg::Rdx;

// Byte offset of a register slot from the file base (`rbp`). Slot i sits
// *below* R0, hence the negative offset.
constexpr std::int64_t slotOffset(int index) { return -16LL * (index + 1); }

// Loads register `index` into `tag`:`payload`. Two moves, the tagged load the
// lowering table budgets.
void loadValue(CodeGen& cg, int index, X64Reg tag, X64Reg payload) {
  cg.movFromMem(tag, kFileBase, slotOffset(index));
  cg.movFromMem(payload, kFileBase, slotOffset(index) + 8);
}

// Stores `tag`:`payload` into register `index`.
void storeValue(CodeGen& cg, int index, X64Reg tag, X64Reg payload) {
  cg.movToMem(kFileBase, slotOffset(index), tag);
  cg.movToMem(kFileBase, slotOffset(index) + 8, payload);
}

// Loads a 64-bit immediate into a register slot as an integer value.
void loadIntImmediate(CodeGen& cg, int index, std::int64_t value) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Int));
  cg.movImm(kTmpTag, value);
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Loads a boolean immediate into a register slot.
void loadBoolImmediate(CodeGen& cg, int index, bool value) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Bool));
  cg.movImmToMem(kFileBase, slotOffset(index) + 8, value ? 1 : 0);
}

// Loads nil into a register slot.
void loadNilImmediate(CodeGen& cg, int index) {
  cg.movImmToMem(kFileBase, slotOffset(index),
                 static_cast<std::int64_t>(AotTag::Nil));
  cg.movImmToMem(kFileBase, slotOffset(index) + 8, 0);
}

// Loads a float/double immediate into a register slot. The payload is the
// IEEE-754 bit pattern of the (already widened) binary64 value, exactly the
// representation the runtime helpers expect.
void loadFloatingImmediate(CodeGen& cg, int index, double value, AotTag tag) {
  cg.movImmToMem(kFileBase, slotOffset(index), static_cast<std::int64_t>(tag));
  cg.movImm(kTmpTag, static_cast<std::int64_t>(doubleBits(value)));
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Loads a string constant into a register slot. The payload is the address of
// an `AotString` laid out in the image (see `StringConstant`): a `lea` of the
// header, since the bytes live in the code buffer, not in the register file.
void loadStringImmediate(CodeGen& cg, int index, CodeGen::Label dataLabel) {
  cg.movImmToMem(kFileBase, slotOffset(index), static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(kTmpTag, dataLabel);
  cg.movToMem(kFileBase, slotOffset(index) + 8, kTmpTag);
}

// Emits a call to a runtime helper that takes two `AotValue`s (rdi:rsi,
// rdx:rcx) plus a 64-bit selector in r8, and stores the returned `AotValue`
// (rax:rdx) in register `dst`.
void callBinaryWithSelector(CodeGen& cg, CodeGen::Label helper, int lhs, int rhs,
                            int dst, std::int64_t selector) {
  loadValue(cg, lhs, X64Reg::Rdi, X64Reg::Rsi);
  loadValue(cg, rhs, X64Reg::Rdx, X64Reg::Rcx);
  cg.movImm(X64Reg::R8, selector);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

// PHASE 7c.1 still lowers a subset. Floats and strings need the arena and
// their own tags (7c.1b); calls, globals, locals and collections need frames
// and the allocator (7c.2/7c.3). Every other opcode raises a `LoweringError`
// instead of emitting wrong code.
[[noreturn]] void unsupported(const char* opName, const char* phase) {
  throw LoweringError(std::string("lowering: opcode '") + opName +
                      "' needs runtime support (" + phase + ")");
}

// Emits a call to a runtime helper that takes one `AotValue` (in rdi:rsi) and
// returns one (in rax:rdx), then stores the result in register `dst`.
void callUnary(CodeGen& cg, CodeGen::Label helper, int src, int dst) {
  loadValue(cg, src, X64Reg::Rdi, X64Reg::Rsi);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

// Same, for a helper taking two `AotValue`s (rdi:rsi, rdx:rcx).
void callBinary(CodeGen& cg, CodeGen::Label helper, int lhs, int rhs, int dst) {
  loadValue(cg, lhs, X64Reg::Rdi, X64Reg::Rsi);
  loadValue(cg, rhs, X64Reg::Rdx, X64Reg::Rcx);
  cg.callLabel(helper);
  storeValue(cg, dst, X64Reg::Rax, X64Reg::Rdx);
}

CodeGen::Label jumpTarget(const Instruction& instr, const Bytecode& proto,
                          const std::vector<CodeGen::Label>& labels) {
  if (instr.jump < 0 || static_cast<std::size_t>(instr.jump) >= proto.code.size()) {
    throw LoweringError("lowering: jump target " + std::to_string(instr.jump) +
                        " is outside the code (size " +
                        std::to_string(proto.code.size()) + ")");
  }
  return labels[static_cast<std::size_t>(instr.jump)];
}

// A string constant laid out in the image: an 8-byte length, then the bytes.
// `label` is bound at the header once the code is complete.
struct StringConstant {
  CodeGen::Label label;
  std::string bytes;
};

// Global variables (PHASE 7c.2a). The compiler names a global by a string
// constant index (`instruction.b`), but the binary has no strings: each
// distinct name gets an index into a flat `AotValue` array. The lowerer
// derives the map from the code itself, in first-assignment order, so the
// `Bytecode` structure and the `.jolc` format stay unchanged.
//
// A `GETGLOBAL` of a name the chunk never assigns is a lowering error naming
// the variable, the compile-time counterpart of the hosted engine's run-time
// `undefined variable`. Builtins (`print`, ...) are globals the hosted engine
// installs up front; the AOT backend has no builtins until calls land
// (7c.2c), at which point they will be seeded as pre-assigned here.
struct GlobalSlots {
  std::unordered_map<int, int> slotOfConstant;  // constant index -> slot
  int count = 0;
};

GlobalSlots collectGlobals(const Bytecode& proto) {
  GlobalSlots globals;
  for (const Instruction& instr : proto.code) {
    if (instr.op != OpCode::SETGLOBAL) continue;
    if (globals.slotOfConstant.find(instr.b) != globals.slotOfConstant.end()) continue;
    globals.slotOfConstant[instr.b] = globals.count++;
  }
  for (const Instruction& instr : proto.code) {
    if (instr.op != OpCode::GETGLOBAL) continue;
    if (globals.slotOfConstant.find(instr.b) != globals.slotOfConstant.end()) continue;
    const Value& name = proto.constants.at(static_cast<std::size_t>(instr.b));
    throw LoweringError("lowering: global '" +
                        (name.isStr() ? name.asStr() : std::string("<non-string>")) +
                        "' is read but never assigned");
  }
  return globals;
}

// Emits the string-constant data islands at the end of the code buffer. Called
// after every code path has ended in `ret`/`ud2`, so control never falls into
// them; the `lea` fixups emitted by `loadStringImmediate` resolve against these
// labels.
void emitStringConstants(CodeGen& cg, const std::vector<StringConstant>& constants) {
  for (const StringConstant& constant : constants) {
    cg.bind(constant.label);
    const std::uint64_t length = constant.bytes.size();
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(constant.bytes.data()),
                 constant.bytes.size());
  }
}

void lowerInstruction(const Instruction& instr, const Bytecode& proto, CodeGen& cg,
                      const std::vector<CodeGen::Label>& labels,
                      const AotRuntimeHandles& rt, const GlobalSlots& globals,
                      std::vector<StringConstant>& stringConstants) {
  switch (instr.op) {
    case OpCode::LOADK: {
      const Value& value = proto.constants.at(static_cast<std::size_t>(instr.b));
      if (value.isInt()) {
        loadIntImmediate(cg, instr.a, value.asInt());
      } else if (value.isBool()) {
        loadBoolImmediate(cg, instr.a, value.asBool());
      } else if (value.isNil()) {
        loadNilImmediate(cg, instr.a);
      } else if (value.isFloat()) {
        loadFloatingImmediate(cg, instr.a, value.asFloat(), AotTag::Float);
      } else if (value.isDouble()) {
        loadFloatingImmediate(cg, instr.a, value.asDouble(), AotTag::Double);
      } else if (value.isStr()) {
        StringConstant constant{cg.newLabel(), value.asStr()};
        loadStringImmediate(cg, instr.a, constant.label);
        stringConstants.push_back(std::move(constant));
      } else {
        unsupported("LOADK (unsupported constant)", "PHASE 7c.2");
      }
      break;
    }

    case OpCode::MOVE:
      loadValue(cg, instr.b, kTmpTag, kTmpPayload);
      storeValue(cg, instr.a, kTmpTag, kTmpPayload);
      break;

    case OpCode::ADD:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Add));
      break;
    case OpCode::SUB:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Sub));
      break;
    case OpCode::MUL:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Mul));
      break;
    case OpCode::DIV:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Div));
      break;
    case OpCode::MOD:
      callBinaryWithSelector(cg, rt.arithValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotArithOp::Mod));
      break;
    case OpCode::POW:
      callBinary(cg, rt.powValues, instr.b, instr.c, instr.a);
      break;

    case OpCode::SHL:
      callBinaryWithSelector(cg, rt.shiftValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotShiftOp::Shl));
      break;
    case OpCode::SHR:
      callBinaryWithSelector(cg, rt.shiftValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(AotShiftOp::Shr));
      break;

    case OpCode::BAND:
    case OpCode::BOR:
    case OpCode::BXOR: {
      // The selector is the third argument, so it goes in r8 (after the two
      // `AotValue`s occupy rdi:rsi and rdx:rcx). AotBitwiseOp matches the
      // hosted operator one-to-one.
      const AotBitwiseOp op = instr.op == OpCode::BAND
                                  ? AotBitwiseOp::And
                                  : (instr.op == OpCode::BOR ? AotBitwiseOp::Or
                                                             : AotBitwiseOp::Xor);
      callBinaryWithSelector(cg, rt.bitwiseValues, instr.b, instr.c, instr.a,
                             static_cast<std::int64_t>(op));
      break;
    }

    case OpCode::UNOP: {
      switch (static_cast<UnaryOp>(instr.c)) {
        case UnaryOp::Negate:
          callUnary(cg, rt.negateValue, instr.b, instr.a);
          break;
        case UnaryOp::Not:
          callUnary(cg, rt.notValue, instr.b, instr.a);
          break;
        case UnaryOp::BitNot:
          callUnary(cg, rt.bitNotValue, instr.b, instr.a);
          break;
        default:
          throw LoweringError("lowering: invalid UNOP selector " +
                              std::to_string(instr.c));
      }
      break;
    }

    case OpCode::EQ:
    case OpCode::NEQ:
    case OpCode::LT:
    case OpCode::LTE:
    case OpCode::GT:
    case OpCode::GTE: {
      // The op selector is the low nibble of the x86-64 condition code, so the
      // runtime dispatches with a `cmp` + `je` chain. Map each bytecode
      // comparison to its `AotCompareOp`.
      AotCompareOp op;
      switch (instr.op) {
        case OpCode::EQ: op = AotCompareOp::Eq; break;
        case OpCode::NEQ: op = AotCompareOp::Ne; break;
        case OpCode::LT: op = AotCompareOp::Lt; break;
        case OpCode::LTE: op = AotCompareOp::Le; break;
        case OpCode::GT: op = AotCompareOp::Gt; break;
        default: op = AotCompareOp::Ge; break;
      }
      loadValue(cg, instr.b, X64Reg::Rdi, X64Reg::Rsi);
      loadValue(cg, instr.c, X64Reg::Rdx, X64Reg::Rcx);
      cg.movImm(X64Reg::R8, static_cast<std::int64_t>(op));
      cg.callLabel(rt.compareValues);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }

    case OpCode::JMP:
      cg.jmp(jumpTarget(instr, proto, labels));
      break;
    case OpCode::JMP_IF_FALSE:
    case OpCode::JMP_IF_TRUE: {
      // The condition is the value's truthiness, not its raw payload: `0` is
      // truthy in Jolt, so `jolt_truthy` decides. Branch on its 0/1 result.
      loadValue(cg, instr.a, X64Reg::Rdi, X64Reg::Rsi);
      cg.callLabel(rt.truthy);
      cg.testReg(X64Reg::Rax);
      cg.jcc(instr.op == OpCode::JMP_IF_FALSE ? Jcc::Eq : Jcc::Ne,
             jumpTarget(instr, proto, labels));
      break;
    }

    case OpCode::HALT:
      // The chunk's result is R0, returned as an `AotValue` in rax:rdx (the
      // System V ABI's two-register return), so the test harness can read both
      // the tag and the payload. The process entry discards it and exits 0.
      // Restore the callee-saved registers the chunk prologue pushed, in
      // reverse order.
      loadValue(cg, 0, X64Reg::Rax, X64Reg::Rdx);
      cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
      cg.pop(X64Reg::R15);
      cg.pop(X64Reg::R14);
      cg.pop(X64Reg::R13);
      cg.pop(X64Reg::R12);
      cg.pop(X64Reg::Rbx);
      cg.pop(X64Reg::Rbp);
      cg.retInsn();
      break;

    // The rest need runtime support and land in later slices of PHASE 7c.
    // Locals, calls and closures need frames (7c.2b/7c.2c); collections and
    // iteration need the arena (7c.3).
    case OpCode::GETGLOBAL: {
      const int slot = globals.slotOfConstant.at(instr.b);
      cg.movImm(X64Reg::Rdi, slot);
      cg.callLabel(rt.globalGet);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::SETGLOBAL: {
      const int slot = globals.slotOfConstant.at(instr.b);
      cg.movImm(X64Reg::Rdi, slot);
      loadValue(cg, instr.a, X64Reg::Rsi, X64Reg::Rdx);
      cg.callLabel(rt.globalSet);
      break;
    }
    case OpCode::DECLLOCAL:
    case OpCode::GETLOCAL:
    case OpCode::SETLOCAL:
      unsupported("DECLLOCAL/GETLOCAL/SETLOCAL", "PHASE 7c.2");
    case OpCode::CALL:
    case OpCode::RETURN:
      unsupported("CALL/RETURN", "PHASE 7c.2");
    case OpCode::CLOSURE:
    case OpCode::GETUPVAL:
    case OpCode::SETUPVAL:
      unsupported("CLOSURE/GETUPVAL/SETUPVAL", "PHASE 7c.2");
    case OpCode::NEWTABLE:
    case OpCode::GETTABLE:
    case OpCode::SETTABLE:
      unsupported("NEWTABLE/GETTABLE/SETTABLE", "PHASE 7c.3");
    case OpCode::IN:
      unsupported("IN", "PHASE 7c.3");
    case OpCode::ITER:
      unsupported("ITER", "PHASE 7c.3");
  }
}

}  // namespace

LoweredChunk lowerToX64(const Target& target, const Bytecode& proto,
                        const CodeGen& /*codegen*/) {
  requireSupportedTarget(target);

  CodeGen cg(target);

  // The entry prelude and the runtime share the program's code buffer, so
  // every `call` resolves through the emitter's labels: no linker, no
  // relocations.
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label chunkEntry = cg.newLabel();
  CodeGen::Label mainEntry = cg.newLabel();
  std::vector<StringConstant> stringConstants;
  const GlobalSlots globals = collectGlobals(proto);

  // One label per instruction index, so a jump (an absolute index into `code`)
  // can target any instruction, forward or backward.
  std::vector<CodeGen::Label> labels;
  labels.reserve(proto.code.size());
  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    labels.push_back(cg.newLabel());
  }

  const std::uint64_t chunkOffset = cg.offset();
  cg.bind(chunkEntry);
  // Save the callee-saved registers the arena and the allocator use (rbx and
  // r12-r15; see `native/entry.h`) before claiming them, and restore them on
  // the way out, so the chunk honours the System V ABI. They are pushed
  // *above* the register file's base, which `rbp` becomes only after the
  // pushes, so the file (which grows down from `rbp`) never overlaps them.
  cg.push(X64Reg::Rbp);
  cg.push(X64Reg::Rbx);
  cg.push(X64Reg::R12);
  cg.push(X64Reg::R13);
  cg.push(X64Reg::R14);
  cg.push(X64Reg::R15);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  const std::int64_t fileBytes =
      16LL * (proto.numRegisters > 0 ? proto.numRegisters : 1);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(fileBytes));
  // Map the arena before the program runs, so the allocator has its
  // cursor/end registers seeded from the first allocation (string
  // concatenation in 7c.1c, the global table below). rsp is 16-byte aligned
  // here (six pushes plus a multiple-of-16 file size), which the calls require.
  cg.callLabel(entry.initArena);
  // Allocate and clear the global table (7c.2a). Every global reads as nil
  // until assigned, matching the hosted engine's "unset" state.
  cg.movImm(X64Reg::Rdi, globals.count);
  cg.callLabel(rt.initGlobals);

  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    cg.bind(labels[i]);
    lowerInstruction(proto.code[i], proto, cg, labels, rt, globals, stringConstants);
  }

  // `main`: run the chunk and return 0, so the process exit code is a clean
  // "success" and does not leak whatever value R0 happens to hold. `_start`
  // (emitted by `emitFreestandingRuntime`) calls this and exits with its
  // result. The frame keeps rsp 16-byte aligned at the `call` into the chunk,
  // which the System V ABI requires (a callee sees rsp % 16 == 8 at entry).
  cg.bind(mainEntry);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.callLabel(chunkEntry);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t startOffset = emitFreestandingRuntime(cg, entry, mainEntry);
  emitAotRuntime(cg, rt, entry);
  // String constants last: their bytes are addressed by `lea` from the code
  // above, and no path falls through into them.
  emitStringConstants(cg, stringConstants);

  cg.finalize();

  LoweredChunk result;
  result.code = cg.code();
  result.programEntry = chunkOffset;
  result.processEntry = startOffset;
  return result;
}
