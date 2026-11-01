#include "vm.h"

#include <iostream>
#include <string>
#include <utility>

#include "ops.h"

// ---------------------------------------------------------------- diagnostics

void VM::fail(const std::string& message, int line, int col) const {
  RuntimeError error;
  error.message = message;
  error.line = line > 0 ? line : currentLine;
  error.col = col > 0 ? col : currentCol;
  error.stack = stack;
  throw error;
}

const Value& VM::numberAt(const Bytecode& bytecode, int index) const {
  if (index < 0 || static_cast<size_t>(index) >= bytecode.numbers.size()) {
    fail("numeric constant index out of range: " + std::to_string(index));
  }
  return bytecode.numbers[static_cast<size_t>(index)];
}

const std::string& VM::stringAt(const Bytecode& bytecode, int index) const {
  if (index < 0 || static_cast<size_t>(index) >= bytecode.strings.size()) {
    fail("string constant index out of range: " + std::to_string(index));
  }
  return bytecode.strings[static_cast<size_t>(index)];
}

// ------------------------------------------------------------------ registers

Value& VM::getReg(int index) {
  if (index < 0 || static_cast<size_t>(index) >= registers_.size()) {
    fail("register index out of range: " + std::to_string(index));
  }
  return registers_[static_cast<size_t>(index)];
}

void VM::setReg(int index, Value value) { getReg(index) = std::move(value); }

CellPtr& VM::localAt(int index) {
  if (index < 0 || static_cast<size_t>(index) >= locals_.size()) {
    fail("local index out of range: " + std::to_string(index));
  }
  return locals_[static_cast<size_t>(index)];
}

// -------------------------------------------------------------------- runtime

void VM::defineGlobal(const std::string& name, Value value, bool /*isImmutable*/) {
  // Immutability of globals is enforced by the compiler, which rejects the
  // assignment before the bytecode exists; the VM has no binding metadata.
  globals_[name] = std::move(value);
}

Value VM::call(const Value& callee, std::vector<Value> args, int line, int col) {
  if (!callee.isFunc()) {
    fail(std::string("cannot call a value of type '") + callee.typeName() + "'", line, col);
  }

  // Keep the callee alive for the whole call: a temporary bound method owns
  // the only strong reference to its `Function` (see Interpreter::call).
  FuncPtr function = callee.funcPtr();
  Function& fn = *function;

  if (fn.isNative()) {
    int savedLine = currentLine;
    int savedCol = currentCol;
    currentLine = line;
    currentCol = col;
    try {
      Value result = fn.native(*this, args);
      currentLine = savedLine;
      currentCol = savedCol;
      return result;
    } catch (RuntimeError& error) {
      if (error.line == 0) {
        error.line = line;
        error.col = col;
      }
      currentLine = savedLine;
      currentCol = savedCol;
      throw;
    }
  }

  // PHASE 6a adds CALL/RETURN and per-function protos. Until then the VM
  // cannot enter a Jolt function, and it says so instead of executing AST.
  fail("cannot call '" + fn.name + "' from the bytecode VM yet", line, col);
}

Value VM::indexGet(const Value& object, const Value& index, int line, int col) {
  return ops::indexGet(*this, object, index, line, col);
}

void VM::indexSet(const Value& object, const Value& index, Value value, int line, int col) {
  ops::indexSet(*this, object, index, std::move(value), line, col);
}

Value VM::memberGet(const Value& object, const std::string& name, int line, int col) {
  return ops::memberGet(*this, object, name, line, col);
}

void VM::memberSet(const Value& object, const std::string& name, Value value, int line, int col) {
  ops::memberSet(*this, object, name, std::move(value), line, col);
}

Value VM::binary(Tok op, const Value& left, const Value& right, int line, int col) {
  return ops::binary(*this, op, left, right, line, col);
}

bool VM::lessThan(const Value& left, const Value& right, int line, int col) {
  return ops::lessThan(*this, left, right, line, col);
}

// ------------------------------------------------------------------ execution

