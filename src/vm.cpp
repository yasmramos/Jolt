#include "vm.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

#include "errors.h"

namespace {

[[noreturn]] void failRuntime(const std::string& message) {
  RuntimeError error;
  error.message = message;
  throw error;
}

std::string typePair(const Value& left, const Value& right) {
  return std::string("'") + left.typeName() + "' and '" + right.typeName() + "'";
}

const Value& numberAt(const Bytecode& bytecode, int index) {
  if (index < 0 || static_cast<size_t>(index) >= bytecode.numbers.size()) {
    failRuntime("numeric constant index out of range: " + std::to_string(index));
  }
  return bytecode.numbers[static_cast<size_t>(index)];
}

const std::string& stringAt(const Bytecode& bytecode, int index) {
  if (index < 0 || static_cast<size_t>(index) >= bytecode.strings.size()) {
    failRuntime("string constant index out of range: " + std::to_string(index));
  }
  return bytecode.strings[static_cast<size_t>(index)];
}

// Integer power with modular overflow, same as the interpreter's `intPow`.
int64_t intPow(int64_t base, int64_t exponent) {
  uint64_t result = 1;
  uint64_t factor = static_cast<uint64_t>(base);
  uint64_t exp = static_cast<uint64_t>(exponent);
  while (exp > 0) {
    if (exp & 1U) result *= factor;
    factor *= factor;
    exp >>= 1U;
  }
  return static_cast<int64_t>(result);
}

// The following functions replicate the interpreter's arithmetic semantics.
// The integer path avoids converting to double and keeps the defined 64-bit
// overflow behavior; the float path is only used when a float is involved.

Value addValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) {
    return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) +
                                      static_cast<uint64_t>(right.asInt())));
  }
  if (left.isNumber() && right.isNumber()) {
    return Value(left.number() + right.number());
  }
  if (left.isStr() && right.isStr()) {
    return Value(left.asStr() + right.asStr());
  }
  failRuntime("unsupported operand types for '+': " + typePair(left, right));
}

Value subValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) {
    return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) -
                                      static_cast<uint64_t>(right.asInt())));
  }
  if (left.isNumber() && right.isNumber()) {
    return Value(left.number() - right.number());
  }
  failRuntime("unsupported operand types for '-': " + typePair(left, right));
}

Value mulValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) {
    return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) *
                                      static_cast<uint64_t>(right.asInt())));
  }
  if (left.isNumber() && right.isNumber()) {
    return Value(left.number() * right.number());
  }
  failRuntime("unsupported operand types for '*': " + typePair(left, right));
}

Value divValues(const Value& left, const Value& right) {
  if (left.isNumber() && right.isNumber()) {
    if (right.number() == 0) failRuntime("division by zero");
    if (left.isInt() && right.isInt()) {
      int64_t a = left.asInt();
      int64_t b = right.asInt();
      if (a == std::numeric_limits<int64_t>::min() && b == -1) return Value(a);
      return Value(a / b);
    }
    return Value(left.number() / right.number());
  }
  failRuntime("unsupported operand types for '/': " + typePair(left, right));
}

Value modValues(const Value& left, const Value& right) {
  if (left.isNumber() && right.isNumber()) {
    if (right.number() == 0) failRuntime("division by zero");
    if (left.isInt() && right.isInt()) {
      int64_t a = left.asInt();
      int64_t b = right.asInt();
      if (a == std::numeric_limits<int64_t>::min() && b == -1) return Value(static_cast<int64_t>(0));
      return Value(a % b);
    }
    return Value(std::fmod(left.number(), right.number()));
  }
  failRuntime("unsupported operand types for '%': " + typePair(left, right));
}

Value powValues(const Value& left, const Value& right) {
  if (left.isNumber() && right.isNumber()) {
    if (left.isInt() && right.isInt() && right.asInt() >= 0) {
      return Value(intPow(left.asInt(), right.asInt()));
    }
    return Value(std::pow(left.number(), right.number()));
  }
  failRuntime("unsupported operand types for '**': " + typePair(left, right));
}

Value negValue(const Value& operand) {
  if (operand.isInt()) {
    return Value(static_cast<int64_t>(0ULL - static_cast<uint64_t>(operand.asInt())));
  }
  if (operand.isFloat()) return Value(-operand.asFloat());
  failRuntime("unsupported unary operator for type '" + std::string(operand.typeName()) + "'");
}

Value bnotValue(const Value& operand) {
  if (operand.isInt()) return Value(~operand.asInt());
  failRuntime("unsupported unary operator for type '" + std::string(operand.typeName()) + "'");
}

Value bandValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) return Value(left.asInt() & right.asInt());
  failRuntime("unsupported operand types for '&': " + typePair(left, right));
}

Value borValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) return Value(left.asInt() | right.asInt());
  failRuntime("unsupported operand types for '|': " + typePair(left, right));
}

Value bxorValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) return Value(left.asInt() ^ right.asInt());
  failRuntime("unsupported operand types for '^': " + typePair(left, right));
}

