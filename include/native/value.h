#pragma once

#include <cstdint>
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
// `Float` and `Str` are reserved for PHASE 7c.1b: their payloads will be a
// binary64 bit pattern and an arena string pointer respectively, and no
// opcode produces them yet, so the lowering rejects them instead of emitting
// wrong code.
enum class AotTag : std::uint64_t {
  Nil = 0,
  Bool = 1,
  Int = 2,
  Float = 3,  // reserved: payload is the IEEE-754 binary64 bit pattern
  Str = 4,    // reserved: payload is a pointer to an arena string object
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
