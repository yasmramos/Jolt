#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
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

  // Union members (PHASE 6.7.4): the alternatives after the primary `name` in
  // `T | E | ...`. An empty vector means "not a union". `nil` is not stored
  // here; it folds into `nullable`, so `int | string | nil` is the union
  // `int | string` with `nullable = true`. Like every type, unions are erased
  // before the bytecode (AGENT.md §3).
  std::vector<std::string> alternatives;

  // Callback signature (PHASE 6.6): when `name == "fn"`, the parameter and
  // return type names of a C function pointer. Only scalar FFI types are
  // allowed (no nested callback, no struct by value), so the signature is
  // stored as plain names instead of a recursive `TypeRef`.
  std::vector<std::string> callbackParams = {};
  std::string callbackReturn = "";  // "void" for a callback that returns nothing

  bool isPresent() const { return !name.empty(); }
  bool isCallback() const { return name == "fn"; }
  bool isUnion() const { return !alternatives.empty(); }

  // Every member of the union, primary first. A non-union yields just `name`.
  std::vector<std::string> members() const {
    std::vector<std::string> all;
    all.reserve(1 + alternatives.size());
    all.push_back(name);
    all.insert(all.end(), alternatives.begin(), alternatives.end());
    return all;
  }
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
// checking. `float` also accepts an int (numeric widening). `void` is a
// return-only type (see `parseTypeAnnotation`). These names match
// `Value::typeName()` so a runtime value and its annotation speak the same
// vocabulary.
inline bool isKnownTypeName(const std::string& name) {
  return name == "nil" || name == "bool" || name == "int" || name == "float" ||
         name == "double" || name == "string" || name == "error" || name == "array" ||
         name == "map" || name == "function" || name == "range" || name == "void" ||
         name == "any" ||
         // FFI scalar vocabulary (PHASE 6.6, compile-time only): pointers and
         // fixed-width/unsigned integers, so a C signature can be spelled the
         // way C spells it (`native fn strlen(s: cstring): size_t;`). They are
         // erased before the bytecode; the runtime representation stays `int`
         // (pointers are opaque handles). The width/signedness is honoured by
         // the FFI marshalling, not by the VM.
         name == "ptr" || name == "cstring" || name == "int8" || name == "int16" ||
         name == "int32" || name == "int64" || name == "uint8" || name == "uint16" ||
         name == "uint32" || name == "uint64" || name == "uint" || name == "size_t";
}

// Pointer-like types: an opaque handle (`ptr` == C `void*`) or a NUL-terminated
// byte string (`cstring` == C `const char*`). `nil` is their NULL.
inline bool isPointerTypeName(const std::string& name) {
  return name == "ptr" || name == "cstring";
}

// Every integer spelling shares one numeric family at compile time: `int`
// (64-bit signed) plus the FFI widths. The runtime holds an `int64`, so they
// are mutually assignable; the annotation only records the width/signedness
// the FFI layer will use when it marshals the value.
inline bool isIntegerTypeName(const std::string& name) {
  return name == "int" || name == "int8" || name == "int16" || name == "int32" ||
         name == "int64" || name == "uint8" || name == "uint16" || name == "uint32" ||
         name == "uint64" || name == "uint" || name == "size_t";
}

