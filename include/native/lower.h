#pragma once

#include <cstdint>
#include <vector>

#include "bytecode.h"
#include "native/codegen.h"
#include "native/target.h"

// Lowers a Bytecode proto to x86-64 machine code.
//
// PHASE 7c.1 lowers the runtime-free opcodes over the boxed `AotValue` (see
// `native/value.h`): LOADK (nil/bool/int/float/double/string), MOVE, ADD, SUB,
// MUL, DIV, MOD, POW, BAND, BOR, BXOR, SHL, SHR, UNOP (negate/not/bit-not),
// EQ, NEQ, LT, LTE, GT, GTE, JMP, JMP_IF_FALSE, JMP_IF_TRUE, HALT; from 7c.2a,
// GETGLOBAL/SETGLOBAL over a flat global table in the arena; from 7c.2b,
// DECLLOCAL/GETLOCAL/SETLOCAL over a second stack region; from 7c.2c.2, CALL,
// RETURN, CLOSURE, GETUPVAL/SETUPVAL and cell capture; from 7c.2d, the
// `print`/`write`/`eprint` builtins, seeded as globals; and from 7c.3,
// NEWTABLE, GETTABLE, SETTABLE, IN and ITER, so arrays, maps, indexing,
// members, `in` and `for-in` compile to native code.
//
// A bytecode register is a 16-byte slot in a register file on the stack, not a
// physical CPU register: a boxed value does not fit in one, and the file is
// what makes `print`, collections and calls possible without a second value
// representation. The result of `lowerToX64` is a self-contained image: the
// program, the `_start` entry point and the embedded runtime, all in one code
// buffer with every `call` resolved through the emitter's labels (no linker,
// no relocations).
//
// A GETGLOBAL of a name the chunk never assigns and that is not a builtin is a
// `LoweringError`. From 7c.2d the `print`, `write` and `eprint` builtins are
// seeded as globals, so a program can print without a runtime to install them.
//
// From 7c.2c.1 every proto is lowered to its own machine function with the
// lowering's own convention `AotValue entry(AotValue* args, long argc,
// AotClosure* self)`. `CALL` builds the argument window in the caller's
// register file and dispatches through `oskel_call`; `RETURN` publishes R[a];
// `CLOSURE` allocates an `AotClosure` for a capture-free child proto.
struct LoweringError : std::exception {
  explicit LoweringError(std::string message) : message(std::move(message)) {}
  std::string message;
  const char* what() const noexcept override { return message.c_str(); }
};

// The lowered image and its entry points. `programEntry` is the chunk's own
// entry: it builds the register file, runs the chunk and returns R0's payload
// in rax (a provisional convention used by the tests; `CALL`/`RETURN` replace
// it in 7c.2). `processEntry` is the `_start` offset to hand to
// `buildStaticElf`: it runs the chunk and exits 0, so a program's exit code
// does not depend on whatever value R0 happens to hold.
struct LoweredChunk {
  std::vector<std::uint8_t> code;
  std::uint64_t programEntry = 0;
  std::uint64_t processEntry = 0;
};

// Lowers `proto` to x86-64 machine code. The lowering assumes the System V
// AMD64 ABI (Linux x86-64). Windows x86-64 support is deferred to PHASE 7e.
LoweredChunk lowerToX64(const Target& target, const Bytecode& proto,
                        const CodeGen& codegen);