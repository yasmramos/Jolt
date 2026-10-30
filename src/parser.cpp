#include "parser.h"

#include <utility>

#include "errors.h"

namespace {

bool isAssignOp(Tok type) {
  switch (type) {
    case Tok::Eq:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::PercentEq:
    case Tok::StarStarEq:
      return true;
    default:
      return false;
  }
}

bool isAssignable(const ExprPtr& expr) {
  return std::holds_alternative<VariableExpr>(expr->node) ||
         std::holds_alternative<IndexExpr>(expr->node) ||
         std::holds_alternative<MemberExpr>(expr->node);
}

}  // namespace

Parser::Parser(std::vector<Token> tokens, std::string where)
    : tokens_(std::move(tokens)), where_(std::move(where)) {}

const Token& Parser::peek(size_t ahead) const {
  size_t index = pos_ + ahead;
  if (index >= tokens_.size()) return tokens_.back();
  return tokens_[index];
}

const Token& Parser::previous() const {
  return pos_ == 0 ? tokens_.front() : tokens_[pos_ - 1];
}

bool Parser::check(Tok type) const { return peek().type == type; }

bool Parser::match(Tok type) {
  if (!check(type)) return false;
  ++pos_;
  return true;
}

const Token& Parser::advance() {
  if (!check(Tok::Eof)) ++pos_;
  return previous();
}

const Token& Parser::consume(Tok type, const std::string& message) {
  if (check(type)) return advance();
  error(peek(), message);
}

std::string Parser::describe(const Token& token) {
  switch (token.type) {
    case Tok::Eof:
      return "end of input";
    case Tok::Ident:
      return "'" + token.lexeme + "'";
    case Tok::Int:
    case Tok::Float:
      return "'" + token.lexeme + "'";
    case Tok::Str:
      return "a string literal";
    default:
      return "'" + token.lexeme + "'";
  }
}

void Parser::error(const Token& token, const std::string& message) const {
  throw CompileError{message + ", found " + describe(token), token.line, token.col};
}

StmtPtr Parser::parseProgram() {
  std::vector<StmtPtr> statements;
  int line = peek().line;
  int col = peek().col;
  while (!check(Tok::Eof)) {
    if (match(Tok::Semicolon)) continue;
    statements.push_back(parseStatement());
  }
  return makeStmt(BlockStmt{std::move(statements)}, line, col);
}

ExprPtr Parser::parseFullExpression() {
  auto expr = parseExpression();
  match(Tok::Semicolon);
  if (!check(Tok::Eof)) error(peek(), "expected end of input");
  return expr;
}

// ---------------------------------------------------------------- statements

StmtPtr Parser::parseStatement() {
  const Token& token = peek();

  if (match(Tok::Let) || match(Tok::Const)) return parseVarDecl();
  if (match(Tok::Fn)) return parseFnDecl();
  if (match(Tok::If)) return parseIf();
  if (match(Tok::While)) return parseWhile();
  if (match(Tok::For)) return parseFor();
  if (match(Tok::Return)) return parseReturn();
  if (match(Tok::Break)) {
    consume(Tok::Semicolon, "expected ';' after 'break'");
    return makeStmt(BreakStmt{}, token.line, token.col);
  }
  if (match(Tok::Continue)) {
    consume(Tok::Semicolon, "expected ';' after 'continue'");
    return makeStmt(ContinueStmt{}, token.line, token.col);
  }
  if (check(Tok::LBrace)) return parseBlock();
  return parseExpressionStatement();
}

StmtPtr Parser::parseVarDecl() {
  const Token& keyword = previous();
  bool isConst = keyword.type == Tok::Const;
  const Token& name = consume(Tok::Ident, "expected a variable name");
  ExprPtr init;
  if (match(Tok::Eq)) {
    init = parseExpression();
  } else if (isConst) {
    error(peek(), "a 'const' declaration requires a value");
  }
  consume(Tok::Semicolon, "expected ';' after variable declaration");
  return makeStmt(LetStmt{name.lexeme, std::move(init), isConst}, keyword.line, keyword.col);
}

StmtPtr Parser::parseFnDecl() {
  const Token& keyword = previous();
  const Token& name = consume(Tok::Ident, "expected a function name");
  auto params = parseParams();
  auto body = parseBlock();
  match(Tok::Semicolon);
  return makeStmt(FnDeclStmt{name.lexeme, std::move(params), std::move(body)}, keyword.line,
                  keyword.col);
}

