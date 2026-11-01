#pragma once

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
  std::vector<Value> registers_;
  std::vector<CellPtr> locals_;
  std::unordered_map<std::string, Value> globals_;

  Value& getReg(int index);
  void setReg(int index, Value value);
  CellPtr& localAt(int index);

  const Value& numberAt(const Bytecode& bytecode, int index) const;
  const std::string& stringAt(const Bytecode& bytecode, int index) const;
};
