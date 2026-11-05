#include "native/lower.h"

#include "native/codegen.h"

#include <cstring>
#include <stdexcept>

namespace {

// Maps bytecode register index (0-11) to an x86-64 register.
// Returns X64Reg::Rax for R0, Rcx for R1, etc.
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
                          " out of range (max 11)");
  }
}

[[noreturn]] void unsupported(const char* opName) {
  throw LoweringError(std::string("lowering: opcode '") + opName +
                      "' needs runtime support (PHASE 7c)");
}

// Emits the machine code for one instruction into `codegen`.
// `labels` maps a bytecode PC to a CodeGen::Label, so jumps can be patched
// after all instructions are emitted.
void lowerInstruction(const Instruction& instr,
                      const Bytecode& proto,
                      CodeGen& cg,
                      const std::vector<CodeGen::Label>& labels) {
  switch (instr.op) {
    case OpCode::LOADK: {
      // Load constant from the pool into R[a].
      // For 7b, we only support int and float constants.
      // Strings and other types need runtime support (PHASE 7c).
      const Value& value = proto.constants.at(static_cast<std::size_t>(instr.b));
      if (value.isInt()) {
        cg.movImm(regFor(instr.a), value.asInt());
      } else if (value.isFloat()) {
        // Load float constant into xmm register. For 7b, only xmm0 is used
        // (simple, not optimal).
        std::uint64_t bits = 0;
        double d = value.asFloat();
        std::memcpy(&bits, &d, sizeof(bits));
        cg.movImm(X64Reg::Rax, static_cast<std::int64_t>(bits));
        // movq xmm0, rax (via stack, since we don't have a direct movq emitter)
        cg.push(X64Reg::Rax);
        cg.movsdToXmm(kXmmBase + 0, X64Reg::Rsp, 0);
        cg.pop(X64Reg::Rax);
      } else {
        throw LoweringError("lowering: LOADK only supports int and float constants in 7b");
      }
      break;
    }

    case OpCode::MOVE:
      cg.movReg(regFor(instr.a), regFor(instr.b));
      break;

    case OpCode::ADD:
      // For 7b: int fast path only. Generic path (strings, floats) is 7c.
      cg.addReg(regFor(instr.a), regFor(instr.c));
      break;

    case OpCode::SUB:
      cg.subReg(regFor(instr.a), regFor(instr.c));
      break;

    case OpCode::MUL:
      cg.imulReg(regFor(instr.a), regFor(instr.c));
      break;

    case OpCode::UNOP: {
      // c selects the operation: 0=negate, 1=not, 2=bit-not.
      const int a = instr.a;
      const int b = instr.b;
      if (a != b) {
        cg.movReg(regFor(a), regFor(b));
      }
      switch (static_cast<UnaryOp>(instr.c)) {
        case UnaryOp::Negate:
          cg.negReg(regFor(a));
          break;
        case UnaryOp::Not:
          // !x = (x == 0) ? 1 : 0
          cg.testReg(regFor(a));
          cg.xorReg(regFor(a), regFor(a));
          cg.movImm(regFor(a), 1);
          break;
        case UnaryOp::BitNot:
          cg.notReg(regFor(a));
          break;
        default:
          throw LoweringError("lowering: invalid UNOP selector " + std::to_string(instr.c));
      }
      break;
    }

    case OpCode::BAND:
      cg.andReg(regFor(instr.a), regFor(instr.c));
      break;
    case OpCode::BOR:
      cg.orReg(regFor(instr.a), regFor(instr.c));
      break;
    case OpCode::BXOR:
      cg.xorReg(regFor(instr.a), regFor(instr.c));
      break;
    case OpCode::SHL:
      cg.movReg(X64Reg::Rcx, regFor(instr.c));
      cg.shlCl(regFor(instr.a));
      break;
    case OpCode::SHR:
      cg.movReg(X64Reg::Rcx, regFor(instr.c));
      cg.sarCl(regFor(instr.a));
      break;

    case OpCode::EQ:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Eq, labels.at(static_cast<std::size_t>(instr.a)));
      break;
    case OpCode::NEQ:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Ne, labels.at(static_cast<std::size_t>(instr.a)));
      break;
    case OpCode::LT:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Lt, labels.at(static_cast<std::size_t>(instr.a)));
      break;
    case OpCode::LTE:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Le, labels.at(static_cast<std::size_t>(instr.a)));
      break;
    case OpCode::GT:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Gt, labels.at(static_cast<std::size_t>(instr.a)));
      break;
    case OpCode::GTE:
      cg.cmpReg(regFor(instr.b), regFor(instr.c));
      cg.jcc(Jcc::Ge, labels.at(static_cast<std::size_t>(instr.a)));
      break;

    case OpCode::JMP:
      cg.jmp(labels.at(static_cast<std::size_t>(instr.jump)));
      break;
    case OpCode::JMP_IF_FALSE:
      cg.testReg(regFor(instr.a));
      cg.jcc(Jcc::Eq, labels.at(static_cast<std::size_t>(instr.jump)));
      break;
    case OpCode::JMP_IF_TRUE:
      cg.testReg(regFor(instr.a));
      cg.jcc(Jcc::Ne, labels.at(static_cast<std::size_t>(instr.jump)));
      break;

    case OpCode::HALT:
      cg.retInsn();
      break;

    case OpCode::DIV:
    case OpCode::MOD:
    case OpCode::POW:
      unsupported("DIV/MOD/POW");
    case OpCode::GETGLOBAL:
    case OpCode::SETGLOBAL:
      unsupported("GETGLOBAL/SETGLOBAL");
    case OpCode::DECLLOCAL:
    case OpCode::GETLOCAL:
    case OpCode::SETLOCAL:
      unsupported("DECLLOCAL/GETLOCAL/SETLOCAL");
    case OpCode::CALL:
    case OpCode::RETURN:
      unsupported("CALL/RETURN");
    case OpCode::CLOSURE:
    case OpCode::GETUPVAL:
    case OpCode::SETUPVAL:
      unsupported("CLOSURE/GETUPVAL/SETUPVAL");
    case OpCode::NEWTABLE:
    case OpCode::GETTABLE:
    case OpCode::SETTABLE:
      unsupported("NEWTABLE/GETTABLE/SETTABLE");
    case OpCode::IN:
      unsupported("IN");
    case OpCode::ITER:
      unsupported("ITER");
    case OpCode::PRINT:
      unsupported("PRINT");
  }
}

}  // namespace

std::vector<std::uint8_t> lowerToX64(const Target& target,
                                      const Bytecode& proto,
                                      const CodeGen& /*codegen*/) {
  requireSupportedTarget(target);

  if (proto.numRegisters > 12) {
    throw LoweringError("lowering: proto '" + proto.name + "' uses " +
                        std::to_string(proto.numRegisters) +
                        " registers; PHASE 7b only supports up to 12");
  }

  CodeGen cg(target);

  // Create a label for each instruction index, so jumps can target any PC.
  std::vector<CodeGen::Label> labels;
  labels.reserve(proto.code.size());
  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    labels.push_back(cg.newLabel());
  }

  // Emit the code.
  for (std::size_t i = 0; i < proto.code.size(); ++i) {
    cg.bind(labels[i]);
    lowerInstruction(proto.code[i], proto, cg, labels);
  }

  cg.finalize();
  return cg.code();
}