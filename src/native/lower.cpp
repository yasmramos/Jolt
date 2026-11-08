#include "native/lower.h"

#include "native/codegen.h"
#include "native/entry.h"
#include "native/runtime.h"
#include "native/value.h"

#include <cstddef>
#include <string>
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

void lowerInstruction(const Instruction& instr, const Bytecode& proto, CodeGen& cg,
                      const std::vector<CodeGen::Label>& labels,
                      const AotRuntimeHandles& rt) {
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
      } else {
        unsupported("LOADK (string)", "PHASE 7c.1c");
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
      loadValue(cg, 0, X64Reg::Rax, X64Reg::Rdx);
      cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
      cg.pop(X64Reg::Rbp);
      cg.retInsn();
      break;

    // The rest need runtime support and land in later slices of PHASE 7c.
    // Globals, locals, calls and closures need frames (7c.2); collections and
    // iteration need the arena (7c.3).
    case OpCode::GETGLOBAL:
    case OpCode::SETGLOBAL:
      unsupported("GETGLOBAL/SETGLOBAL", "PHASE 7c.2");
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

  // One label per instruction index, so a jump (an absolute index into `code`)
  // can target any instruction, forward or backward.
  std::vector<CodeGen::Label> labels;
  labels.reserve(proto.code.size());
  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    labels.push_back(cg.newLabel());
  }

  const std::uint64_t chunkOffset = cg.offset();
  cg.bind(chunkEntry);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  const std::int64_t fileBytes =
      16LL * (proto.numRegisters > 0 ? proto.numRegisters : 1);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(fileBytes));

  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    cg.bind(labels[i]);
    lowerInstruction(proto.code[i], proto, cg, labels, rt);
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

  cg.finalize();

  LoweredChunk result;
  result.code = cg.code();
  result.programEntry = chunkOffset;
  result.processEntry = startOffset;
  return result;
}
