#include "compiler.h"

#include <memory>
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

// Unary operators with their own semantics. `+x` is compiled as `x` instead.
bool unaryOpCode(Tok op, UnaryOp& out) {
  switch (op) {
    case Tok::Minus:
      out = UnaryOp::Negate;
      return true;
    case Tok::Bang:
      out = UnaryOp::Not;
      return true;
    case Tok::Tilde:
      out = UnaryOp::BitNot;
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
  int reg = nextReg_++;
  if (nextReg_ > maxReg_) maxReg_ = nextReg_;
  return reg;
}

void Compiler::freeReg(int count) {
  nextReg_ -= count;
  if (nextReg_ < 0) nextReg_ = 0;
}

int Compiler::addConstant(const Value& value) {
  for (size_t i = 0; i < bc_.constants.size(); ++i) {
    const Value& existing = bc_.constants[i];
    // The type is part of the identity: `1` (int) and `1.0` (float) are not
    // the same constant even though they are numerically equal.
    if (existing.type() == value.type() && existing.equals(value)) {
      return static_cast<int>(i);
    }
  }
  bc_.constants.push_back(value);
  return static_cast<int>(bc_.constants.size() - 1);
}

int Compiler::addString(const std::string& value) { return addConstant(Value(value)); }

int Compiler::emit(OpCode op, int a, int b, int c) {
  Instruction instruction;
  instruction.op = op;
  instruction.a = a;
  instruction.b = b;
  instruction.c = c;
  instruction.line = currentLine_;
  instruction.col = currentCol_;
  return bytecodeEmit(bc_, instruction);
}

int Compiler::emitJump(OpCode op, int a) {
  Instruction instruction;
  instruction.op = op;
  instruction.a = a;
  instruction.jump = -1;  // pending backpatching
  instruction.line = currentLine_;
  instruction.col = currentCol_;
  return bytecodeEmit(bc_, instruction);
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
  scope.baseSlot = nextLocal_;
  scopes_.push_back(std::move(scope));
}

void Compiler::popScope() {
  if (scopes_.size() <= 1) return;
  nextLocal_ = scopes_.back().baseSlot;
  scopes_.pop_back();
}

int Compiler::declareLocal(const std::string& name, bool isImmutable) {
  int slot = nextLocal_++;
  if (nextLocal_ > maxLocal_) maxLocal_ = nextLocal_;
  Scope& scope = scopes_.back();
  scope.vars[name] = LocalVar{slot, isImmutable};
  return slot;
}

bool Compiler::resolveLocalIn(const std::vector<Scope>& scopes, const std::string& name,
                              LocalVar& out) {
  // From the innermost scope to the outermost: the first match wins (shadowing).
  for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
    auto found = it->vars.find(name);
    if (found != it->vars.end()) {
      out = found->second;
      return true;
    }
  }
  return false;
}

bool Compiler::resolveLocal(const std::string& name, LocalVar& out) const {
  return resolveLocalIn(scopes_, name, out);
}

// ------------------------------------------------------------------- upvalues

// Adds a capture for `name` to `proto`, whose scopes live in `scopes` and
// whose transitive captures are tracked in `names`. Returns the index of the
// new (or already present) entry in `proto.upvalues`, and keeps `names`
// parallel to that vector so later lookups by name work.

// Walks out from the frame described by (`scopes`, `proto`, `names`) looking
// for `name`. If it is a local of this frame, captures it directly; otherwise
// recurses into the enclosing function (when provided) and re-exports that
// frame's upvalue index. Returns -1 when nothing captures the name, i.e. it
// resolves to a global.
int Compiler::resolveFromFrame(std::vector<Scope>& scopes, Bytecode& proto,
                               std::unordered_map<std::string, int>& names,
                               FuncState* parent, const std::string& name) {
  LocalVar local;
  if (resolveLocalIn(scopes, name, local)) {
    return captureVariable(scopes, proto, names, name);
  }
  if (!parent) return -1;
  int parentIndex = resolveFromFrame(parent->scopes, parent->bc, parent->upvalueNames,
                                     parent->enclosing, name);
  if (parentIndex < 0) return -1;
  // Re-export: the same variable is reached through the parent's slot in its
  // own capture list, so nested closures share one cell per variable.
  auto existing = names.find(name);
  if (existing != names.end()) return existing->second;
  proto.upvalues.push_back(UpvalDesc{false, parentIndex});
  names[name] = static_cast<int>(proto.upvalues.size()) - 1;
  return static_cast<int>(proto.upvalues.size()) - 1;
}

// Returns true when `proto` is the frame currently being compiled (its state
// lives in `bc_`, `maxLocal_` and friends instead of a saved FuncState).
bool Compiler::isCurrentFrame(const Bytecode& proto) const {
  if (funcStack_.empty()) return &proto == &bc_;
  return &proto == &funcStack_.back().bc;
}

