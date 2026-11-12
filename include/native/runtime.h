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
  // The same printer without the trailing newline, for the `print`/`write`/
  // `eprint` builtins, which join their arguments with single spaces and add
  // one newline themselves. `printValueRaw` writes to stdout, `printValueErrRaw`
  // to stderr (PHASE 7c.2d).
  CodeGen::Label printValueRaw;     // void jolt_print_value_raw(AotValue v)
  CodeGen::Label printValueErrRaw;  // void jolt_print_value_err_raw(AotValue v)
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
  // Closures and calls (PHASE 7c.2c.1). `closure` allocates an `AotClosure` in
  // the arena for a proto's code address (rdi), arity (rsi) and captured-cell
  // count (rdx), and returns it as a `Func` value in rax:rdx. The lowering
  // writes the captured cell pointers into the header itself (each is a plain
  // 8-byte store), so `closure` only zero-fills the slot area. `call`
  // dispatches a callee (rdi:rsi) with `argc` arguments (rdx) whose window
  // starts at `args` (rcx): it checks the callee is a function, then calls its
  // code with the lowering's own function convention (`AotValue entry(AotValue*
  // args, long argc, AotClosure* self)`), returning the result in rax:rdx. A
  // non-function callee is a fatal error.
  CodeGen::Label closure;        // AotValue jolt_closure(void* code, long arity, long upvalueCount)
  CodeGen::Label call;           // AotValue jolt_call(AotValue callee, long argc, AotValue* args)
  // `AotValue* jolt_cell_new(AotValue v)`: allocate a 16-byte cell in the arena
  // holding `v` (rdi:rsi) and return its address (rax). A captured local lives
  // in a cell shared with the closures that capture it, so a write through the
  // closure and a read in the frame see the same value (PHASE 7c.2c.2).
  CodeGen::Label cellNew;        // AotValue* jolt_cell_new(AotValue v)
  // Fatal "wrong number of arguments" report. A function's prologue calls it
  // when the argument count does not match its declared arity, the runtime
  // counterpart of the hosted engine's arity check in `pushJoltFrame`.
  CodeGen::Label arityError;     // void jolt_arity_error(void)
  // The `print`/`write`/`eprint` builtins, as machine-code functions with the
  // same convention as a bytecode proto (`AotValue entry(AotValue* args, long
  // argc, AotClosure* self)`). The chunk seeds each into its global slot as an
  // `AotClosure`, so a `CALL` reaches them through the ordinary `jolt_call`
  // path. `print` and `eprint` add a trailing newline, `write` does not;
  // `eprint` writes to stderr. Arguments are joined with single spaces
  // (PHASE 7c.2d).
  CodeGen::Label printBuiltin;   // AotValue jolt_builtin_print(AotValue* args, long argc, AotClosure* self)
  CodeGen::Label writeBuiltin;   // AotValue jolt_builtin_write(...)
  CodeGen::Label eprintBuiltin;  // AotValue jolt_builtin_eprint(...)
  // Collections (PHASE 7c.3). `tableNew` allocates an empty array (kind 0) or
  // map (kind 1); `tableSet`/`tableGet` are the dynamic-index forms
  // (`R[b][R[c]]`), `memberSet`/`memberGet` the constant-name forms
  // (`R[b].name`); `contains` implements `in`; `iterNext` advances a `for-in`
  // cursor, taking a pointer to the cursor slot (rdi) and the iterable
  // (rdx:rcx) and returning the item (rax:rdx) with the done flag in r8;
  // `strEq` compares two `AotString`s by content (map keys are compared by
  // value, not pointer).
  CodeGen::Label tableNew;   // AotValue jolt_table_new(long kind)
  CodeGen::Label tableGet;   // AotValue jolt_table_get(AotValue obj, AotValue index)
  CodeGen::Label memberGet;  // AotValue jolt_member_get(AotValue obj, AotValue name)
  CodeGen::Label tableSet;   // void jolt_table_set(AotValue obj, AotValue index, AotValue value)
  CodeGen::Label memberSet;  // void jolt_member_set(AotValue obj, AotValue name, AotValue value)
  CodeGen::Label contains;   // AotValue jolt_contains(AotValue needle, AotValue haystack)
  CodeGen::Label iterNext;   // AotValue jolt_iter_next(AotValue* cursor, AotValue iterable)
  CodeGen::Label strEq;      // long jolt_str_eq(AotString* a, AotString* b)
  // The hidden `$mapValue` helper (`for k, v in map`), with the proto
  // convention. It returns the value paired with a key, so the key/value loop
  // form lowers to an ordinary call.
  CodeGen::Label mapValueBuiltin;  // AotValue jolt_builtin_map_value(AotValue* args, long argc, AotClosure* self)
  // The remaining hidden helpers the compiler emits for PHASE 6.5 features.
  // They use the proto convention too and are seeded into their global slots
  // like `print` and `$mapValue`, so each one is an ordinary call.
  // `$isError` (try/catch) reports whether a value is an error; the AOT runtime
  // has no error values yet, so it is always false and the success path of
  // `try`/`x catch y` runs (a real failure still exits 70).
  CodeGen::Label isErrorBuiltin;   // AotValue jolt_builtin_is_error(AotValue* args, long argc, AotClosure* self)
  // `$nonNull` (`a!!`): returns the operand, or fails when it is nil.
  CodeGen::Label nonNullBuiltin;   // AotValue jolt_builtin_non_null(AotValue* args, long argc, AotClosure* self)
  // `$str` (string interpolation and computed keys): renders the operand to a
  // string, byte-identical to the hosted `Value::toString`.
  CodeGen::Label strBuiltin;       // AotValue jolt_builtin_str(AotValue* args, long argc, AotClosure* self)
  // `$range` (`a..b` / `a..=b`): allocates a `Range` value from two integer
  // bounds and an inclusive flag.
  CodeGen::Label rangeBuiltin;     // AotValue jolt_builtin_range(AotValue* args, long argc, AotClosure* self)
  // `$slice` (`obj[start:end:step]`): slices a string by UTF-8 character or an
  // array by element, following Python's bounds rule.
  CodeGen::Label sliceBuiltin;     // AotValue jolt_builtin_slice(AotValue* args, long argc, AotClosure* self)
};

inline AotRuntimeHandles newAotRuntimeHandles(CodeGen& code) {
  return AotRuntimeHandles{
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
      code.newLabel(), code.newLabel(), code.newLabel(), code.newLabel(),
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
