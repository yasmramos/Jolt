#pragma once

#include <string>
#include <unordered_map>
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

  const std::string& where() const { return where_; }

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::string where_;
  // Folded values of `const` declarations seen so far, used by the parser to
  // evaluate constant expressions at parse time (see foldConstant).
  std::unordered_map<std::string, Value> constValues_;
  // True while parseProgram is running at statement level: top-level consts
  // are folded later in resolveTopLevelConsts (after the whole program was
  // parsed), so nested blocks keep folding eagerly during parsing.
  bool atTopLevel_ = false;

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
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseFor();
  StmtPtr parseReturn();
  StmtPtr parseBlock();
  StmtPtr parseExpressionStatement();

  ExprPtr parseExpression();
  ExprPtr parseAssignment();
  ExprPtr parseTernary();
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

  ExprPtr buildString(const Token& token);
  std::vector<std::string> parseParams();
};
