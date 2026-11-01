#include "lexer.h"

#include "value.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

struct Keyword {
  const char* name;
  Tok type;
};

const Keyword kKeywords[] = {
    {"let", Tok::Let},       {"var", Tok::Var},        {"const", Tok::Const},
    {"fn", Tok::Fn},
    {"if", Tok::If},         {"else", Tok::Else},       {"while", Tok::While},
    {"for", Tok::For},       {"in", Tok::In},           {"break", Tok::Break},
    {"continue", Tok::Continue}, {"return", Tok::Return}, {"true", Tok::True},
    {"false", Tok::False},   {"nil", Tok::Nil},
};

class Lexer {
 public:
  Lexer(const std::string& source, int startLine, int startCol)
      : src_(source), line_(startLine), col_(startCol) {}

  std::vector<Token> run() {
    std::vector<Token> tokens;
    if (src_.size() >= 2 && src_[0] == '#' && src_[1] == '!') skipLine();
    while (true) {
      skipTrivia();
      if (atEnd()) break;
      tokens.push_back(next());
    }
    Token eof;
    eof.type = Tok::Eof;
    eof.line = line_;
    eof.col = col_;
    tokens.push_back(eof);
    return tokens;
  }

 private:
  const std::string& src_;
  size_t pos_ = 0;
  int line_;
  int col_;

  [[noreturn]] void error(const std::string& message, int line, int col) const {
    throw CompileError{message, line, col};
  }

  bool atEnd() const { return pos_ >= src_.size(); }
  char peek(size_t ahead = 0) const {
    size_t index = pos_ + ahead;
    return index < src_.size() ? src_[index] : '\0';
  }

  char advance() {
    char c = src_[pos_++];
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    return c;
  }

  void skipLine() {
    while (!atEnd() && peek() != '\n') advance();
  }

