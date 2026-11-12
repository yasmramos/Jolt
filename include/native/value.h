#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

// Boxed value of the freestanding AOT runtime (PHASE 7c.1).
//
// The development engine stores a `Value` as a reference-counted, 16-byte
// tagged union that can own heap objects and take part in a cycle collector
// (see `value.h`). None of that can live in a generated binary: there is no
// libc, no `malloc` and no unwinder, and the runtime is a region, not a heap
// (see `native/arena.h`). The AOT runtime therefore has its *own* value type
// that shares the *semantics* of `ops.h` -- what `+` does, what is truthy, how
// two values compare -- but not one line of code with the hosted one.
//
// `AotValue` is two 64-bit words: a tag and a payload. Sixteen bytes is not an
// accident -- it is the smallest size the System V AMD64 ABI passes and returns
// in two registers (rdi:rsi as arguments, rax:rdx as a result), so a register
// file of `AotValue`s costs exactly two loads or stores per value and every
// runtime helper takes and returns a value without touching memory. The layout
// is fixed here, next to the tags, because it is nearly impossible to change
// once machine code depends on it (AGENT.md points at LuaJIT's NaN-boxing for
// the same reason).

// Value tags. The names and the ordering mirror the hosted `VType` where a
// kind exists in both worlds, so the two engines agree on what a value *is*.
// `Float` and `Double` carry the IEEE-754 binary64 bit pattern of the value in
// their payload; a `Float` stores a double that holds exactly a binary32 value
// (the hosted engine widens binary32 to binary64 the same way), so rounding it
// back to binary32 is lossless. `Str`'s payload is the address of an
// `AotString` (see below): a string is a pointer, not a value, so `AotValue`
// stays 16 bytes.
enum class AotTag : std::uint64_t {
  Nil = 0,
  Bool = 1,
  Int = 2,
  Float = 3,   // payload is the IEEE-754 binary64 bit pattern of a binary32 value
  Double = 4,  // payload is the IEEE-754 binary64 bit pattern
  Str = 5,     // payload is the address of an `AotString`
  Func = 6,    // payload is the address of an `AotClosure`
  Array = 7,   // payload is the address of an `AotArray`
  Map = 8,     // payload is the address of an `AotMap`
  Range = 9,   // payload is the address of an `AotRange`
};

// On-image layout of a string: an 8-byte length followed by the bytes. The
// bytes are not NUL-terminated -- a Jolt string can hold an embedded NUL -- so
// every string helper carries the length explicitly, exactly like `std::string`
// and unlike C. A `Str` payload points at the length word, so a helper reads
// the length from `[ptr]` and the first byte from `[ptr + 8]`.
//
// The layout is fixed here for the same reason as `AotValue`: once machine
// code walks strings with `movzx`, the offsets are part of the ABI.
struct AotString {
  std::uint64_t length;
  // The bytes follow immediately; this struct only names the header.
};

constexpr std::size_t kAotStringHeaderSize = sizeof(std::uint64_t);

// On-image layout of a closure (PHASE 7c.2c.1): the code address of the
// function's entry, the number of arguments it expects, then the captured
// cells. A `Func` value's payload points at `code`. From 7c.2c.2 a proto with
// upvalues captures its defining frame's cells: `upvalueCount` says how many
// pointers follow the header, each pointing at a 16-byte `AotValue` cell
// shared with the defining frame (so a write through the closure is visible
// to the frame and to every other closure that captured the same cell).
//
// A bytecode function's machine-code entry follows the *lowering's own*
// convention, not the C ABI: `AotValue entry(AotValue* args, long argc,
// AotClosure* self)`. `args` points at the caller's argument window (the
// caller's R[a+1 .. a+argc]); the callee copies the arguments into its own
// register file, so the caller's window is never aliased. `self` is the
// closure being entered, so `GETUPVAL`/`SETUPVAL` and a nested `CLOSURE` read
// the captured cells through it. This differs from a runtime helper (which
// takes two `AotValue`s in rdi:rsi/rdx:rcx) on purpose: a function has a
// variable argument count, which does not fit the two-register value ABI.
struct AotClosure {
  std::uint64_t code;          // address of the function's machine code
  std::uint64_t arity;         // number of declared parameters
  std::uint64_t upvalueCount;  // number of captured cells that follow
  // The captured cells (pointers to 16-byte `AotValue` slots) follow.
};