std::vector<std::string> Parser::parseParams() {
  consume(Tok::LParen, "expected '(' to start the parameter list");
  std::vector<std::string> params;
  if (!check(Tok::RParen)) {
    while (true) {
      if (check(Tok::RParen)) break;
      const Token& param = consume(Tok::Ident, "expected a parameter name");
      params.push_back(param.lexeme);
      if (!match(Tok::Comma)) break;
    }
  }
  consume(Tok::RParen, "expected ')' after the parameter list");
  return params;
}

StmtPtr Parser::parseIf() {
  const Token& keyword = previous();
  consume(Tok::LParen, "expected '(' after 'if'");
  auto condition = parseExpression();
  consume(Tok::RParen, "expected ')' after the condition");
  auto thenBranch = parseBlock();
  StmtPtr elseBranch;
  if (match(Tok::Else)) {
    if (match(Tok::If)) {
      elseBranch = parseIf();
    } else {
      elseBranch = parseBlock();
    }
  }
  match(Tok::Semicolon);
  return makeStmt(IfStmt{std::move(condition), std::move(thenBranch), std::move(elseBranch)},
                  keyword.line, keyword.col);
}

StmtPtr Parser::parseWhile() {
  const Token& keyword = previous();
  consume(Tok::LParen, "expected '(' after 'while'");
  auto condition = parseExpression();
  consume(Tok::RParen, "expected ')' after the condition");
  auto body = parseBlock();
  match(Tok::Semicolon);
  return makeStmt(WhileStmt{std::move(condition), std::move(body)}, keyword.line, keyword.col);
}

StmtPtr Parser::parseFor() {
  const Token& keyword = previous();

  if (!check(Tok::LParen)) {
    const Token& var = consume(Tok::Ident, "expected a loop variable or '(' after 'for'");
    consume(Tok::In, "expected 'in' in the for-in loop");
    auto iterable = parseExpression();
    auto body = parseBlock();
    match(Tok::Semicolon);
    return makeStmt(ForInStmt{var.lexeme, std::move(iterable), std::move(body)}, keyword.line,
                    keyword.col);
  }

  consume(Tok::LParen, "expected '(' after 'for'");
  StmtPtr init;
  if (match(Tok::Let) || match(Tok::Const)) {
    init = parseVarDecl();  // consumes the ';'
  } else if (match(Tok::Semicolon)) {
    init = nullptr;
  } else {
    const Token& start = peek();
    auto expr = parseExpression();
    consume(Tok::Semicolon, "expected ';' after the for-loop initializer");
    init = makeStmt(ExprStmt{std::move(expr)}, start.line, start.col);
  }

  ExprPtr condition;
  if (!check(Tok::Semicolon)) condition = parseExpression();
  consume(Tok::Semicolon, "expected ';' after the for-loop condition");

  ExprPtr step;
  if (!check(Tok::RParen)) step = parseExpression();
  consume(Tok::RParen, "expected ')' after the for-loop clauses");

  auto body = parseBlock();
  match(Tok::Semicolon);
  return makeStmt(ForStmt{std::move(init), std::move(condition), std::move(step), std::move(body)},
                  keyword.line, keyword.col);
}

StmtPtr Parser::parseReturn() {
  const Token& keyword = previous();
  ExprPtr value;
  if (!check(Tok::Semicolon)) {
    value = parseExpression();
  }
  consume(Tok::Semicolon, "expected ';' after 'return'");
  return makeStmt(ReturnStmt{std::move(value)}, keyword.line, keyword.col);
}

StmtPtr Parser::parseBlock() {
  const Token& open = consume(Tok::LBrace, "expected '{'");
  std::vector<StmtPtr> statements;
  while (!check(Tok::RBrace)) {
    if (check(Tok::Eof)) error(peek(), "expected '}' to close the block");
    if (match(Tok::Semicolon)) continue;
    statements.push_back(parseStatement());
  }
  consume(Tok::RBrace, "expected '}' to close the block");
  return makeStmt(BlockStmt{std::move(statements)}, open.line, open.col);
}

StmtPtr Parser::parseExpressionStatement() {
  const Token& start = peek();
  auto expr = parseExpression();
  consume(Tok::Semicolon, "expected ';' after expression");
  return makeStmt(ExprStmt{std::move(expr)}, start.line, start.col);
}

// --------------------------------------------------------------- expressions

ExprPtr Parser::parseExpression() { return parseAssignment(); }

