#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "env.h"
#include "errors.h"
#include "runtime.h"
#include "value.h"

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
  ~Interpreter();

  // An interpreter owns its global scope. Moving transfers it (the REPL's
  // `:clear` assigns a fresh interpreter), and copying is not allowed: two
  // interpreters sharing one scope would clear each other's definitions.
  Interpreter(Interpreter&&) = default;
  Interpreter& operator=(Interpreter&&) = default;

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
