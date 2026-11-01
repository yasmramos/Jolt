#include "parser.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "errors.h"

namespace {

// Collects the statements of a block-like node (BlockStmt) for rewriting.
const std::vector<StmtPtr>* blockStatements(const StmtNode& node) {
  if (const auto* block = std::get_if<BlockStmt>(&node)) return &block->stmts;
  return nullptr;
}

}  // namespace

// Parse-time resolution of top-level `const` declarations. Runs as a second
// pass over the finished program because `const` values are resolved while
// parsing: a const may legally reference another top-level const declared
// later in the file. The pass hoists top-level consts to the front of the
// program (in source order), folds each const initializer against the table
// of constants recorded so far, normalizes the node to a literal (so every
// backend gets the folded value without repeating the fold) and records the
// result for later references. Consts nested inside functions or blocks keep
// their folded values from the first parse pass and are not hoisted, so
// their scope rules stay honest.
void Parser::resolveTopLevelConsts(StmtPtr& program) {
  const std::vector<StmtPtr>* stmts = blockStatements(program->node);
  if (!stmts) return;
  std::vector<StmtPtr> reordered;
  reordered.reserve(stmts->size());
  std::vector<StmtPtr> consts;
  for (const StmtPtr& stmt : *stmts) {
    const auto* decl = std::get_if<LetStmt>(&stmt->node);
    if (decl && decl->kind == Tok::Const) {
      consts.push_back(stmt);
    } else {
      reordered.push_back(stmt);
    }
  }
  if (consts.empty()) return;

  // Build the const table. A top-level const initializer may reference
  // another top-level const declared later in the file, so a single
  // sequential pass is not enough: iterate to a fixed point. Declarations
  // whose initializer still depends on an unresolved const are retried once
  // the missing value becomes available; cycles and genuinely non-constant
  // expressions are reported against the first declaration left unresolved.
  struct Pending {
    StmtPtr stmt;
    std::string name;
    std::string lastReason;
  };
  std::vector<Pending> pending;
  for (const StmtPtr& stmt : consts) {
    const auto& decl = std::get<LetStmt>(stmt->node);
    pending.push_back(Pending{stmt, decl.name, "initializer is not a constant expression"});
  }
  bool progressed = true;
  while (!pending.empty() && progressed) {
    progressed = false;
    for (size_t index = 0; index < pending.size();) {
      auto& item = pending[index];
      auto& decl = std::get<LetStmt>(item.stmt->node);
      Value foldedValue;
      std::string reason;
      if (foldConstant(*decl.init, foldedValue, reason)) {
        // Normalize to a literal so backends never re-evaluate the expression.
        decl.init = makeExpr(LiteralExpr{foldedValue}, item.stmt->line, item.stmt->col);
        constValues_[item.name] = std::move(foldedValue);
        pending.erase(pending.begin() + static_cast<long>(index));
        progressed = true;
      } else {
        item.lastReason = std::move(reason);
        ++index;
      }
    }
  }
  if (!pending.empty()) {
    const Pending& first = pending.front();
    throw CompileError{"'const' '" + first.name + "' initializer must be a constant expression: " +
                           first.lastReason,
                       first.stmt->line, first.stmt->col};
  }
  reordered.insert(reordered.begin(), consts.begin(), consts.end());
  BlockStmt rebuilt{std::move(reordered)};
  program->node = std::move(rebuilt);
}

Parser::TopLevelScope::TopLevelScope(bool& flag) : flag_(flag), previous_(flag) {
  flag_ = true;
}

Parser::TopLevelScope::~TopLevelScope() { flag_ = previous_; }

