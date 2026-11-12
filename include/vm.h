#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode.h"
#include "builtins.h"
#include "jit/jit.h"
#include "runtime.h"
#include "value.h"

// Register-based virtual machine (PHASE 3-6).
//
// Executes the bytecode produced by the compiler. All registers and global
// variables are `Value`; there are no per-type arrays. Arithmetic and every
// other value operation go through `ops.h`, the same code the AST interpreter
// uses, so both engines agree by construction.
//
// Globals persist across `run()` calls (useful for a REPL); registers and
// locals are reset at the start of every execution.
//
// The standard library is not registered automatically: the CLI calls
// `registerBuiltins(vm)` explicitly, which keeps the VM linkable without the
// AST interpreter (the bytecode test suites need only the compiler and VM).
class VM : public Runtime {
 public:
  VM() { registerBuiltins(*this); }
  explicit VM(Platform& platform) : Runtime(platform) { registerBuiltins(*this); }

  // Runs the bytecode from the first instruction until HALT or the end.
  // Returns the value produced by a root-level RETURN (used by the REPL for
  // expression chunks); a program that ends with HALT returns nil.
  // Throws `RuntimeError` if the bytecode attempts an invalid operation.
  Value run(const Bytecode& bytecode);

  // Returns the value of a global, or nullptr if it does not exist.
  const Value* findGlobal(const std::string& name) const;

  // All globals, used by the REPL's `:vars` command. The map is owned by the
  // VM and must not outlive it.
  const std::unordered_map<std::string, Value>& globals() const { return globals_; }

  // Forgets every global. The caller re-registers the standard library when
  // it wants to keep it (the REPL's `:clear`).
  void clearGlobals() { globals_.clear(); }

  // Number of registers reserved by the last execution.
  int registerCount() const { return static_cast<int>(registers_.size()); }

  // Number of local cells reserved by the last execution.
  int localCount() const { return static_cast<int>(locals_.size()); }

  // Enables or disables the JIT. Off by default: the development engine turns
  // it on (`oskel`, `-e`, the REPL) and `oskelc` never does, because the AOT
  // product must not carry a JIT (AGENT.md §8). While enabled, a call to a
  // proto the JIT accepts runs the compiled code; every other call is
  // interpreted exactly as before, so results never change.
  void setJitEnabled(bool enabled) { jitEnabled_ = enabled; }
  bool jitEnabled() const { return jitEnabled_; }
  // Number of protos compiled so far (0 when the JIT is disabled).
  std::size_t jitCompiledCount() const { return jit_.compiledCount(); }

  Value call(const Value& callee, std::vector<Value> args, int line, int col) override;
  void defineGlobal(const std::string& name, Value value, bool isImmutable = false) override;

  [[noreturn]] void raise(const std::string& message, int line, int col) const override;

  Value indexGet(const Value& object, const Value& index, int line, int col) override;
  void indexSet(const Value& object, const Value& index, Value value, int line, int col) override;
  Value memberGet(const Value& object, const std::string& name, int line, int col) override;
  void memberSet(const Value& object, const std::string& name, Value value, int line, int col) override;
  Value binary(Tok op, const Value& left, const Value& right, int line, int col) override;
  bool lessThan(const Value& left, const Value& right, int line, int col) override;

 private:
  static constexpr std::size_t kNoResult = static_cast<std::size_t>(-1);

  // One activation record. Registers and local cells live in two stacks shared
  // by every live frame; `regBase`/`localBase` are the frame's windows in them.
  // `resultReg` is the absolute register that receives the return value, or
  // kNoResult for the root chunk and for calls started outside `run()`.
  struct CallFrame {
    std::shared_ptr<const Bytecode> proto;
    std::size_t pc = 0;
    // Absolute slot in `scratchCells_` receiving the return value when
    // `resultReg` is kNoResult, or kNoResult for frames without a scratch
    // destination (the root chunk).
    std::size_t resultScratch = kNoResult;
    std::size_t resultReg = kNoResult;
    std::size_t regBase = 0;
    std::size_t localBase = 0;
    // Captured cells of the running closure (PHASE 6b). Empty for the root
    // chunk and for plain calls without a closure object.
    std::vector<CellPtr> upvalues;
  };

  // Registers of every live frame, one window per frame, in call order.
  std::vector<Value> registers_;
  std::vector<CallFrame> frames_;
  std::vector<CellPtr> locals_;
  // Dedicated per-call return cells for the kNoResult protocol (PHASE 6b fix).
  // The scratch used to live one slot below the callee's local window inside
  // `locals_`, where a nested call could overwrite it while its own frame was
  // still running. A separate vector indexed by frame removes that structural
  // collision entirely: the cell cannot be clobbered and survives the shrinking
  // of the shared local windows.
  std::vector<CellPtr> scratchCells_;
  std::unordered_map<std::string, Value> globals_;

  // Development-engine JIT (slice 1). Disabled unless the CLI turns it on, so
  // the interpreter path is unchanged when it is off.
  Jit jit_;
  bool jitEnabled_ = false;

  // Returns the compiled function for `proto` when the JIT is enabled and
  // accepts it, or nullptr to interpret. The arguments must all be integers
  // and the arity must match, because the native code reads integer slots only.
  JitFunction* jitFor(const std::shared_ptr<const Bytecode>& proto, const std::vector<Value>& args);

  // Returns the dotted path to `name` when it exists as a member of a visible
  // global table, or "" when it does not. A forgotten namespace root is a
  // common mistake (`import std.math;` binds `std.math`, not `math`), so a
  // failed global read reports `std.math` instead of a bare "undefined
  // variable". Only globals that are maps are searched, hidden globals
  // (`$module$...`) are skipped, and a table that contains itself terminates.
  std::string namespaceHint(const std::string& name) const;

  Value& getReg(int index);
  void setReg(int index, Value value);
  CellPtr& localAt(int index);

  // Runs until HALT or until a RETURN drops below `stopDepth`; the latter
  // returns the value produced by the frame that crossed the boundary.
  Value runFrames(std::size_t stopDepth);

  // Pushes the frame for a bytecode function, copying its arguments into the
  // new window. Throws on arity, depth or a function without a proto.
  void pushOskelFrame(FuncPtr function, std::vector<Value> args, std::size_t resultReg, int line,
                     int col);

  Value callNative(Function& fn, std::vector<Value> args, int line, int col);

  // Resolves every `extern "C"` symbol in `proto` and its children and binds
  // each to a marshalling native, before the proto runs. A symbol the platform
  // cannot resolve is a clear runtime error, so a program never calls a wild
  // address. Idempotent: a REPL entry that re-runs a proto re-binds the same
  // globals.
  void installExterns(const Bytecode& proto);
  void installExtern(const ExternSymbol& symbol);

  const Value& constantAt(const Bytecode& bytecode, int index) const;
  const std::string& nameAt(const Bytecode& bytecode, int index) const;
};
