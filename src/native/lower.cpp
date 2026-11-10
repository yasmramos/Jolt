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

// Local cells (PHASE 7c.2b) live in a second region of the same frame, right
// below the register file. Slot `b` sits at `[rbp - 16 * (numRegisters + b + 1)]`.
// The `numRegisters` argument is the *allocated* file size (at least 1), which
// is what the frame actually reserved, not the proto's declared count.
constexpr std::int64_t localSlotOffset(int numRegisters, int index) {
  return -16LL * (numRegisters + index + 1);
}

// The frame of the root chunk: six callee-saved registers are pushed (rbp is
// the file base; rbx and r12-r15 hold the arena/allocator state and the global
// table, and the System V ABI requires them preserved at the process
// boundary), then a block of `16 * (fileRegisters + fileLocals)` bytes below
// rbp holds the register file and the local slots.
//
// The chunk is entered through a `call` (from `main`), so rsp is 8 mod 16; the
// six pushes leave it 8 mod 16 and the multiple-of-16 frame keeps it there, so
// one extra 8-byte slot below the file brings it back to 0 mod 16, which every
// `call` the body makes requires. The epilogue drops it with `mov rsp, rbp`.
constexpr std::int64_t frameBytes(int fileRegisters, int fileLocals) {
  return 16LL * (fileRegisters + fileLocals);
}

void emitRootPrologue(CodeGen& cg, int fileRegisters, int fileLocals) {
  cg.push(X64Reg::Rbp);
  cg.push(X64Reg::Rbx);
  cg.push(X64Reg::R12);
  cg.push(X64Reg::R13);
  cg.push(X64Reg::R14);
  cg.push(X64Reg::R15);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(frameBytes(fileRegisters, fileLocals) + 8));
}

void emitRootEpilogue(CodeGen& cg) {
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::R15);
  cg.pop(X64Reg::R14);
  cg.pop(X64Reg::R13);
  cg.pop(X64Reg::R12);
  cg.pop(X64Reg::Rbx);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
}

// The frame of a bytecode function (a child proto). It pushes only rbp: the
// arena cursor (r13), the arena end (r14) and the global table base (r15) are
// the *program's* state, not this call's, so a function must NOT save and
// restore them -- an allocation inside a function has to advance the shared
// cursor, and restoring it on return would lose the allocation. rbp is the
// caller's register-file base, so it must be preserved. One push keeps rsp
// 16-byte aligned at the body's first `call` (entry rsp % 16 == 8).
void emitFunctionPrologue(CodeGen& cg, int fileRegisters, int fileLocals) {
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.subRegImm32(X64Reg::Rsp, static_cast<std::int32_t>(frameBytes(fileRegisters, fileLocals)));
}

void emitFunctionEpilogue(CodeGen& cg) {
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();
}

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
// their own tags (7c.1b); calls and collections need frames and the allocator
// (7c.2c/7c.3). Every other opcode raises a `LoweringError`
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
// A `GETGLOBAL` of a name the program never assigns is a lowering error naming
// the variable, the compile-time counterpart of the hosted engine's run-time
// `undefined variable`. Builtins (`print`, ...) are globals the hosted engine
// installs up front; the AOT backend has no builtins until calls land
// (7c.2c), at which point they will be seeded as pre-assigned here.
//
// The key is the *name*, not the constant index: each proto has its own
// constant pool, so index 3 in the root and index 3 in a function are usually
// different names, and a recursion (`fact` reading `fact`) must resolve to one
// slot across protos.
struct GlobalSlots {
  std::unordered_map<std::string, int> slotOfName;
  int count = 0;
};

GlobalSlots collectGlobals(const std::vector<const Bytecode*>& protos) {
  GlobalSlots globals;
  auto nameOf = [](const Bytecode& proto, int index) -> const std::string& {
    const Value& name = proto.constants.at(static_cast<std::size_t>(index));
    if (!name.isStr()) {
      throw LoweringError("lowering: global name is not a string constant");
    }
    return name.asStr();
  };
  // First-assignment order across every proto, so a function and its caller
  // (recursion) agree on the slot of a shared name.
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::SETGLOBAL) continue;
      const std::string& name = nameOf(*proto, instr.b);
      if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
      globals.slotOfName[name] = globals.count++;
    }
  }
  for (const Bytecode* proto : protos) {
    for (const Instruction& instr : proto->code) {
      if (instr.op != OpCode::GETGLOBAL) continue;
      const std::string& name = nameOf(*proto, instr.b);
      if (globals.slotOfName.find(name) != globals.slotOfName.end()) continue;
      throw LoweringError("lowering: global '" + name + "' is read but never assigned");
    }
  }
  return globals;
}

