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

struct InterpPart {
  std::string text;  // used when expr == nullptr
  ExprPtr expr;
  int line = 1;
  int col = 1;
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
  ExprPtr target;
  Tok op = Tok::Eq;
  ExprPtr value;
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
  std::string name;
  std::vector<std::string> params;
  StmtPtr body;
};

using ExprNode =
    std::variant<LiteralExpr, InterpExpr, VariableExpr, ArrayExpr, MapExpr, UnaryExpr,
                 BinaryExpr, AssignExpr, TernaryExpr, CallExpr, IndexExpr, MemberExpr,
                 RangeExpr, LambdaExpr>;

struct Expr {
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
