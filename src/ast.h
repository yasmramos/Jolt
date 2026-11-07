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

// ------------------------------------------------------------- type annotations

// An optional type annotation (`int`, `string`, ...). An empty `name` means
// "no annotation", so the same struct models both "typed" and "untyped"
// without a separate flag. `nullable` is the trailing `?` of `string?`
// (PHASE 6.5 nullability).
//
// Types are a compile-time-only concept: they are validated and checked by the
// parser and compiler, then erased. They never reach the bytecode, so the
// register VM and the AOT backend see exactly the same instructions with or
// without an annotation (AGENT.md §3: the bytecode must stay low level).
struct TypeRef {
  std::string name;
  bool nullable = false;

  bool isPresent() const { return !name.empty(); }
};

// One function parameter: a name plus an optional type annotation
// (`fn f(a: int, b) {}`).
struct Param {
  std::string name;
  TypeRef type;
  // `rest...` (PHASE 6.5): a variadic parameter. It must be the last one, and
  // its `type` describes the *elements*, not the array; at run time `name`
  // binds an array holding every extra argument. A function has at most one.
  bool variadic = false;
};

// The primitive type names accepted in an annotation. `any` opts out of
// checking. `float` also accepts an int (numeric widening). These names match
// `Value::typeName()` so a runtime value and its annotation speak the same
// vocabulary.
inline bool isKnownTypeName(const std::string& name) {
  return name == "nil" || name == "bool" || name == "int" || name == "float" ||
         name == "double" || name == "string" || name == "error" || name == "array" ||
         name == "map" || name == "function" || name == "range" || name == "any";
}

// Human-readable form of a type for diagnostics: `int`, `string?`, `any`.
inline std::string describeType(const TypeRef& type) {
  if (!type.isPresent()) return "any";
  return type.nullable ? type.name + "?" : type.name;
}

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

// One field of a `struct` declaration: a name plus an optional type annotation
// (`struct Point { x: int; y: int; }`).
struct StructField {
  std::string name;
  TypeRef type;
};

// One entry of a struct literal (`Point { x: 1, y: 2 }`). The field name is
// always an identifier, so no computed key is possible.
struct StructLitEntry {
  std::string name;
  ExprPtr value;
};

