#pragma once

#include <string>
#include <utility>
#include <vector>

#include "errors.h"
#include "lexer.h"
#include "value.h"

// Engine interface shared by the AST interpreter (PHASE 2) and the bytecode
// VM (PHASE 3-6).
//
// Everything the standard library needs from the engine goes through this
// class, so a builtin such as `map`, `filter` or `sort` can invoke a Jolt
// function without knowing whether the caller is the interpreter or the VM.
// That is what lets PHASE 6 run the stdlib against VM closures instead of
// re-entering the AST.
//
// The per-type semantics (indexing, members, operators) live in `ops.h`; this
// class only declares the hooks that need engine state (globals, call stack,
// error reporting).
class Runtime {
 public:
  virtual ~Runtime() = default;

  // Calls `callee` with `args`, reporting errors at `line`/`col`.
  virtual Value call(const Value& callee, std::vector<Value> args, int line, int col) = 0;

  virtual void defineGlobal(const std::string& name, Value value, bool isImmutable = false) = 0;

  // Throws a RuntimeError, defaulting the position to the current expression
  // when `line`/`col` are 0.
  [[noreturn]] virtual void fail(const std::string& message, int line = 0, int col = 0) const = 0;

  virtual Value indexGet(const Value& object, const Value& index, int line, int col) = 0;
  virtual void indexSet(const Value& object, const Value& index, Value value, int line, int col) = 0;
  virtual Value memberGet(const Value& object, const std::string& name, int line, int col) = 0;
  virtual void memberSet(const Value& object, const std::string& name, Value value, int line, int col) = 0;
  virtual Value binary(Tok op, const Value& left, const Value& right, int line, int col) = 0;
  virtual bool lessThan(const Value& left, const Value& right, int line, int col) = 0;

  void addNative(const std::string& name, NativeFn fn) {
    defineGlobal(name, makeNative(name, std::move(fn)));
  }

  std::string sourceName;
  std::vector<std::string> scriptArgs;
  std::vector<Frame> stack;
  int maxDepth = 2000;
  int depth = 0;
  int currentLine = 1;
  int currentCol = 1;

 protected:
  Runtime() = default;
};
