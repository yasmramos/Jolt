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
  // Numeric binary arithmetic with the int/float/double promotion of the
  // hosted engine. The selector (AotArithOp) is the third argument.
  CodeGen::Label arithValues;    // AotValue jolt_arith(AotValue a, AotValue b, long op)
  // `**`: integer power for int/int with a non-negative exponent (the hosted
  // `intPow`); every other case needs libm and is rejected for now.
  CodeGen::Label powValues;      // AotValue jolt_pow(AotValue a, AotValue b)
  // Integer `<<`/`>>`, selected by AotShiftOp; the shift count is a second
  // `AotValue` and is range-checked against 0..63.
  CodeGen::Label shiftValues;    // AotValue jolt_shift(AotValue a, AotValue b, long op)
  CodeGen::Label bitwiseValues;  // AotValue jolt_bitwise(AotValue a, AotValue b, long op)
  CodeGen::Label negateValue;    // AotValue jolt_negate(AotValue a)
  CodeGen::Label notValue;       // AotValue jolt_not(AotValue a)
  CodeGen::Label bitNotValue;    // AotValue jolt_bit_not(AotValue a)
  CodeGen::Label compareValues;  // AotValue jolt_compare(AotValue a, AotValue b, long op)
  CodeGen::Label truthy;         // long      jolt_truthy(AotValue a)
  // Bump allocation from the arena seeded by `jolt_init_arena`: returns an
  // 8-byte-aligned pointer, or null when the region is exhausted. The size is
  // in rdi.
  CodeGen::Label alloc;          // void* jolt_alloc(long size)
  // String concatenation: allocates `lenA + lenB + 8` bytes, writes the
  // combined length and bytes, and returns an `AotValue` (tag Str). Used by
  // `jolt_arith` for `Add` when both operands are strings.
  CodeGen::Label concat;         // AotValue jolt_concat(AotValue a, AotValue b)
  // The global table (PHASE 7c.2a). It is a contiguous array of `AotValue`
  // slots in the arena; the chunk allocates it once at entry and keeps its
  // base in r15. `initGlobals` takes the slot count in rdi. `globalSet` takes
  // the slot index in rdi and the value in rsi:rdx; `globalGet` takes the slot
  // index in rdi and returns the value in rax:rdx. Global names never reach
  // the binary: the compiler assigns each one an index, exactly like a local.
  CodeGen::Label initGlobals;    // void jolt_init_globals(long count)
  CodeGen::Label globalSet;      // void jolt_global_set(long slot, AotValue v)
  CodeGen::Label globalGet;      // AotValue jolt_global_get(long slot)
};

inline AotRuntimeHandles newAotRuntimeHandles(CodeGen& code) {
  return AotRuntimeHandles{
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(),
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

// The selector `jolt_arith` accepts: `+`, `-`, `*`, `/` and `%`. `Add` and
// `Sub` also concatenate strings once strings land (7c.1c); for now a
// non-numeric operand is the "unsupported operand types" error.
enum class AotArithOp : std::int64_t {
  Add = 1,
  Sub = 2,
  Mul = 3,
  Div = 4,
  Mod = 5,
};

// The selector `jolt_shift` accepts. `>>` is arithmetic in Jolt (it preserves
// the sign), so the two directions are distinct helpers.
enum class AotShiftOp : std::int64_t {
  Shl = 1,
  Shr = 2,
};

// Appends every runtime helper to `code` and binds `handles`. `entry` comes
// from `entry.h`: the runtime prints through `jolt_write` and reports a fatal
// error through `jolt_exit`, so the caller must emit the entry prelude into
// the same buffer (see `emitFreestandingRuntime`).
void emitAotRuntime(CodeGen& code, const AotRuntimeHandles& handles,
                    const RuntimeHandles& entry);