int Compiler::captureVariable(std::vector<Scope>& scopes, Bytecode& proto,
                              std::unordered_map<std::string, int>& names,
                              const std::string& name) {
  auto existing = names.find(name);
  if (existing != names.end()) return existing->second;
  LocalVar local;
  if (!resolveLocalIn(scopes, name, local)) return -1;
  proto.upvalues.push_back(UpvalDesc{true, local.slot});
  names[name] = static_cast<int>(proto.upvalues.size()) - 1;
  // Block-scoped variables can leave their scope (and lower the frame's
  // high-water mark) before an inner closure captures them. The VM sizes a
  // frame's local window from `numLocals`, so raise it to cover every
  // captured slot even after the variable goes out of scope (PHASE 6b fix).
  const int needed = local.slot + 1;
  if (isCurrentFrame(proto)) {
    if (needed > maxLocal_) maxLocal_ = needed;
  } else {
    for (FuncState& state : funcStack_) {
      if (&state.bc == &proto) {
        if (needed > state.maxLocal) state.maxLocal = needed;
        break;
      }
    }
  }
  return names[name];
}

int Compiler::resolveUpvalue(const std::string& name) {
  return resolveFromFrame(scopes_, bc_, currentUpvalueNames_,
                          funcStack_.empty() ? nullptr : &funcStack_.back(), name);
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
  bytecodeInit(bc_);
  nextReg_ = 0;
  maxReg_ = 0;
  nextLocal_ = 0;
  maxLocal_ = 0;
  scopes_.clear();
  loops_.clear();
  globalConsts_.clear();
  inFunction_ = false;

  if (!program) fail("empty program", 1, 1);

  // The main chunk uses the global scope: its `let`s are globals, as in the
  // interpreter (and so the REPL keeps definitions between entries).
  scopes_.push_back(Scope{});
  if (const auto* block = std::get_if<BlockStmt>(&program->node)) {
    // Top-level function declarations are hoisted, like `execBlock` does in
    // the interpreter, so a call can appear before the declaration.
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      }
    }
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (std::holds_alternative<FnDeclStmt>(child->node)) continue;
      compileStmt(*child);
    }
  } else {
    compileStmt(*program);
  }

  emit(OpCode::HALT);

  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;
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
  currentLine_ = stmt.line;
  currentCol_ = stmt.col;
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
          [&](const FnDeclStmt& s) { compileFnDecl(s, stmt.line, stmt.col); },
          [&](const BlockStmt& s) {
            pushScope();
            compileBlockBody(s);
            popScope();
          },
          [&](const ReturnStmt& s) { compileReturn(s); },
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
    emit(OpCode::LOADK, reg, addConstant(Value()));
  }

  if (inGlobalScope()) {
    // Top level: global variable (persists in the REPL).
    emit(OpCode::SETGLOBAL, reg, addString(s.name));
    declareGlobal(s.name, s.kind != Tok::Var);
  } else if (s.kind == Tok::Var && scopes_.size() > 1) {
    // `var` redeclares an existing binding of an enclosing scope in the same
    // frame: it assigns to that slot instead of creating a new cell, matching
    // the interpreter's `Env::define` overwrite behavior.
    LocalVar outer;
    const bool reassign =
        resolveLocalIn(std::vector<Scope>(scopes_.begin(), scopes_.end() - 1), s.name, outer);
    if (reassign) {
      emit(OpCode::SETLOCAL, reg, outer.slot);
    } else {
      int slot = declareLocal(s.name, false);
      emit(OpCode::DECLLOCAL, reg, slot);
    }
  } else {
    // Nested block: frame-local variable. The declaration happens after the
    // initializer is compiled, exactly like `Env::define`.
    int slot = declareLocal(s.name, s.kind != Tok::Var);
    emit(OpCode::DECLLOCAL, reg, slot);
  }
  freeReg();
}

void Compiler::compileFnDecl(const FnDeclStmt& fn, int line, int col) {
  currentLine_ = line;
  currentCol_ = col;
  int child = compileFunctionProto(fn.name, fn.params, fn.body, line, col);
  int reg = allocReg();
  emit(OpCode::CLOSURE, reg, child);

  if (inGlobalScope()) {
    // Top-level functions are globals; the interpreter hoists them in the global
    // scope, where they persist across REPL entries.
    emit(OpCode::SETGLOBAL, reg, addString(fn.name));
    declareGlobal(fn.name, false);
  } else {
    // Local declaration: a frame slot holding the closure value (PHASE 6b).
    // Unlike `let`, a function name is mutable, matching the interpreter.
    int slot = declareLocal(fn.name, false);
    emit(OpCode::DECLLOCAL, reg, slot);
  }
  freeReg();
}

