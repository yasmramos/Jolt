#include "compiler.h"

#include <string>
#include <utility>
#include <variant>

#include "errors.h"
#include "util.h"

namespace {

// Throws a compile error with the position of the current node.
[[noreturn]] void fail(const std::string& message, int line, int col) {
  throw CompileError{message, line, col};
}

// Human-readable operator name, used only for diagnostics.
std::string opName(Tok op) {
  switch (op) {
    case Tok::Plus:
      return "+";
    case Tok::Minus:
      return "-";
    case Tok::Star:
      return "*";
    case Tok::Slash:
      return "/";
    case Tok::Percent:
      return "%";
    case Tok::StarStar:
      return "**";
    case Tok::EqEq:
      return "==";
    case Tok::BangEq:
      return "!=";
    case Tok::Lt:
      return "<";
    case Tok::LtEq:
      return "<=";
    case Tok::Gt:
      return ">";
    case Tok::GtEq:
      return ">=";
    case Tok::AmpAmp:
      return "&&";
    case Tok::PipePipe:
      return "||";
    case Tok::Amp:
      return "&";
    case Tok::Pipe:
      return "|";
    case Tok::Caret:
      return "^";
    case Tok::Shl:
      return "<<";
    case Tok::Shr:
      return ">>";
    case Tok::In:
      return "in";
    default:
      return "?";
  }
}

// Binary operators that already have a dedicated opcode.
bool binaryOpCode(Tok op, OpCode& out) {
  switch (op) {
    case Tok::Plus:
      out = OpCode::ADD;
      return true;
    case Tok::Minus:
      out = OpCode::SUB;
      return true;
    case Tok::Star:
      out = OpCode::MUL;
      return true;
    case Tok::Slash:
      out = OpCode::DIV;
      return true;
    case Tok::Percent:
      out = OpCode::MOD;
      return true;
    case Tok::StarStar:
      out = OpCode::POW;
      return true;
    case Tok::Amp:
      out = OpCode::BAND;
      return true;
    case Tok::Pipe:
      out = OpCode::BOR;
      return true;
    case Tok::Caret:
      out = OpCode::BXOR;
      return true;
    case Tok::Shl:
      out = OpCode::SHL;
      return true;
    case Tok::Shr:
      out = OpCode::SHR;
      return true;
    case Tok::EqEq:
      out = OpCode::EQ;
      return true;
    case Tok::BangEq:
      out = OpCode::NEQ;
      return true;
    case Tok::Lt:
      out = OpCode::LT;
      return true;
    case Tok::LtEq:
      out = OpCode::LTE;
      return true;
    case Tok::Gt:
      out = OpCode::GT;
      return true;
    case Tok::GtEq:
      out = OpCode::GTE;
      return true;
    default:
      return false;
  }
}

// Compound assignment operators: `x op= y`.
bool compoundOpCode(Tok op, OpCode& out) {
  switch (op) {
    case Tok::PlusEq:
      out = OpCode::ADD;
      return true;
    case Tok::MinusEq:
      out = OpCode::SUB;
      return true;
    case Tok::StarEq:
      out = OpCode::MUL;
      return true;
    case Tok::SlashEq:
      out = OpCode::DIV;
      return true;
    case Tok::PercentEq:
      out = OpCode::MOD;
      return true;
    case Tok::StarStarEq:
      out = OpCode::POW;
      return true;
    default:
      return false;
  }
}

}  // namespace

// ------------------------------------------------------------ registers and pools

int Compiler::allocReg() {
  int reg = next_reg_++;
  if (next_reg_ > max_reg_) max_reg_ = next_reg_;
  return reg;
}

void Compiler::freeReg(int count) {
  next_reg_ -= count;
  if (next_reg_ < 0) next_reg_ = 0;
}

int Compiler::addConstant(const Value& value) {
  for (size_t i = 0; i < bc_.numbers.size(); ++i) {
    const Value& existing = bc_.numbers[i];
    // The type is part of the identity: `1` (int) and `1.0` (float) are not
    // the same constant even though they are numerically equal.
    if (existing.type() == value.type() && existing.equals(value)) {
      return static_cast<int>(i);
    }
  }
  bc_.numbers.push_back(value);
  return static_cast<int>(bc_.numbers.size() - 1);
}

int Compiler::addString(const std::string& value) {
  for (size_t i = 0; i < bc_.strings.size(); ++i) {
    if (bc_.strings[i] == value) return static_cast<int>(i);
  }
  bc_.strings.push_back(value);
  return static_cast<int>(bc_.strings.size() - 1);
}

int Compiler::emit(OpCode op, int a, int b, int c) {
  Instruction instruction;
  instruction.op = op;
  instruction.a = a;
  instruction.b = b;
  instruction.c = c;
  return bytecode_emit(bc_, instruction);
}

