#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "errors.h"

// AST to bytecode compiler (PHASE 2-5).
//
// Translates the language into the register-based instructions of
// `include/bytecode.h`. When it meets a node that does not belong to the
// current phase yet, it throws `CompileError` with line/column, so the
// pipeline can report it in the same format as the parser.
//
// Current scope:
//   - nil, boolean, numeric and string literals
//   - global variables and block-local variables
//   - simple and compound assignment
//   - arithmetic (+ - * / % **), comparisons, unary operators and bitwise ops
//   - if/else, while, classic for, break and continue
//   - short-circuit operators && and ||, and the ternary operator
//   - print(expr) with a single argument
//   - top-level `fn` declarations, parameters, `return` and general calls
//
// PHASE 6b: upvalues, lambdas and local function declarations.
// PHASE 6c-6d: collections, `in` and for-in.
class Compiler {
 public:
  Compiler() = default;

  // Compiles a whole program. The resulting bytecode ends with HALT.
  Bytecode compile(const StmtPtr& program);

 private:
  struct LocalVar {
    int slot = 0;
    bool isImmutable = false;
  };

  struct Scope {
    std::unordered_map<std::string, LocalVar> vars;
    int baseSlot = 0;
  };

  struct Loop {
    std::vector<int> breaks;
    std::vector<int> continues;
  };

  Bytecode bc_;
  int nextReg_ = 0;  // next free temporary register
  int maxReg_ = 0;   // maximum number of registers in use at once
  int nextLocal_ = 0;
  int maxLocal_ = 0;

  std::vector<Scope> scopes_;
  std::vector<Loop> loops_;
  std::unordered_set<std::string> globalConsts_;

  // True while compiling a function body. It keeps block locals local even
  // when the body's own scope is the innermost one.
  bool inFunction_ = false;

  // Position of the node being compiled; stamped onto every emitted
  // instruction so the VM can report runtime errors with line/column.
  int currentLine_ = 1;
  int currentCol_ = 1;

  int allocReg();
  void freeReg(int count = 1);

  int addConstant(const Value& value);
  int addString(const std::string& value);

  int emit(OpCode op, int a = 0, int b = 0, int c = 0);
  int emitJump(OpCode op, int a = 0);
  void patchJump(int index, int target);
  int currentOffset() const;

  // Scopes and variables.
  bool inGlobalScope() const { return !inFunction_ && scopes_.size() <= 1; }
  void pushScope();
  void popScope();
  int declareLocal(const std::string& name, bool isImmutable);
  bool resolveLocal(const std::string& name, LocalVar& out) const;
  void declareGlobal(const std::string& name, bool isImmutable);

  int compileExpr(const Expr& expr);
  int compileVariable(const std::string& name);
  int compileAssign(const AssignExpr& assign, int line, int col);
  int compileTernary(const TernaryExpr& expr);
  int compileCall(const CallExpr& call, int line, int col);

  void compileStmt(const Stmt& stmt);
  void compileBlockBody(const BlockStmt& block);
  void compileScopedBlock(const Stmt& stmt);
  void compileLet(const LetStmt& stmt);
  void compileFnDecl(const FnDeclStmt& fn, int line, int col);
  void compileReturn(const ReturnStmt& stmt);
  void compileIf(const IfStmt& stmt);
  void compileWhile(const WhileStmt& stmt);
  void compileFor(const ForStmt& stmt);
  void compileBreak(const Stmt& stmt);
  void compileContinue(const Stmt& stmt);
  void compilePrint(const Expr& expr);
  bool isPrintCall(const Expr& expr) const;

  // Compiles `body` as a child proto of the chunk being built and returns its
  // index in `bc_.children`. The parent compiler state is saved and restored.
  int compileFunctionProto(const std::string& name, const std::vector<std::string>& params,
                           const StmtPtr& body, int line, int col);
};

// Shortcut equivalent to `Compiler{}.compile(program)`.
Bytecode compileProgram(const StmtPtr& program);