void VM::run(const Bytecode& bytecode) {
  const int registerCount = bytecode.numRegisters > 0 ? bytecode.numRegisters : 0;
  const int localCount = bytecode.numLocals > 0 ? bytecode.numLocals : 0;
  registers_.assign(static_cast<size_t>(registerCount), Value{});
  locals_.assign(static_cast<size_t>(localCount), CellPtr{});
  depth = 0;
  stack.clear();

  const size_t size = bytecode.code.size();
  size_t pc = 0;

  while (pc < size) {
    const Instruction& instruction = bytecode.code[pc++];
    if (instruction.line > 0) {
      currentLine = instruction.line;
      currentCol = instruction.col;
    }

    // Position of the current instruction, for the shared value semantics.
    const int line = instruction.line;
    const int col = instruction.col;

    switch (instruction.op) {
      case OpCode::LOADK:
        setReg(instruction.a, numberAt(bytecode, instruction.b));
        break;

      case OpCode::LOADKS:
        setReg(instruction.a, Value(stringAt(bytecode, instruction.b)));
        break;

      case OpCode::LOADNIL:
        setReg(instruction.a, Value{});
        break;

      case OpCode::LOADBOOL:
        setReg(instruction.a, Value(instruction.b != 0));
        break;

      case OpCode::MOVE:
        setReg(instruction.a, getReg(instruction.b));
        break;

      case OpCode::ADD:
        setReg(instruction.a, binary(Tok::Plus, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SUB:
        setReg(instruction.a, binary(Tok::Minus, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::MUL:
        setReg(instruction.a, binary(Tok::Star, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::DIV:
        setReg(instruction.a, binary(Tok::Slash, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::MOD:
        setReg(instruction.a, binary(Tok::Percent, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::POW:
        setReg(instruction.a,
               binary(Tok::StarStar, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::NEG:
        setReg(instruction.a, ops::unary(*this, Tok::Minus, getReg(instruction.b), line, col));
        break;

      case OpCode::NOT:
        setReg(instruction.a, ops::unary(*this, Tok::Bang, getReg(instruction.b), line, col));
        break;

      case OpCode::BAND:
        setReg(instruction.a, binary(Tok::Amp, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::BOR:
        setReg(instruction.a, binary(Tok::Pipe, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::BXOR:
        setReg(instruction.a, binary(Tok::Caret, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SHL:
        setReg(instruction.a, binary(Tok::Shl, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SHR:
        setReg(instruction.a, binary(Tok::Shr, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::BNOT:
        setReg(instruction.a, ops::unary(*this, Tok::Tilde, getReg(instruction.b), line, col));
        break;

      case OpCode::EQ:
        setReg(instruction.a, Value(getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::NEQ:
        setReg(instruction.a, Value(!getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::LT:
        setReg(instruction.a,
               Value(lessThan(getReg(instruction.b), getReg(instruction.c), line, col)));
        break;

      case OpCode::LTE:
        setReg(instruction.a,
               Value(!lessThan(getReg(instruction.c), getReg(instruction.b), line, col)));
        break;

      case OpCode::GT:
        setReg(instruction.a,
               Value(lessThan(getReg(instruction.c), getReg(instruction.b), line, col)));
        break;

      case OpCode::GTE:
        setReg(instruction.a,
               Value(!lessThan(getReg(instruction.b), getReg(instruction.c), line, col)));
        break;

      case OpCode::JMP:
        if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
          fail("invalid jump target: " + std::to_string(instruction.jump));
        }
        pc = static_cast<size_t>(instruction.jump);
        break;

      case OpCode::JMP_IF_FALSE:
        if (!getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            fail("invalid jump target: " + std::to_string(instruction.jump));
          }
          pc = static_cast<size_t>(instruction.jump);
        }
        break;

      case OpCode::JMP_IF_TRUE:
        if (getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            fail("invalid jump target: " + std::to_string(instruction.jump));
          }
          pc = static_cast<size_t>(instruction.jump);
        }
        break;

      case OpCode::SETGLOBAL:
        globals_[stringAt(bytecode, instruction.b)] = getReg(instruction.a);
        break;

      case OpCode::GETGLOBAL: {
        const std::string& name = stringAt(bytecode, instruction.b);
        auto it = globals_.find(name);
        if (it == globals_.end()) fail("undefined variable '" + name + "'", line, col);
        setReg(instruction.a, it->second);
        break;
      }

      case OpCode::DECLLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        cell = makeRef<Cell>();
        cell->value = getReg(instruction.a);
        break;
      }

      case OpCode::SETLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        if (!cell) fail("undefined local variable", line, col);
        cell->value = getReg(instruction.a);
        break;
      }

      case OpCode::GETLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        if (!cell) fail("undefined local variable", line, col);
        setReg(instruction.a, cell->value);
        break;
      }

      case OpCode::PRINT:
        std::cout << getReg(instruction.a).toString() << '\n';
        std::cout.flush();
        break;

      case OpCode::HALT:
        return;
    }
  }
}

const Value* VM::findGlobal(const std::string& name) const {
  auto it = globals_.find(name);
  return it == globals_.end() ? nullptr : &it->second;
}