int Compiler::emitJump(OpCode op, int a) {
  Instruction instruction;
  instruction.op = op;
  instruction.a = a;
  instruction.jump = -1;  // pending backpatching
  return bytecode_emit(bc_, instruction);
}

void Compiler::patchJump(int index, int target) {
  if (index < 0 || static_cast<size_t>(index) >= bc_.code.size()) {
    fail("internal compiler error: invalid jump", 0, 0);
  }
  bc_.code[static_cast<size_t>(index)].jump = target;
}

int Compiler::currentOffset() const { return static_cast<int>(bc_.code.size()); }

// ------------------------------------------------------------------ scopes

void Compiler::pushScope() {
  Scope scope;
  scope.baseSlot = next_local_;
  scopes_.push_back(std::move(scope));
}

void Compiler::popScope() {
  if (scopes_.size() <= 1) return;
  next_local_ = scopes_.back().baseSlot;
  scopes_.pop_back();
}

int Compiler::declareLocal(const std::string& name, bool isImmutable) {
  int slot = next_local_++;
  if (next_local_ > max_local_) max_local_ = next_local_;
  Scope& scope = scopes_.back();
  scope.vars[name] = LocalVar{slot, isImmutable};
  return slot;
}

bool Compiler::resolveLocal(const std::string& name, LocalVar& out) const {
  // From the innermost scope to the outermost: the first match wins (shadowing).
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto found = it->vars.find(name);
    if (found != it->vars.end()) {
      out = found->second;
      return true;
    }
  }
  return false;
}

void Compiler::declareGlobal(const std::string& name, bool isImmutable) {
  if (isImmutable) {
    globalConsts_.insert(name);
  } else {
    globalConsts_.erase(name);
  }
}

// -------------------------------------------------------------------- program

Bytecode Compiler::compile(const StmtPtr& program) {
  bytecode_init(bc_);
  next_reg_ = 0;
  max_reg_ = 0;
  next_local_ = 0;
  max_local_ = 0;
  scopes_.clear();
  loops_.clear();
  globalConsts_.clear();

  if (!program) fail("empty program", 1, 1);

  // The main chunk uses the global scope: its `let`s are globals, as in the
  // interpreter (and so the REPL keeps definitions between entries).
  scopes_.push_back(Scope{});
  if (const auto* block = std::get_if<BlockStmt>(&program->node)) {
    compileBlockBody(*block);
  } else {
    compileStmt(*program);
  }

  emit(OpCode::HALT);

  bc_.num_registers = max_reg_;
  bc_.num_locals = max_local_;
  return std::move(bc_);
}

Bytecode compileProgram(const StmtPtr& program) { return Compiler{}.compile(program); }

// ----------------------------------------------------------------- statements

void Compiler::compileBlockBody(const BlockStmt& block) {
  for (const auto& child : block.stmts) {
    if (child) compileStmt(*child);
  }
}

void Compiler::compileScopedBlock(const Stmt& stmt) {
  const auto* block = std::get_if<BlockStmt>(&stmt.node);
  if (!block) {
    compileStmt(stmt);
    return;
  }
  pushScope();
  compileBlockBody(*block);
  popScope();
}

void Compiler::compileStmt(const Stmt& stmt) {
  std::visit(
      Overloaded{
          [&](const ExprStmt& s) {
            if (isPrintCall(*s.expr)) {
              compilePrint(*s.expr);
            } else {
              compileExpr(*s.expr);
              freeReg();
            }
          },
          [&](const LetStmt& s) { compileLet(s); },
          [&](const BlockStmt& s) {
            pushScope();
            compileBlockBody(s);
            popScope();
          },
          [&](const IfStmt& s) { compileIf(s); },
          [&](const WhileStmt& s) { compileWhile(s); },
          [&](const ForStmt& s) { compileFor(s); },
          [&](const BreakStmt&) { compileBreak(stmt); },
          [&](const ContinueStmt&) { compileContinue(stmt); },
          [&](const auto&) {
            fail("this statement is not supported by the bytecode compiler yet (Phase 6)",
                 stmt.line, stmt.col);
          },
      },
      stmt.node);
}

void Compiler::compileLet(const LetStmt& s) {
  int reg;
  if (s.init) {
    reg = compileExpr(*s.init);
  } else {
    reg = allocReg();
    emit(OpCode::LOADNIL, reg);
  }

  if (inGlobalScope()) {
    // Top level: global variable (persists in the REPL).
    emit(OpCode::SETGLOBAL, reg, addString(s.name));
    declareGlobal(s.name, s.kind != Tok::Var);
  } else {
    // Nested block: frame-local variable. The declaration happens after the
    // initializer is compiled, exactly like `Env::define`.
    int slot = declareLocal(s.name, s.kind != Tok::Var);
    emit(OpCode::DECLLOCAL, reg, slot);
  }
  freeReg();
}

