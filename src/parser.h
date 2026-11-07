#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "lexer.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens, std::string where = "<source>");

  // Parses a whole program: a sequence of statements until end of input.
  StmtPtr parseProgram();

  // Fold `const` declarations after the program has been parsed: evaluate
  // each const initializer at compile time and replace uses of the constant
  // in the top-level statements with literal nodes (see foldConstant).
  void resolveTopLevelConsts(StmtPtr& program);

  // RAII helper: marks the parser as being at program statement level so
  // top-level `const` initializers skip eager folding (they are folded by
  // resolveTopLevelConsts after the whole program is parsed). Nested blocks
  // clear the flag again, keeping their eager per-statement folding.
  class TopLevelScope {
   public:
    explicit TopLevelScope(bool& flag);
    ~TopLevelScope();
    TopLevelScope(const TopLevelScope&) = delete;
    TopLevelScope& operator=(const TopLevelScope&) = delete;

   private:
    bool& flag_;
    bool previous_;
  };

  // Parses a single expression (optionally followed by ';') and requires the
  // end of input. Used by the REPL and by string interpolation.
  ExprPtr parseFullExpression();

  // Makes the `struct` names declared by earlier REPL entries visible to this
  // parser, so a literal (`Point { ... }`) or an annotation written in a later
  // entry is still recognized. A file parse never needs this: every struct is
  // declared in the same source. Only `structNames_` is seeded, so re-declaring
  // a struct in a later entry stays allowed, exactly like redefining a `let`
  // or a `fn`.
  void seedStructNames(const std::unordered_set<std::string>& names) {
    structNames_.insert(names.begin(), names.end());
  }

  const std::string& where() const { return where_; }

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::string where_;
  // Folded values of `const` declarations seen so far, used by the parser to
  // evaluate constant expressions at parse time (see foldConstant).
  std::unordered_map<std::string, Value> constValues_;
  // Names of the `struct` types declared so far (PHASE 6.5). A struct name is
  // accepted as a type annotation and lets `Name { ... }` be recognized as a
  // struct literal instead of a variable followed by a block (which would
  // otherwise be ambiguous in `for x in name { ... }`).
  std::unordered_set<std::string> structNames_;
  // Names of the `struct` types actually declared during this parse. Kept
  // apart from `structNames_` (which also holds pre-scanned names) so a
  // redeclaration in the same file is caught while a name merely made visible
  // by the pre-scan is not mistaken for one.
  std::unordered_set<std::string> declaredStructs_;
  // True while parseProgram is running at statement level: top-level consts
  // are folded later in resolveTopLevelConsts (after the whole program was
  // parsed), so nested blocks keep folding eagerly during parsing.
  bool atTopLevel_ = false;
  // Number of enclosing `{ ... }` blocks being parsed. `import` is only valid
  // at depth 0 (a program's top level), so an import nested in a block or a
  // function body is rejected at parse time.
  int nestingDepth_ = 0;

  bool foldConstant(const Expr& expr, Value& out, std::string& reason) const;

  const Token& peek(size_t ahead = 0) const;
  const Token& previous() const;
  bool check(Tok type) const;
  bool match(Tok type);
  const Token& advance();
  const Token& consume(Tok type, const std::string& message);
  [[noreturn]] void error(const Token& token, const std::string& message) const;
  static std::string describe(const Token& token);

  StmtPtr parseStatement();
  StmtPtr parseVarDecl();
  StmtPtr parseFnDecl();
  // `native fn name(params): ret;` (PHASE 6.5): a bodyless declaration that
  // binds a name to a host-registered native function.
  StmtPtr parseNativeDecl();
  // `struct Name { field: T; ... }` (PHASE 6.5). A data-only declaration.
  StmtPtr parseStructDecl();
  // Body that follows a `=>`: a `{ ... }` block, or a single expression that
  // is desugared into a block holding one `return`. `arrow` is the `=>` token.
  StmtPtr parseFunctionBody(const Token& arrow);
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseFor();
  StmtPtr parseReturn();
  StmtPtr parseDefer();
  StmtPtr parseImport();
  StmtPtr parseSwitch();
  std::string parseImportPath();
  StmtPtr parseBlock();
  StmtPtr parseExpressionStatement();

  ExprPtr parseExpression();
  ExprPtr parseAssignment();
  ExprPtr parseTernary();
  ExprPtr parseCatch();
  ExprPtr parseNullCoalesce();
  ExprPtr parseOr();
  ExprPtr parseAnd();
  ExprPtr parseBitOr();
  ExprPtr parseBitXor();
  ExprPtr parseBitAnd();
  ExprPtr parseEquality();
  ExprPtr parseComparison();
  ExprPtr parseShift();
  ExprPtr parseRange();
  ExprPtr parseAdditive();
  ExprPtr parseMultiplicative();
  ExprPtr parsePower();
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  // Parses the `{ field: value, ... }` body of a struct literal whose type
  // name has already been consumed. The parser only builds the node; checking
  // the field names and types against the declaration is the compiler's job.
  ExprPtr parseStructLiteralBody(const std::string& typeName, int line, int col);
  // True when the upcoming tokens start a lambda: `x =>` or `(a, b) =>`.
  bool atLambda() const;
  ExprPtr parseLambda();

  ExprPtr buildString(const Token& token);
  std::vector<Param> parseParams();
  // Parses an optional `: <type>` annotation after a name (variable, parameter
  // or return type). Returns an empty TypeRef when no `:` is present. Rejects
  // unknown type names at parse time.
  TypeRef parseTypeAnnotation();
};
