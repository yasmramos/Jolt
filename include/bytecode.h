#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "value.h"

// Low-level bytecode for Jolt's register-based VM.
//
// Every instruction works on registers and is designed to map to a single
// native instruction in PHASE 7. The set is deliberately small: literals share
// one tagged constant pool, unary operators share one opcode whose `c` field
// selects the operation, and collections share the GET/SET table pair.
//
// PHASE 6 adds functions, collections and iteration, so the budget is exactly
// 40 opcodes: the ones below. Anything that needs more must first consolidate.

enum class OpCode : std::uint8_t {
  LOADK,   // R[a] = constants[b]  (nil, bool, int, float or string)
  MOVE,    // R[a] = R[b]

  ADD,     // R[a] = R[b] + R[c]
  SUB,     // R[a] = R[b] - R[c]
  MUL,     // R[a] = R[b] * R[c]
  DIV,     // R[a] = R[b] / R[c]
  MOD,     // R[a] = R[b] % R[c]
  POW,     // R[a] = R[b] ** R[c]
  UNOP,    // R[a] = unary(UnaryOp(c), R[b])

  BAND,    // R[a] = R[b] & R[c]
  BOR,     // R[a] = R[b] | R[c]
  BXOR,    // R[a] = R[b] ^ R[c]
  SHL,     // R[a] = R[b] << R[c]
  SHR,     // R[a] = R[b] >> R[c]

  EQ,      // R[a] = R[b] == R[c]
  NEQ,     // R[a] = R[b] != R[c]
  LT,      // R[a] = R[b] <  R[c]
  LTE,     // R[a] = R[b] <= R[c]
  GT,      // R[a] = R[b] >  R[c]
  GTE,     // R[a] = R[b] >= R[c]

  // Control flow. `jump` is an absolute index into `code`.
  JMP,          // pc = jump
  JMP_IF_FALSE, // if !R[a] then pc = jump
  JMP_IF_TRUE,  // if R[a] then pc = jump

  // Variables. Globals are resolved by name (a string constant in the pool);
  // locals by cell index in the current frame.
  SETGLOBAL,  // globals[constants[b]] = R[a]
  GETGLOBAL,  // R[a] = globals[constants[b]]
  DECLLOCAL,  // locals[b] = new cell holding R[a]
  SETLOCAL,   // locals[b]->value = R[a]
  GETLOCAL,   // R[a] = locals[b]->value

  // Functions (PHASE 6). `CALL` uses a contiguous argument window starting at
  // `b + 1`, so a call needs no separate argument-array allocation.
  CALL,       // R[a] = call(R[b], R[b+1 .. b+c])
  RETURN,     // return R[a] from the current frame
  CLOSURE,    // R[a] = closure(proto constants[b], captures listed in `c`)
  GETUPVAL,   // R[a] = upvalues[b]->value
  SETUPVAL,   // upvalues[b]->value = R[a]

  // Collections and iteration (PHASE 6). `GETTABLE`/`SETTABLE` cover both
  // numeric indices and members: the compiler picks the mode through `c`.
  NEWTABLE,   // R[a] = new table (array or map selected by `c`)
  GETTABLE,   // R[a] = R[b][R[c]]
  SETTABLE,   // R[b][R[c]] = R[a]
  ITER,       // R[a] = next item of R[b], or R[a] = nil when exhausted
  IN,         // R[a] = R[b] in R[c]

  PRINT,      // print R[a]
  HALT,       // stop execution
};

// Selector stored in `Instruction::c` for `UNOP`. Keeping the operator in the
// instruction lets PHASE 7 emit one native instruction per case without a
// second dispatch table.
enum class UnaryOp : std::uint8_t {
  Negate = 0,  // -x
  Not = 1,     // !x
  BitNot = 2,  // ~x
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
  // Tagged constant pool. Uses `Value` so the difference between integers and
  // floats survives, and so strings and global names live in the same pool.
  // From PHASE 6 the pool also holds function protos.
  std::vector<Value> constants;
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
