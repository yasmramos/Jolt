#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "runtime.h"
#include "value.h"

struct Binding {
  Value value;
  bool isImmutable = false;
};

struct Env : std::enable_shared_from_this<Env> {
  // The link to the enclosing scope is strong: a closure that captures a
  // scope keeps its whole lexical chain alive, so free variables stay
  // resolvable after the scope that created the closure has returned.
  //
  // Reference counting alone cannot free the cycles this allows (a function
  // stored in the very scope it closes over); those are reclaimed by the
  // cycle collector in gc.h.
  std::shared_ptr<Env> parent;
  std::unordered_map<std::string, Binding> vars;

  explicit Env(std::shared_ptr<Env> parent = nullptr) : parent(std::move(parent)) {}

  // Walks up the scope chain looking for `name`; nullptr when it is not visible.
  Binding* find(const std::string& name);
  void define(const std::string& name, Value value, bool isImmutable = false);
  enum class Assign { Ok, NotFound, Immutable };
  Assign assign(const std::string& name, Value value);

  // Removes every binding (used when resetting the interpreter between runs).
  void clear() { vars.clear(); }
};

struct BreakSignal {
  int line = 0;
  int col = 0;
};
struct ContinueSignal {
  int line = 0;
  int col = 0;
};
struct ReturnSignal {
  Value value;
};

class Interpreter : public Runtime {
 public:
  explicit Interpreter(std::string sourceName = "<script>");

  void run(const StmtPtr& program);
  void runStatement(const Stmt& stmt);
  void exec(const Stmt& stmt);
  Value eval(const Expr& expr);
  Value evalRepl(const ExprPtr& expr);

  Value call(const Value& callee, std::vector<Value> args, int line, int col) override;

  void defineGlobal(const std::string& name, Value value, bool isImmutable = false) override;

  [[noreturn]] void fail(const std::string& message, int line = 0, int col = 0) const override;
  [[noreturn]] void failHere(const std::string& message) const;

  // Helpers shared with the builtin library.
  Value indexGet(const Value& object, const Value& index, int line, int col) override;
  void indexSet(const Value& object, const Value& index, Value value, int line, int col) override;
  Value memberGet(const Value& object, const std::string& name, int line, int col) override;
  void memberSet(const Value& object, const std::string& name, Value value, int line, int col) override;
  Value binary(Tok op, const Value& left, const Value& right, int line, int col) override;
  bool lessThan(const Value& left, const Value& right, int line, int col) override;

  std::shared_ptr<Env> globals;
  std::shared_ptr<Env> env;

 private:
  void execBlock(const Stmt& block, const std::shared_ptr<Env>& target, bool newScope);
  Value unary(Tok op, const Value& operand, int line, int col);
  Value assignTo(const Expr& target, Value value);
  FuncPtr makeFunction(const std::string& name, const std::vector<std::string>& params,
                       const StmtPtr& body, std::shared_ptr<Env> closure);
};
