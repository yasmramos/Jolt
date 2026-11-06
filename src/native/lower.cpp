#include "native/lower.h"

#include "native/codegen.h"

#include <cstddef>
#include <string>

namespace {

// R0-R11 map to System V general-purpose registers. r15 is deliberately left
// out of the window and used as the lowering's scratch register.
X64Reg regFor(int index) {
  switch (index) {
    case 0: return X64Reg::Rax;
    case 1: return X64Reg::Rcx;
    case 2: return X64Reg::Rdx;
    case 3: return X64Reg::Rsi;
    case 4: return X64Reg::Rdi;
    case 5: return X64Reg::R8;
    case 6: return X64Reg::R9;
    case 7: return X64Reg::R10;
    case 8: return X64Reg::R11;
    case 9: return X64Reg::R12;
    case 10: return X64Reg::R13;
    case 11: return X64Reg::R14;
    default:
      throw LoweringError("lowering: register index " + std::to_string(index) +
                          " out of range (0-11)");
  }
}

// PHASE 7b lowers only the opcodes that need no runtime, and only for integer
// values: a register holds a raw 64-bit integer with no tag, so a float, string
// or nil constant has no representation yet. The remaining opcodes arrive in
// 7c together with the boxed `Value` representation and the embedded runtime.
[[noreturn]] void unsupported(const char* opName, const char* phase) {
  throw LoweringError(std::string("lowering: opcode '") + opName +
                      "' needs runtime support (" + phase + ")");
}

// Emits `R[a] = R[b] OP R[c]` for a binary opcode. The compiler always emits
// `a == b` (the left operand's register is reused as the destination), so the
// common case is a single ALU instruction. The general case is handled too:
// when the destination aliases the right operand, that operand is stashed in
// the scratch register before the destination is overwritten.
void emitBinary(CodeGen& cg, const Instruction& instr,
                void (CodeGen::*op)(X64Reg, X64Reg)) {
  X64Reg a = regFor(instr.a);
  X64Reg b = regFor(instr.b);
  X64Reg c = regFor(instr.c);

  if (instr.a == instr.c && instr.a != instr.b) {
    cg.movReg(X64Reg::R15, c);
    cg.movReg(a, b);
    (cg.*op)(a, X64Reg::R15);
    return;
  }
  if (instr.a != instr.b) cg.movReg(a, b);
  (cg.*op)(a, c);
}

// Materializes the 0/1 result of a comparison into R[a]: `cmp`, then `setcc`
// on the low byte, then `movzx` to clear the rest of the register. Three
// instructions is the documented exception to the 1:1/1:2 budget (AGENT.md
// §3.2): x86-64 has no single instruction that writes a full-width 0/1, and
// the byte must be zero-extended so the value is a clean integer.
void emitCompare(CodeGen& cg, const Instruction& instr, Jcc cc) {
  cg.cmpReg(regFor(instr.b), regFor(instr.c));
  cg.setcc(cc, regFor(instr.a));
  cg.movzx(regFor(instr.a), regFor(instr.a));
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
                      const std::vector<CodeGen::Label>& labels) {
  switch (instr.op) {
    case OpCode::LOADK: {
      const Value& value = proto.constants.at(static_cast<std::size_t>(instr.b));
      if (value.isInt()) {
        cg.movImm(regFor(instr.a), value.asInt());
      } else if (value.isBool()) {
        cg.movImm(regFor(instr.a), value.asBool() ? 1 : 0);
      } else {
        unsupported("LOADK (float/string/nil)", "PHASE 7c");
      }
      break;
    }

    case OpCode::MOVE:
      cg.movReg(regFor(instr.a), regFor(instr.b));
      break;

    case OpCode::ADD: emitBinary(cg, instr, &CodeGen::addReg); break;
    case OpCode::SUB: emitBinary(cg, instr, &CodeGen::subReg); break;
    case OpCode::MUL: emitBinary(cg, instr, &CodeGen::imulReg); break;

    case OpCode::BAND: emitBinary(cg, instr, &CodeGen::andReg); break;
    case OpCode::BOR: emitBinary(cg, instr, &CodeGen::orReg); break;
    case OpCode::BXOR: emitBinary(cg, instr, &CodeGen::xorReg); break;

    case OpCode::UNOP: {
      X64Reg a = regFor(instr.a);
      if (instr.a != instr.b) cg.movReg(a, regFor(instr.b));
      switch (static_cast<UnaryOp>(instr.c)) {
        case UnaryOp::Negate:
          cg.negReg(a);
          break;
        case UnaryOp::BitNot:
          cg.notReg(a);
          break;
        case UnaryOp::Not:
          // !x is (x == 0), materialized as a 0/1 integer.
          cg.testReg(a);
          cg.setcc(Jcc::Eq, a);
          cg.movzx(a, a);
          break;
        default:
          throw LoweringError("lowering: invalid UNOP selector " +
                              std::to_string(instr.c));
      }
      break;
    }

    case OpCode::EQ: emitCompare(cg, instr, Jcc::Eq); break;
    case OpCode::NEQ: emitCompare(cg, instr, Jcc::Ne); break;
    case OpCode::LT: emitCompare(cg, instr, Jcc::Lt); break;
    case OpCode::LTE: emitCompare(cg, instr, Jcc::Le); break;
    case OpCode::GT: emitCompare(cg, instr, Jcc::Gt); break;
    case OpCode::GTE: emitCompare(cg, instr, Jcc::Ge); break;

    case OpCode::JMP:
      cg.jmp(jumpTarget(instr, proto, labels));
      break;
    case OpCode::JMP_IF_FALSE:
      cg.testReg(regFor(instr.a));
      cg.jcc(Jcc::Eq, jumpTarget(instr, proto, labels));
      break;
    case OpCode::JMP_IF_TRUE:
      cg.testReg(regFor(instr.a));
      cg.jcc(Jcc::Ne, jumpTarget(instr, proto, labels));
      break;

    case OpCode::HALT:
      cg.retInsn();
      break;

    // The rest need runtime support and land in PHASE 7c. DIV/MOD need the
    // division-by-zero error path; SHL/SHR need the shift count in cl (R1),
    // which would clobber a live register of the window.
    case OpCode::DIV:
    case OpCode::MOD:
    case OpCode::POW:
      unsupported("DIV/MOD/POW", "PHASE 7c");
    case OpCode::SHL:
    case OpCode::SHR:
      unsupported("SHL/SHR", "PHASE 7c");
    case OpCode::GETGLOBAL:
    case OpCode::SETGLOBAL:
      unsupported("GETGLOBAL/SETGLOBAL", "PHASE 7c");
    case OpCode::DECLLOCAL:
    case OpCode::GETLOCAL:
    case OpCode::SETLOCAL:
      unsupported("DECLLOCAL/GETLOCAL/SETLOCAL", "PHASE 7c");
    case OpCode::CALL:
    case OpCode::RETURN:
      unsupported("CALL/RETURN", "PHASE 7c");
    case OpCode::CLOSURE:
    case OpCode::GETUPVAL:
    case OpCode::SETUPVAL:
      unsupported("CLOSURE/GETUPVAL/SETUPVAL", "PHASE 7c");
    case OpCode::NEWTABLE:
    case OpCode::GETTABLE:
    case OpCode::SETTABLE:
      unsupported("NEWTABLE/GETTABLE/SETTABLE", "PHASE 7c");
    case OpCode::IN:
      unsupported("IN", "PHASE 7c");
    case OpCode::ITER:
      unsupported("ITER", "PHASE 7c");
  }
}

}  // namespace

std::vector<std::uint8_t> lowerToX64(const Target& target, const Bytecode& proto,
                                     const CodeGen& /*codegen*/) {
  requireSupportedTarget(target);

  if (proto.numRegisters > 12) {
    throw LoweringError("lowering: proto '" + proto.name + "' uses " +
                        std::to_string(proto.numRegisters) +
                        " registers; PHASE 7b only supports up to 12");
  }

  CodeGen cg(target);

  // One label per instruction index, so a jump (an absolute index into `code`)
  // can target any instruction, forward or backward.
  std::vector<CodeGen::Label> labels;
  labels.reserve(proto.code.size());
  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    labels.push_back(cg.newLabel());
  }

  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    cg.bind(labels[i]);
    lowerInstruction(proto.code[i], proto, cg, labels);
  }

  cg.finalize();
  return cg.code();
}
