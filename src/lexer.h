#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "errors.h"

enum class Tok {
  // literals and names
  Int,
  Float,
  Str,
  Ident,
  // keywords
  Let,
  Const,
  Fn,
  If,
  Else,
  While,
  For,
  In,
  Break,
  Continue,
  Return,
  True,
  False,
  Nil,
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
  Colon,
  Semicolon,
  Question,
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
  Lt,
  LtEq,
  Gt,
  GtEq,
  AmpAmp,
  PipePipe,
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
  double floatValue = 0;
  std::vector<TokenPart> parts;

  bool is(Tok t) const { return type == t; }
};

std::vector<Token> lex(const std::string& source, int startLine = 1, int startCol = 1);