  void skipTrivia() {
    while (!atEnd()) {
      char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        advance();
      } else if (c == '/' && peek(1) == '/') {
        skipLine();
      } else if (c == '/' && peek(1) == '*') {
        int startLine = line_;
        int startCol = col_;
        advance();
        advance();
        int depth = 1;
        while (!atEnd() && depth > 0) {
          if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            ++depth;
          } else if (peek() == '*' && peek(1) == '/') {
            advance();
            advance();
            --depth;
          } else {
            advance();
          }
        }
        if (depth > 0) error("unterminated block comment", startLine, startCol);
      } else {
        break;
      }
    }
  }

  static bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
  }
  static bool isIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }
  static bool isDigit(char c) { return c >= '0' && c <= '9'; }

  Token next() {
    int startLine = line_;
    int startCol = col_;
    char c = peek();

    if (isIdentStart(c)) {
      size_t start = pos_;
      while (!atEnd() && isIdentPart(peek())) advance();
      std::string text = src_.substr(start, pos_ - start);
      for (const auto& keyword : kKeywords) {
        if (text == keyword.name) {
          Token token;
          token.type = keyword.type;
          token.lexeme = text;
          token.line = startLine;
          token.col = startCol;
          return token;
        }
      }
      Token token;
      token.type = Tok::Ident;
      token.lexeme = std::move(text);
      token.line = startLine;
      token.col = startCol;
      return token;
    }

    if (isDigit(c)) return number();

    if (c == '"') return stringToken();

    return operatorToken();
  }

  Token number() {
    int startLine = line_;
    int startCol = col_;
    size_t start = pos_;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' ||
                          peek(1) == 'B' || peek(1) == 'o' || peek(1) == 'O')) {
      advance();
      char marker = advance();
      int base = 10;
      if (marker == 'x' || marker == 'X') base = 16;
      if (marker == 'b' || marker == 'B') base = 2;
      if (marker == 'o' || marker == 'O') base = 8;

      size_t digitsStart = pos_;
      while (!atEnd()) {
        char ch = peek();
        if (isDigit(ch) || ch == '_' ||
            (base == 16 && std::isxdigit(static_cast<unsigned char>(ch)))) {
          advance();
        } else {
          break;
        }
      }
      std::string raw = src_.substr(digitsStart, pos_ - digitsStart);
      std::string digits;
      for (char ch : raw) {
        if (ch != '_') digits += ch;
      }
      if (digits.empty()) error("expected digits after base prefix", startLine, startCol);

      errno = 0;
      char* end = nullptr;
      long long value = std::strtoll(digits.c_str(), &end, base);
      if (errno == ERANGE) error("integer literal is out of range", startLine, startCol);
      if (end == nullptr || *end != '\0') {
        error("invalid digit for base " + std::to_string(base), startLine, startCol);
      }
      Token token;
      token.type = Tok::Int;
      token.lexeme = src_.substr(start, pos_ - start);
      token.intValue = value;
      token.line = startLine;
      token.col = startCol;
      return token;
    }

    bool isFloat = false;
    while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    if (peek() == '.' && isDigit(peek(1))) {
      isFloat = true;
      advance();
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    }
    if (peek() == 'e' || peek() == 'E') {
      isFloat = true;
      advance();
      if (peek() == '+' || peek() == '-') advance();
      if (!isDigit(peek())) error("malformed exponent", startLine, startCol);
      while (!atEnd() && (isDigit(peek()) || peek() == '_')) advance();
    }

    std::string raw = src_.substr(start, pos_ - start);
    std::string text;
    for (char ch : raw) {
      if (ch != '_') text += ch;
    }

    Token token;
    token.line = startLine;
    token.col = startCol;
    token.lexeme = raw;
    if (isFloat) {
      errno = 0;
      char* end = nullptr;
      double value = std::strtod(text.c_str(), &end);
      if (errno == ERANGE) error("float literal is out of range", startLine, startCol);
      if (end == nullptr || *end != '\0') error("malformed float literal", startLine, startCol);
      token.type = Tok::Float;
      token.floatValue = value;
    } else {
      errno = 0;
      char* end = nullptr;
      long long value = std::strtoll(text.c_str(), &end, 10);
      if (errno == ERANGE) error("integer literal is out of range", startLine, startCol);
      if (end == nullptr || *end != '\0') error("malformed integer literal", startLine, startCol);
      token.type = Tok::Int;
      token.intValue = value;
    }
    return token;
  }

  void skipQuoted() {
    int startLine = line_;
    int startCol = col_;
    advance();  // opening quote
    while (!atEnd()) {
      if (peek() == '\\') {
        advance();
        if (atEnd()) break;
        advance();
      } else if (peek() == '"') {
        advance();
        return;
      } else {
        advance();
      }
    }
    error("unterminated string", startLine, startCol);
  }

  void scanInterpolation(int startLine, int startCol) {
    int depth = 1;
    while (!atEnd()) {
      char c = peek();
      if (c == '"') {
        skipQuoted();
      } else if (c == '/' && peek(1) == '/') {
        skipLine();
      } else if (c == '/' && peek(1) == '*') {
        advance();
        advance();
        int commentDepth = 1;
        while (!atEnd() && commentDepth > 0) {
          if (peek() == '/' && peek(1) == '*') {
            advance();
            advance();
            ++commentDepth;
          } else if (peek() == '*' && peek(1) == '/') {
            advance();
            advance();
            --commentDepth;
          } else {
            advance();
          }
        }
        if (commentDepth > 0) error("unterminated block comment", startLine, startCol);
      } else if (c == '{') {
        ++depth;
        advance();
      } else if (c == '}') {
        --depth;
        advance();
        if (depth == 0) return;
      } else {
        advance();
      }
    }
    error("unterminated string interpolation", startLine, startCol);
  }

  std::string readEscape(int startLine, int startCol) {
    advance();  // backslash
    if (atEnd()) error("unterminated escape sequence", startLine, startCol);
    char c = advance();
    switch (c) {
      case 'n':
        return "\n";
      case 't':
        return "\t";
      case 'r':
        return "\r";
      case '0':
        return std::string(1, '\0');
      case '\\':
        return "\\";
      case '"':
        return "\"";
      case '$':
        return "$";
      case 'x': {
        int value = 0;
        for (int i = 0; i < 2; ++i) {
          if (atEnd() || !std::isxdigit(static_cast<unsigned char>(peek()))) {
            error("expected two hexadecimal digits after '\\x'", startLine, startCol);
          }
          char digit = advance();
          value = value * 16 + (isDigit(digit) ? digit - '0' : (std::tolower(digit) - 'a' + 10));
        }
        return std::string(1, static_cast<char>(value));
      }
      case 'u': {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
          if (atEnd() || !std::isxdigit(static_cast<unsigned char>(peek()))) {
            error("expected four hexadecimal digits after '\\u'", startLine, startCol);
          }
          char digit = advance();
          value = value * 16 + (isDigit(digit) ? digit - '0' : (std::tolower(digit) - 'a' + 10));
        }
        std::string encoded;
        appendUtf8(encoded, value);
        return encoded;
      }
      default:
        error(std::string("unknown escape sequence '\\") + c + "'", startLine, startCol);
    }
  }

  Token stringToken() {
    int startLine = line_;
    int startCol = col_;
    advance();  // opening quote

    std::vector<TokenPart> parts;
    std::string literal;
    int partLine = line_;
    int partCol = col_;
    bool interpolated = false;

    while (true) {
      if (atEnd()) error("unterminated string", startLine, startCol);
      char c = peek();
      if (c == '"') {
        advance();
        break;
      }
      if (c == '\\') {
        literal += readEscape(line_, col_);
        continue;
      }
      if (c == '$' && peek(1) == '{') {
        if (!literal.empty() || interpolated) {
          parts.push_back(TokenPart{false, literal, partLine, partCol});
          literal.clear();
        }
        advance();
        advance();  // ${
        int exprLine = line_;
        int exprCol = col_;
        size_t exprStart = pos_;
        scanInterpolation(exprLine, exprCol);
        size_t exprEnd = pos_ - 1;  // exclude closing '}'
        parts.push_back(TokenPart{true, src_.substr(exprStart, exprEnd - exprStart), exprLine, exprCol});
        interpolated = true;
        partLine = line_;
        partCol = col_;
        continue;
      }
      literal += advance();
    }

    parts.push_back(TokenPart{false, literal, partLine, partCol});

    Token token;
    token.type = Tok::Str;
    token.line = startLine;
    token.col = startCol;
    token.parts = std::move(parts);
    return token;
  }

  Token operatorToken() {
    int startLine = line_;
    int startCol = col_;
    size_t start = pos_;

    auto make = [&](Tok type, size_t length) {
      std::string text = src_.substr(start, length);
      for (size_t i = 0; i < length; ++i) advance();
      Token token;
      token.type = type;
      token.lexeme = std::move(text);
      token.line = startLine;
      token.col = startCol;
      return token;
    };

    char c = peek();
    char c1 = peek(1);
    char c2 = peek(2);

    switch (c) {
      case '(':
        return make(Tok::LParen, 1);
      case ')':
        return make(Tok::RParen, 1);
      case '{':
        return make(Tok::LBrace, 1);
      case '}':
        return make(Tok::RBrace, 1);
      case '[':
        return make(Tok::LBracket, 1);
      case ']':
        return make(Tok::RBracket, 1);
      case ',':
        return make(Tok::Comma, 1);
      case ';':
        return make(Tok::Semicolon, 1);
      case '?':
        return make(Tok::Question, 1);
      case ':':
        return make(Tok::Colon, 1);
      case '~':
        return make(Tok::Tilde, 1);
      case '.':
        if (c1 == '.' && c2 == '=') return make(Tok::DotDotEq, 3);
        if (c1 == '.') return make(Tok::DotDot, 2);
        return make(Tok::Dot, 1);
      case '+':
        if (c1 == '=') return make(Tok::PlusEq, 2);
        return make(Tok::Plus, 1);
      case '-':
        if (c1 == '=') return make(Tok::MinusEq, 2);
        return make(Tok::Minus, 1);
      case '*':
        if (c1 == '*' && c2 == '=') return make(Tok::StarStarEq, 3);
        if (c1 == '*') return make(Tok::StarStar, 2);
        if (c1 == '=') return make(Tok::StarEq, 2);
        return make(Tok::Star, 1);
      case '/':
        if (c1 == '=') return make(Tok::SlashEq, 2);
        return make(Tok::Slash, 1);
      case '%':
        if (c1 == '=') return make(Tok::PercentEq, 2);
        return make(Tok::Percent, 1);
      case '=':
        if (c1 == '=') return make(Tok::EqEq, 2);
        if (c1 == '>') return make(Tok::Arrow, 2);
        return make(Tok::Eq, 1);
      case '!':
        if (c1 == '=') return make(Tok::BangEq, 2);
        return make(Tok::Bang, 1);
      case '<':
        if (c1 == '=') return make(Tok::LtEq, 2);
        if (c1 == '<') return make(Tok::Shl, 2);
        return make(Tok::Lt, 1);
      case '>':
        if (c1 == '=') return make(Tok::GtEq, 2);
        if (c1 == '>') return make(Tok::Shr, 2);
        return make(Tok::Gt, 1);
      case '&':
        if (c1 == '&') return make(Tok::AmpAmp, 2);
        return make(Tok::Amp, 1);
      case '|':
        if (c1 == '|') return make(Tok::PipePipe, 2);
        return make(Tok::Pipe, 1);
      case '^':
        return make(Tok::Caret, 1);
      default:
        break;
    }

    error(std::string("unexpected character '") + c + "'", startLine, startCol);
  }
};

}  // namespace

std::vector<Token> lex(const std::string& source, int startLine, int startCol) {
  return Lexer(source, startLine, startCol).run();
}
