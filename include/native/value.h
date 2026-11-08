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
// back to binary32 is lossless. `Str` is reserved for PHASE 7c.1c: its payload
// will be an arena string pointer, and no opcode produces it yet, so the
// lowering rejects a string constant instead of emitting wrong code.
enum class AotTag : std::uint64_t {
  Nil = 0,
  Bool = 1,
  Int = 2,
  Float = 3,   // payload is the IEEE-754 binary64 bit pattern of a binary32 value
  Double = 4,  // payload is the IEEE-754 binary64 bit pattern
  Str = 5,     // reserved: payload is a pointer to an arena string object
};

struct AotValue {
  std::uint64_t tag;
  std::uint64_t payload;
};

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
