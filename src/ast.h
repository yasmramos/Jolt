#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "lexer.h"
#include "value.h"

struct Expr;
struct Stmt;

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

// ---------------------------------------------------------------- expressions

struct LiteralExpr {
  Value value;
};

// One piece of an interpolated string: either a literal chunk or an embedded
// expression. The variant makes the two cases mutually exclusive by design,
// so there is no "expr == nullptr means use text" convention to remember.
struct InterpPart {
  std::variant<std::string, ExprPtr> content;
  int line = 1;
  int col = 1;

  bool isText() const { return std::holds_alternative<std::string>(content); }
  const std::string& text() const { return std::get<std::string>(content); }
  const ExprPtr& expr() const { return std::get<ExprPtr>(content); }
};

// "Hello ${name}!"
struct InterpExpr {
  std::vector<InterpPart> parts;
};

struct VariableExpr {
  std::string name;
};

struct ArrayExpr {
  std::vector<ExprPtr> elements;
};

struct MapEntry {
  std::string key;
  ExprPtr value;
};

struct MapExpr {
  std::vector<MapEntry> entries;
};

struct UnaryExpr {
  Tok op = Tok::Nil;
  ExprPtr operand;
};

struct BinaryExpr {
  ExprPtr lhs;
  Tok op = Tok::Nil;
  ExprPtr rhs;
  int opLine = 1;
  int opCol = 1;
};

struct AssignExpr {
  // Invariant (enforced by the parser via isAssignable()): `target` is always
  // a VariableExpr, IndexExpr or MemberExpr. Any other shape is rejected at
  // parse time, so evaluators never need to re-check it at runtime.
  ExprPtr target;
  Tok op = Tok::Eq;
  ExprPtr value;
  // Position of the assignment operator itself, which usually differs from
  // Expr::line/col of the enclosing node; kept intentionally for precise
  // diagnostics (same convention as BinaryExpr::opLine/opCol).
  int opLine = 1;
  int opCol = 1;
};

struct TernaryExpr {
  ExprPtr condition;
  ExprPtr thenExpr;
  ExprPtr elseExpr;
};

struct CallExpr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
};

struct IndexExpr {
  ExprPtr object;
  ExprPtr index;
};

struct MemberExpr {
  ExprPtr object;
  std::string name;
};

struct RangeExpr {
  ExprPtr start;
  ExprPtr end;
  bool inclusive = false;
};

struct LambdaExpr {
  // Convention: `body` is always a BlockStmt. An expression-bodied lambda
  // (fn x => x * 2) is desugared by the parser into a BlockStmt containing a
  // single ReturnStmt, so evaluators only ever see statement bodies and an
  // implicit "no return" yields nil. `name` may be empty for anonymous
  // lambdas; it is only used for diagnostics and function values.
  std::string name;
  std::vector<std::string> params;
  StmtPtr body;
};

using ExprNode =
    std::variant<LiteralExpr, InterpExpr, VariableExpr, ArrayExpr, MapExpr, UnaryExpr,
                 BinaryExpr, AssignExpr, TernaryExpr, CallExpr, IndexExpr, MemberExpr,
                 RangeExpr, LambdaExpr>;

struct Expr {
  // Position convention: every Expr/Stmt carries the line/col of the token
  // that starts (or, for operator nodes, produces) the node. Leaf nodes such
  // as LiteralExpr or BreakStmt deliberately have no extra position fields;
  // operator positions live as opLine/opCol on BinaryExpr and AssignExpr only.
  ExprNode node;
  int line = 1;
  int col = 1;
};

// ----------------------------------------------------------------- statements

struct ExprStmt {
  ExprPtr expr;
};

struct LetStmt {
  std::string name;
  ExprPtr init;  // may be null
  bool isConst = false;
};

struct FnDeclStmt {
  // `body` follows the same convention as LambdaExpr::body: always a
  // BlockStmt. A function whose body has no ReturnStmt (or a bare `return;`)
  // implicitly evaluates to nil; the interpreter handles that, so an explicit
  // returnsValue flag would be redundant here.
  std::string name;
  std::vector<std::string> params;
  StmtPtr body;  // BlockStmt
};

struct BlockStmt {
  std::vector<StmtPtr> stmts;
};

struct IfStmt {
  ExprPtr condition;
  StmtPtr thenBranch;
  StmtPtr elseBranch;  // may be null
};

struct WhileStmt {
  ExprPtr condition;
  StmtPtr body;
};

struct ForStmt {
  StmtPtr init;  // may be null
  ExprPtr condition;
  ExprPtr step;
  StmtPtr body;
};

struct ForInStmt {
  std::string var;
  ExprPtr iterable;
  StmtPtr body;
};

struct BreakStmt {};

struct ContinueStmt {};

struct ReturnStmt {
  ExprPtr value;  // may be null
};

using StmtNode = std::variant<ExprStmt, LetStmt, FnDeclStmt, BlockStmt, IfStmt, WhileStmt,
                              ForStmt, ForInStmt, BreakStmt, ContinueStmt, ReturnStmt>;

struct Stmt {
  StmtNode node;
  int line = 1;
  int col = 1;
};

template <class T>
ExprPtr makeExpr(T node, int line, int col) {
  auto expr = std::make_shared<Expr>();
  expr->node = std::move(node);
  expr->line = line;
  expr->col = col;
  return expr;
}

template <class T>
StmtPtr makeStmt(T node, int line, int col) {
  auto stmt = std::make_shared<Stmt>();
  stmt->node = std::move(node);
  stmt->line = line;
  stmt->col = col;
  return stmt;
}
