#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "errors.h"

enum class Tok {
  // literals and names
  Int,
  // `1.5` (no suffix) is a `double`; `1.5f`/`1.5F` is a `float` (Java-style).
  Double,
  Float,
  Str,
  Ident,
  // keywords
  Var,
  Let,
  Const,
  Fn,
  If,
  Else,
  While,
  Do,
  For,
  In,
  Break,
  Continue,
  Return,
  Import,
  Switch,
  Case,
  Default,
  Defer,
  Struct,
  Native,
  Extern,
  Try,
  Catch,
  True,
  False,
  Nil,
  // `@` starts an attribute (`@MustUse fn f() ...`).
  At,
  // punctuation
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Dot,
  DotDot,
  DotDotEq,
  Ellipsis,
  Colon,
  Semicolon,
  Question,
  QuestionDot,
  QuestionQuestion,
  QuestionQuestionEq,
  Arrow,
  // operators
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  StarStar,
  Eq,
  EqEq,
  Bang,
  BangEq,
  BangBang,
  Lt,
  LtEq,
  Gt,
  GtEq,
  AmpAmp,
  AmpAmpEq,
  PipePipe,
  PipePipeEq,
  Amp,
  Pipe,
  Caret,
  Tilde,
  Shl,
  Shr,
  PlusEq,
  MinusEq,
  StarEq,
  SlashEq,
  PercentEq,
  StarStarEq,
  Eof
};

// A piece of a string literal: either literal text or the source of an
// interpolated `${ ... }` expression.
struct TokenPart {
  bool isExpr = false;
  std::string text;
  int line = 1;
  int col = 1;
};

struct Token {
  Tok type = Tok::Eof;
  std::string lexeme;
  int line = 1;
  int col = 1;
  int64_t intValue = 0;
  // True when an integer literal is exactly 2^63 (`9223372036854775808`), the
  // magnitude of `INT64_MIN`, which has no positive 64-bit representation. Such
  // a literal is only valid directly under a unary minus (`-9223372036854775808`
  // is the smallest `int`); anywhere else it is a compile error.
  bool intNeedsNegation = false;
  double floatValue = 0;
  std::vector<TokenPart> parts;

  bool is(Tok t) const { return type == t; }
};

std::vector<Token> lex(const std::string& source, int startLine = 1, int startCol = 1);
