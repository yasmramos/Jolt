#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "bytecode.h"
#include "value.h"

// Just-in-time compiler for the development engine.
//
// The JIT belongs to `jolt` (run, REPL, `-e`) only. `joltc` and the AOT runtime
// never link it: the product is compiled ahead of time, so a JIT there would
// violate the "no VM-only features" rule (AGENT.md §8). It is also the reason
// the AOT backend must stay able to reject this header (the compiler boundary
// check forbids it there).
//
// Slice 1 compiles a *leaf* proto -- no `CALL`, no `CLOSURE`, no collections,
// no globals -- whose body a dataflow analysis proves only ever manipulates
// `nil`, `bool` and `int`. Those values carry no reference count, so the
// generated code touches neither the heap nor the cycle collector: it is a
// pure integer function that makes no call back into the runtime. A proto
// outside the subset is simply not compiled and keeps running on the
// interpreter, so the JIT can never change a program's result; it only changes
// how fast the subset runs.
//
// The bytecode contract is untouched: no new opcode, no superinstruction, no
// inline cache and no runtime specialization (AGENT.md §3.3/§8). The lowering
// has the same 1:1 shape the AOT backend uses and goes through the same
// `native/codegen.h` x86-64 assembler, so no external JIT library is needed
// (AGENT.md §5.1).

// Executable memory holding one compiled function. It is mapped read/write,
// filled, then re-protected read/execute, so the mapping is never writable and
// executable at the same time.
class JitCode {
 public:
  JitCode() = default;
  ~JitCode();
  JitCode(JitCode&& other) noexcept;
  JitCode& operator=(JitCode&& other) noexcept;
  JitCode(const JitCode&) = delete;
  JitCode& operator=(const JitCode&) = delete;

  // Copies `bytes` into fresh executable memory. Returns false on failure or
  // when this host has no JIT backend (the caller then interprets the proto).
  bool assign(const std::vector<std::uint8_t>& bytes);

  void* entry() const { return entry_; }
  bool empty() const { return entry_ == nullptr; }

 private:
  void release();

  void* mapping_ = nullptr;
  std::size_t mappingSize_ = 0;
  void* entry_ = nullptr;
};

// One compiled proto: an executable entry point plus the arity it was compiled
// for. The native convention is `void entry(const Value* args, int64_t argc,
// Value* out)`: it reads `args[0 .. numParams - 1]` and writes the returned
// value to `*out`.
class JitFunction {
 public:
  using Entry = void (*)(const Value* args, std::int64_t argc, Value* out);

  JitFunction(JitCode code, int numParams) : code_(std::move(code)), numParams_(numParams) {}

  int numParams() const { return numParams_; }

  // Runs the native code. `args` must hold exactly `numParams` integers; the
  // caller (the VM) guarantees both before calling.
  Value call(const std::vector<Value>& args) const;

 private:
  JitCode code_;
  int numParams_ = 0;
};

// Compiles `proto` to machine code, or returns nullptr when it is outside the
// supported subset or this host has no backend. Exposed for tests; production
// goes through `Jit::get`, which caches the result.
std::unique_ptr<JitFunction> jitCompile(const Bytecode& proto);

// Per-VM cache of compiled protos. The cache holds a strong reference to each
// proto it compiled, so a proto address can never be reused by a different
// proto while its entry is cached. Single-threaded, like the VM.
class Jit {
 public:
  Jit() = default;

  // Returns the compiled function for `proto`, compiling it on first call.
  // Returns nullptr when the proto is outside the subset.
  JitFunction* get(const std::shared_ptr<const Bytecode>& proto);

  // Drops every compiled function.
  void clear() { functions_.clear(); }

  // Number of protos that were successfully compiled (a proto outside the
  // subset is cached as a miss but not counted).
  std::size_t compiledCount() const {
    std::size_t count = 0;
    for (const auto& item : functions_) {
      if (item.second.function) ++count;
    }
    return count;
  }

 private:
  struct Entry {
    std::shared_ptr<const Bytecode> proto;  // keeps the address stable
    std::unique_ptr<JitFunction> function;
  };
  std::unordered_map<const Bytecode*, Entry> functions_;
};
