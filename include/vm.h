#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "bytecode.h"
#include "value.h"

// Register-based virtual machine (PHASE 3-5).
//
// Executes the bytecode produced by the compiler. All registers and global
// variables are `Value`; there are no per-type arrays. Arithmetic has fast
// paths for integers and floats.
//
// Globals persist across `run()` calls (useful for a REPL); registers and
// locals are reset at the start of every execution.
class VM {
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

 private:
  std::vector<Value> registers_;
  std::vector<CellPtr> locals_;
  std::unordered_map<std::string, Value> globals_;

  Value& getReg(int index);
  void setReg(int index, Value value);
  CellPtr& localAt(int index);
};