// `Point { x: 1, y: 2 }` (PHASE 6.5). A struct value is a map at runtime, so
// the literal compiles to the same NEWTABLE/SETTABLE sequence as a map
// literal; `typeName` is kept so the compiler can check the field names and
// types against the declaration.
struct StructLitExpr {
  std::string typeName;
  std::vector<StructLitEntry> entries;
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

// `obj[start:end]` (slicing syntax): a half-open copy `[start, end)`. Either
// bound may be omitted (`obj[:]`, `obj[1:]`, `obj[:3]`), which is represented
// as a null `start`/`end`; a negative bound counts from the end and is clamped
// to the valid range, exactly like the `slice`/`substr` methods. It is
// read-only (not an assignment target) and compiles to one `$slice` helper
// call, so it adds no opcode.
struct SliceExpr {
  ExprPtr object;
  ExprPtr start;
  ExprPtr end;
};

struct MemberExpr {
  ExprPtr object;
  std::string name;
  // `a?.b` (safe navigation, PHASE 6.5): when `object` is nil the whole
  // expression yields nil instead of raising "has no member". Plain `a.b`
  // leaves this false.
  bool safe = false;
};

// `a!!` (PHASE 6.5): asserts the operand is not nil and yields it unchanged.
// A nil operand raises a runtime error at the position of the operator.
struct NonNullExpr {
  ExprPtr operand;
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

// `try expr` (PHASE 6.7.3): when the operand evaluates to an error value, the
// enclosing function returns it immediately; otherwise the operand is yielded
// unchanged. It is the propagation half of the error model (Rust's `?`), the
// postfix `catch` is the recovery half. Both compile to `CALL $isError` plus a
// conditional jump and a `RETURN`/`MOVE`, so neither adds an opcode.
struct TryExpr {
  ExprPtr operand;
};

// `expr catch default` (PHASE 6.7.3): when the left side is an error value the
// whole expression evaluates to `fallback`; otherwise it is the left value.
// Left-associative and lower precedence than `??`, so `a() catch b() catch c`
// recovers at the first failure.
struct CatchExpr {
  ExprPtr operand;
  ExprPtr fallback;
};

struct LambdaExpr {
  // Always anonymous: `x => x * 2`, `(a, b) => a + b`, `() => { ... }`.
  // Convention: `body` is always a BlockStmt. An expression-bodied lambda is
  // desugared by the parser into a BlockStmt containing a single ReturnStmt,
  // so evaluators only ever see statement bodies and an implicit "no return"
  // yields nil. Named functions are declared with FnDeclStmt (`fn name() {}`).
  std::vector<Param> params;
  StmtPtr body;
};

using ExprNode =
    std::variant<LiteralExpr, InterpExpr, VariableExpr, ArrayExpr, MapExpr, StructLitExpr,
                 UnaryExpr, BinaryExpr, AssignExpr, TernaryExpr, CallExpr, IndexExpr, SliceExpr,
                 MemberExpr, NonNullExpr, RangeExpr, TryExpr, CatchExpr, LambdaExpr>;

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
  TypeRef type;               // optional annotation, e.g. `let x: int = 5`
  ExprPtr init;               // may be null only when kind == Tok::Var
  Tok kind = Tok::Var;        // Var | Let | Const
};

struct FnDeclStmt {
  // `body` follows the same convention as LambdaExpr::body: always a
  // BlockStmt. A function whose body has no ReturnStmt (or a bare `return;`)
  // implicitly evaluates to nil; the interpreter handles that, so an explicit
  // returnsValue flag would be redundant here.
  std::string name;
  std::vector<Param> params;
  TypeRef returnType;  // optional, e.g. `fn f(): int`
  StmtPtr body;  // BlockStmt; null for a `native fn` (no body)
  // `native fn name(params): ret;` (PHASE 6.5): a bodyless declaration that
  // binds `name` to a native function the host registered (see
  // `registerBuiltins`). The parser rejects a body on a native declaration, so
  // `body` is null exactly when this is true.
  bool isNative = false;
};

// `struct Point { x: int; y: int; }` (PHASE 6.5). A data-only type: no
// methods, no inheritance. It is compile-time metadata that tells the compiler
// the field names and their optional types; a `Point { ... }` literal then
// builds a plain map (see StructLitExpr), so the declaration itself emits no
// bytecode and adds no opcode.
struct StructDeclStmt {
  std::string name;
  std::vector<StructField> fields;
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

// `defer { ... }`: registers a block to run when the enclosing scope exits,
// whether it exits normally (falling off the end), through `return`, or
// through `break`/`continue` that leave the scope. Deferred blocks run in
// LIFO order (the last one declared runs first), like Go's `defer`.
//
// The body is always a BlockStmt, so it creates its own scope and its own
// nested defers. A `defer` may only appear inside a function body: at the
// program top level there is no scope exit to attach it to.
struct DeferStmt {
  StmtPtr body;  // BlockStmt
};

// `import std.math;` / `import std.math.*;` / `import std.math as m;` /
// `import { sqrt, pow } from std.math;`.
//
// The declaration always names a module with its dotted path (`std.math`).
// `mode` selects how its public names enter the current scope:
//   Qualified -> bound under the path (`std.math.sqrt`) or under `alias`;
//   Wildcard  -> every public name enters the scope unqualified (`sqrt`);
//   Selective -> only `names` enter the scope unqualified (`sqrt`, `pow`).
struct ImportStmt {
  enum class Mode { Qualified, Wildcard, Selective };

  std::string module;        // dotted path, e.g. "std.math"
  Mode mode = Mode::Qualified;
  std::string alias;         // Qualified only: bound under this name instead
  std::vector<std::string> names;  // Selective only: the selected names
};

// One `case` of a `switch`. `values` holds the case labels, each a literal
// (`case 1:`) or a range (`case 10..=20:`). A case with several labels
// (`case 2: case 3:`) matches if any of them does. There is no fallthrough:
// the body of the matching case is the only one that runs.
struct SwitchCase {
  std::vector<ExprPtr> values;
  StmtPtr body;  // BlockStmt
};

// `switch (subject) { case 1: { ... } case 2: case 3: { ... } default: { ... } }`.
// The subject is evaluated exactly once and compared against each case label
// (equality for literals, membership for ranges). A null `defaultBody` means
// there is no `default` clause; an empty block is a distinct, present clause.
struct SwitchStmt {
  ExprPtr subject;
  std::vector<SwitchCase> cases;
  StmtPtr defaultBody;  // may be null
};

using StmtNode = std::variant<ExprStmt, LetStmt, FnDeclStmt, StructDeclStmt, BlockStmt, IfStmt,
                              WhileStmt, ForStmt, ForInStmt, BreakStmt, ContinueStmt, ReturnStmt,
                              ImportStmt, SwitchStmt, DeferStmt>;

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