// Every proto in the tree, root first, then its descendants (pre-order). Each
// one is emitted as its own machine-code function; a `CLOSURE` needs its child
// entry label, so the labels are created for the whole tree before any body is
// emitted.
void collectProtos(const Bytecode& proto, std::vector<const Bytecode*>& out) {
  out.push_back(&proto);
  for (const std::shared_ptr<Bytecode>& child : proto.children) {
    if (child) collectProtos(*child, out);
  }
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
                      int fileRegisters,
                      const std::unordered_map<const Bytecode*, CodeGen::Label>& protoEntry,
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
      loadValue(cg, 0, X64Reg::Rax, X64Reg::Rdx);
      emitRootEpilogue(cg);
      break;

    // Globals (7c.2a), locals (7c.2b) and calls/closures (7c.2c.1) are lowered.
    // Upvalues need capturable cells (7c.2c.2); collections and iteration need
    // the arena (7c.3).
    case OpCode::GETGLOBAL: {
      const std::string& name = proto.constants.at(static_cast<std::size_t>(instr.b)).asStr();
      const int slot = globals.slotOfName.at(name);
      cg.movImm(X64Reg::Rdi, slot);
      cg.callLabel(rt.globalGet);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::SETGLOBAL: {
      const std::string& name = proto.constants.at(static_cast<std::size_t>(instr.b)).asStr();
      const int slot = globals.slotOfName.at(name);
      cg.movImm(X64Reg::Rdi, slot);
      loadValue(cg, instr.a, X64Reg::Rsi, X64Reg::Rdx);
      cg.callLabel(rt.globalSet);
      break;
    }
    // Locals (PHASE 7c.2b). `b` is the cell index in the current frame. Until
    // frames and closures land (7c.2c) a local is a plain 16-byte slot in the
    // second region of the chunk frame -- no `Cell`, no arena -- because a
    // `CLOSURE` (the only way to capture one) is not lowered yet, so a captured
    // local cannot reach the backend. `DECLLOCAL` boxes R[a] into the slot and
    // `SETLOCAL` overwrites it; the hosted engine's "undefined local" check
    // needs a null cell, which a stack slot cannot express, so a well-formed
    // chunk (the compiler always emits `DECLLOCAL` first) is assumed.
    case OpCode::DECLLOCAL:
    case OpCode::SETLOCAL: {
      loadValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      cg.movToMem(kFileBase, localSlotOffset(fileRegisters, instr.b), X64Reg::Rax);
      cg.movToMem(kFileBase, localSlotOffset(fileRegisters, instr.b) + 8, X64Reg::Rdx);
      break;
    }
    case OpCode::GETLOCAL: {
      cg.movFromMem(X64Reg::Rax, kFileBase, localSlotOffset(fileRegisters, instr.b));
      cg.movFromMem(X64Reg::Rdx, kFileBase, localSlotOffset(fileRegisters, instr.b) + 8);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::CALL: {
      // R[a] = callee, R[a+1 .. a+b] = arguments, contiguous in this frame.
      // Pass the callee (rdi:rsi), the count (rdx) and the argument window's
      // address (rcx) to `jolt_call`, which checks the callee is a function and
      // dispatches it. The result comes back in rax:rdx, straight into R[a].
      loadValue(cg, instr.a, X64Reg::Rdi, X64Reg::Rsi);
      cg.movImm(X64Reg::Rdx, instr.b);
      cg.leaMem(X64Reg::Rcx, kFileBase, slotOffset(instr.a + 1));
      cg.callLabel(rt.call);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::RETURN:
      // Publish R[a] as the function's result (rax:rdx) and leave the frame.
      // The root chunk does not use RETURN (it ends in HALT), so this is always
      // a bytecode function's frame.
      loadValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      emitFunctionEpilogue(cg);
      break;
    case OpCode::CLOSURE: {
      if (instr.b < 0 || static_cast<std::size_t>(instr.b) >= proto.children.size()) {
        throw LoweringError("lowering: CLOSURE child index " + std::to_string(instr.b) +
                            " is out of range");
      }
      const Bytecode* child = proto.children[static_cast<std::size_t>(instr.b)].get();
      // A proto with upvalues captures its defining frame's locals, which needs
      // capturable cells (7c.2c.2); only a capture-free closure is lowered here.
      if (!child->upvalues.empty()) {
        unsupported("CLOSURE with captured upvalues", "PHASE 7c.2c.2");
      }
      // jolt_closure(code, arity) -> Func in rax:rdx, stored in R[a].
      cg.leaRipLabel(X64Reg::Rdi, protoEntry.at(child));
      cg.movImm(X64Reg::Rsi, child->numParams);
      cg.callLabel(rt.closure);
      storeValue(cg, instr.a, X64Reg::Rax, X64Reg::Rdx);
      break;
    }
    case OpCode::GETUPVAL:
    case OpCode::SETUPVAL:
      unsupported("GETUPVAL/SETUPVAL", "PHASE 7c.2c.2");
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

  // Every proto in the tree, root first. Each becomes its own machine function;
  // `protoEntry` maps a proto to its entry label so a `CLOSURE` can address a
  // child's code before that child's body is emitted.
  std::vector<const Bytecode*> protos;
  collectProtos(proto, protos);
  std::unordered_map<const Bytecode*, CodeGen::Label> protoEntry;
  for (const Bytecode* p : protos) {
    protoEntry[p] = (p == &proto) ? chunkEntry : cg.newLabel();
  }

  const GlobalSlots globals = collectGlobals(protos);

  const std::uint64_t chunkOffset = cg.offset();

  // The root chunk: build the register file, map the arena and the global
  // table, then run the body. It is the only frame that saves rbx/r12-r15
  // (they carry the program's arena/global state down to every call) and the
  // only one that maps the arena.
  cg.bind(chunkEntry);
  const int rootRegisters = proto.numRegisters > 0 ? proto.numRegisters : 1;
  const int rootLocals = proto.numLocals > 0 ? proto.numLocals : 0;
  emitRootPrologue(cg, rootRegisters, rootLocals);
  cg.callLabel(entry.initArena);
  cg.movImm(X64Reg::Rdi, globals.count);
  cg.callLabel(rt.initGlobals);
  {
    std::vector<CodeGen::Label> labels;
    labels.reserve(proto.code.size());
    for (std::size_t i = 0; i < proto.code.size(); ++i) labels.push_back(cg.newLabel());
    for (std::size_t i = 0; i < proto.code.size(); ++i) {
      cg.bind(labels[i]);
      lowerInstruction(proto.code[i], proto, cg, labels, rt, globals, rootRegisters,
                       protoEntry, stringConstants);
    }
  }

  // Every child proto: a function with the lowering's own convention
  // `AotValue entry(AotValue* args, long argc, AotClosure* self)`. It binds its
  // arguments from the caller's window into R0..R(n-1) and, past the last
  // statement, returns R0 (the compiler appends a `return nil`-style fallback,
  // so a body always ends in RETURN).
  for (const Bytecode* p : protos) {
    if (p == &proto) continue;
    const Bytecode& fn = *p;
    cg.bind(protoEntry[p]);
    const int fnRegisters = fn.numRegisters > 0 ? fn.numRegisters : 1;
    const int fnLocals = fn.numLocals > 0 ? fn.numLocals : 0;
    emitFunctionPrologue(cg, fnRegisters, fnLocals);

    // A variadic function packs its extras into an array, which needs a
    // collection (7c.3); reject it before emitting anything else.
    if (fn.variadic) unsupported("variadic function", "PHASE 7c.3");

    // The compiler does not check a user function's arity -- the hosted engine
    // does at run time -- so the prologue checks `argc` (rsi) against the
    // declared parameter count and reports the mismatch. A non-variadic
    // function needs an exact match.
    {
      CodeGen::Label arityOk = cg.newLabel();
      cg.cmpImm32(X64Reg::Rsi, fn.numParams);
      cg.jcc(Jcc::Eq, arityOk);
      cg.callLabel(rt.arityError);
      cg.bind(arityOk);
    }

    // Copy the arguments into the register file. `args` points at the caller's
    // R[a+1]; the file grows *down*, so consecutive registers sit 16 bytes
    // *below* each other, and args[i] is at `args - 16*i`. Copying rather than
    // aliasing keeps the callee's writes out of the caller's registers.
    for (int i = 0; i < fn.numParams; ++i) {
      const std::int64_t off = -16LL * i;
      cg.movFromMem(X64Reg::Rax, X64Reg::Rdi, off);
      cg.movFromMem(X64Reg::Rdx, X64Reg::Rdi, off + 8);
      storeValue(cg, i, X64Reg::Rax, X64Reg::Rdx);
    }

    std::vector<CodeGen::Label> labels;
    labels.reserve(fn.code.size());
    for (std::size_t i = 0; i < fn.code.size(); ++i) labels.push_back(cg.newLabel());
    for (std::size_t i = 0; i < fn.code.size(); ++i) {
      cg.bind(labels[i]);
      lowerInstruction(fn.code[i], fn, cg, labels, rt, globals, fnRegisters,
                       protoEntry, stringConstants);
    }
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
