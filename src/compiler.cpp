#include "compiler.h"

#include <algorithm>
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

int Compiler::allocLocalSlot() {
  int slot = nextLocal_++;
  if (nextLocal_ > maxLocal_) maxLocal_ = nextLocal_;
  return slot;
}

int Compiler::declareLocal(const std::string& name, bool isImmutable) {
  int slot = allocLocalSlot();
  Scope& scope = scopes_.back();
  scope.vars[name] = LocalVar{name, slot, isImmutable};
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

int Compiler::resolveFromFrame(Bytecode& proto, std::unordered_map<std::string, int>& names,
                               FuncState* parent, const std::string& name) {
  // The outermost function has nothing around it to capture from.
  if (!parent) return -1;

  auto existing = names.find(name);
  if (existing != names.end()) return existing->second;

  UpvalDesc descriptor;
  LocalVar local;
  if (resolveLocalIn(parent->scopes, name, local)) {
    descriptor = UpvalDesc{true, local.slot};
  } else {
    int outer = resolveFromFrame(parent->bc, parent->upvalueNames, parent->enclosing, name);
    if (outer < 0) return -1;
    descriptor = UpvalDesc{false, outer};
  }

  proto.upvalues.push_back(descriptor);
  const int index = static_cast<int>(proto.upvalues.size()) - 1;
  names[name] = index;
  return index;
}

int Compiler::resolveUpvalue(const std::string& name) {
  return resolveFromFrame(bc_, currentUpvalueNames_,
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

void Compiler::resetChunk(const std::string& name) {
  bytecodeInit(bc_);
  bc_.name = name;
  nextReg_ = 0;
  maxReg_ = 0;
  nextLocal_ = 0;
  maxLocal_ = 0;
  scopes_.clear();
  loops_.clear();
  funcStack_.clear();
  currentUpvalueNames_.clear();
  globalConsts_.clear();
  predeclaredLetSlots_.clear();
  predeclaredFnSlots_.clear();
  inFunction_ = false;
  currentLine_ = 1;
  currentCol_ = 1;
}

Bytecode Compiler::compile(const StmtPtr& program) {
  resetChunk();

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

Bytecode Compiler::compileExpression(const ExprPtr& expr) {
  resetChunk("<expression>");
  if (!expr) fail("empty expression", 1, 1);

  // Expressions run in the global scope: a REPL entry can read and write the
  // globals defined by earlier entries.
  scopes_.push_back(Scope{});
  int result = compileExpr(*expr);
  emit(OpCode::RETURN, result);
  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;
  return std::move(bc_);
}

Bytecode Compiler::compileRepl(const StmtPtr& program) {
  resetChunk();
  if (!program) fail("empty program", 1, 1);

  scopes_.push_back(Scope{});
  const auto* block = std::get_if<BlockStmt>(&program->node);
  if (!block) {
    compileStmt(*program);
    emit(OpCode::HALT);
  } else {
    // Hoist top-level function declarations, then compile every statement.
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      }
    }
    const size_t count = block->stmts.size();
    for (size_t i = 0; i < count; ++i) {
      const StmtPtr& child = block->stmts[i];
      if (!child || std::holds_alternative<FnDeclStmt>(child->node)) continue;
      const bool isLast = i + 1 == count;
      if (isLast) {
        if (const auto* expression = std::get_if<ExprStmt>(&child->node)) {
          int result = compileExpr(*expression->expr);
          emit(OpCode::RETURN, result);
          bc_.numRegisters = maxReg_;
          bc_.numLocals = maxLocal_;
          return std::move(bc_);
        }
      }
      compileStmt(*child);
    }
    emit(OpCode::HALT);
  }

  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;
  return std::move(bc_);
}

Bytecode compileProgram(const StmtPtr& program) { return Compiler{}.compile(program); }

// ----------------------------------------------------------------- statements

void Compiler::compileBlockBody(const BlockStmt& block) {
  // Function declarations are hoisted within their block, so a call can appear
  // before the declaration and mutually recursive local functions can resolve
  // each other. Block-local `let`/`var` declarations also need their slots
  // reserved up front: a hoisted function declared after such a binding must
  // capture its cell (the interpreter resolves free names against the whole
  // block environment).
  bool hasHoistedFunction = false;
  for (const auto& child : block.stmts) {
    if (child && std::holds_alternative<FnDeclStmt>(child->node)) {
      hasHoistedFunction = true;
      break;
    }
  }

  if (!inGlobalScope() && hasHoistedFunction) {
    std::unordered_map<std::string, int> blockSlots;
    for (const auto& [name, local] : scopes_.back().vars) blockSlots[name] = local.slot;

    int nilReg = -1;
    auto ensureNil = [&]() {
      if (nilReg < 0) {
        nilReg = allocReg();
        emit(OpCode::LOADK, nilReg, addConstant(Value()));
      }
    };

    // Reserve slots in source order. A `var` reuses a binding already present
    // in this scope; `let`/`const` always create a fresh cell, like
    // `Env::define` overwriting an existing name.
    for (const auto& child : block.stmts) {
      if (!child) continue;
      if (const auto* let = std::get_if<LetStmt>(&child->node)) {
        auto found = blockSlots.find(let->name);
        if (let->kind == Tok::Var && found != blockSlots.end()) {
          predeclaredLetSlots_[let] = found->second;
        } else {
          int slot = allocLocalSlot();
          blockSlots[let->name] = slot;
          predeclaredLetSlots_[let] = slot;
          // Create the cell before any closure is built: a hoisted function
          // captures the slot while it is still nil, so the cell must already
          // exist and be the one the later declaration fills in.
          ensureNil();
          emit(OpCode::DECLLOCAL, nilReg, slot);
        }
      } else if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        auto found = blockSlots.find(fn->name);
        int slot;
        if (found != blockSlots.end()) {
          slot = found->second;
        } else {
          slot = allocLocalSlot();
          blockSlots[fn->name] = slot;
          ensureNil();
          emit(OpCode::DECLLOCAL, nilReg, slot);
        }
        predeclaredFnSlots_[fn] = slot;
      }
    }
    if (nilReg >= 0) freeReg();

    // Compile hoisted functions with every block-local name visible, so their
    // bodies capture the right cells even when the declaration appears later.
    std::unordered_map<std::string, LocalVar> beforeHoisting = scopes_.back().vars;
    for (const auto& [name, slot] : blockSlots) {
      scopes_.back().vars[name] = LocalVar{name, slot, false};
    }
    for (const auto& child : block.stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      }
    }

    // Restore source-order visibility for ordinary statements. Only hoisted
    // function names stay visible before their textual declaration.
    scopes_.back().vars = std::move(beforeHoisting);
    for (const auto& child : block.stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        int slot = predeclaredFnSlots_[fn];
        scopes_.back().vars[fn->name] = LocalVar{fn->name, slot, false};
      }
    }
  } else if (hasHoistedFunction) {
    // A block compiled directly in the global scope has no local slots; the
    // declarations still run before the other statements, as in `compile`.
    for (const auto& child : block.stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      }
    }
  }
  for (const auto& child : block.stmts) {
    if (!child) continue;
    if (std::holds_alternative<FnDeclStmt>(child->node)) continue;
    compileStmt(*child);
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
          [&](const ForInStmt& s) { compileForIn(s); },
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
  } else {
    // `compileBlockBody` reserved a slot (and created its cell) for every
    // direct declaration of the block. Fill that cell instead of allocating a
    // second one, so hoisted closures keep sharing it.
    auto predeclared = predeclaredLetSlots_.find(&s);
    if (predeclared != predeclaredLetSlots_.end()) {
      int slot = predeclared->second;
      emit(OpCode::SETLOCAL, reg, slot);
      scopes_.back().vars[s.name] = LocalVar{s.name, slot, s.kind != Tok::Var};
    } else if (s.kind == Tok::Var) {
      // `var` redeclares a binding only when it already exists in the *current*
      // scope. A declaration in a nested block shadows an enclosing binding by
      // creating a new cell, exactly like `Env::define` does.
      auto existing = scopes_.back().vars.find(s.name);
      if (existing != scopes_.back().vars.end()) {
        emit(OpCode::SETLOCAL, reg, existing->second.slot);
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
    // `compileBlockBody` reserves the slot so mutual recursion resolves and a
    // later declaration can reuse the same cell; fill it in place.
    auto predeclared = predeclaredFnSlots_.find(&fn);
    if (predeclared != predeclaredFnSlots_.end()) {
      int slot = predeclared->second;
      emit(OpCode::SETLOCAL, reg, slot);
      scopes_.back().vars[fn.name] = LocalVar{fn.name, slot, false};
    } else {
      auto existing = scopes_.back().vars.find(fn.name);
      if (existing != scopes_.back().vars.end()) {
        emit(OpCode::SETLOCAL, reg, existing->second.slot);
      } else {
        int slot = declareLocal(fn.name, false);
        emit(OpCode::DECLLOCAL, reg, slot);
      }
    }
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
  // A `return` at the top level of a function body is followed by the
  // implicit epilogue emitted in `compileFunctionProto`. Skipping the second
  // RETURN when the last instruction already returns keeps the frame stack
  // consistent: a duplicate RETURN would pop the caller's frame too (PHASE 6b).
  const bool alreadyReturns = !bc_.code.empty() &&
                              bc_.code.back().op == OpCode::RETURN &&
                              bc_.code.back().a == reg;
  if (!alreadyReturns) emit(OpCode::RETURN, reg);
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

void Compiler::compileForIn(const ForInStmt& s) {
  // Evaluate the iterable once. The cursor starts as nil, which tells ITER to
  // initialize it from the iterable on the first call (0 for arrays, strings
  // and maps; the start bound for ranges).
  int iterable = compileExpr(*s.iterable);
  int cursor = allocReg();
  emit(OpCode::LOADK, cursor, addConstant(Value()));
  int item = allocReg();
  int done = allocReg();

  int loopStart = currentOffset();
  int iterIndex = emit(OpCode::ITER, item, iterable, cursor);
  bc_.code[static_cast<size_t>(iterIndex)].jump = done;
  int exitJump = emitJump(OpCode::JMP_IF_TRUE, done);

  // Each turn gets a fresh cell for the loop variable, so closures created in
  // the body capture the value of their own iteration (interpreter parity).
  pushScope();
  int slot = declareLocal(s.var, false);
  emit(OpCode::DECLLOCAL, item, slot);

  loops_.push_back(Loop{});
  if (const auto* block = std::get_if<BlockStmt>(&s.body->node)) {
    compileBlockBody(*block);
  } else {
    compileStmt(*s.body);
  }
  Loop loop = std::move(loops_.back());
  loops_.pop_back();

  int backJump = emitJump(OpCode::JMP);
  patchJump(backJump, loopStart);

  int after = currentOffset();
  patchJump(exitJump, after);
  for (int index : loop.breaks) patchJump(index, after);
  for (int index : loop.continues) patchJump(index, loopStart);

  popScope();
  freeReg(nextReg_ - iterable);
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
  if (const auto* target = std::get_if<VariableExpr>(&assign.target->node)) {
    const std::string& name = target->name;
    LocalVar local;
    bool isLocal = resolveLocal(name, local);

    if (isLocal && local.isImmutable) {
      fail("cannot assign to immutable variable '" + name + "'", line, col);
    }
    // A captured local of an enclosing function becomes an upvalue read/write
    // on the shared cell (PHASE 6b). The lookup must happen after the compile
    // of the value below only insofar as register allocation is concerned;
    // doing it first keeps the resolution stable and mirrors `compileVariable`.
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

  // Indexed and member assignment share the same shape: evaluate the receiver
  // and key once, then either store the value or read/operate/store for a
  // compound operator. The receiver register becomes the result so the
  // register stack stays balanced for the enclosing expression.
  int object = 0;
  int key = 0;
  if (const auto* index = std::get_if<IndexExpr>(&assign.target->node)) {
    object = compileExpr(*index->object);
    key = compileExpr(*index->index);
  } else if (const auto* member = std::get_if<MemberExpr>(&assign.target->node)) {
    object = compileExpr(*member->object);
    key = -addString(member->name) - 1;  // constant member name
  } else {
    fail("invalid assignment target", line, col);
  }

  if (assign.op == Tok::Eq) {
    int value = compileExpr(*assign.value);
    emit(OpCode::SETTABLE, value, object, key);
    emit(OpCode::MOVE, object, value);
    freeReg(nextReg_ - object - 1);
    return object;
  }

  OpCode op;
  if (!compoundOpCode(assign.op, op)) {
    fail("unsupported compound assignment operator", assign.opLine, assign.opCol);
  }

  int current = allocReg();
  emit(OpCode::GETTABLE, current, object, key);
  int right = compileExpr(*assign.value);
  emit(op, current, current, right);
  freeReg();
  emit(OpCode::SETTABLE, current, object, key);
  emit(OpCode::MOVE, object, current);
  freeReg(nextReg_ - object - 1);
  return object;
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

int Compiler::compileHelperCall(const std::string& helper, const std::vector<int>& args,
                                int line, int col) {
  // Hidden helper names contain `$`, which the lexer never accepts in an
  // identifier, so user code cannot shadow them. They are registered by the
  // standard library (`registerBuiltins`) together with `print` and friends.
  int callee = allocReg();
  emit(OpCode::GETGLOBAL, callee, addString(helper));
  for (int source : args) {
    int argument = allocReg();
    emit(OpCode::MOVE, argument, source);
  }
  currentLine_ = line;
  currentCol_ = col;
  emit(OpCode::CALL, callee, static_cast<int>(args.size()));
  freeReg(static_cast<int>(args.size()));
  return callee;
}

int Compiler::compileToString(int source, int line, int col) {
  return compileHelperCall("$str", {source}, line, col);
}

int Compiler::compileArray(const ArrayExpr& array) {
  int table = allocReg();
  emit(OpCode::NEWTABLE, table, static_cast<int>(array.elements.size()), 0);
  for (size_t i = 0; i < array.elements.size(); ++i) {
    int index = allocReg();
    emit(OpCode::LOADK, index, addConstant(Value(static_cast<int64_t>(i))));
    int value = compileExpr(*array.elements[i]);
    emit(OpCode::SETTABLE, value, table, index);
    freeReg(nextReg_ - index);
  }
  return table;
}

int Compiler::compileMap(const MapExpr& map) {
  int table = allocReg();
  emit(OpCode::NEWTABLE, table, 0, 1);
  for (const MapEntry& entry : map.entries) {
    int key;
    if (entry.keyExpr) {
      // Computed keys use the language's canonical string form, exactly like
      // the interpreter (`Value::toString`).
      int rawKey = compileExpr(*entry.keyExpr);
      key = compileToString(rawKey, entry.keyExpr->line, entry.keyExpr->col);
    } else {
      key = -addString(entry.literalKey) - 1;
    }
    int value = compileExpr(*entry.value);
    emit(OpCode::SETTABLE, value, table, key);
    freeReg(nextReg_ - table - 1);
  }
  return table;
}

int Compiler::compileRange(const RangeExpr& range, int line, int col) {
  int start = compileExpr(*range.start);
  int end = compileExpr(*range.end);
  int inclusive = allocReg();
  emit(OpCode::LOADK, inclusive, addConstant(Value(range.inclusive)));
  int result = compileHelperCall("$range", {start, end, inclusive}, line, col);
  emit(OpCode::MOVE, start, result);
  freeReg(nextReg_ - start - 1);
  return start;
}

int Compiler::compileInterpolation(const InterpExpr& interpolation, int /*line*/, int /*col*/) {
  int result = allocReg();
  emit(OpCode::LOADK, result, addConstant(Value(std::string())));
  for (const InterpPart& part : interpolation.parts) {
    int text;
    if (part.isText()) {
      text = allocReg();
      emit(OpCode::LOADK, text, addConstant(Value(part.text())));
    } else {
      int value = compileExpr(*part.expr());
      text = compileToString(value, part.expr()->line, part.expr()->col);
    }
    emit(OpCode::ADD, result, result, text);
    freeReg(nextReg_ - result - 1);
  }
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
          [&](const InterpExpr& interpolation) -> int {
            return compileInterpolation(interpolation, expr.line, expr.col);
          },
          [&](const VariableExpr& variable) -> int { return compileVariable(variable.name); },
          [&](const ArrayExpr& array) -> int { return compileArray(array); },
          [&](const MapExpr& map) -> int { return compileMap(map); },
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

            if (binary.op == Tok::In) {
              int left = compileExpr(*binary.lhs);
              int right = compileExpr(*binary.rhs);
              currentLine_ = binary.opLine;
              currentCol_ = binary.opCol;
              emit(OpCode::IN, left, left, right);
              freeReg();
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
          [&](const IndexExpr& index) -> int {
            int object = compileExpr(*index.object);
            int key = compileExpr(*index.index);
            emit(OpCode::GETTABLE, object, object, key);
            freeReg();
            return object;
          },
          [&](const MemberExpr& member) -> int {
            int object = compileExpr(*member.object);
            int key = -addString(member.name) - 1;
            emit(OpCode::GETTABLE, object, object, key);
            return object;
          },
          [&](const RangeExpr& range) -> int {
            return compileRange(range, expr.line, expr.col);
          },
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
