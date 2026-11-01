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
  // Scalar map keys are stored directly in `literalKey` (the common case:
  // identifiers, string literals and numeric literals). Any other key shape
  // (variables, expressions) is stored in `keyExpr` and evaluated at runtime;
  // its Value is then normalized to its canonical toString() form, because
  // the runtime Map is a string-keyed container. Exactly one of the two is
  // set: keyExpr == nullptr <=> literalKey holds the key.
  std::string literalKey;
  ExprPtr keyExpr;  // may be null
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
  // A range is a lazy Value (Range{start, end, inclusive}); it never
  // materializes its elements. Iteration cost depends on the consumer:
  // `for (i in 0..10)` steps through the bounds without allocating an array
  // (see ForInStmt in the interpreter); builtins that must realize a sequence
  // (map/filter/reduce/...) expand it eagerly and are documented as such.
  ExprPtr start;
  ExprPtr end;
  bool inclusive = false;
};

struct LambdaExpr {
  // Always anonymous: `x => x * 2`, `(a, b) => a + b`, `() => { ... }`.
  // Convention: `body` is always a BlockStmt. An expression-bodied lambda is
  // desugared by the parser into a BlockStmt containing a single ReturnStmt,
  // so evaluators only ever see statement bodies and an implicit "no return"
  // yields nil. Named functions are declared with FnDeclStmt (`fn name() {}`).
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
  // Declaration keyword semantics, modeled as a single enum so no invalid
  // state (e.g. "mutable and constant at once") can be represented:
  //   Var   -> mutable binding; the initializer is optional (binds nil).
  //   Let   -> immutable binding evaluated at runtime; requires an initializer.
  //   Const -> immutable binding whose value is known at compile time;
  //            requires an initializer with a constant expression
  //            (literals, or references to previously declared consts).
  // The parser enforces every rule above, so the interpreter/compiler only
  // need to honor `kind != Tok::Var` for immutability.
  std::string name;
  ExprPtr init;               // may be null only when kind == Tok::Var
  Tok kind = Tok::Var;        // Var | Let | Const
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
  // Classic C-style loop, a distinct node from ForInStmt on purpose: the
  // three optional parts are nullable by design -- `init` may be null (or any
  // statement, typically LetStmt), and a null `condition`/`step` means
  // "always true" / "no step" (e.g. `for (;;)`, while-style loops).
  StmtPtr init;  // may be null
  ExprPtr condition;  // may be null: loop runs until break/return
  ExprPtr step;       // may be null
  StmtPtr body;
};

struct ForInStmt {
  // Iterates arrays, strings (by UTF-8 char), maps (by key) and ranges.
  // Ranges are consumed lazily from their bounds instead of being expanded
  // into a temporary vector, so `for i in 0..1_000_000 {}` stays O(1) memory.
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