ExprPtr Parser::parseAssignment() {
  auto target = parseTernary();
  if (isAssignOp(peek().type)) {
    const Token& op = advance();
    if (!isAssignable(target)) error(op, "invalid assignment target");
    auto value = parseAssignment();
    return makeExpr(AssignExpr{std::move(target), op.type, std::move(value), op.line, op.col},
                    op.line, op.col);
  }
  return target;
}

ExprPtr Parser::parseTernary() {
  const Token& start = peek();
  auto condition = parseOr();
  if (match(Tok::Question)) {
    auto thenExpr = parseAssignment();
    consume(Tok::Colon, "expected ':' in ternary expression");
    auto elseExpr = parseAssignment();
    return makeExpr(TernaryExpr{std::move(condition), std::move(thenExpr), std::move(elseExpr)},
                    start.line, start.col);
  }
  return condition;
}

ExprPtr Parser::parseOr() {
  auto lhs = parseAnd();
  while (match(Tok::PipePipe)) {
    const Token& op = previous();
    auto rhs = parseAnd();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseAnd() {
  auto lhs = parseBitOr();
  while (match(Tok::AmpAmp)) {
    const Token& op = previous();
    auto rhs = parseBitOr();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitOr() {
  auto lhs = parseBitXor();
  while (match(Tok::Pipe)) {
    const Token& op = previous();
    auto rhs = parseBitXor();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitXor() {
  auto lhs = parseBitAnd();
  while (match(Tok::Caret)) {
    const Token& op = previous();
    auto rhs = parseBitAnd();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseBitAnd() {
  auto lhs = parseEquality();
  while (match(Tok::Amp)) {
    const Token& op = previous();
    auto rhs = parseEquality();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseEquality() {
  auto lhs = parseComparison();
  while (check(Tok::EqEq) || check(Tok::BangEq)) {
    const Token& op = advance();
    auto rhs = parseComparison();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseComparison() {
  auto lhs = parseShift();
  while (check(Tok::Lt) || check(Tok::LtEq) || check(Tok::Gt) || check(Tok::GtEq) ||
         check(Tok::In)) {
    const Token& op = advance();
    auto rhs = parseShift();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseShift() {
  auto lhs = parseRange();
  while (check(Tok::Shl) || check(Tok::Shr)) {
    const Token& op = advance();
    auto rhs = parseRange();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseRange() {
  const Token& start = peek();
  auto lhs = parseAdditive();
  if (check(Tok::DotDot) || check(Tok::DotDotEq)) {
    const Token& op = advance();
    auto rhs = parseAdditive();
    return makeExpr(RangeExpr{std::move(lhs), std::move(rhs), op.type == Tok::DotDotEq}, start.line,
                    start.col);
  }
  return lhs;
}

ExprPtr Parser::parseAdditive() {
  auto lhs = parseMultiplicative();
  while (check(Tok::Plus) || check(Tok::Minus)) {
    const Token& op = advance();
    auto rhs = parseMultiplicative();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parseMultiplicative() {
  auto lhs = parseUnary();
  while (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent)) {
    const Token& op = advance();
    auto rhs = parseUnary();
    lhs = makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                   op.col);
  }
  return lhs;
}

ExprPtr Parser::parsePower() {
  auto lhs = parsePostfix();
  if (match(Tok::StarStar)) {
    const Token& op = previous();
    auto rhs = parseUnary();
    return makeExpr(BinaryExpr{std::move(lhs), op.type, std::move(rhs), op.line, op.col}, op.line,
                    op.col);
  }
  return lhs;
}

ExprPtr Parser::parseUnary() {
  if (check(Tok::Bang) || check(Tok::Minus) || check(Tok::Plus) || check(Tok::Tilde)) {
    const Token& op = advance();
    auto operand = parseUnary();
    return makeExpr(UnaryExpr{op.type, std::move(operand)}, op.line, op.col);
  }
  return parsePower();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (match(Tok::LParen)) {
      const Token& open = previous();
      std::vector<ExprPtr> args;
      if (!check(Tok::RParen)) {
        while (true) {
          if (check(Tok::RParen)) break;
          args.push_back(parseExpression());
          if (!match(Tok::Comma)) break;
        }
      }
      consume(Tok::RParen, "expected ')' after the arguments");
      expr = makeExpr(CallExpr{std::move(expr), std::move(args)}, open.line, open.col);
    } else if (match(Tok::LBracket)) {
      const Token& open = previous();
      auto index = parseExpression();
      consume(Tok::RBracket, "expected ']' after the index");
      expr = makeExpr(IndexExpr{std::move(expr), std::move(index)}, open.line, open.col);
    } else if (match(Tok::Dot)) {
      const Token& dot = previous();
      const Token& name = consume(Tok::Ident, "expected a member name after '.'");
      expr = makeExpr(MemberExpr{std::move(expr), name.lexeme}, dot.line, dot.col);
    } else {
      break;
    }
  }
  return expr;
}

ExprPtr Parser::parsePrimary() {
  const Token& token = peek();

  if (match(Tok::Int)) {
    return makeExpr(LiteralExpr{Value(token.intValue)}, token.line, token.col);
  }
  if (match(Tok::Float)) {
    return makeExpr(LiteralExpr{Value(token.floatValue)}, token.line, token.col);
  }
  if (match(Tok::Str)) return buildString(token);
  if (match(Tok::True)) return makeExpr(LiteralExpr{Value(true)}, token.line, token.col);
  if (match(Tok::False)) return makeExpr(LiteralExpr{Value(false)}, token.line, token.col);
  if (match(Tok::Nil)) return makeExpr(LiteralExpr{Value()}, token.line, token.col);
  if (match(Tok::Ident)) {
    return makeExpr(VariableExpr{token.lexeme}, token.line, token.col);
  }
  if (match(Tok::LParen)) {
    auto expr = parseExpression();
    consume(Tok::RParen, "expected ')' after the expression");
    return expr;
  }
  if (match(Tok::LBracket)) {
    std::vector<ExprPtr> elements;
    if (!check(Tok::RBracket)) {
      while (true) {
        if (check(Tok::RBracket)) break;
        elements.push_back(parseExpression());
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBracket, "expected ']' to close the array literal");
    return makeExpr(ArrayExpr{std::move(elements)}, token.line, token.col);
  }
  if (match(Tok::LBrace)) {
    std::vector<MapEntry> entries;
    if (!check(Tok::RBrace)) {
      while (true) {
        if (check(Tok::RBrace)) break;
        std::string key;
        if (check(Tok::Ident)) {
          key = advance().lexeme;
        } else if (check(Tok::Str)) {
          const Token& keyToken = advance();
          if (keyToken.parts.size() != 1 || keyToken.parts[0].isExpr) {
            error(keyToken, "map keys must be plain strings or identifiers");
          }
          key = keyToken.parts[0].text;
        } else {
          error(peek(), "expected a string or identifier as a map key");
        }
        consume(Tok::Colon, "expected ':' after the map key");
        entries.push_back(MapEntry{key, parseExpression()});
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBrace, "expected '}' to close the map literal");
    return makeExpr(MapExpr{std::move(entries)}, token.line, token.col);
  }
  if (match(Tok::Fn)) {
    const Token& fnToken = previous();
    std::string name;
    if (check(Tok::Ident)) name = advance().lexeme;
    auto params = parseParams();
    StmtPtr body;
    if (match(Tok::Arrow)) {
      auto result = parseExpression();
      std::vector<StmtPtr> statements;
      statements.push_back(makeStmt(ReturnStmt{std::move(result)}, fnToken.line, fnToken.col));
      body = makeStmt(BlockStmt{std::move(statements)}, fnToken.line, fnToken.col);
    } else {
      body = parseBlock();
    }
    return makeExpr(LambdaExpr{std::move(name), std::move(params), std::move(body)}, fnToken.line,
                    fnToken.col);
  }

  error(token, "expected an expression");
}

ExprPtr Parser::buildString(const Token& token) {
  if (token.parts.size() == 1 && !token.parts[0].isExpr) {
    return makeExpr(LiteralExpr{Value(token.parts[0].text)}, token.line, token.col);
  }

  InterpExpr node;
  for (const auto& part : token.parts) {
    InterpPart piece;
    piece.line = part.line;
    piece.col = part.col;
    if (part.isExpr) {
      try {
        auto tokens = lex(part.text, part.line, part.col);
        Parser sub(std::move(tokens), where_);
        piece.expr = sub.parseFullExpression();
      } catch (const CompileError& err) {
        throw CompileError{"in string interpolation: " + err.message, err.line, err.col};
      }
    } else {
      piece.text = part.text;
    }
    node.parts.push_back(std::move(piece));
  }
  return makeExpr(std::move(node), token.line, token.col);
}
