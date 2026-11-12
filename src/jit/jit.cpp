#include "jit/jit.h"

#include <algorithm>
#include <cstring>

#include "native/codegen.h"
#include "native/target.h"

#if defined(_WIN32)
// Executable memory is not mapped on Windows in this slice; the JIT reports
// "no backend" and the VM keeps interpreting.
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

// The JIT emits x86-64 for the System V ABI only, like the AOT backend. On any
// other host there is nothing to run, so `jitCompile` bails out immediately
// instead of producing bytes the process cannot execute.
constexpr bool jitHostSupported() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
  return true;
#else
  return false;
#endif
}

// -------------------------------------------------------------- value analysis
//
// The JIT only compiles a proto whose body provably manipulates `nil`, `bool`
// and `int` alone. Those three value kinds carry no reference count and own no
// memory, so the generated code never has to touch the heap or the cycle
// collector: it reads and writes a self-contained register file and returns a
// value. Everything else (strings, floats, containers, functions, globals) is
// left to the interpreter.
//
// `Ty` is the per-register lattice element. `Undef` means "not yet known" and
// is only meaningful while the fixpoint runs; once it settles, a read of an
// `Undef` or `Top` register means the proto is outside the subset.
enum class Ty : std::uint8_t { Undef, Nil, Bool, Int, Top };

Ty mergeTy(Ty a, Ty b) {
  if (a == Ty::Undef) return b;
  if (b == Ty::Undef) return a;
  if (a == b) return a;
  return Ty::Top;
}

bool isScalar(Ty t) { return t == Ty::Nil || t == Ty::Bool || t == Ty::Int; }

// The lattice element a constant maps to, or `Top` when the constant is a kind
// the JIT does not handle.
Ty constTy(const Value& value) {
  switch (value.type()) {
    case VType::Nil:
      return Ty::Nil;
    case VType::Bool:
      return Ty::Bool;
    case VType::Int:
      return Ty::Int;
    default:
      return Ty::Top;
  }
}

std::uint8_t tagOf(Ty t) {
  switch (t) {
    case Ty::Nil:
      return static_cast<std::uint8_t>(VType::Nil);
    case Ty::Bool:
      return static_cast<std::uint8_t>(VType::Bool);
    default:
      return static_cast<std::uint8_t>(VType::Int);
  }
}

struct Analysis {
  bool supported = false;
  int numRegs = 0;
  int numLocals = 0;
  std::vector<bool> reachable;
  // Type of every register and local on entry to each instruction. Index
  // `numRegs + l` is local `l`.
  std::vector<std::vector<Ty>> stateIn;
};

