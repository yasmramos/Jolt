#pragma once

#include <cstdint>

#include "native/codegen.h"
#include "native/entry.h"
#include "native/value.h"

// Embedded runtime for the freestanding AOT backend (PHASE 7c.1).
//
// A generated binary has no libc, so every operation the bytecode needs that
// is not a single machine instruction is *emitted as machine code* into the
// same buffer as the program -- exactly the way `entry.cpp` appends `_start`
// and the syscall wrappers. There is no linker, no relocations and no shared
// library: a `call` from a lowered opcode targets a label this module binds.
//
// The helpers implement the value semantics the hosted engine implements in
// `ops.h`. They must agree with it -- what `+` does, what is truthy, how two
// values compare, what a type mismatch reports -- without sharing any code,
// because `ops.h` is built on the reference-counted `Value` and a throwing
// `Runtime`, neither of which exists in a `-nostdlib` binary.

// Labels for the runtime helpers. The caller creates them up front so a
// program lowered *before* the runtime can already reference them, then calls
// `emitAotRuntime` to bind them.
//
// Every helper follows the System V AMD64 ABI: an `AotValue` is passed in two
// registers (tag then payload, in rdi:rsi for the first and rdx:rcx for the
// second) and returned in rax:rdx.
struct AotRuntimeHandles {
  CodeGen::Label printValue;     // void  jolt_print_value(AotValue v)
  CodeGen::Label addValues;      // AotValue jolt_add(AotValue a, AotValue b)
  CodeGen::Label subValues;      // AotValue jolt_sub(AotValue a, AotValue b)
  CodeGen::Label mulValues;      // AotValue jolt_mul(AotValue a, AotValue b)
  CodeGen::Label bitwiseValues;  // AotValue jolt_bitwise(AotValue a, AotValue b, long op)
  CodeGen::Label negateValue;    // AotValue jolt_negate(AotValue a)
  CodeGen::Label notValue;       // AotValue jolt_not(AotValue a)
  CodeGen::Label bitNotValue;    // AotValue jolt_bit_not(AotValue a)
  CodeGen::Label compareValues;  // AotValue jolt_compare(AotValue a, AotValue b, long op)
  CodeGen::Label truthy;         // long      jolt_truthy(AotValue a)
};

inline AotRuntimeHandles newAotRuntimeHandles(CodeGen& code) {
  return AotRuntimeHandles{
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(),
  };
}

// The selector `jolt_compare` accepts: the low nibble of the x86-64 condition
// code for each bytecode comparison, so the runtime can branch with a plain
// `cmp` + `je` chain. The values match `OpCode::EQ`, `NEQ`, `LT`, `LTE`, `GT`
// and `GTE` one-to-one.
enum class AotCompareOp : std::int64_t {
  Eq = 4,
  Ne = 5,
  Lt = 12,
  Gt = 15,
  Le = 14,
  Ge = 13,
};

// The selector `jolt_bitwise` accepts.
enum class AotBitwiseOp : std::int64_t {
  And = 1,
  Or = 2,
  Xor = 3,
};

// Appends every runtime helper to `code` and binds `handles`. `entry` comes
// from `entry.h`: the runtime prints through `jolt_write` and reports a fatal
// error through `jolt_exit`, so the caller must emit the entry prelude into
// the same buffer (see `emitFreestandingRuntime`).
void emitAotRuntime(CodeGen& code, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry);