constexpr std::size_t kAotClosureHeaderSize = 3 * sizeof(std::uint64_t);

// On-image layout of an array (PHASE 7c.3): a live length, a capacity and the
// address of the items block (an array of 16-byte `AotValue`s). The arena never
// frees, so growth allocates a fresh, larger block and copies the items; the
// old block is abandoned. An `Array` payload points at the header, so a helper
// reads the length from `[ptr]` and the first item from `[items]`.
struct AotArray {
  std::uint64_t length;
  std::uint64_t capacity;
  std::uint64_t items;  // address of the items block
};

constexpr std::size_t kAotArrayHeaderSize = 3 * sizeof(std::uint64_t);

// On-image layout of a map (PHASE 7c.3): an insertion-ordered array of entries
// with string keys, matching the hosted `Map` (which keeps insertion order too).
// A linear scan is enough for the sizes a compiled desktop program builds at
// start-up; a hash index can come later without changing the layout's contract.
// Each entry is a 24-byte record: the key's `AotString` address, then the
// 16-byte value.
struct AotMap {
  std::uint64_t length;
  std::uint64_t capacity;
  std::uint64_t entries;  // address of the entries block
};

constexpr std::size_t kAotMapHeaderSize = 3 * sizeof(std::uint64_t);

// On-image layout of a range (PHASE 7c.4c): two int64 bounds and an inclusive
// flag. A `Range` payload points at the header. `for-in` walks from `start`
// towards `end` (ascending when `start <= end`), inclusive or exclusive.
struct AotRange {
  std::int64_t start;
  std::int64_t end;
  std::uint64_t inclusive;
};

constexpr std::size_t kAotRangeHeaderSize = 3 * sizeof(std::uint64_t);

// On-image layout of a string builder (PHASE 7c.4b.2): a growable byte buffer in
// the arena, used to render a collection or function to an `AotString` for
// string interpolation (`"${[1, 2]}"`). `length` is the bytes used, `capacity`
// the bytes allocated and `data` the address of the block. Growth allocates a
// fresh, larger block and copies the bytes; the arena never frees, so the old
// block is abandoned. The builder is a helper's private state, never a `Value`.
struct AotStringBuilder {
  std::uint64_t length;
  std::uint64_t capacity;
  std::uint64_t data;  // address of the bytes block
};

constexpr std::size_t kAotSbHeaderSize = 3 * sizeof(std::uint64_t);

struct AotValue {
  std::uint64_t tag;
  std::uint64_t payload;
};

constexpr std::size_t kAotMapEntrySize = 8 + sizeof(AotValue);

static_assert(sizeof(AotValue) == 16,
              "AotValue must be 16 bytes to be passed and returned in two registers");
static_assert(std::is_trivially_copyable<AotValue>::value,
              "AotValue crosses the ABI boundary and must stay trivially copyable");
static_assert(alignof(AotValue) == 8, "AotValue is two naturally aligned 64-bit words");

// Builders used by the tests and by the runtime prelude. They are `constexpr`
// so a caller can name a constant value without a constructor call.
constexpr AotValue aotNil() { return AotValue{static_cast<std::uint64_t>(AotTag::Nil), 0}; }
constexpr AotValue aotBool(bool value) {
  return AotValue{static_cast<std::uint64_t>(AotTag::Bool), value ? 1u : 0u};
}
constexpr AotValue aotInt(std::int64_t value) {
  return AotValue{static_cast<std::uint64_t>(AotTag::Int), static_cast<std::uint64_t>(value)};
}

// The payload of a float/double is the IEEE-754 bit pattern, not the numeric
// value: a runtime helper moves it into an xmm register with `movq` and never
// reinterprets it through an integer ALU, so a signalling NaN or a negative
// zero survives the round trip unchanged. `memcpy` is the portable way to
// reinterpret the bits and is a no-op at `-O2`.
inline std::uint64_t doubleBits(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}
inline AotValue aotDouble(double value) {
  return AotValue{static_cast<std::uint64_t>(AotTag::Double), doubleBits(value)};
}
inline AotValue aotFloat(double value) {
  return AotValue{static_cast<std::uint64_t>(AotTag::Float), doubleBits(value)};
}