Value shlValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) {
    int64_t shift = right.asInt();
    if (shift < 0 || shift > 63) failRuntime("shift count must be between 0 and 63");
    return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt())
                                      << static_cast<uint64_t>(shift)));
  }
  failRuntime("unsupported operand types for '<<': " + typePair(left, right));
}

Value shrValues(const Value& left, const Value& right) {
  if (left.isInt() && right.isInt()) {
    int64_t shift = right.asInt();
    if (shift < 0 || shift > 63) failRuntime("shift count must be between 0 and 63");
    return Value(left.asInt() >> shift);
  }
  failRuntime("unsupported operand types for '>>': " + typePair(left, right));
}

// Same ordering relation as `Interpreter::lessThan`.
bool lessThan(const Value& left, const Value& right) {
  if (left.isNumber() && right.isNumber()) {
    if (left.isInt() && right.isInt()) return left.asInt() < right.asInt();
    return left.number() < right.number();
  }
  if (left.isStr() && right.isStr()) return left.asStr() < right.asStr();
  if (left.isBool() && right.isBool()) return !left.asBool() && right.asBool();
  failRuntime("cannot compare '" + std::string(left.typeName()) + "' with '" +
              right.typeName() + "'");
}

Value ltValues(const Value& left, const Value& right) { return Value(lessThan(left, right)); }
Value lteValues(const Value& left, const Value& right) { return Value(!lessThan(right, left)); }
Value gtValues(const Value& left, const Value& right) { return Value(lessThan(right, left)); }
Value gteValues(const Value& left, const Value& right) { return Value(!lessThan(left, right)); }

}  // namespace

// ------------------------------------------------------------------ registers

Value& VM::getReg(int index) {
  if (index < 0 || static_cast<size_t>(index) >= registers_.size()) {
    failRuntime("register index out of range: " + std::to_string(index));
  }
  return registers_[static_cast<size_t>(index)];
}

void VM::setReg(int index, Value value) { getReg(index) = std::move(value); }

CellPtr& VM::localAt(int index) {
  if (index < 0 || static_cast<size_t>(index) >= locals_.size()) {
    failRuntime("local index out of range: " + std::to_string(index));
  }
  return locals_[static_cast<size_t>(index)];
}

// ------------------------------------------------------------------ execution

void VM::run(const Bytecode& bytecode) {
  const int registerCount = bytecode.numRegisters > 0 ? bytecode.numRegisters : 0;
  const int localCount = bytecode.numLocals > 0 ? bytecode.numLocals : 0;
  registers_.assign(static_cast<size_t>(registerCount), Value{});
  locals_.assign(static_cast<size_t>(localCount), CellPtr{});

  const size_t size = bytecode.code.size();
  size_t pc = 0;

  while (pc < size) {
    const Instruction& instruction = bytecode.code[pc++];

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
        setReg(instruction.a, addValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::SUB:
        setReg(instruction.a, subValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::MUL:
        setReg(instruction.a, mulValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::DIV:
        setReg(instruction.a, divValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::MOD:
        setReg(instruction.a, modValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::POW:
        setReg(instruction.a, powValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::NEG:
        setReg(instruction.a, negValue(getReg(instruction.b)));
        break;

      case OpCode::NOT:
        setReg(instruction.a, Value(!getReg(instruction.b).truthy()));
        break;

      case OpCode::BAND:
        setReg(instruction.a, bandValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::BOR:
        setReg(instruction.a, borValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::BXOR:
        setReg(instruction.a, bxorValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::SHL:
        setReg(instruction.a, shlValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::SHR:
        setReg(instruction.a, shrValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::BNOT:
        setReg(instruction.a, bnotValue(getReg(instruction.b)));
        break;

      case OpCode::EQ:
        setReg(instruction.a, Value(getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::NEQ:
        setReg(instruction.a, Value(!getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::LT:
        setReg(instruction.a, ltValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::LTE:
        setReg(instruction.a, lteValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::GT:
        setReg(instruction.a, gtValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::GTE:
        setReg(instruction.a, gteValues(getReg(instruction.b), getReg(instruction.c)));
        break;

      case OpCode::JMP:
        if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
          failRuntime("invalid jump target: " + std::to_string(instruction.jump));
        }
        pc = static_cast<size_t>(instruction.jump);
        break;

      case OpCode::JMP_IF_FALSE:
        if (!getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            failRuntime("invalid jump target: " + std::to_string(instruction.jump));
          }
          pc = static_cast<size_t>(instruction.jump);
        }
        break;

      case OpCode::JMP_IF_TRUE:
        if (getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            failRuntime("invalid jump target: " + std::to_string(instruction.jump));
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
        if (it == globals_.end()) failRuntime("undefined variable '" + name + "'");
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
        if (!cell) failRuntime("undefined local variable");
        cell->value = getReg(instruction.a);
        break;
      }

      case OpCode::GETLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        if (!cell) failRuntime("undefined local variable");
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