StmtPtr Parser::parseProgram() {
  TopLevelScope topLevel(atTopLevel_);
  std::vector<StmtPtr> statements;
  int line = peek().line;
  int col = peek().col;
  while (!check(Tok::Eof)) {
    if (match(Tok::Semicolon)) continue;
    statements.push_back(parseStatement());
  }
  auto program = makeStmt(BlockStmt{std::move(statements)}, line, col);
  // Second pass: `const` values are resolved at parse time, so a const may
  // reference another top-level const declared later in the file. Hoisting
  // top-level consts to the front of the program makes their folded values
  // available to every subsequent const initializer. Re-running the pass is
  // idempotent (the parser state was consumed by the first pass).
  resolveTopLevelConsts(program);
  return program;
}

// Wrapping integer arithmetic, matching the interpreter's overflow behavior.
int64_t wrapAdd(int64_t left, int64_t right) {
  return static_cast<int64_t>(static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
}
int64_t wrapSub(int64_t left, int64_t right) {
  return static_cast<int64_t>(static_cast<uint64_t>(left) - static_cast<uint64_t>(right));
}
int64_t wrapMul(int64_t left, int64_t right) {
  return static_cast<int64_t>(static_cast<uint64_t>(left) * static_cast<uint64_t>(right));
}

// Binary exponentiation on integers (exponent assumed non-negative),
// mirroring the interpreter's `intPow`.
int64_t foldIntPow(int64_t base, int64_t exponent) {
  uint64_t result = 1;
  uint64_t factor = static_cast<uint64_t>(base);
  uint64_t exp = static_cast<uint64_t>(exponent);
  while (exp > 0) {
    if (exp & 1U) result *= factor;
    factor *= factor;
    exp >>= 1U;
  }
  return static_cast<int64_t>(result);
}

bool isComparisonOp(Tok op) {
  switch (op) {
    case Tok::EqEq:
    case Tok::BangEq:
    case Tok::Lt:
    case Tok::LtEq:
    case Tok::Gt:
    case Tok::GtEq:
      return true;
    default:
      return false;
  }
}

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

ExprPtr Parser::parseFullExpression() {
  auto expr = parseExpression();
  match(Tok::Semicolon);
  if (!check(Tok::Eof)) error(peek(), "expected end of input");
  return expr;
}

// ---------------------------------------------------------------- statements

StmtPtr Parser::parseStatement() {
  const Token& token = peek();

  if (match(Tok::Var) || match(Tok::Let) || match(Tok::Const)) return parseVarDecl();
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
  // Binding kinds (see LetStmt): `var` is mutable with an optional
  // initializer; `let` is immutable and evaluated at runtime; `const` is
  // immutable and must be initialized with a compile-time constant
  // expression. The parser enforces every rule so downstream phases only
  // need to honor immutability.
  const bool isImmutable = keyword.type != Tok::Var;
  const Token& name = consume(Tok::Ident, "expected a variable name");
  ExprPtr init;
  if (match(Tok::Eq)) {
    init = parseExpression();
    // Top-level consts are folded in resolveTopLevelConsts after the whole
    // program is parsed (so they may reference other top-level consts);
    // nested ones must only use literals and already-known values here.
    if (keyword.type == Tok::Const && !atTopLevel_) {
      Value constantValue;
      std::string reason;
      if (!foldConstant(*init, constantValue, reason)) {
        error(keyword, "'const' initializer must be a constant expression: " + reason);
      }
      // Normalize the node to a literal so every backend gets the folded
      // value without repeating the fold.
      init = makeExpr(LiteralExpr{constantValue}, keyword.line, keyword.col);
    }
  } else if (isImmutable) {
    error(peek(), "'" + keyword.lexeme + "' declaration requires a value (use 'var' for a mutable binding)");
  }
  consume(Tok::Semicolon, "expected ';' after variable declaration");
  LetStmt stmt{name.lexeme, std::move(init), keyword.type};
  return makeStmt(std::move(stmt), keyword.line, keyword.col);
}

// Compile-time constant folding for `const` initializers. Supported:
// literals (int/float/string/bool/nil), references to previously declared
// consts, unary +/-/! and binary arithmetic/comparison over constants.
// Anything else (calls, arrays/maps, non-const variables) is rejected with
// a human-readable reason.
bool Parser::foldConstant(const Expr& expr, Value& out, std::string& reason) const {
  // Variant alternatives are addressed by type (never by raw index): the
  // order of ExprNode alternatives changes as nodes are added to the AST.
  if (const auto* lit = std::get_if<LiteralExpr>(&expr.node)) {
    out = lit->value;
    return true;
  }
  if (const auto* var = std::get_if<VariableExpr>(&expr.node)) {
    auto it = constValues_.find(var->name);
    if (it == constValues_.end()) {
      reason = "'" + var->name + "' is not a previously declared 'const'";
      return false;
    }
    out = it->second;
    return true;
  }
  if (const auto* u = std::get_if<UnaryExpr>(&expr.node)) {
    Value operand;
    if (!foldConstant(*u->operand, operand, reason)) return false;
    switch (u->op) {
        case Tok::Bang:
          out = Value(!operand.truthy());
          return true;
        case Tok::Minus:
          if (operand.isInt()) {
            out = Value(static_cast<int64_t>(0ULL - static_cast<uint64_t>(operand.asInt())));
            return true;
          }
          if (operand.isFloat()) { out = Value(-operand.asFloat()); return true; }
          reason = "unary '-' needs a numeric constant";
          return false;
        case Tok::Plus:
          if (operand.isNumber()) { out = operand; return true; }
          reason = "unary '+' needs a numeric constant";
          return false;
        case Tok::Tilde:
          if (operand.isInt()) { out = Value(~operand.asInt()); return true; }
          reason = "unary '~' needs an integer constant";
          return false;
        default:
          reason = "unsupported unary operator in a constant expression";
          return false;
      }
  }
  if (const auto* bPtr = std::get_if<BinaryExpr>(&expr.node)) {
      const BinaryExpr& b = *bPtr;
      // Short-circuit operators fold their operands and apply the same
      // lazy semantics as the interpreter.
      if (b.op == Tok::AmpAmp || b.op == Tok::PipePipe) {
        Value left;
        if (!foldConstant(*b.lhs, left, reason)) return false;
        const bool takeRight = (b.op == Tok::AmpAmp) ? left.truthy() : !left.truthy();
        if (!takeRight) { out = left; return true; }
        return foldConstant(*b.rhs, out, reason);
      }
      Value left;
      Value right;
      if (!foldConstant(*b.lhs, left, reason)) return false;
      if (!foldConstant(*b.rhs, right, reason)) return false;
      // String concatenation of two constant strings.
      if (b.op == Tok::Plus && left.isStr() && right.isStr()) {
        out = Value(left.asStr() + right.asStr());
        return true;
      }
      // Comparisons mirror the interpreter's ordering for numbers, strings
      // and booleans.
      if (isComparisonOp(b.op)) {
        if (b.op == Tok::EqEq) { out = Value(left.equals(right)); return true; }
        if (b.op == Tok::BangEq) { out = Value(!left.equals(right)); return true; }
        bool ordered = false;
        int64_t cmpRank = 0;  // -1: left < right, 0: equal, 1: left > right
        if (left.isNumber() && right.isNumber()) {
          if (left.isInt() && right.isInt()) {
            cmpRank = left.asInt() < right.asInt() ? -1 : (right.asInt() < left.asInt() ? 1 : 0);
          } else {
            cmpRank = left.number() < right.number() ? -1 : (right.number() < left.number() ? 1 : 0);
          }
          ordered = true;
        } else if (left.isStr() && right.isStr()) {
          cmpRank = left.asStr() < right.asStr() ? -1 : (right.asStr() < left.asStr() ? 1 : 0);
          ordered = true;
        } else if (left.isBool() && right.isBool()) {
          cmpRank = (!left.asBool() && right.asBool()) ? -1
                    : ((left.asBool() && !right.asBool()) ? 1 : 0);
          ordered = true;
        }
        if (!ordered) {
          reason = "cannot compare '" + std::string(left.typeName()) + "' with '" +
                   std::string(right.typeName()) + "' in a constant expression";
          return false;
        }
        switch (b.op) {
          case Tok::Lt: out = Value(cmpRank < 0); return true;
          case Tok::LtEq: out = Value(cmpRank <= 0); return true;
          case Tok::Gt: out = Value(cmpRank > 0); return true;
          case Tok::GtEq: out = Value(cmpRank >= 0); return true;
          default: return false;
        }
      }
      // Numeric promotion mirrors the interpreter: int op float -> float.
      if (!left.isNumber() || !right.isNumber()) {
        reason = "arithmetic in a constant expression needs numeric operands";
        return false;
      }
      if (left.isInt() && right.isInt()) {
        const int64_t a = left.asInt();
        const int64_t c = right.asInt();
        switch (b.op) {
          case Tok::Plus: out = Value(wrapAdd(a, c)); return true;
          case Tok::Minus: out = Value(wrapSub(a, c)); return true;
          case Tok::Star: out = Value(wrapMul(a, c)); return true;
          case Tok::Slash:
            if (c == 0) { reason = "division by zero in a constant expression"; return false; }
            if (a == INT64_MIN && c == -1) { out = Value(a); return true; }
            out = Value(a / c); return true;
          case Tok::Percent:
            if (c == 0) { reason = "division by zero in a constant expression"; return false; }
            if (a == INT64_MIN && c == -1) { out = Value(static_cast<int64_t>(0)); return true; }
            out = Value(a % c); return true;
          case Tok::StarStar:
            if (c >= 0) { out = Value(foldIntPow(a, c)); return true; }
            out = Value(std::pow(static_cast<double>(a), static_cast<double>(c))); return true;
          case Tok::Amp: out = Value(a & c); return true;
          case Tok::Pipe: out = Value(a | c); return true;
          case Tok::Caret: out = Value(a ^ c); return true;
          case Tok::Shl:
            if (c < 0 || c > 63) { reason = "shift count must be between 0 and 63"; return false; }
            out = Value(static_cast<int64_t>(static_cast<uint64_t>(a) << static_cast<uint64_t>(c)));
            return true;
          case Tok::Shr:
            if (c < 0 || c > 63) { reason = "shift count must be between 0 and 63"; return false; }
            out = Value(a >> c);
            return true;
          default:
            reason = "unsupported binary operator in a constant expression";
            return false;
        }
      }
      const double a = left.number();
      const double c = right.number();
      switch (b.op) {
        case Tok::Plus: out = Value(a + c); return true;
        case Tok::Minus: out = Value(a - c); return true;
        case Tok::Star: out = Value(a * c); return true;
        case Tok::Slash:
          if (c == 0) { reason = "division by zero in a constant expression"; return false; }
          out = Value(a / c); return true;
        case Tok::Percent:
          if (c == 0) { reason = "division by zero in a constant expression"; return false; }
          out = Value(std::fmod(a, c)); return true;
        case Tok::StarStar: out = Value(std::pow(a, c)); return true;
        default:
          reason = "unsupported binary operator in a constant expression";
          return false;
      }
  }
  reason = "expression is not a compile-time constant";
  return false;
}

StmtPtr Parser::parseFnDecl() {
  const Token& keyword = previous();
  if (check(Tok::LParen)) {
    error(peek(),
          "'fn' declares a named function; write an anonymous function as a lambda: "
          "'x => expr' or '(a, b) => expr'");
  }
  const Token& name = consume(Tok::Ident, "expected a function name");
  auto params = parseParams();
  StmtPtr body;
  if (check(Tok::Arrow)) {
    const Token& arrow = advance();
    const bool isBlock = check(Tok::LBrace);
    body = parseFunctionBody(arrow);
    // An expression body is a statement of its own and needs a terminator.
    if (isBlock) {
      match(Tok::Semicolon);
    } else {
      consume(Tok::Semicolon, "expected ';' after the function body");
    }
  } else {
    body = parseBlock();
    match(Tok::Semicolon);
  }
  return makeStmt(FnDeclStmt{name.lexeme, std::move(params), std::move(body)}, keyword.line,
                  keyword.col);
}

StmtPtr Parser::parseFunctionBody(const Token& arrow) {
  if (check(Tok::LBrace)) return parseBlock();
  auto result = parseExpression();
  std::vector<StmtPtr> statements;
  statements.push_back(makeStmt(ReturnStmt{std::move(result)}, arrow.line, arrow.col));
  return makeStmt(BlockStmt{std::move(statements)}, arrow.line, arrow.col);
}

bool Parser::atLambda() const {
  if (check(Tok::Ident)) return peek(1).type == Tok::Arrow;
  if (!check(Tok::LParen)) return false;
  // `(` [ident {, ident} [,]] `)` `=>`. Parameters are plain identifiers, so
  // this cannot be confused with a parenthesized expression.
  size_t i = 1;
  while (peek(i).type == Tok::Ident) {
    ++i;
    if (peek(i).type != Tok::Comma) break;
    ++i;
  }
  return peek(i).type == Tok::RParen && peek(i + 1).type == Tok::Arrow;
}

ExprPtr Parser::parseLambda() {
  const Token& first = peek();
  std::vector<std::string> params;
  if (check(Tok::Ident)) {
    params.push_back(advance().lexeme);
  } else {
    params = parseParams();
  }
  const Token& arrow = consume(Tok::Arrow, "expected '=>' after the lambda parameters");
  auto body = parseFunctionBody(arrow);
  return makeExpr(LambdaExpr{std::move(params), std::move(body)}, first.line, first.col);
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
  if (match(Tok::Var) || match(Tok::Let) || match(Tok::Const)) {
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
  if (atLambda()) return parseLambda();
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
        // Key forms: identifier, string literal, numeric/bool/nil literal
        // (stored as literalKey), or `[expr]` for computed keys (keyExpr).
        std::string key;
        ExprPtr keyExpr;
        if (check(Tok::Ident)) {
          key = advance().lexeme;
        } else if (check(Tok::Str)) {
          const Token& keyToken = advance();
          if (keyToken.parts.size() != 1 || keyToken.parts[0].isExpr) {
            error(keyToken, "map keys must be plain strings or identifiers");
          }
          key = keyToken.parts[0].text;
        } else if (check(Tok::Int)) {
          key = std::to_string(advance().intValue);
        } else if (check(Tok::Float)) {
          key = Value(previous().floatValue).toString();
        } else if (check(Tok::True)) {
          advance();
          key = "true";
        } else if (check(Tok::False)) {
          advance();
          key = "false";
        } else if (check(Tok::Nil)) {
          advance();
          key = "nil";
        } else if (check(Tok::LBracket)) {
          advance();
          keyExpr = parseExpression();
          consume(Tok::RBracket, "expected ']' after the computed map key");
        } else {
          error(peek(), "expected a string, identifier, scalar literal or [expr] map key");
        }
        consume(Tok::Colon, "expected ':' after the map key");
        entries.push_back(MapEntry{std::move(key), std::move(keyExpr), parseExpression()});
        if (!match(Tok::Comma)) break;
      }
    }
    consume(Tok::RBrace, "expected '}' to close the map literal");
    return makeExpr(MapExpr{std::move(entries)}, token.line, token.col);
  }
  if (check(Tok::Fn)) {
    error(peek(),
          "'fn' only declares named functions; write a lambda as 'x => expr' or "
          "'(a, b) => expr'");
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
        piece.content = sub.parseFullExpression();
      } catch (const CompileError& err) {
        throw CompileError{"in string interpolation: " + err.message, err.line, err.col};
      }
    } else {
      piece.content = part.text;
    }
    node.parts.push_back(std::move(piece));
  }
  return makeExpr(std::move(node), token.line, token.col);
}
