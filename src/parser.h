#pragma once

#include <string>
#include <vector>

#include "ast.h"
#include "lexer.h"

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens, std::string where = "<source>");

  // Parses a whole program: a sequence of statements until end of input.
  StmtPtr parseProgram();

  // Parses a single expression (optionally followed by ';') and requires the
  // end of input. Used by the REPL and by string interpolation.
  ExprPtr parseFullExpression();

  const std::string& where() const { return where_; }

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::string where_;

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