void Compiler::compileIf(const IfStmt& s) {
  int condition = compileExpr(*s.condition);
  int elseJump = emitJump(OpCode::JMP_IF_FALSE, condition);
  freeReg();

  // The `then` branch always creates a scope, like `execBlock(..., true)`.
  compileScopedBlock(*s.thenBranch);

  if (s.elseBranch) {
    int endJump = emitJump(OpCode::JMP);
    patchJump(elseJump, currentOffset());
    // `else { ... }` creates a scope because it is a BlockStmt; `else if` is
    // compiled in the current scope, exactly like `exec` in the interpreter.
    compileStmt(*s.elseBranch);
    patchJump(endJump, currentOffset());
  } else {
    patchJump(elseJump, currentOffset());
  }
}

void Compiler::compileWhile(const WhileStmt& s) {
  int loopStart = currentOffset();
  int condition = compileExpr(*s.condition);
  int exitJump = emitJump(OpCode::JMP_IF_FALSE, condition);
  freeReg();

  loops_.push_back(Loop{});
  compileScopedBlock(*s.body);
  Loop loop = std::move(loops_.back());
  loops_.pop_back();

  int backJump = emitJump(OpCode::JMP);
  patchJump(backJump, loopStart);

  int after = currentOffset();
  patchJump(exitJump, after);
  for (int index : loop.breaks) patchJump(index, after);
  for (int index : loop.continues) patchJump(index, loopStart);  // re-evaluates the condition
}

void Compiler::compileFor(const ForStmt& s) {
  // `for` creates its own scope for the initializer; the body creates another.
  pushScope();
  if (s.init) compileStmt(*s.init);

  int loopStart = currentOffset();
  int exitJump = -1;
  if (s.condition) {
    int condition = compileExpr(*s.condition);
    exitJump = emitJump(OpCode::JMP_IF_FALSE, condition);
    freeReg();
  }

  loops_.push_back(Loop{});
  compileScopedBlock(*s.body);
  Loop loop = std::move(loops_.back());
  loops_.pop_back();

  // `continue` jumps to the step, not to the condition.
  int stepStart = currentOffset();
  if (s.step) {
    compileExpr(*s.step);
    freeReg();
  }

  int backJump = emitJump(OpCode::JMP);
  patchJump(backJump, loopStart);

  int after = currentOffset();
  if (exitJump >= 0) patchJump(exitJump, after);
  for (int index : loop.breaks) patchJump(index, after);
  for (int index : loop.continues) patchJump(index, stepStart);

  popScope();
}

void Compiler::compileBreak(const Stmt& stmt) {
  if (loops_.empty()) fail("'break' outside of a loop", stmt.line, stmt.col);
  int jump = emitJump(OpCode::JMP);
  loops_.back().breaks.push_back(jump);
}

void Compiler::compileContinue(const Stmt& stmt) {
  if (loops_.empty()) fail("'continue' outside of a loop", stmt.line, stmt.col);
  int jump = emitJump(OpCode::JMP);
  loops_.back().continues.push_back(jump);
}

// ---------------------------------------------------------------- expressions

int Compiler::compileVariable(const std::string& name) {
  LocalVar local;
  int reg = allocReg();
  if (resolveLocal(name, local)) {
    emit(OpCode::GETLOCAL, reg, local.slot);
  } else {
    emit(OpCode::GETGLOBAL, reg, addString(name));
  }
  return reg;
}

int Compiler::compileAssign(const AssignExpr& assign, int line, int col) {
  const auto* target = std::get_if<VariableExpr>(&assign.target->node);
  if (!target) {
    fail("only assignment to variables is supported by the bytecode compiler yet (Phase 6)",
         line, col);
  }

  const std::string& name = target->name;
  LocalVar local;
  bool isLocal = resolveLocal(name, local);

  if (isLocal && local.isImmutable) {
    fail("cannot assign to immutable variable '" + name + "'", line, col);
  }
  if (!isLocal && globalConsts_.find(name) != globalConsts_.end()) {
    fail("cannot assign to immutable variable '" + name + "'", line, col);
  }

  if (assign.op == Tok::Eq) {
    int value = compileExpr(*assign.value);
    if (isLocal) {
      emit(OpCode::SETLOCAL, value, local.slot);
    } else {
      emit(OpCode::SETGLOBAL, value, addString(name));
    }
    return value;
  }

  OpCode op;
  if (!compoundOpCode(assign.op, op)) {
    fail("unsupported compound assignment operator", assign.opLine, assign.opCol);
  }

  int current = allocReg();
  if (isLocal) {
    emit(OpCode::GETLOCAL, current, local.slot);
  } else {
    emit(OpCode::GETGLOBAL, current, addString(name));
  }
  int right = compileExpr(*assign.value);
  emit(op, current, current, right);
  freeReg();

  if (isLocal) {
    emit(OpCode::SETLOCAL, current, local.slot);
  } else {
    emit(OpCode::SETGLOBAL, current, addString(name));
  }
  return current;
}