// Forward dataflow to a fixpoint, then a verification pass. The fixpoint is a
// sound over-approximation: a register the fixpoint cannot pin down to a
// concrete scalar fails the verification pass, so an unsupported proto is
// rejected rather than miscompiled.
Analysis analyze(const Bytecode& proto) {
  Analysis result;
  result.numRegs = std::max(proto.numRegisters, proto.numParams);
  result.numLocals = std::max(proto.numLocals, 0);
  const int total = result.numRegs + result.numLocals;
  const int n = static_cast<int>(proto.code.size());

  // A proto with children, captures or externs could instantiate a closure or
  // call out at run time; none of that is expressible in the subset.
  if (!proto.children.empty() || !proto.upvalues.empty() || !proto.externs.empty() ||
      proto.variadic) {
    return result;
  }

  result.reachable.assign(n, false);
  result.stateIn.assign(n, std::vector<Ty>(total, Ty::Undef));
  if (n == 0) {
    result.supported = true;
    return result;
  }

  // Entry state: the VM only enters the JIT with all-integer arguments, so the
  // parameters start as `Int`. Locals start undeclared.
  result.reachable[0] = true;
  for (int i = 0; i < proto.numParams && i < result.numRegs; ++i) {
    result.stateIn[0][i] = Ty::Int;
  }

  auto indexOk = [&](int r) { return r >= 0 && r < total; };

  // Transfer function: writes the out-state of one instruction in place. A
  // violated requirement by a *concrete* input is not fatal here; the
  // verification pass below catches it once the fixpoint has settled.
  auto transfer = [&](const Instruction& in, std::vector<Ty>& st) {
    auto reg = [&](int r) -> Ty { return indexOk(r) ? st[r] : Ty::Top; };
    auto local = [&](int l) -> Ty { return indexOk(result.numRegs + l) ? st[result.numRegs + l] : Ty::Top; };
    auto setLocal = [&](int l, Ty t) { if (indexOk(result.numRegs + l)) st[result.numRegs + l] = t; };

    switch (in.op) {
      case OpCode::LOADK:
        if (indexOk(in.a) && in.b >= 0 && static_cast<std::size_t>(in.b) < proto.constants.size()) {
          st[in.a] = constTy(proto.constants[static_cast<std::size_t>(in.b)]);
        }
        break;
      case OpCode::MOVE:
        if (indexOk(in.a)) st[in.a] = reg(in.b);
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR: {
        const Ty x = reg(in.b), y = reg(in.c);
        if (indexOk(in.a)) {
          if (x == Ty::Int && y == Ty::Int) st[in.a] = Ty::Int;
          else if (x == Ty::Undef || y == Ty::Undef) st[in.a] = Ty::Undef;
          else st[in.a] = Ty::Top;
        }
        break;
      }
      case OpCode::UNOP: {
        const Ty x = reg(in.b);
        if (!indexOk(in.a)) break;
        if (static_cast<UnaryOp>(in.c) == UnaryOp::Not) {
          st[in.a] = Ty::Bool;  // `!x` is a bool whatever `x` is
        } else if (x == Ty::Int) {
          st[in.a] = Ty::Int;
        } else {
          st[in.a] = x == Ty::Undef ? Ty::Undef : Ty::Top;
        }
        break;
      }
      case OpCode::EQ:
      case OpCode::NEQ:
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE:
        if (indexOk(in.a)) st[in.a] = Ty::Bool;
        break;
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL:
        setLocal(in.b, reg(in.a));
        break;
      case OpCode::GETLOCAL:
        if (indexOk(in.a)) st[in.a] = local(in.b);
        break;
      case OpCode::JMP:
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
      case OpCode::RETURN:
      case OpCode::HALT:
        break;
      default:
        break;
    }
  };

  auto successors = [&](int i, std::vector<int>& out) {
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    switch (in.op) {
      case OpCode::JMP:
        out.push_back(in.jump);
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
        out.push_back(in.jump);
        out.push_back(i + 1);
        break;
      case OpCode::RETURN:
      case OpCode::HALT:
        break;
      default:
        out.push_back(i + 1);
        break;
    }
  };

  bool changed = true;
  int guard = 0;
  while (changed && guard++ < 100000) {
    changed = false;
    for (int i = 0; i < n; ++i) {
      if (!result.reachable[static_cast<std::size_t>(i)]) continue;
      std::vector<Ty> out = result.stateIn[static_cast<std::size_t>(i)];
      transfer(proto.code[static_cast<std::size_t>(i)], out);
      std::vector<int> succ;
      successors(i, succ);
      for (int s : succ) {
        if (s < 0 || s > n) continue;  // validated in the verification pass
        if (s == n) continue;          // falling off the end returns nil
        if (!result.reachable[static_cast<std::size_t>(s)]) {
          result.reachable[static_cast<std::size_t>(s)] = true;
          changed = true;
        }
        std::vector<Ty>& dst = result.stateIn[static_cast<std::size_t>(s)];
        for (int k = 0; k < total; ++k) {
          const Ty merged = mergeTy(dst[static_cast<std::size_t>(k)], out[static_cast<std::size_t>(k)]);
          if (merged != dst[static_cast<std::size_t>(k)]) {
            dst[static_cast<std::size_t>(k)] = merged;
            changed = true;
          }
        }
      }
    }
  }

  // Verification pass against the settled state. Every register read must be a
  // concrete scalar and every opcode must be one the JIT can lower.
  for (int i = 0; i < n; ++i) {
    if (!result.reachable[static_cast<std::size_t>(i)]) continue;
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const std::vector<Ty>& st = result.stateIn[static_cast<std::size_t>(i)];
    auto reg = [&](int r) -> Ty { return indexOk(r) ? st[static_cast<std::size_t>(r)] : Ty::Top; };
    auto local = [&](int l) -> Ty {
      return indexOk(result.numRegs + l) ? st[static_cast<std::size_t>(result.numRegs + l)] : Ty::Top;
    };
    auto scalarReg = [&](int r) { return isScalar(reg(r)); };
    auto intReg = [&](int r) { return reg(r) == Ty::Int; };

    switch (in.op) {
      case OpCode::LOADK:
        if (!indexOk(in.a) || in.b < 0 || static_cast<std::size_t>(in.b) >= proto.constants.size() ||
            constTy(proto.constants[static_cast<std::size_t>(in.b)]) == Ty::Top) {
          return result;
        }
        break;
      case OpCode::MOVE:
        if (!indexOk(in.a) || !scalarReg(in.b)) return result;
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR:
        if (!indexOk(in.a) || !intReg(in.b) || !intReg(in.c)) return result;
        break;
      case OpCode::UNOP:
        if (!indexOk(in.a) || !indexOk(in.b)) return result;
        if (static_cast<UnaryOp>(in.c) == UnaryOp::Not) {
          if (!scalarReg(in.b)) return result;
        } else if (static_cast<UnaryOp>(in.c) == UnaryOp::Negate ||
                   static_cast<UnaryOp>(in.c) == UnaryOp::BitNot) {
          if (!intReg(in.b)) return result;
        } else {
          return result;  // unknown selector
        }
        break;
      case OpCode::EQ:
      case OpCode::NEQ:
        if (!indexOk(in.a)) return result;
        if (!((intReg(in.b) && intReg(in.c)) || (reg(in.b) == Ty::Bool && reg(in.c) == Ty::Bool))) {
          return result;
        }
        break;
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE:
        if (!indexOk(in.a) || !intReg(in.b) || !intReg(in.c)) return result;
        break;
      case OpCode::JMP:
        if (in.jump < 0 || in.jump > n) return result;
        break;
      case OpCode::JMP_IF_FALSE:
      case OpCode::JMP_IF_TRUE:
        if (!indexOk(in.a) || !scalarReg(in.a) || in.jump < 0 || in.jump > n) return result;
        break;
      case OpCode::DECLLOCAL:
        // The local is being declared here, so only the source must be scalar;
        // the slot itself is Undef until this instruction defines it.
        if (!scalarReg(in.a) || !indexOk(result.numRegs + in.b)) return result;
        break;
      case OpCode::SETLOCAL:
        if (!scalarReg(in.a) || !indexOk(result.numRegs + in.b) || !isScalar(local(in.b))) return result;
        break;
      case OpCode::GETLOCAL:
        if (!indexOk(in.a) || !indexOk(result.numRegs + in.b) || !isScalar(local(in.b))) return result;
        break;
      case OpCode::RETURN:
        if (!indexOk(in.a) || !scalarReg(in.a)) return result;
        break;
      case OpCode::HALT:
        break;
      default:
        return result;  // globals, calls, closures, collections, iteration
    }
  }

  result.supported = true;
  return result;
}

