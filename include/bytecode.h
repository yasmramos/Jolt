#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "value.h"

// Low-level bytecode for Jolt's register-based VM.
// Each instruction operates on registers and is designed to map to a single
// native instruction in the future PHASE 7.

enum class OpCode : std::uint8_t {
  LOADK,      // R[a] = numbers[b]  (numeric constant: int or float)
  LOADKS,     // R[a] = strings[b]  (string constant)
  LOADNIL,    // R[a] = nil
  LOADBOOL,   // R[a] = (b != 0)
  MOVE,       // R[a] = R[b]

  ADD,        // R[a] = R[b] + R[c]
  SUB,        // R[a] = R[b] - R[c]
  MUL,        // R[a] = R[b] * R[c]
  DIV,        // R[a] = R[b] / R[c]
  MOD,        // R[a] = R[b] % R[c]
  POW,        // R[a] = R[b] ** R[c]
  NEG,        // R[a] = -R[b]
  NOT,        // R[a] = !R[b]

  BAND,       // R[a] = R[b] & R[c]
  BOR,        // R[a] = R[b] | R[c]
  BXOR,       // R[a] = R[b] ^ R[c]
  SHL,        // R[a] = R[b] << R[c]
  SHR,        // R[a] = R[b] >> R[c]
  BNOT,       // R[a] = ~R[b]

  EQ,         // R[a] = R[b] == R[c]
  NEQ,        // R[a] = R[b] != R[c]
  LT,         // R[a] = R[b] <  R[c]
  LTE,        // R[a] = R[b] <= R[c]
  GT,         // R[a] = R[b] >  R[c]
  GTE,        // R[a] = R[b] >= R[c]

  // Control flow. `jump` is an absolute index into `code`.
  JMP,          // pc = jump
  JMP_IF_FALSE, // if !R[a] then pc = jump
  JMP_IF_TRUE,  // if R[a] then pc = jump

  // Variables. Globals are resolved by name (string pool); locals by cell
  // index in the current frame.
  SETGLOBAL,  // globals[strings[b]] = R[a]
  GETGLOBAL,  // R[a] = globals[strings[b]]
  DECLLOCAL,  // locals[b] = new cell holding R[a]
  SETLOCAL,   // locals[b]->value = R[a]
  GETLOCAL,   // R[a] = locals[b]->value

  PRINT,      // print R[a]
  HALT,       // stop execution
};

struct Instruction {
  OpCode op = OpCode::HALT;
  int a = 0;
  int b = 0;
  int c = 0;
  int jump = 0;
  // Source position, used only for runtime diagnostics. Native codegen in
  // PHASE 7 ignores these fields.
  int line = 0;
  int col = 0;
};

struct Bytecode {
  std::vector<Instruction> code;
  // Numeric constant pool. Uses `Value` to preserve the difference between
  // integers and floats (a `double` would turn everything into a float).
  std::vector<Value> numbers;
  std::vector<std::string> strings;
  int numRegisters = 0;
  // Number of local cells the frame needs.
  int numLocals = 0;
};

// Resets the bytecode while keeping the capacity of its containers.
void bytecodeInit(Bytecode& bc);

// Appends an instruction and returns its index.
int bytecodeEmit(Bytecode& bc, const Instruction& instruction);

// Releases the bytecode contents. In C++ the containers manage their own
// memory, but the function is kept for symmetry with the original design.
void bytecodeFree(Bytecode& bc);
