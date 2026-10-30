#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "errors.h"

// Compilador de AST a bytecode (FASE 2-5).
//
// Traduce el lenguaje a las instrucciones register-based de
// `include/bytecode.h`. Cuando encuentra un nodo que todavia no pertenece a
// esta fase lanza `CompileError` con linea/columna, de modo que el pipeline
// pueda reportarlo con el mismo formato que el parser.
//
// Alcance actual:
//   - literales nil, booleanos, numericos y de cadena
//   - variables globales y locales de bloque
//   - asignacion simple y compuesta
//   - aritmetica (+ - * / % **), comparaciones, unarios y bits
//   - if/else, while, for clasico, break y continue
//   - operadores cortocircuitados && y ||, y ternario
//   - print(expr) con un argumento
//
// FASE 6: funciones, closures, llamadas, colecciones y for-in.
class Compiler {
 public:
  Compiler() = default;

  // Compila un programa completo. El bytecode resultante termina en HALT.
  Bytecode compile(const StmtPtr& program);

 private:
  struct LocalVar {
    int slot = 0;
    bool isConst = false;
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
  int next_reg_ = 0;  // siguiente registro temporal libre
  int max_reg_ = 0;   // maximo numero de registros usados a la vez
  int next_local_ = 0;
  int max_local_ = 0;

  std::vector<Scope> scopes_;
  std::vector<Loop> loops_;
  std::unordered_set<std::string> globalConsts_;

  int allocReg();
  void freeReg(int count = 1);

  int addConstant(const Value& value);
  int addString(const std::string& value);

  int emit(OpCode op, int a = 0, int b = 0, int c = 0);
  int emitJump(OpCode op, int a = 0);
  void patchJump(int index, int target);
  int currentOffset() const;

  // Scopes y variables.
  bool inGlobalScope() const { return scopes_.size() <= 1; }
  void pushScope();
  void popScope();
  int declareLocal(const std::string& name, bool isConst);
  bool resolveLocal(const std::string& name, LocalVar& out) const;
  void declareGlobal(const std::string& name, bool isConst);

  int compileExpr(const Expr& expr);
  int compileVariable(const std::string& name);
  int compileAssign(const AssignExpr& assign, int line, int col);
  int compileTernary(const TernaryExpr& expr);

  void compileStmt(const Stmt& stmt);
  void compileBlockBody(const BlockStmt& block);
  void compileScopedBlock(const Stmt& stmt);
  void compileLet(const LetStmt& stmt);
  void compileIf(const IfStmt& stmt);
  void compileWhile(const WhileStmt& stmt);
  void compileFor(const ForStmt& stmt);
  void compileBreak(const Stmt& stmt);
  void compileContinue(const Stmt& stmt);
  void compilePrint(const Expr& expr);
  bool isPrintCall(const Expr& expr) const;
};

// Atajo equivalente a `Compiler{}.compile(program)`.
Bytecode compileProgram(const StmtPtr& program);