// ------------------------------------------------------------------ lowering

// Emits the native body of a supported proto. The convention matches
// `JitFunction::Entry`: args in rdi, argc in rsi, out pointer in rdx.
std::vector<std::uint8_t> emitProto(const Bytecode& proto, const Analysis& analysis) {
  CodeGen cg(Target::linuxX64());

  const int numRegs = analysis.numRegs;
  const int numLocals = analysis.numLocals;
  const int n = static_cast<int>(proto.code.size());
  const std::int64_t frameSize = 16 * (static_cast<std::int64_t>(numRegs + numLocals) + 1);

  auto regDisp = [](int r) { return -16 * (static_cast<std::int64_t>(r) + 1); };
  auto localDisp = [&](int l) { return -16 * (static_cast<std::int64_t>(numRegs + l) + 1); };
  auto outDisp = [&]() { return -16 * (static_cast<std::int64_t>(numRegs + numLocals) + 1); };

  const std::uint8_t kTagBool = tagOf(Ty::Bool);
  const std::uint8_t kTagInt = tagOf(Ty::Int);

  // One label per instruction plus one for the implicit end.
  std::vector<CodeGen::Label> labels(static_cast<std::size_t>(n) + 1);
  for (auto& label : labels) label = cg.newLabel();
  const CodeGen::Label endLabel = labels[static_cast<std::size_t>(n)];

  // Writes a full tagged slot (payload qword + tag byte).
  auto storeSlot = [&](X64Reg base, std::int64_t disp, std::uint8_t tag, std::int64_t payload) {
    cg.movImm(X64Reg::R11, payload);
    cg.movToMem(base, disp + 8, X64Reg::R11);
    cg.movImmByteToMem(base, disp, tag);
  };
  // Copies a tagged slot (payload qword + tag byte) without touching refcounts:
  // only nil/bool/int reach here, and none of them owns a reference.
  auto copySlot = [&](X64Reg dstBase, std::int64_t dstDisp, X64Reg srcBase, std::int64_t srcDisp) {
    cg.movFromMem(X64Reg::R10, srcBase, srcDisp + 8);
    cg.movToMem(dstBase, dstDisp + 8, X64Reg::R10);
    cg.movzxFromMem(X64Reg::R11, srcBase, srcDisp);
    cg.movByteToMem(dstBase, dstDisp, X64Reg::R11);
  };
  auto storeBool = [&](X64Reg base, std::int64_t disp, X64Reg src) {
    cg.movzx(X64Reg::R10, src);
    cg.movToMem(base, disp + 8, X64Reg::R10);
    cg.movImmByteToMem(base, disp, kTagBool);
  };
  auto epilogue = [&]() {
    cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
    cg.pop(X64Reg::Rbp);
    cg.retInsn();
  };
  auto returnValue = [&](int reg) {
    cg.movFromMem(X64Reg::R11, X64Reg::Rbp, outDisp());          // out pointer
    cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(reg) + 8);   // payload
    cg.movToMem(X64Reg::R11, 8, X64Reg::R10);
    cg.movzxFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(reg));     // tag
    cg.movByteToMem(X64Reg::R11, 0, X64Reg::R10);
    epilogue();
  };
  auto returnNil = [&]() {
    cg.movFromMem(X64Reg::R11, X64Reg::Rbp, outDisp());
    cg.movImm(X64Reg::R10, 0);
    cg.movToMem(X64Reg::R11, 8, X64Reg::R10);
    cg.movByteToMem(X64Reg::R11, 0, X64Reg::R10);
    epilogue();
  };

  // Prologue: frame, save the out pointer, copy the incoming arguments.
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(frameSize));
  cg.movToMem(X64Reg::Rbp, outDisp(), X64Reg::Rdx);
  for (int i = 0; i < proto.numParams && i < numRegs; ++i) {
    cg.movFromMem(X64Reg::R10, X64Reg::Rdi, 16 * i + 8);
    cg.movToMem(X64Reg::Rbp, regDisp(i) + 8, X64Reg::R10);
    cg.movzxFromMem(X64Reg::R10, X64Reg::Rdi, 16 * i);
    cg.movByteToMem(X64Reg::Rbp, regDisp(i), X64Reg::R10);
  }

  for (int i = 0; i < n; ++i) {
    if (!analysis.reachable[static_cast<std::size_t>(i)]) continue;
    cg.bind(labels[static_cast<std::size_t>(i)]);
    const Instruction& in = proto.code[static_cast<std::size_t>(i)];
    const std::vector<Ty>& st = analysis.stateIn[static_cast<std::size_t>(i)];
    auto tyOf = [&](int r) { return st[static_cast<std::size_t>(r)]; };

    switch (in.op) {
      case OpCode::LOADK: {
        const Value& k = proto.constants[static_cast<std::size_t>(in.b)];
        const Ty t = constTy(k);
        const std::int64_t payload = (t == Ty::Int) ? k.asInt() : (t == Ty::Bool ? (k.asBool() ? 1 : 0) : 0);
        storeSlot(X64Reg::Rbp, regDisp(in.a), tagOf(t), payload);
        break;
      }
      case OpCode::MOVE:
        copySlot(X64Reg::Rbp, regDisp(in.a), X64Reg::Rbp, regDisp(in.b));
        break;
      case OpCode::ADD:
      case OpCode::SUB:
      case OpCode::MUL:
      case OpCode::BAND:
      case OpCode::BOR:
      case OpCode::BXOR: {
        cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.b) + 8);
        cg.movFromMem(X64Reg::R11, X64Reg::Rbp, regDisp(in.c) + 8);
        switch (in.op) {
          case OpCode::ADD: cg.addReg(X64Reg::R10, X64Reg::R11); break;
          case OpCode::SUB: cg.subReg(X64Reg::R10, X64Reg::R11); break;
          case OpCode::MUL: cg.imulReg(X64Reg::R10, X64Reg::R11); break;
          case OpCode::BAND: cg.andReg(X64Reg::R10, X64Reg::R11); break;
          case OpCode::BOR: cg.orReg(X64Reg::R10, X64Reg::R11); break;
          default: cg.xorReg(X64Reg::R10, X64Reg::R11); break;
        }
        cg.movToMem(X64Reg::Rbp, regDisp(in.a) + 8, X64Reg::R10);
        cg.movImmByteToMem(X64Reg::Rbp, regDisp(in.a), kTagInt);
        break;
      }
      case OpCode::UNOP: {
        const UnaryOp op = static_cast<UnaryOp>(in.c);
        if (op == UnaryOp::Not) {
          // Oskel truthiness: only `nil` and `false` are falsy, so `!int` is
          // always false and `!nil` always true; only a bool operand needs a
          // real test.
          const Ty operand = tyOf(in.b);
          if (operand == Ty::Nil) {
            storeSlot(X64Reg::Rbp, regDisp(in.a), kTagBool, 1);
          } else if (operand == Ty::Int) {
            storeSlot(X64Reg::Rbp, regDisp(in.a), kTagBool, 0);
          } else {
            cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.b) + 8);
            cg.testReg(X64Reg::R10);
            cg.setcc(Jcc::Eq, X64Reg::R10);
            storeBool(X64Reg::Rbp, regDisp(in.a), X64Reg::R10);
          }
          break;
        }
        cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.b) + 8);
        if (op == UnaryOp::Negate) cg.negReg(X64Reg::R10);
        else cg.notReg(X64Reg::R10);
        cg.movToMem(X64Reg::Rbp, regDisp(in.a) + 8, X64Reg::R10);
        cg.movImmByteToMem(X64Reg::Rbp, regDisp(in.a), kTagInt);
        break;
      }
      case OpCode::EQ:
      case OpCode::NEQ:
      case OpCode::LT:
      case OpCode::LTE:
      case OpCode::GT:
      case OpCode::GTE: {
        cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.b) + 8);
        cg.movFromMem(X64Reg::R11, X64Reg::Rbp, regDisp(in.c) + 8);
        cg.cmpReg(X64Reg::R10, X64Reg::R11);
        Jcc cc = Jcc::Eq;
        switch (in.op) {
          case OpCode::EQ: cc = Jcc::Eq; break;
          case OpCode::NEQ: cc = Jcc::Ne; break;
          case OpCode::LT: cc = Jcc::Lt; break;
          case OpCode::LTE: cc = Jcc::Le; break;
          case OpCode::GT: cc = Jcc::Gt; break;
          default: cc = Jcc::Ge; break;
        }
        cg.setcc(cc, X64Reg::R10);
        storeBool(X64Reg::Rbp, regDisp(in.a), X64Reg::R10);
        break;
      }
      case OpCode::JMP:
        cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        break;
      case OpCode::JMP_IF_FALSE:
        // `nil` is always falsy, a non-nil int is always truthy, and only a
        // bool needs the payload test.
        if (tyOf(in.a) == Ty::Nil) {
          cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        } else if (tyOf(in.a) == Ty::Bool) {
          cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.a) + 8);
          cg.testReg(X64Reg::R10);
          cg.jcc(Jcc::Eq, labels[static_cast<std::size_t>(in.jump)]);
        }
        break;
      case OpCode::JMP_IF_TRUE:
        if (tyOf(in.a) == Ty::Int) {
          cg.jmp(labels[static_cast<std::size_t>(in.jump)]);
        } else if (tyOf(in.a) == Ty::Bool) {
          cg.movFromMem(X64Reg::R10, X64Reg::Rbp, regDisp(in.a) + 8);
          cg.testReg(X64Reg::R10);
          cg.jcc(Jcc::Ne, labels[static_cast<std::size_t>(in.jump)]);
        }
        break;
      case OpCode::DECLLOCAL:
      case OpCode::SETLOCAL:
        copySlot(X64Reg::Rbp, localDisp(in.b), X64Reg::Rbp, regDisp(in.a));
        break;
      case OpCode::GETLOCAL:
        copySlot(X64Reg::Rbp, regDisp(in.a), X64Reg::Rbp, localDisp(in.b));
        break;
      case OpCode::RETURN:
        returnValue(in.a);
        break;
      case OpCode::HALT:
        returnNil();
        break;
      default:
        break;  // unreachable: the analysis rejected every other opcode
    }
  }

  // Falling off the end is an implicit `return nil`, exactly like the VM.
  cg.bind(endLabel);
  returnNil();
  cg.finalize();
  return cg.code();
}

}  // namespace