// Human-readable form of a type for diagnostics: `int`, `string?`, `any`,
// `int | string` for a union, `(int | string)?` for a nullable union, and
// `fn(int, int): int` for a callback signature.
inline std::string describeType(const TypeRef& type) {
  if (!type.isPresent()) return "any";
  if (type.isCallback()) {
    std::string out = "fn(";
    for (std::size_t i = 0; i < type.callbackParams.size(); ++i) {
      if (i != 0) out += ", ";
      out += type.callbackParams[i];
    }
    out += "): ";
    out += type.callbackReturn.empty() ? "void" : type.callbackReturn;
    return out;
  }
  std::string base = type.name;
  if (type.isUnion()) {
    for (const std::string& alternative : type.alternatives) {
      base += " | " + alternative;
    }
    // Parenthesize before the nullable `?` so `int | string | nil` is not read
    // as `int | string?`.
    return type.nullable ? "(" + base + ")?" : base;
  }
  return type.nullable ? base + "?" : base;
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
// to the valid range, exactly like the `slice`/`substr` methods. An optional
// `step` (`obj[start:end:step]`) selects every `step`-th element and may be
// negative to walk backwards. It is read-only (not an assignment target) and
// compiles to one `$slice` helper call, so it adds no opcode.
struct SliceExpr {
  ExprPtr object;
  ExprPtr start;
  ExprPtr end;
  ExprPtr step;
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

  // Frees the tree iteratively instead of recursively, so a deep tree (a long
  // chain of binary operators, or nesting just under the parser's limit) does
  // not overflow the stack while being destroyed. See `teardownAst` below.
  ~Expr();
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
  // `@MustUse fn f() ...` (PHASE 6.7.5): the caller must do something with the
  // return value. Discarding a call to it is a compile-time warning, not an
  // error, so inference never rejects a program that would have run. `_ = f();`
  // acknowledges the discard.
  bool mustUse = false;
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

// `do { ... } while (cond);` (PHASE 6.5): a post-test loop. The body runs once
// before the condition is evaluated, so the body always runs at least once.
// It is a distinct node from WhileStmt so `continue` jumps to the condition
// (not the top) and the body's scope is entered before the first test.
struct DoWhileStmt {
  StmtPtr body;
  ExprPtr condition;
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
  // `valueVar` is empty for the single-variable form; when it is set
  // (`for k, v in map`) the first variable receives the key and the second the
  // matching value, so a map is walked as key/value pairs.
  std::string var;
  std::string valueVar;
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

// `extern "C" { fn name(params): ret; ... }` or `extern fn name(params): ret;`
// (PHASE 6.6). A declaration of C functions: the ABI string (`"C"` is the only
// one supported) plus the functions it declares. The single-line form is sugar
// for a one-entry block with an implicit `"C"` ABI, so both spellings share one
// representation. Each declaration is a bodyless `FnDeclStmt` with `isNative`
// set, so it follows exactly the `native fn` path: it emits no bytecode and only
// records a signature for call checking. The declaration is module-level
// metadata, like `import`, `struct` and `native fn`, and is erased before the
// bytecode.
//
// This is the *declaration* half of the FFI. Binding the symbol to a real C
// function (the marshalling and the call lowering without `libffi`) is a later
// slice; until then the name binds to the host function of the same name, just
// like `native fn`.
struct ExternBlockStmt {
  std::string abi;                 // the ABI string literal, e.g. "C"
  std::vector<FnDeclStmt> decls;   // each `fn ...;`, all `isNative`
};

using StmtNode = std::variant<ExprStmt, LetStmt, FnDeclStmt, StructDeclStmt, BlockStmt, IfStmt,
                              WhileStmt, DoWhileStmt, ForStmt, ForInStmt, BreakStmt, ContinueStmt,
                              ReturnStmt, ImportStmt, SwitchStmt, DeferStmt, ExternBlockStmt>;

struct Stmt {
  StmtNode node;
  int line = 1;
  int col = 1;

  // Iterative teardown, same rationale as `Expr::~Expr`.
  ~Stmt();
};

// Moves the child nodes of `expr`/`stmt` into the work lists, so the caller can
// free the whole subtree without recursing: the node itself stays alive until
// its own `ExprPtr`/`StmtPtr` is dropped, but it no longer owns its children
// once they are moved out. A null child is skipped.
inline void pushChild(std::vector<ExprPtr>& exprs, ExprPtr& child) {
  if (child) exprs.push_back(std::move(child));
}
inline void pushChild(std::vector<StmtPtr>& stmts, StmtPtr& child) {
  if (child) stmts.push_back(std::move(child));
}

inline void collectExprChildren(Expr& expr, std::vector<ExprPtr>& exprs,
                                std::vector<StmtPtr>& stmts) {
  std::visit(
      [&](auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, InterpExpr>) {
          for (InterpPart& part : node.parts) {
            if (auto* inner = std::get_if<ExprPtr>(&part.content)) pushChild(exprs, *inner);
          }
        } else if constexpr (std::is_same_v<T, ArrayExpr>) {
          for (ExprPtr& element : node.elements) pushChild(exprs, element);
        } else if constexpr (std::is_same_v<T, MapExpr>) {
          for (MapEntry& entry : node.entries) {
            pushChild(exprs, entry.keyExpr);
            pushChild(exprs, entry.value);
          }
        } else if constexpr (std::is_same_v<T, StructLitExpr>) {
          for (StructLitEntry& entry : node.entries) pushChild(exprs, entry.value);
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
          pushChild(exprs, node.operand);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
          pushChild(exprs, node.lhs);
          pushChild(exprs, node.rhs);
        } else if constexpr (std::is_same_v<T, AssignExpr>) {
          pushChild(exprs, node.target);
          pushChild(exprs, node.value);
        } else if constexpr (std::is_same_v<T, TernaryExpr>) {
          pushChild(exprs, node.condition);
          pushChild(exprs, node.thenExpr);
          pushChild(exprs, node.elseExpr);
        } else if constexpr (std::is_same_v<T, CallExpr>) {
          pushChild(exprs, node.callee);
          for (ExprPtr& arg : node.args) pushChild(exprs, arg);
        } else if constexpr (std::is_same_v<T, IndexExpr>) {
          pushChild(exprs, node.object);
          pushChild(exprs, node.index);
        } else if constexpr (std::is_same_v<T, SliceExpr>) {
          pushChild(exprs, node.object);
          pushChild(exprs, node.start);
          pushChild(exprs, node.end);
          pushChild(exprs, node.step);
        } else if constexpr (std::is_same_v<T, MemberExpr>) {
          pushChild(exprs, node.object);
        } else if constexpr (std::is_same_v<T, NonNullExpr>) {
          pushChild(exprs, node.operand);
        } else if constexpr (std::is_same_v<T, RangeExpr>) {
          pushChild(exprs, node.start);
          pushChild(exprs, node.end);
        } else if constexpr (std::is_same_v<T, TryExpr>) {
          pushChild(exprs, node.operand);
        } else if constexpr (std::is_same_v<T, CatchExpr>) {
          pushChild(exprs, node.operand);
          pushChild(exprs, node.fallback);
        } else if constexpr (std::is_same_v<T, LambdaExpr>) {
          pushChild(stmts, node.body);
        }
        // LiteralExpr and VariableExpr are leaves: nothing to collect.
      },
      expr.node);
}

inline void collectStmtChildren(Stmt& stmt, std::vector<ExprPtr>& exprs,
                                std::vector<StmtPtr>& stmts) {
  std::visit(
      [&](auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ExprStmt>) {
          pushChild(exprs, node.expr);
        } else if constexpr (std::is_same_v<T, LetStmt>) {
          pushChild(exprs, node.init);
        } else if constexpr (std::is_same_v<T, FnDeclStmt>) {
          pushChild(stmts, node.body);
        } else if constexpr (std::is_same_v<T, BlockStmt>) {
          for (StmtPtr& inner : node.stmts) pushChild(stmts, inner);
        } else if constexpr (std::is_same_v<T, IfStmt>) {
          pushChild(exprs, node.condition);
          pushChild(stmts, node.thenBranch);
          pushChild(stmts, node.elseBranch);
        } else if constexpr (std::is_same_v<T, WhileStmt>) {
          pushChild(exprs, node.condition);
          pushChild(stmts, node.body);
        } else if constexpr (std::is_same_v<T, DoWhileStmt>) {
          pushChild(stmts, node.body);
          pushChild(exprs, node.condition);
        } else if constexpr (std::is_same_v<T, ForStmt>) {
          pushChild(stmts, node.init);
          pushChild(exprs, node.condition);
          pushChild(exprs, node.step);
          pushChild(stmts, node.body);
        } else if constexpr (std::is_same_v<T, ForInStmt>) {
          pushChild(exprs, node.iterable);
          pushChild(stmts, node.body);
        } else if constexpr (std::is_same_v<T, ReturnStmt>) {
          pushChild(exprs, node.value);
        } else if constexpr (std::is_same_v<T, SwitchStmt>) {
          pushChild(exprs, node.subject);
          for (SwitchCase& entry : node.cases) {
            for (ExprPtr& value : entry.values) pushChild(exprs, value);
            pushChild(stmts, entry.body);
          }
          pushChild(stmts, node.defaultBody);
        } else if constexpr (std::is_same_v<T, DeferStmt>) {
          pushChild(stmts, node.body);
        } else if constexpr (std::is_same_v<T, ExternBlockStmt>) {
          for (FnDeclStmt& decl : node.decls) pushChild(stmts, decl.body);
        }
        // StructDeclStmt, BreakStmt, ContinueStmt and ImportStmt hold no child
        // nodes.
      },
      stmt.node);
}

inline void teardownAst(std::vector<ExprPtr> exprs, std::vector<StmtPtr> stmts) {
  while (!exprs.empty() || !stmts.empty()) {
    if (!exprs.empty()) {
      ExprPtr node = std::move(exprs.back());
      exprs.pop_back();
      // A tree node is the sole owner of its children; a shared node (which a
      // tree never has) is left to its other owners. Moving the children out
      // before the node's last reference drops keeps the destructor from
      // recursing: when it runs, it finds nothing left to free.
      if (node && node.use_count() == 1) collectExprChildren(*node, exprs, stmts);
    } else {
      StmtPtr node = std::move(stmts.back());
      stmts.pop_back();
      if (node && node.use_count() == 1) collectStmtChildren(*node, exprs, stmts);
    }
  }
}

inline Expr::~Expr() {
  std::vector<ExprPtr> exprs;
  std::vector<StmtPtr> stmts;
  collectExprChildren(*this, exprs, stmts);
  teardownAst(std::move(exprs), std::move(stmts));
}

inline Stmt::~Stmt() {
  std::vector<ExprPtr> exprs;
  std::vector<StmtPtr> stmts;
  collectStmtChildren(*this, exprs, stmts);
  teardownAst(std::move(exprs), std::move(stmts));
}

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