int Compiler::compileTernary(const TernaryExpr& ternary) {
  int result = compileExpr(*ternary.condition);
  int elseJump = emitJump(OpCode::JMP_IF_FALSE, result);

  int thenReg = compileExpr(*ternary.thenExpr);
  emit(OpCode::MOVE, result, thenReg);
  freeReg();

  int endJump = emitJump(OpCode::JMP);
  patchJump(elseJump, currentOffset());

  int elseReg = compileExpr(*ternary.elseExpr);
  emit(OpCode::MOVE, result, elseReg);
  freeReg();

  patchJump(endJump, currentOffset());
  return result;
}

int Compiler::compileExpr(const Expr& expr) {
  return std::visit(
      Overloaded{
          [&](const LiteralExpr& literal) -> int {
            const Value& value = literal.value;
            if (value.isNil()) {
              int reg = allocReg();
              emit(OpCode::LOADNIL, reg);
              return reg;
            }
            if (value.isBool()) {
              int reg = allocReg();
              emit(OpCode::LOADBOOL, reg, value.asBool() ? 1 : 0);
              return reg;
            }
            if (value.isNumber()) {
              int reg = allocReg();
              emit(OpCode::LOADK, reg, addConstant(value));
              return reg;
            }
            if (value.isStr()) {
              int reg = allocReg();
              emit(OpCode::LOADKS, reg, addString(value.asStr()));
              return reg;
            }
            fail("unsupported literal in the bytecode compiler", expr.line, expr.col);
          },
          [&](const VariableExpr& variable) -> int { return compileVariable(variable.name); },
          [&](const UnaryExpr& unary) -> int {
            int operand = compileExpr(*unary.operand);
            switch (unary.op) {
              case Tok::Minus:
                emit(OpCode::NEG, operand, operand);
                break;
              case Tok::Plus:
                break;  // +x is x
              case Tok::Bang:
                emit(OpCode::NOT, operand, operand);
                break;
              case Tok::Tilde:
                emit(OpCode::BNOT, operand, operand);
                break;
              default:
                fail("unsupported unary operator", expr.line, expr.col);
            }
            return operand;
          },
          [&](const BinaryExpr& binary) -> int {
            // Short circuit: the right-hand side is not evaluated unless needed.
            if (binary.op == Tok::AmpAmp || binary.op == Tok::PipePipe) {
              int left = compileExpr(*binary.lhs);
              int endJump = emitJump(binary.op == Tok::AmpAmp ? OpCode::JMP_IF_FALSE
                                                              : OpCode::JMP_IF_TRUE,
                                     left);
              int right = compileExpr(*binary.rhs);
              emit(OpCode::MOVE, left, right);
              freeReg();
              patchJump(endJump, currentOffset());
              return left;
            }

            OpCode op;
            if (!binaryOpCode(binary.op, op)) {
              fail("binary operator '" + opName(binary.op) +
                       "' is not supported by the bytecode compiler yet (Phase 6)",
                   binary.opLine, binary.opCol);
            }

            // The left operand is compiled and its register is reused as the
            // destination; the right one is a temporary freed afterwards.
            int left = compileExpr(*binary.lhs);
            int right = compileExpr(*binary.rhs);
            emit(op, left, left, right);
            freeReg();
            return left;
          },
          [&](const AssignExpr& assign) -> int {
            return compileAssign(assign, expr.line, expr.col);
          },
          [&](const TernaryExpr& ternary) -> int { return compileTernary(ternary); },
          [&](const auto&) -> int {
            fail("this expression is not supported by the bytecode compiler yet (Phase 6)",
                 expr.line, expr.col);
          },
      },
      expr.node);
}

// ---------------------------------------------------------------------- print

bool Compiler::isPrintCall(const Expr& expr) const {
  const auto* call = std::get_if<CallExpr>(&expr.node);
  if (!call || !call->callee) return false;
  const auto* callee = std::get_if<VariableExpr>(&call->callee->node);
  return callee != nullptr && callee->name == "print";
}

void Compiler::compilePrint(const Expr& expr) {
  const auto& call = std::get<CallExpr>(expr.node);
  if (call.args.size() != 1) {
    fail("print(...) expects exactly one argument in the bytecode compiler", expr.line, expr.col);
  }
  int reg = compileExpr(*call.args[0]);
  emit(OpCode::PRINT, reg);
  freeReg();
}