// --------------------------------------------------------------- executable memory

JitCode::~JitCode() { release(); }

JitCode::JitCode(JitCode&& other) noexcept
    : mapping_(other.mapping_), mappingSize_(other.mappingSize_), entry_(other.entry_) {
  other.mapping_ = nullptr;
  other.mappingSize_ = 0;
  other.entry_ = nullptr;
}

JitCode& JitCode::operator=(JitCode&& other) noexcept {
  if (this != &other) {
    release();
    mapping_ = other.mapping_;
    mappingSize_ = other.mappingSize_;
    entry_ = other.entry_;
    other.mapping_ = nullptr;
    other.mappingSize_ = 0;
    other.entry_ = nullptr;
  }
  return *this;
}

void JitCode::release() {
#if !defined(_WIN32)
  if (mapping_ != nullptr) {
    munmap(mapping_, mappingSize_);
  }
#endif
  mapping_ = nullptr;
  mappingSize_ = 0;
  entry_ = nullptr;
}

bool JitCode::assign(const std::vector<std::uint8_t>& bytes) {
  release();
  if (!jitHostSupported() || bytes.empty()) return false;
#if defined(_WIN32)
  return false;
#else
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) return false;
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < bytes.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return false;
  std::memcpy(mem, bytes.data(), bytes.size());
  // The mapping is never writable and executable at the same time.
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    munmap(mem, span);
    return false;
  }
  mapping_ = mem;
  mappingSize_ = span;
  entry_ = mem;
  return true;
