#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode.h"
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
  VM() = default;

  // Runs the bytecode from the first instruction until HALT or the end.
  // Throws `RuntimeError` if the bytecode attempts an invalid operation.
  void run(const Bytecode& bytecode);

  // Returns the value of a global, or nullptr if it does not exist.
  const Value* findGlobal(const std::string& name) const;

  // Number of registers reserved by the last execution.
  int registerCount() const { return static_cast<int>(registers_.size()); }

  // Number of local cells reserved by the last execution.
  int localCount() const { return static_cast<int>(locals_.size()); }

  Value call(const Value& callee, std::vector<Value> args, int line, int col) override;
  void defineGlobal(const std::string& name, Value value, bool isImmutable = false) override;

  [[noreturn]] void fail(const std::string& message, int line = 0, int col = 0) const override;

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
    std::size_t resultReg = kNoResult;
    std::size_t regBase = 0;
    std::size_t localBase = 0;
  };

  // Registers of every live frame, one window per frame, in call order.
  std::vector<Value> registers_;
  std::vector<CallFrame> frames_;
  std::vector<CellPtr> locals_;
  std::unordered_map<std::string, Value> globals_;

  Value& getReg(int index);
  void setReg(int index, Value value);
  CellPtr& localAt(int index);

  // Runs until HALT or until a RETURN drops below `stopDepth`; the latter
  // returns the value produced by the frame that crossed the boundary.
  Value runFrames(std::size_t stopDepth);

  // Pushes the frame for a bytecode function, copying its arguments into the
  // new window. Throws on arity, depth or a function without a proto.
  void pushJoltFrame(FuncPtr function, std::vector<Value> args, std::size_t resultReg, int line,
                     int col);

  Value callNative(Function& fn, std::vector<Value> args, int line, int col);

  const Value& constantAt(const Bytecode& bytecode, int index) const;
  const std::string& nameAt(const Bytecode& bytecode, int index) const;
};
