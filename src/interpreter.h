#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "value.h"

struct Binding {
  Value value;
  bool isConst = false;
};

struct Env {
  std::shared_ptr<Env> parent;
  std::unordered_map<std::string, Binding> vars;

  explicit Env(std::shared_ptr<Env> parent = nullptr) : parent(std::move(parent)) {}

  Binding* find(const std::string& name);
  void define(const std::string& name, Value value, bool isConst = false);
  enum class Assign { Ok, NotFound, Const };
  Assign assign(const std::string& name, Value value);
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

class Interpreter {
 public:
  explicit Interpreter(std::string sourceName = "<script>");

  void run(const StmtPtr& program);
  void runStatement(const Stmt& stmt);
  void exec(const Stmt& stmt);
  Value eval(const Expr& expr);
  Value evalRepl(const ExprPtr& expr);

  Value call(const Value& callee, std::vector<Value> args, int line, int col);

  void defineGlobal(const std::string& name, Value value, bool isConst = false);
  void addNative(const std::string& name, NativeFn fn);

  [[noreturn]] void fail(const std::string& message, int line = 0, int col = 0) const;
  [[noreturn]] void failHere(const std::string& message) const;

  // Helpers shared with the builtin library.
  Value indexGet(const Value& object, const Value& index, int line, int col);
  void indexSet(const Value& object, const Value& index, Value value, int line, int col);
  Value memberGet(const Value& object, const std::string& name, int line, int col);
  void memberSet(const Value& object, const std::string& name, Value value, int line, int col);
  Value binary(Tok op, const Value& left, const Value& right, int line, int col);
  bool lessThan(const Value& left, const Value& right, int line, int col);

  std::string sourceName;
  std::vector<std::string> scriptArgs;
  std::shared_ptr<Env> globals;
  std::shared_ptr<Env> env;
  std::vector<Frame> stack;
  int maxDepth = 2000;
  int depth = 0;
  int currentLine = 1;
  int currentCol = 1;

 private:
  void execBlock(const Stmt& block, const std::shared_ptr<Env>& target, bool newScope);
  Value unary(Tok op, const Value& operand, int line, int col);
  Value assignTo(const Expr& target, Value value);
  FuncPtr makeFunction(const std::string& name, const std::vector<std::string>& params,
                       const StmtPtr& body, std::shared_ptr<Env> closure);
};