#endif
}

// --------------------------------------------------------------- public API

Value JitFunction::call(const std::vector<Value>& args) const {
  Value out;
  auto entry = reinterpret_cast<Entry>(code_.entry());
  entry(args.data(), static_cast<std::int64_t>(args.size()), &out);
  return out;
}

std::unique_ptr<JitFunction> jitCompile(const Bytecode& proto) {
  if (!jitHostSupported()) return nullptr;
  const Analysis analysis = analyze(proto);
  if (!analysis.supported) return nullptr;
  std::vector<std::uint8_t> bytes = emitProto(proto, analysis);
  if (bytes.empty()) return nullptr;
  JitCode code;
  if (!code.assign(bytes)) return nullptr;
  return std::unique_ptr<JitFunction>(new JitFunction(std::move(code), proto.numParams));
}

JitFunction* Jit::get(const std::shared_ptr<const Bytecode>& proto) {
  if (!proto) return nullptr;
  const Bytecode* key = proto.get();
  auto it = functions_.find(key);
  if (it != functions_.end()) return it->second.function.get();

  std::unique_ptr<JitFunction> compiled = jitCompile(*proto);
  Entry entry;
  entry.proto = proto;
  entry.function = std::move(compiled);
  JitFunction* result = entry.function.get();
  // Cache even a failed compilation (nullptr function) so a proto outside the
  // subset is analyzed once, not on every call.
  functions_.emplace(key, std::move(entry));
  return result;
}