void Compiler::compileReturn(const ReturnStmt& stmt) {
  if (!inFunction_) fail("'return' outside of a function", currentLine_, currentCol_);

  int reg;
  if (stmt.value) {
    reg = compileExpr(*stmt.value);
  } else {
    reg = allocReg();
    emit(OpCode::LOADK, reg, addConstant(Value()));
  }
  emit(OpCode::RETURN, reg);
  freeReg();
}

int Compiler::compileFunctionProto(const std::string& name,
                                   const std::vector<std::string>& params, const StmtPtr& body,
                                   int line, int col) {
  // Save the parent chunk and its compilation state. Functions are compiled
  // into their own proto with fresh registers, locals, scopes and loops.
  Bytecode parent = std::move(bc_);
  auto parentUpvalueNames = std::move(currentUpvalueNames_);
  const int parentNextReg = nextReg_;
  const int parentMaxReg = maxReg_;
  const int parentNextLocal = nextLocal_;
  const int parentMaxLocal = maxLocal_;
  std::vector<Scope> parentScopes = std::move(scopes_);
  std::vector<Loop> parentLoops = std::move(loops_);
  const int parentLine = currentLine_;
  const int parentCol = currentCol_;
  const bool parentInFunction = inFunction_;

  bc_ = Bytecode{};
  bc_.name = name;
  bc_.numParams = static_cast<int>(params.size());
  currentUpvalueNames_.clear();
  nextReg_ = 0;
  maxReg_ = 0;
  nextLocal_ = 0;
  maxLocal_ = 0;
  scopes_.clear();
  loops_.clear();
  inFunction_ = true;

  // The child frame keeps the parent's state alive so nested closures can
  // resolve and re-export upvalues through it (see `resolveFromFrame`). A
  // deque keeps these addresses stable while more frames are pushed.
  FuncState saved;
  saved.bc = std::move(parent);
  saved.scopes = std::move(parentScopes);
  saved.loops = std::move(parentLoops);
  saved.nextReg = parentNextReg;
  saved.maxReg = parentMaxReg;
  saved.nextLocal = parentNextLocal;
  saved.maxLocal = parentMaxLocal;
  saved.line = parentLine;
  saved.col = parentCol;
  saved.inFunction = parentInFunction;
  saved.upvalueNames = std::move(parentUpvalueNames);
  saved.enclosing = funcStack_.empty() ? nullptr : &funcStack_.back();
  funcStack_.push_back(std::move(saved));

  // The function body lives in the parameter scope; it is not a nested block.
  pushScope();
  for (const std::string& param : params) declareLocal(param, false);

  // Incoming arguments already sit in R0 .. R(n-1), so the prologue only has
  // to box each one into its cell. Afterwards the registers are reusable.
  for (size_t i = 0; i < params.size(); ++i) {
    emit(OpCode::DECLLOCAL, static_cast<int>(i), static_cast<int>(i));
  }
  maxReg_ = bc_.numParams;
  nextReg_ = 0;

  if (!body) {
    fail("internal compiler error: function body is missing", line, col);
  }
  if (const auto* block = std::get_if<BlockStmt>(&body->node)) {
    compileBlockBody(*block);
  } else {
    compileStmt(*body);
  }

  // Falling off the end of a function returns nil, like the interpreter.
  int nilReg = allocReg();
  emit(OpCode::LOADK, nilReg, addConstant(Value()));
  emit(OpCode::RETURN, nilReg);

  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;

  Bytecode proto = std::move(bc_);
  // The frame pushed for this child is finished; its `enclosing` pointer was
  // the previous top, so popping it leaves the parent frame valid.
  FuncState savedParent = std::move(funcStack_.back());
  funcStack_.pop_back();
  bc_ = std::move(savedParent.bc);
  currentUpvalueNames_ = std::move(savedParent.upvalueNames);
  nextReg_ = savedParent.nextReg;
  maxReg_ = savedParent.maxReg;
  nextLocal_ = savedParent.nextLocal;
  maxLocal_ = savedParent.maxLocal;
  scopes_ = std::move(savedParent.scopes);
  loops_ = std::move(savedParent.loops);
  currentLine_ = savedParent.line;
  currentCol_ = savedParent.col;
  inFunction_ = savedParent.inFunction;

  int index = static_cast<int>(bc_.children.size());
  bc_.children.push_back(std::make_shared<Bytecode>(std::move(proto)));
  return index;
}

int Compiler::compileLambda(const LambdaExpr& lambda, int line, int col) {
  // A lambda is an anonymous function value: compile its body as a child proto
  // and instantiate it with CLOSURE, exactly like a named declaration but
  // without binding a name anywhere.
  currentLine_ = line;
  currentCol_ = col;
  int child = compileFunctionProto("<anonymous>", lambda.params, lambda.body, line, col);
  int reg = allocReg();
  emit(OpCode::CLOSURE, reg, child);
  return reg;
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
  } else if (int upvalue = resolveUpvalue(name); upvalue >= 0) {
    // Captured by an enclosing function: a shared cell reached through the
    // closure's upvalue list (PHASE 6b).
    emit(OpCode::GETUPVAL, reg, upvalue);
  } else {
    emit(OpCode::GETGLOBAL, reg, addString(name));
  }
  return reg;
}

int Compiler::compileCall(const CallExpr& call, int line, int col) {
  // `CALL` reads a contiguous window, so the callee and every argument must be
  // in consecutive registers. The counter discipline of `compileExpr` already
  // guarantees it; the check turns a future regression into a clear error.
  int callee = compileExpr(*call.callee);
  for (const auto& arg : call.args) {
    int expected = nextReg_;
    int reg = compileExpr(*arg);
    if (reg != expected) {
      fail("internal compiler error: call arguments are not contiguous", line, col);
    }
  }

  currentLine_ = line;
  currentCol_ = col;
  emit(OpCode::CALL, callee, static_cast<int>(call.args.size()));
  freeReg(static_cast<int>(call.args.size()));
  return callee;
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
  // A captured local of an enclosing function becomes an upvalue read/write on
  // the shared cell (PHASE 6b). The lookup must happen after the compile of
  // the value below only insofar as register allocation is concerned; doing it
  // first keeps the resolution stable and mirrors `compileVariable`.
  int upvalueSlot = isLocal ? -1 : resolveUpvalue(name);
  if (!isLocal && upvalueSlot < 0 && globalConsts_.find(name) != globalConsts_.end()) {
    fail("cannot assign to immutable variable '" + name + "'", line, col);
  }

  if (assign.op == Tok::Eq) {
    int value = compileExpr(*assign.value);
    if (isLocal) {
      emit(OpCode::SETLOCAL, value, local.slot);
    } else if (upvalueSlot >= 0) {
      emit(OpCode::SETUPVAL, value, upvalueSlot);
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
  } else if (upvalueSlot >= 0) {
    emit(OpCode::GETUPVAL, current, upvalueSlot);
  } else {
    emit(OpCode::GETGLOBAL, current, addString(name));
  }
  int right = compileExpr(*assign.value);
  emit(op, current, current, right);
  freeReg();

  if (isLocal) {
    emit(OpCode::SETLOCAL, current, local.slot);
  } else if (upvalueSlot >= 0) {
    emit(OpCode::SETUPVAL, current, upvalueSlot);
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
  currentLine_ = expr.line;
  currentCol_ = expr.col;
  return std::visit(
      Overloaded{
          [&](const LiteralExpr& literal) -> int {
            const Value& value = literal.value;
            // Every literal, including nil/booleans/strings, lives in the same
            // tagged pool; `LOADK` uses the value's type at runtime.
            if (value.isNil() || value.isBool() || value.isNumber() || value.isStr()) {
              int reg = allocReg();
              emit(OpCode::LOADK, reg, addConstant(value));
              return reg;
            }
            fail("unsupported literal in the bytecode compiler", expr.line, expr.col);
          },
          [&](const VariableExpr& variable) -> int { return compileVariable(variable.name); },
          [&](const UnaryExpr& unary) -> int {
            int operand = compileExpr(*unary.operand);
            if (unary.op == Tok::Plus) return operand;  // +x is x
            UnaryOp op;
            if (!unaryOpCode(unary.op, op)) {
              fail("unsupported unary operator", expr.line, expr.col);
            }
            // Point runtime errors at the operator, like the interpreter.
            currentLine_ = expr.line;
            currentCol_ = expr.col;
            emit(OpCode::UNOP, operand, operand, static_cast<int>(op));
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
            // Point runtime errors at the operator, like the interpreter.
            currentLine_ = binary.opLine;
            currentCol_ = binary.opCol;
            emit(op, left, left, right);
            freeReg();
            return left;
          },
          [&](const AssignExpr& assign) -> int {
            return compileAssign(assign, expr.line, expr.col);
          },
          [&](const CallExpr& call) -> int { return compileCall(call, expr.line, expr.col); },
          [&](const LambdaExpr& lambda) -> int { return compileLambda(lambda, expr.line, expr.col); },
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
  // Only the single-argument form uses the PRINT opcode, which needs no
  // registered builtin. Other arities become normal calls to `print`.
  if (!call || !call->callee || call->args.size() != 1) return false;
  const auto* callee = std::get_if<VariableExpr>(&call->callee->node);
  if (callee == nullptr || callee->name != "print") return false;
  // A local named `print` shadows the builtin and must be called normally.
  LocalVar local;
  return !resolveLocal("print", local);
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
