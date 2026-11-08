#include "compiler.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "errors.h"
#include "ffi_types.h"
#include "util.h"

namespace {

// Throws a compile error with the position of the current node.
[[noreturn]] void fail(const std::string& message, int line, int col) {
  throw CompileError{message, line, col};
}

// Splits a dotted module path into its segments: "std.math" -> {"std","math"}.
std::vector<std::string> splitDotted(const std::string& path) {
  std::vector<std::string> segments;
  size_t start = 0;
  while (start <= path.size()) {
    size_t dot = path.find('.', start);
    if (dot == std::string::npos) {
      segments.push_back(path.substr(start));
      break;
    }
    segments.push_back(path.substr(start, dot - start));
    start = dot + 1;
  }
  return segments;
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
    case Tok::QuestionQuestion:
      return "??";
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
  // Any ordinary emission invalidates a previous conditional `RETURN`: it is
  // only "the last instruction" until something else is emitted.
  lastWasConditionalReturn_ = false;
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

int Compiler::declareLocal(const std::string& name, bool isImmutable, const TypeRef& type) {
  int slot = allocLocalSlot();
  Scope& scope = scopes_.back();
  scope.vars[name] = LocalVar{name, slot, isImmutable, type};
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

// --------------------------------------------------------------- type checks

void Compiler::declareGlobalType(const std::string& name, const TypeRef& type) {
  if (type.isPresent()) {
    globalTypes_[name] = type;
  } else {
    globalTypes_.erase(name);
  }
}

bool Compiler::lookupGlobalType(const std::string& name, TypeRef& out) const {
  auto found = globalTypes_.find(name);
  if (found == globalTypes_.end()) return false;
  out = found->second;
  return true;
}

// A value of type `from` may be stored in a slot of type `to` when either side
// is unknown/`any`, when the types match, or when `from` is `int` widening to
// `float`. `nil` only fits a nullable target. This is the single place the
// assignment rule lives, shared by declarations, plain assignments and
// parameters.
void Compiler::checkAssignable(const TypeRef& from, const TypeRef& to,
                               const std::string& context, int line, int col) {
  if (!to.isPresent() || !from.isPresent()) return;  // any / unknown
  if (to.name == "any" || from.name == "any") return;

  // `void` is a return-only type: it is never a value, so nothing can be
  // stored into it and a `void` expression cannot be used where a value is
  // expected (PHASE 6.6 maps it to C's `void`).
  if (to.name == "void") {
    fail("cannot store a value into '" + describeType(to) + "' in " + context +
             ": 'void' is only a return type",
         line, col);
  }
  if (from.name == "void") {
    fail("cannot use a 'void' expression as a value in " + context, line, col);
  }

  // `nil` fits any nullable target, whatever its type. It is also the NULL of
  // the FFI pointer types, so it is assignable to `ptr`/`cstring` even when the
  // annotation is not written nullable (`native fn free(p: ptr); free(nil);`).
  if (from.name == "nil") {
    if (to.nullable || isPointerTypeName(to.name)) return;
    fail("cannot assign 'nil' to '" + describeType(to) + "' in " + context +
             ": a nullable value is not allowed here (use '" + to.name + "?')",
         line, col);
  }

  if (from.name == to.name) {
    // Same type; a nullable value does not fit a non-nullable slot unless the
    // value is provably non-nil (which `from.nullable` already encodes).
    if (from.nullable && !to.nullable) {
      fail("cannot assign '" + describeType(from) + "' to '" + describeType(to) + "' in " +
               context + ": a nullable value is not allowed here (use '" + to.name + "?')",
           line, col);
    }
    return;
  }

  // FFI pointers (PHASE 6.6): a `cstring` is also a `ptr` (upcast to `void*`);
  // the reverse needs an explicit cast, so it is not implicit. A Jolt `string`
  // is not assignable to `cstring`: `cstring` is an owned handle, so binding a
  // string to it would leave a dangling pointer once the string is collected.
  // Passing a string as a `cstring` *argument* is allowed instead, because the
  // string stays alive for the whole call (see `checkNativeCall`).
  if (from.name == "cstring" && to.name == "ptr") return;

  // Every integer spelling is one numeric family at compile time (the runtime
  // holds an `int64`), so `int`, the fixed-width types, `uint` and `size_t`
  // are mutually assignable.
  if (isIntegerTypeName(from.name) && isIntegerTypeName(to.name)) return;

  // Numeric widening follows Java: int -> float -> double. Narrowing
  // (double -> float, float -> int) is not implicit; use an explicit conversion.
  if (isIntegerTypeName(from.name) && (to.name == "float" || to.name == "double")) return;
  if (from.name == "float" && to.name == "double") return;

  fail("cannot assign '" + describeType(from) + "' to '" + describeType(to) + "' in " +
           context,
       line, col);
}

// A `native fn` declares the arity and the argument types the host function
// expects. The check is best-effort: an unannotated parameter means the
// argument is not checked, so a native with a loose signature still compiles.
void Compiler::checkNativeCall(const std::string& name, const NativeSig& sig,
                               const CallExpr& call, int line, int col) {
  const bool variadic = !sig.params.empty() && sig.params.back().variadic;
  // A variadic native accepts `params.size() - 1` or more arguments; the extras
  // are checked against the variadic parameter's element type.
  const size_t required = sig.params.size() - (variadic ? 1 : 0);
  const bool arityOk =
      variadic ? call.args.size() >= required : call.args.size() == required;
  if (!arityOk) {
    fail("'" + name + "' expects " + std::to_string(required) + " argument" +
             (required == 1 ? "" : "s") + (variadic ? " or more" : "") + ", got " +
             std::to_string(call.args.size()),
         line, col);
  }
  for (size_t i = 0; i < call.args.size(); ++i) {
    // Arguments past the fixed parameters all bind the variadic element type.
    const size_t slot = i < required ? i : sig.params.size() - 1;
    const TypeRef& param = sig.params[slot].type;
    if (!param.isPresent()) continue;
    const TypeRef argument = inferExprType(*call.args[i]);
    // A Jolt `string` is NUL-terminated and stays alive for the whole call, so
    // it is a valid `const char *` *argument* even though it is not assignable
    // to an owned `cstring` binding (see `checkAssignable`).
    if (param.name == "cstring" && argument.name == "string" && !argument.nullable) continue;
    checkAssignable(argument, param, "argument " + std::to_string(i + 1) + " of '" + name + "'",
                    call.args[i]->line, call.args[i]->col);
  }
}

// Resolves `name` the way a variable read does and returns its native
// signature when the binding is a `native fn`. The scopes of the function
// being compiled come first, then every enclosing frame, innermost first: the
// first binding wins, so a local or an upvalue that shadows a native name is a
// different function and yields null. A name with no local binding is a
// global, which is a native when it was declared as one.
const Compiler::NativeSig* Compiler::nativeSigFor(const std::string& name) const {
  auto declared = nativeSigs_.find(name);
  const NativeSig* sig = declared == nativeSigs_.end() ? nullptr : &declared->second;

  LocalVar local;
  if (resolveLocalIn(scopes_, name, local)) return local.isNative ? sig : nullptr;
  for (auto frame = funcStack_.rbegin(); frame != funcStack_.rend(); ++frame) {
    if (resolveLocalIn(frame->scopes, name, local)) return local.isNative ? sig : nullptr;
  }
  return sig;
}

TypeRef Compiler::inferExprType(const Expr& expr) const {
  if (const auto* literal = std::get_if<LiteralExpr>(&expr.node)) {
    TypeRef type;
    type.name = literal->value.typeName();  // "nil" for a nil literal
    type.nullable = literal->value.isNil();
    return type;
  }
  if (const auto* variable = std::get_if<VariableExpr>(&expr.node)) {
    LocalVar local;
    if (resolveLocal(variable->name, local)) return local.type;
    TypeRef global;
    if (lookupGlobalType(variable->name, global)) return global;
    return TypeRef{};
  }
  // A struct literal has the struct's own type, so `let p: Point = Point { ... }`
  // is checked like any other annotated binding.
  if (const auto* literal = std::get_if<StructLitExpr>(&expr.node)) {
    return TypeRef{literal->typeName, false};
  }
  // `a!!` narrows away the nullability of its operand (PHASE 6.5).
  if (const auto* assertion = std::get_if<NonNullExpr>(&expr.node)) {
    TypeRef inner = inferExprType(*assertion->operand);
    inner.nullable = false;
    return inner;
  }
  // `a ?? b`: when `a` is a nullable `T?`, the result is `b`'s type (or a
  // non-null `T`); otherwise it is `a`'s type unchanged.
  if (const auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    if (binary->op == Tok::QuestionQuestion) {
      TypeRef left = inferExprType(*binary->lhs);
      if (left.isPresent() && left.nullable) {
        TypeRef right = inferExprType(*binary->rhs);
        if (right.isPresent()) return right;
        left.nullable = false;
      }
      return left;
    }
  }
  // `try e` (PHASE 6.7.3): on the error path the function returns immediately,
  // so the value's type on the success path is the operand's. Propagating it
  // keeps `let x: int = try f();` checked when `f` is a declared `native fn`.
  if (const auto* attempt = std::get_if<TryExpr>(&expr.node)) {
    return inferExprType(*attempt->operand);
  }
  // `e catch d` (PHASE 6.7.3): the result is either the operand or the
  // fallback. Without the union types of PHASE 6.7.4 there is no type to give
  // it, so it stays unknown and no assignment check is applied.
  if (std::holds_alternative<CatchExpr>(expr.node)) {
    return TypeRef{};
  }
  // A call to a declared `native fn` has its declared return type, so
  // `let y: float = sqrt(x)` is checked like any other annotated binding.
  if (const auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (const auto* callee = std::get_if<VariableExpr>(&call->callee->node)) {
      if (const NativeSig* sig = nativeSigFor(callee->name)) return sig->returnType;
    }
  }
  return TypeRef{};  // any other shape: unknown, so no check
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
  deferStack_.clear();
  funcStack_.clear();
  currentUpvalueNames_.clear();
  currentReturnType_ = TypeRef{};
  lastWasConditionalReturn_ = false;
  predeclaredLetSlots_.clear();
  predeclaredFnSlots_.clear();
  inFunction_ = false;
  currentLine_ = 1;
  currentCol_ = 1;
  moduleBody_ = false;
  modulePath_.clear();
  moduleImportNames_.clear();
  loadingStack_.clear();
}

void Compiler::clearSession() {
  globalConsts_.clear();
  userGlobals_.clear();
  globalTypes_.clear();
  structDefs_.clear();
  nativeSigs_.clear();
  loadedModules_.clear();
  materializedModules_.clear();
  namespaces_.clear();
  boundImports_.clear();
  wildcardBindings_.clear();
}

Bytecode Compiler::compile(const StmtPtr& program) {
  clearSession();
  resetChunk();
  return compileTopLevelBody(program);
}

// The shared body of `compile` and `compileLoaded`: compiles a program in the
// global scope (its `let`s are globals) with the session state the caller has
// already set up. The caller owns `resetChunk`/`clearSession` and the import
// loader, so the two entry points differ only in what session they present.
Bytecode Compiler::compileTopLevelBody(const StmtPtr& program) {
  if (!program) fail("empty program", 1, 1);

  // The main chunk uses the global scope: its `let`s are globals, as in the
  // interpreter (and so the REPL keeps definitions between entries).
  scopes_.push_back(Scope{});
  if (const auto* block = std::get_if<BlockStmt>(&program->node)) {
    // Register the struct shapes first so a hoisted function (compiled next)
    // can already build a literal.
    registerStructDecls(*block);
    // Register native signatures too: a hoisted function may call one and must
    // be checked against its signature.
    registerNativeDecls(*block);
    // Top-level function declarations are hoisted, like `execBlock` does in
    // the interpreter, so a call can appear before the declaration. An
    // `extern "C"` block is hoisted too: its declarations are natives, and a
    // hoisted function may call one, so the binding must exist regardless of
    // where the block appears in the file.
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      } else if (const auto* ext = std::get_if<ExternBlockStmt>(&child->node)) {
        compileExternBlock(*ext, child->line, child->col);
      }
    }
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (std::holds_alternative<FnDeclStmt>(child->node)) continue;
      if (std::holds_alternative<ExternBlockStmt>(child->node)) continue;
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
  // globals defined by earlier entries. The session state (types, imports) is
  // kept, so an expression sees the definitions of previous entries.
  scopes_.push_back(Scope{});
  int result = compileExpr(*expr);
  emit(OpCode::RETURN, result);
  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;
  return std::move(bc_);
}

Bytecode Compiler::compileRepl(const StmtPtr& program) {
  // A definition is committed only when its entry compiles: the session state
  // is snapshotted and restored if the entry throws. Without this, an entry
  // that materializes an import and then fails would leave that import marked
  // as bound although its bytecode never ran, and the next entry would see a
  // module that is not there.
  SessionSnapshot saved = snapshotSession();
  try {
    return compileReplEntry(program);
  } catch (...) {
    restoreSession(std::move(saved));
    throw;
  }
}

Compiler::SessionSnapshot Compiler::snapshotSession() const {
  SessionSnapshot snapshot;
  snapshot.globalConsts = globalConsts_;
  snapshot.userGlobals = userGlobals_;
  snapshot.globalTypes = globalTypes_;
  snapshot.structDefs = structDefs_;
  snapshot.nativeSigs = nativeSigs_;
  snapshot.loadedModules = loadedModules_;
  snapshot.materializedModules = materializedModules_;
  snapshot.namespaces = namespaces_;
  snapshot.boundImports = boundImports_;
  snapshot.wildcardBindings = wildcardBindings_;
  return snapshot;
}

void Compiler::restoreSession(SessionSnapshot snapshot) {
  globalConsts_ = std::move(snapshot.globalConsts);
  userGlobals_ = std::move(snapshot.userGlobals);
  globalTypes_ = std::move(snapshot.globalTypes);
  structDefs_ = std::move(snapshot.structDefs);
  nativeSigs_ = std::move(snapshot.nativeSigs);
  loadedModules_ = std::move(snapshot.loadedModules);
  materializedModules_ = std::move(snapshot.materializedModules);
  namespaces_ = std::move(snapshot.namespaces);
  boundImports_ = std::move(snapshot.boundImports);
  wildcardBindings_ = std::move(snapshot.wildcardBindings);
}

Bytecode Compiler::compileLoaded(const StmtPtr& program, ImportLoader loader,
                                 const std::string& name) {
  // A loaded file runs in the current session: it sees the session's structs,
  // types and imports, and the definitions it adds stay for later entries,
  // unlike `compile` which starts clean. Only the file's own import loader is
  // swapped in (so its imports resolve relative to the file) and restored
  // afterwards. The session is snapshotted first so a file that fails to
  // compile leaves the session untouched, exactly like a failed REPL entry.
  SessionSnapshot saved = snapshotSession();
  ImportLoader savedLoader = std::move(importLoader_);
  importLoader_ = std::move(loader);
  try {
    resetChunk(name);
    Bytecode result = compileTopLevelBody(program);
    importLoader_ = std::move(savedLoader);
    return result;
  } catch (...) {
    importLoader_ = std::move(savedLoader);
    restoreSession(std::move(saved));
    throw;
  }
}

Bytecode Compiler::compileReplEntry(const StmtPtr& program) {
  resetChunk();
  if (!program) fail("empty program", 1, 1);

  scopes_.push_back(Scope{});
  const auto* block = std::get_if<BlockStmt>(&program->node);
  if (!block) {
    compileStmt(*program);
    emit(OpCode::HALT);
  } else {
    // Register struct shapes first, then hoist top-level function declarations.
    registerStructDecls(*block);
    registerNativeDecls(*block);
    for (const auto& child : block->stmts) {
      if (!child) continue;
      if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        compileFnDecl(*fn, child->line, child->col);
      } else if (const auto* ext = std::get_if<ExternBlockStmt>(&child->node)) {
        compileExternBlock(*ext, child->line, child->col);
      }
    }
    const size_t count = block->stmts.size();
    for (size_t i = 0; i < count; ++i) {
      const StmtPtr& child = block->stmts[i];
      if (!child || std::holds_alternative<FnDeclStmt>(child->node)) continue;
      if (std::holds_alternative<ExternBlockStmt>(child->node)) continue;
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
  // A `struct` is compile-time metadata, so register the whole block's shapes
  // before compiling anything: a hoisted function that builds a literal must
  // already see them.
  registerStructDecls(block);
  registerNativeDecls(block);
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
    std::unordered_map<std::string, TypeRef> blockTypes;
    std::unordered_map<std::string, bool> blockNatives;
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
    // `Env::define` overwriting an existing name. The binding's type (PHASE
    // 6.5) is recorded here too, so a hoisted closure sees the annotated or
    // inferred type of a binding declared later.
    for (const auto& child : block.stmts) {
      if (!child) continue;
      if (const auto* let = std::get_if<LetStmt>(&child->node)) {
        TypeRef type = let->type;
        if (!type.isPresent() && let->init) {
          type = inferExprType(*let->init);
          // A `void` initializer has no value, so the binding stays untyped
          // (mirrors `compileLet`); otherwise a hoisted closure reading it
          // would see a bogus `void` type.
          if (type.name == "void") type = TypeRef{};
        }
        blockTypes[let->name] = type;
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
        blockTypes[fn->name] = TypeRef{"function", false};
        blockNatives[fn->name] = fn->isNative;
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
      scopes_.back().vars[name] =
          LocalVar{name, slot, false, blockTypes[name], blockNatives[name]};
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
        scopes_.back().vars[fn->name] =
            LocalVar{fn->name, slot, false, TypeRef{"function", false}, fn->isNative};
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
  // Normal fall-through leaves the scope, so its deferred blocks run now.
  emitDeferredBlocks(scopes_.size());
  popScope();
}

void Compiler::compileStmt(const Stmt& stmt) {
  currentLine_ = stmt.line;
  currentCol_ = stmt.col;
  std::visit(
      Overloaded{
          [&](const ExprStmt& s) {
              compileExpr(*s.expr);
              freeReg();
          },
          [&](const LetStmt& s) { compileLet(s); },
          [&](const FnDeclStmt& s) { compileFnDecl(s, stmt.line, stmt.col); },
          [&](const StructDeclStmt& s) { compileStructDecl(s, stmt.line, stmt.col); },
          [&](const BlockStmt& s) {
            pushScope();
            compileBlockBody(s);
            // A standalone block is a scope: its deferred blocks run on exit.
            emitDeferredBlocks(scopes_.size());
            popScope();
          },
          [&](const ReturnStmt& s) { compileReturn(s); },
          [&](const IfStmt& s) { compileIf(s); },
          [&](const WhileStmt& s) { compileWhile(s); },
          [&](const ForStmt& s) { compileFor(s); },
          [&](const ForInStmt& s) { compileForIn(s); },
          [&](const SwitchStmt& s) { compileSwitch(s); },
          [&](const BreakStmt&) { compileBreak(stmt); },
          [&](const ContinueStmt&) { compileContinue(stmt); },
          [&](const DeferStmt& s) { compileDefer(s); },
          [&](const ImportStmt& s) { compileImport(s, stmt.line, stmt.col); },
          [&](const ExternBlockStmt& s) { compileExternBlock(s, stmt.line, stmt.col); },
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

  // Type handling (PHASE 6.5). An explicit annotation wins and is checked
  // against the initializer; without one, the type is inferred from the
  // initializer when it is evident. Types are metadata only, so the emitted
  // code below is the same either way.
  TypeRef declared = s.type;
  if (s.init) {
    TypeRef inferred = inferExprType(*s.init);
    if (declared.isPresent()) {
      checkAssignable(inferred, declared, "'" + s.name + "'", s.init->line, s.init->col);
    } else if (inferred.isPresent() && inferred.name != "nil" && inferred.name != "void") {
      // `let x = nil` stays untyped, so a later `x = 5` is allowed. So does
      // `let x = voidNative();`: a void expression has no value to type.
      declared = inferred;
    }
  } else if (declared.isPresent()) {
    // `var x: int;` binds nil, which only a nullable target accepts.
    checkAssignable(TypeRef{"nil", true}, declared, "'" + s.name + "'", currentLine_,
                    currentCol_);
  }

  if (inGlobalScope()) {
    // Top level: global variable (persists in the REPL).
    emit(OpCode::SETGLOBAL, reg, addString(s.name));
    declareGlobal(s.name, s.kind != Tok::Var);
    declareGlobalType(s.name, declared);
    userGlobals_.insert(s.name);
    // A global binding replaces a `native fn` of the same name, so its
    // signature no longer applies to calls of the name.
    nativeSigs_.erase(s.name);
  } else {
    // `compileBlockBody` reserved a slot (and created its cell) for every
    // direct declaration of the block. Fill that cell instead of allocating a
    // second one, so hoisted closures keep sharing it.
    auto predeclared = predeclaredLetSlots_.find(&s);
    if (predeclared != predeclaredLetSlots_.end()) {
      int slot = predeclared->second;
      emit(OpCode::SETLOCAL, reg, slot);
      scopes_.back().vars[s.name] = LocalVar{s.name, slot, s.kind != Tok::Var, declared};
    } else if (s.kind == Tok::Var) {
      // `var` redeclares a binding only when it already exists in the *current*
      // scope. A declaration in a nested block shadows an enclosing binding by
      // creating a new cell, exactly like `Env::define` does.
      auto existing = scopes_.back().vars.find(s.name);
      if (existing != scopes_.back().vars.end()) {
        emit(OpCode::SETLOCAL, reg, existing->second.slot);
        existing->second.type = declared;
      } else {
        int slot = declareLocal(s.name, false, declared);
        emit(OpCode::DECLLOCAL, reg, slot);
      }
    } else {
      // Nested block: frame-local variable. The declaration happens after the
      // initializer is compiled, exactly like `Env::define`.
      int slot = declareLocal(s.name, s.kind != Tok::Var, declared);
      emit(OpCode::DECLLOCAL, reg, slot);
    }
  }
  freeReg();
}

// ------------------------------------------------------------------- modules

ImportedModule Compiler::importModule(const StmtPtr& program, const std::string& path) {
  // A module is compiled like a function body: its top-level bindings become
  // locals of the module's own frame (never globals), so a module's internals
  // cannot leak into the importing program. The proto returns a table of the
  // module's public names. `moduleBody_` tells `compileLet`/`compileFnDecl`
  // that a declaration at scope depth 0 is still a local.
  //
  // The module is compiled in a fresh chunk: the compiler state of the chunk
  // that requested the import (its registers, scopes and loops) is saved and
  // restored around the module, exactly as `compileFunctionProto` does for a
  // function. A module that imports another module nests these save/restores.
  Bytecode savedChunk = std::move(bc_);
  auto savedUpvalueNames = std::move(currentUpvalueNames_);
  const int savedNextReg = nextReg_;
  const int savedMaxReg = maxReg_;
  const int savedNextLocal = nextLocal_;
  const int savedMaxLocal = maxLocal_;
  std::vector<Scope> savedScopes = std::move(scopes_);
  std::vector<Loop> savedLoops = std::move(loops_);
  const int savedLine = currentLine_;
  const int savedCol = currentCol_;
  const bool savedInFunction = inFunction_;
  const bool savedModuleBody = moduleBody_;
  auto savedLetSlots = std::move(predeclaredLetSlots_);
  auto savedFnSlots = std::move(predeclaredFnSlots_);
  auto savedGlobalConsts = std::move(globalConsts_);
  auto savedUserGlobals = std::move(userGlobals_);
  auto savedGlobalTypes = std::move(globalTypes_);
  auto savedStructDefs = std::move(structDefs_);
  auto savedNativeSigs = std::move(nativeSigs_);
  std::deque<FuncState> savedFuncStack = std::move(funcStack_);
  // A module's imports are its own: the namespace tables, bound names and
  // wildcard bindings it creates must not be visible to the importing program
  // (nor to a sibling module), so they are saved and restored around the body.
  // `modulePath_`/`moduleImportNames_` give the module-local global names its
  // imports bind to (see `importBindingName`).
  auto savedNamespaces = std::move(namespaces_);
  auto savedBoundImports = std::move(boundImports_);
  auto savedWildcardBindings = std::move(wildcardBindings_);
  std::string savedModulePath = std::move(modulePath_);
  auto savedModuleImportNames = std::move(moduleImportNames_);

  bc_ = Bytecode{};
  bc_.name = path;
  currentUpvalueNames_.clear();
  nextReg_ = 0;
  maxReg_ = 0;
  nextLocal_ = 0;
  maxLocal_ = 0;
  scopes_.clear();
  loops_.clear();
  funcStack_.clear();
  inFunction_ = false;
  moduleBody_ = true;
  modulePath_ = path;
  moduleImportNames_.clear();
  predeclaredLetSlots_.clear();
  predeclaredFnSlots_.clear();
  globalConsts_.clear();
  userGlobals_.clear();
  globalTypes_.clear();
  structDefs_.clear();
  nativeSigs_.clear();
  namespaces_.clear();
  boundImports_.clear();
  wildcardBindings_.clear();

  std::vector<std::string> symbols;
  const auto* block = program ? std::get_if<BlockStmt>(&program->node) : nullptr;
  if (block) {
    // Public names, in declaration order: every top-level binding whose name
    // does not start with '_' (the private marker).
    for (const auto& child : block->stmts) {
      if (!child) continue;
      const std::string* name = nullptr;
      if (const auto* let = std::get_if<LetStmt>(&child->node)) {
        name = &let->name;
      } else if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
        name = &fn->name;
      }
      if (name != nullptr && !name->empty() && (*name)[0] != '_') {
        symbols.push_back(*name);
      }
    }
  }

  // Record every name the module's own imports will introduce before the body
  // is compiled, so a hoisted function that references an imported name
  // resolves it to the module-local hidden global (see `importBindingName`).
  if (block) predeclareModuleImports(*block);

  compileModuleBody(program);
  moduleBody_ = false;

  ImportedModule module;
  module.path = path;
  module.code = std::make_shared<Bytecode>(std::move(bc_));
  module.symbols = std::move(symbols);

  bc_ = std::move(savedChunk);
  currentUpvalueNames_ = std::move(savedUpvalueNames);
  nextReg_ = savedNextReg;
  maxReg_ = savedMaxReg;
  nextLocal_ = savedNextLocal;
  maxLocal_ = savedMaxLocal;
  scopes_ = std::move(savedScopes);
  loops_ = std::move(savedLoops);
  currentLine_ = savedLine;
  currentCol_ = savedCol;
  inFunction_ = savedInFunction;
  moduleBody_ = savedModuleBody;
  predeclaredLetSlots_ = std::move(savedLetSlots);
  predeclaredFnSlots_ = std::move(savedFnSlots);
  globalConsts_ = std::move(savedGlobalConsts);
  userGlobals_ = std::move(savedUserGlobals);
  globalTypes_ = std::move(savedGlobalTypes);
  structDefs_ = std::move(savedStructDefs);
  nativeSigs_ = std::move(savedNativeSigs);
  funcStack_ = std::move(savedFuncStack);
  namespaces_ = std::move(savedNamespaces);
  boundImports_ = std::move(savedBoundImports);
  wildcardBindings_ = std::move(savedWildcardBindings);
  modulePath_ = std::move(savedModulePath);
  moduleImportNames_ = std::move(savedModuleImportNames);
  return module;
}

// Compiles a module's top-level statements into `bc_` and returns its public
// surface as a table. Bindings stay frame-local; only names that do not start
// with '_' are exported.
void Compiler::compileModuleBody(const StmtPtr& program) {
  pushScope();

  if (const auto* block = program ? std::get_if<BlockStmt>(&program->node) : nullptr) {
    // `compileBlockBody` hoists function declarations and pre-declares block
    // locals, exactly as it does for any other block, so a module function can
    // reference a binding declared later in the file.
    compileBlockBody(*block);
  } else if (program) {
    compileStmt(*program);
  }

  // Return the module's public surface as a table: { name = value, ... }.
  // Every top-level declaration lives in the scope pushed above; one that
  // starts with '_' stays private and is simply left out of the table.
  int table = allocReg();
  emit(OpCode::NEWTABLE, table, 0, 1);
  for (const auto& entry : scopes_.back().vars) {
    const std::string& name = entry.first;
    if (name.empty() || name[0] == '_') continue;
    int value = allocReg();
    emit(OpCode::GETLOCAL, value, entry.second.slot);
    emit(OpCode::SETTABLE, value, table, -addString(name) - 1);
    freeReg();
  }
  emit(OpCode::RETURN, table);

  popScope();
  bc_.numRegisters = maxReg_;
  bc_.numLocals = maxLocal_;
}

const ImportedModule& Compiler::loadModule(const std::string& path, int line, int col) {
  auto cached = loadedModules_.find(path);
  if (cached != loadedModules_.end()) return cached->second;

  // A path already being loaded is a cycle (`a -> b -> a`). Report it instead
  // of recursing forever: without this guard the loader would keep re-entering
  // itself until the stack overflows. The message lists the chain so the cycle
  // is visible at a glance.
  if (std::find(loadingStack_.begin(), loadingStack_.end(), path) != loadingStack_.end()) {
    std::string chain;
    bool started = false;
    for (const std::string& active : loadingStack_) {
      if (!started && active != path) continue;
      started = true;
      chain += active;
      chain += " -> ";
    }
    chain += path;
    fail("cyclic import detected: " + chain, line, col);
  }

  if (!importLoader_) {
    fail("module '" + path + "' not found", line, col);
  }
  StmtPtr program = importLoader_(path, line, col);
  loadingStack_.push_back(path);
  ImportedModule module;
  {
    // Pop the path on scope exit, including when compiling the module body
    // throws, so a failed import cannot leave the stack dirty for the next
    // compile (the REPL reuses one Compiler across entries).
    struct PopGuard {
      std::vector<std::string>& stack;
      ~PopGuard() { stack.pop_back(); }
    } guard{loadingStack_};
    module = importModule(program, path);
  }
  auto inserted = loadedModules_.emplace(path, std::move(module));
  return inserted.first->second;
}

void Compiler::compileImport(const ImportStmt& import, int line, int col) {
  const ImportedModule& module = loadModule(import.module, line, col);

  if (import.mode == ImportStmt::Mode::Qualified) {
    std::string boundName = import.alias.empty() ? import.module : import.alias;
    compileQualifiedImport(module, boundName, line, col);
    return;
  }

  std::vector<std::string> names;
  if (import.mode == ImportStmt::Mode::Wildcard) {
    names = module.symbols;
  } else {
    names = import.names;
    // Validate that every selected name is actually exported.
    for (const std::string& name : names) {
      if (std::find(module.symbols.begin(), module.symbols.end(), name) ==
          module.symbols.end()) {
        fail("module '" + import.module + "' has no export named '" + name + "'", line, col);
      }
    }
  }
  compileNameImport(module, names, line, col);
}

void Compiler::materializeModule(const ImportedModule& module, int line, int col) {
  (void)line;
  (void)col;
  if (materializedModules_.find(module.path) != materializedModules_.end()) return;
  materializedModules_.insert(module.path);

  // A hidden global name contains `$`, which the lexer never accepts in an
  // identifier, so user code cannot collide with it. The hidden global keeps
  // the module's exported table alive for the whole program and, because the
  // `materializedModules_` guard fires first, the body runs at most once even
  // when several imports name the same module.
  int child = static_cast<int>(bc_.children.size());
  bc_.children.push_back(module.code);
  int reg = allocReg();
  emit(OpCode::CLOSURE, reg, child);
  emit(OpCode::CALL, reg, 0);
  emit(OpCode::SETGLOBAL, reg, addString("$module$" + module.path));
  freeReg();
}

std::string Compiler::importBindingName(const std::string& visible) const {
  // Outside a module body (or for a name the module did not import) the binding
  // is the program global, so the visible name is used unchanged.
  if (modulePath_.empty() || moduleImportNames_.find(visible) == moduleImportNames_.end()) {
    return visible;
  }
  // Inside a module, its own imports bind to hidden per-module globals. The `$`
  // is never accepted in an identifier, so the name cannot collide with user
  // code, and two modules importing the same name get distinct bindings.
  return "$imp$" + modulePath_ + "$" + visible;
}

void Compiler::predeclareModuleImports(const BlockStmt& block) {
  // Record every name the module's imports will introduce, before the body is
  // compiled, so a hoisted function that references an imported name binds to
  // the module-local global instead of the program global. Loading a wildcard
  // module here is what lets its exported names be known up front; the load is
  // cached, so `compileImport` reuses it.
  for (const auto& child : block.stmts) {
    if (!child) continue;
    const auto* import = std::get_if<ImportStmt>(&child->node);
    if (!import) continue;
    if (import->mode == ImportStmt::Mode::Qualified) {
      const std::string& bound = import->alias.empty() ? import->module : import->alias;
      // Only the root of the namespace is a global; deeper segments are members
      // of it, so a read of `a.b.c` starts by reading the module-local `a`.
      moduleImportNames_.insert(bound.substr(0, bound.find('.')));
    } else if (import->mode == ImportStmt::Mode::Wildcard) {
      const ImportedModule& module = loadModule(import->module, child->line, child->col);
      for (const std::string& name : module.symbols) moduleImportNames_.insert(name);
    } else {
      for (const std::string& name : import->names) moduleImportNames_.insert(name);
    }
  }
}

int Compiler::ensureNamespace(const std::string& prefix, int line, int col) {
  (void)line;
  (void)col;
  std::vector<std::string> segments = splitDotted(prefix);

  // Walk the path from the root, materializing every missing segment as a
  // table member of its parent. Only the root is a global: a nested namespace
  // such as `a.b` lives inside `a`, so re-visiting it must read it through the
  // parent (`GETTABLE`), never with `GETGLOBAL` (there is no `a.b` global).
  // `current` always holds the table for the deepest segment, so the walk can
  // continue and the caller receives the innermost namespace to attach the
  // module to. Registers for outer segments are left allocated; the caller
  // frees the whole span when it is done.
  int current = -1;
  std::string path;
  for (const std::string& segment : segments) {
    path = path.empty() ? segment : path + "." + segment;
    const bool exists = namespaces_.find(path) != namespaces_.end();
    int next = allocReg();
    if (!exists) {
      emit(OpCode::NEWTABLE, next, 0, 1);
      if (current < 0) {
        // Root: the namespace table is a global (module-local inside a module,
        // so a module's own namespace never reaches the importing program).
        emit(OpCode::SETGLOBAL, next, addString(importBindingName(path)));
        declareGlobal(path, false);
      } else {
        // Intermediate: attach the new table to its parent.
        emit(OpCode::SETTABLE, next, current, -addString(segment) - 1);
      }
      namespaces_.insert(path);
    } else if (current < 0) {
      emit(OpCode::GETGLOBAL, next, addString(importBindingName(path)));
    } else {
      emit(OpCode::GETTABLE, next, current, -addString(segment) - 1);
    }
    current = next;
  }
  return current;
}

void Compiler::compileQualifiedImport(const ImportedModule& module, const std::string& boundName,
                                      int line, int col) {
  std::vector<std::string> segments = splitDotted(boundName);
  // The module body runs on import even when the binding below is skipped, so
  // its side effects match the wildcard form.
  materializeModule(module, line, col);
  // A simple bound name (the alias form) must not silently rebind a name the
  // module declares itself: the local binding wins, as for a wildcard import.
  if (segments.size() == 1 && userGlobals_.find(boundName) != userGlobals_.end()) return;
  if (boundImports_.find(boundName) != boundImports_.end()) return;
  boundImports_.insert(boundName);

  int table = allocReg();
  emit(OpCode::GETGLOBAL, table, addString("$module$" + module.path));

  // Bind the module table at the visible name. A dotted path becomes a real
  // namespace tree of maps (`std.math.sqrt` is the ordinary member chain); a
  // bare alias binds the table directly.
  if (segments.size() == 1) {
    emit(OpCode::SETGLOBAL, table, addString(importBindingName(boundName)));
    declareGlobal(boundName, false);
  } else {
    int parent = ensureNamespace(boundName.substr(0, boundName.rfind('.')), line, col);
    emit(OpCode::SETTABLE, table, parent, -addString(segments.back()) - 1);
    freeReg();
  }
  freeReg(nextReg_ - table);
}

void Compiler::compileNameImport(const ImportedModule& module,
                                 const std::vector<std::string>& names, int line, int col) {
  materializeModule(module, line, col);

  for (const std::string& name : names) {
    // A name the importing module declares itself wins over the import: the
    // import must not silently shadow a local `let`, `var` or `fn`, whatever
    // the order of the declarations. Such a name is not a wildcard binding, so
    // it never triggers an ambiguity either.
    if (userGlobals_.find(name) != userGlobals_.end()) continue;

    // Conflict detection: a name already introduced by another wildcard is
    // ambiguous unless this import is the very same module. This is the
    // compile-time guarantee that makes `.*` safe (no silent shadowing).
    auto existing = wildcardBindings_.find(name);
    if (existing != wildcardBindings_.end() && existing->second != module.path) {
      fail("'" + name + "' is ambiguous between " + existing->second + " and " +
               module.path + "; use '" + existing->second + "." + name + "' or '" +
               module.path + "." + name + "' to disambiguate",
           line, col);
    }
    wildcardBindings_[name] = module.path;

    int table = allocReg();
    emit(OpCode::GETGLOBAL, table, addString("$module$" + module.path));
    int value = allocReg();
    emit(OpCode::GETTABLE, value, table, -addString(name) - 1);
    emit(OpCode::SETGLOBAL, value, addString(importBindingName(name)));
    declareGlobal(name, false);
    freeReg(nextReg_ - table);
  }
}

void Compiler::compileFnDecl(const FnDeclStmt& fn, int line, int col) {
  currentLine_ = line;
  currentCol_ = col;
  int reg = allocReg();
  if (fn.isNative) {
    // A `native fn` has no body: it binds the name to the function the host
    // registered under the same name (the registry `registerBuiltins` fills).
    // `GETGLOBAL` reports a clear error at run time when the host does not
    // provide it, which for a module means when the module is loaded. No
    // opcode is added and no proto is compiled.
    emit(OpCode::GETGLOBAL, reg, addString(fn.name));
  } else {
    int child = compileFunctionProto(fn.name, fn.params, fn.body, line, col, fn.returnType);
    emit(OpCode::CLOSURE, reg, child);
  }

  if (inGlobalScope()) {
    // Top-level functions are globals; the interpreter hoists them in the global
    // scope, where they persist across REPL entries.
    emit(OpCode::SETGLOBAL, reg, addString(fn.name));
    declareGlobal(fn.name, false);
    declareGlobalType(fn.name, TypeRef{"function", false});
    userGlobals_.insert(fn.name);
    // Keep the signature table in step with the binding that actually wins: a
    // native keeps its signature, any other global of the same name drops it.
    if (fn.isNative) {
      nativeSigs_[fn.name] = NativeSig{fn.params, fn.returnType};
    } else {
      nativeSigs_.erase(fn.name);
    }
  } else {
    // Local declaration: a frame slot holding the closure value (PHASE 6b).
    // `compileBlockBody` reserves the slot so mutual recursion resolves and a
    // later declaration can reuse the same cell; fill it in place.
    auto predeclared = predeclaredFnSlots_.find(&fn);
    if (predeclared != predeclaredFnSlots_.end()) {
      int slot = predeclared->second;
      emit(OpCode::SETLOCAL, reg, slot);
      scopes_.back().vars[fn.name] =
          LocalVar{fn.name, slot, false, TypeRef{"function", false}, fn.isNative};
    } else {
      auto existing = scopes_.back().vars.find(fn.name);
      if (existing != scopes_.back().vars.end()) {
        emit(OpCode::SETLOCAL, reg, existing->second.slot);
        existing->second.isNative = fn.isNative;
      } else {
        int slot = declareLocal(fn.name, false, TypeRef{"function", false});
        emit(OpCode::DECLLOCAL, reg, slot);
        scopes_.back().vars[fn.name].isNative = fn.isNative;
      }
    }
  }
  freeReg();
}

// A `struct` declaration is pure compile-time metadata: it emits no bytecode.
// It records the field names and their optional types so a later `Name { ... }`
// literal can be checked against them. The declaration is module-level (the
// parser rejects a nested one), so a plain insert is enough; a duplicate name
// is already rejected by the parser.
void Compiler::compileStructDecl(const StructDeclStmt& decl, int line, int col) {
  (void)line;
  (void)col;
  structDefs_[decl.name] = decl.fields;
}

// Registers every `struct` declared directly in `block` before anything else in
// it is compiled. Function declarations are hoisted and compiled first, so
// without this pass a hoisted function that builds a struct literal would not
// yet see the shape when its body is compiled.
void Compiler::registerStructDecls(const BlockStmt& block) {
  for (const auto& child : block.stmts) {
    if (!child) continue;
    if (const auto* decl = std::get_if<StructDeclStmt>(&child->node)) {
      structDefs_[decl->name] = decl->fields;
    }
  }
}

// Registers every `native fn` declared directly in `block` before anything else
// in it is compiled. A hoisted function is compiled first, so without this pass
// a call to a native declared later in the file would not be checked.
void Compiler::registerNativeDecls(const BlockStmt& block) {
  for (const auto& child : block.stmts) {
    if (!child) continue;
    if (const auto* fn = std::get_if<FnDeclStmt>(&child->node)) {
      if (fn->isNative) nativeSigs_[fn->name] = NativeSig{fn->params, fn->returnType};
    } else if (const auto* ext = std::get_if<ExternBlockStmt>(&child->node)) {
      // An `extern "C"` declaration is a native, so a hoisted function that
      // calls one is checked against its signature exactly like `native fn`.
      for (const FnDeclStmt& decl : ext->decls) {
        nativeSigs_[decl.name] = NativeSig{decl.params, decl.returnType};
      }
    }
  }
}

// An `extern "C" { ... }` block emits no bytecode of its own: each declaration
// binds its name to the C symbol of the same name, exactly like a top-level
// `native fn`. The block is pure compile-time metadata (the ABI string is
// validated by the parser), so this validates each signature, records it in
// `bc_.externs` for the engine to resolve, and then follows the `native fn`
// path so the call site is type-checked and the binding is emitted.
void Compiler::compileExternBlock(const ExternBlockStmt& block, int line, int col) {
  for (const FnDeclStmt& fn : block.decls) {
    if (std::any_of(fn.params.begin(), fn.params.end(),
                    [](const Param& p) { return p.variadic; })) {
      fail("extern \"C\" function '" + fn.name + "' cannot be variadic", line, col);
    }
    std::vector<std::string> paramTypes;
    paramTypes.reserve(fn.params.size());
    for (const Param& param : fn.params) paramTypes.push_back(param.type.name);
    const std::string returnType = fn.returnType.name;
    std::string problem = ffi::validateSignature(paramTypes, returnType);
    if (!problem.empty()) {
      fail("extern \"C\" function '" + fn.name + "': " + problem, line, col);
    }
    // The engine reads this to resolve the symbol and build the marshalling
    // thunk; the compiler stays free of any host/ABI dependency (§2 boundary).
    bc_.externs.push_back(ExternSymbol{fn.name, std::move(paramTypes), returnType});
    compileFnDecl(fn, line, col);
  }
}

void Compiler::compileReturn(const ReturnStmt& stmt) {
  if (!inFunction_) fail("'return' outside of a function", currentLine_, currentCol_);
  // A `return` inside a `defer` block is rejected: the block is inlined at the
  // scope exit, so returning from it would re-enter the teardown and could
  // recurse forever. A nested function inside the deferred body clears
  // `deferStack_`, so its own `return` is still allowed.
  if (!deferStack_.empty()) {
    fail("'return' is not allowed inside a 'defer' block", currentLine_, currentCol_);
  }

  int reg;
  if (stmt.value) {
    if (currentReturnType_.isPresent() && currentReturnType_.name == "void") {
      fail("cannot return a value from a function declared to return 'void'; "
           "use a bare 'return;'",
           stmt.value->line, stmt.value->col);
    }
    // Check the returned value against the declared return type (PHASE 6.5),
    // when both are known. Types are metadata only: the check never changes
    // the emitted code.
    if (currentReturnType_.isPresent()) {
      TypeRef valueType = inferExprType(*stmt.value);
      checkAssignable(valueType, currentReturnType_, "the return type", stmt.value->line,
                      stmt.value->col);
    }
    reg = compileExpr(*stmt.value);
  } else {
    if (currentReturnType_.isPresent() && currentReturnType_.name != "any" &&
        currentReturnType_.name != "void" && !currentReturnType_.nullable) {
      fail("cannot return 'nil' from a function declared to return '" +
               describeType(currentReturnType_) + "' (use '" + currentReturnType_.name + "?')",
           currentLine_, currentCol_);
    }
    reg = allocReg();
    emit(OpCode::LOADK, reg, addConstant(Value()));
  }
  // Leaving the function exits every scope in it, so every deferred block runs
  // (LIFO) before the RETURN. The value stays live in `reg` while they run, so
  // the deferred blocks cannot change what the function returns. This is also
  // why `defer` needs no opcode and no VM support: the blocks are inlined here.
  emitDeferredBlocks(1);
  // A `return` at the top level of a function body is followed by the
  // implicit epilogue emitted in `compileFunctionProto`. Skipping the second
  // RETURN when the last instruction already returns keeps the frame stack
  // consistent: a duplicate RETURN would pop the caller's frame too (PHASE 6b).
  const bool alreadyReturns = !bc_.code.empty() &&
                              bc_.code.back().op == OpCode::RETURN &&
                              bc_.code.back().a == reg && !lastWasConditionalReturn_;
  if (!alreadyReturns) emit(OpCode::RETURN, reg);
  freeReg();
}

int Compiler::compileFunctionProto(const std::string& name,
                                   const std::vector<Param>& params, const StmtPtr& body,
                                   int line, int col, const TypeRef& returnType) {
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
  std::vector<DeferCtx> parentDeferStack = std::move(deferStack_);
  const int parentLine = currentLine_;
  const int parentCol = currentCol_;
  const bool parentInFunction = inFunction_;
  const TypeRef parentReturnType = currentReturnType_;

  bc_ = Bytecode{};
  bc_.name = name;
  bc_.numParams = static_cast<int>(params.size());
  // A trailing `rest...` parameter makes the function variadic: the frame takes
  // `numParams - 1` or more arguments and the VM binds the last slot to an
  // array of the extras (PHASE 6.5). The parser guarantees it is the last one.
  bc_.variadic = !params.empty() && params.back().variadic;
  currentUpvalueNames_.clear();
  nextReg_ = 0;
  maxReg_ = 0;
  nextLocal_ = 0;
  maxLocal_ = 0;
  scopes_.clear();
  loops_.clear();
  // A function body is a fresh `defer` boundary: a `return` inside it belongs
  // to this function, so any outer `defer` context must not reject it.
  deferStack_.clear();
  inFunction_ = true;

  // The child frame keeps the parent's state alive so nested closures can
  // resolve and re-export upvalues through it (see `resolveFromFrame`). A
  // deque keeps these addresses stable while more frames are pushed.
  FuncState saved;
  saved.bc = std::move(parent);
  saved.scopes = std::move(parentScopes);
  saved.loops = std::move(parentLoops);
  saved.deferStack = std::move(parentDeferStack);
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
  for (const Param& param : params) {
    // A variadic parameter binds an array of the extra arguments, so its
    // annotation describes the elements and its binding type is `array`.
    TypeRef bindingType = param.variadic ? TypeRef{"array", false} : param.type;
    declareLocal(param.name, false, bindingType);
  }
  currentReturnType_ = returnType;

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

  // Falling off the end exits the function body's scope, so its deferred
  // blocks run before the implicit nil return (an explicit `return` already
  // emitted them on its own path).
  emitDeferredBlocks(1);

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
  deferStack_ = std::move(savedParent.deferStack);
  currentLine_ = savedParent.line;
  currentCol_ = savedParent.col;
  inFunction_ = savedParent.inFunction;
  currentReturnType_ = parentReturnType;

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
  int child = compileFunctionProto("<anonymous>", lambda.params, lambda.body, line, col,
                                   TypeRef{});
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
  // `while` has no scope of its own; the body's scope (pushed by
  // `compileScopedBlock`) is one deeper. A `break`/`continue` unwinds down to
  // it, so a `defer` in the body runs even when the loop is left early.
  loops_.back().deferMinDepth = scopes_.size() + 1;
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
  // Unwind down to the body's scope (one deeper than the `for` scope) so a
  // `defer` in the body runs on `break`/`continue`.
  loops_.back().deferMinDepth = scopes_.size() + 1;
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
  // ITER has three operands (item, iterable, cursor). The `done` flag needs
  // a fourth output, so the compiler stores its register in `instruction.jump`
  // and the VM reads it from there. That is a private convention, not a real
  // jump target: ITER is not a jump instruction.
  bc_.code[static_cast<size_t>(iterIndex)].jump = done;

  int exitJump = emitJump(OpCode::JMP_IF_TRUE, done);
  // Each turn gets a fresh cell for the loop variable, so closures created in
  // the body capture the value of their own iteration (interpreter parity).
  pushScope();
  int slot = declareLocal(s.var, false);
  emit(OpCode::DECLLOCAL, item, slot);

  loops_.push_back(Loop{});
  // `for-in` compiles the body inline in the loop-variable scope, so that
  // scope (the current one) is the depth `break`/`continue` unwind to.
  loops_.back().deferMinDepth = scopes_.size();
  if (const auto* block = std::get_if<BlockStmt>(&s.body->node)) {
    compileBlockBody(*block);
  } else {
    compileStmt(*s.body);
  }
  // End of a normal iteration: the body's deferred blocks run before looping.
  emitDeferredBlocks(scopes_.size());
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

void Compiler::compileSwitch(const SwitchStmt& s) {
  // The subject is evaluated exactly once and kept in a register for the whole
  // switch. Every case label is compared against it: equality for a literal,
  // membership (`IN`) for a range. There is no fallthrough, so the matching
  // case's body is the only one that runs and each body ends with a jump past
  // the remaining cases.
  int subject = compileExpr(*s.subject);

  const size_t caseCount = s.cases.size();
  std::vector<int> testStart(caseCount, 0);   // where each case's label tests begin
  std::vector<int> bodyStart(caseCount, 0);   // where each case's body begins
  std::vector<int> noMatch(caseCount, -1);    // jump taken when no label matched
  std::vector<std::vector<int>> matchJumps(caseCount);
  std::vector<int> endJumps;

  for (size_t i = 0; i < caseCount; ++i) {
    const SwitchCase& entry = s.cases[i];
    testStart[i] = currentOffset();
    for (const ExprPtr& label : entry.values) {
      if (std::holds_alternative<RangeExpr>(label->node)) {
        int range = compileExpr(*label);
        emit(OpCode::IN, range, subject, range);
        matchJumps[i].push_back(emitJump(OpCode::JMP_IF_TRUE, range));
        freeReg();
      } else {
        int value = compileExpr(*label);
        emit(OpCode::EQ, value, subject, value);
        matchJumps[i].push_back(emitJump(OpCode::JMP_IF_TRUE, value));
        freeReg();
      }
    }
    // No label matched: skip this body and fall to the next case's tests.
    noMatch[i] = emitJump(OpCode::JMP);

    bodyStart[i] = currentOffset();
    compileScopedBlock(*entry.body);
    endJumps.push_back(emitJump(OpCode::JMP));
  }

  const int defaultStart = currentOffset();
  if (s.defaultBody) compileScopedBlock(*s.defaultBody);
  const int end = currentOffset();

  for (size_t i = 0; i < caseCount; ++i) {
    for (int jump : matchJumps[i]) patchJump(jump, bodyStart[i]);
    const int target = (i + 1 < caseCount) ? testStart[i + 1] : defaultStart;
    patchJump(noMatch[i], target);
  }
  for (int jump : endJumps) patchJump(jump, end);

  freeReg();  // release the subject register
}

void Compiler::compileBreak(const Stmt& stmt) {
  if (loops_.empty()) fail("'break' outside of a loop", stmt.line, stmt.col);
  // A `break` that would leave a `defer` body is rejected: the deferred block
  // is inlined at the scope exit, so jumping out of it mid-teardown would make
  // the order in which deferred blocks run ambiguous (and could re-enter the
  // same block while it is being emitted). A loop declared inside the deferred
  // body raises `loops_.size()` past the recorded depth and is still allowed.
  if (!deferStack_.empty() && deferStack_.back().loopDepth >= loops_.size()) {
    fail("'break' is not allowed inside a 'defer' block", stmt.line, stmt.col);
  }
  emitDeferredBlocks(loops_.back().deferMinDepth);
  int jump = emitJump(OpCode::JMP);
  loops_.back().breaks.push_back(jump);
}

void Compiler::compileContinue(const Stmt& stmt) {
  if (loops_.empty()) fail("'continue' outside of a loop", stmt.line, stmt.col);
  if (!deferStack_.empty() && deferStack_.back().loopDepth >= loops_.size()) {
    fail("'continue' is not allowed inside a 'defer' block", stmt.line, stmt.col);
  }
  emitDeferredBlocks(loops_.back().deferMinDepth);
  int jump = emitJump(OpCode::JMP);
  loops_.back().continues.push_back(jump);
}

void Compiler::compileDefer(const DeferStmt& stmt) {
  if (!inFunction_) {
    fail("'defer' outside of a function", currentLine_, currentCol_);
  }
  if (!stmt.body) {
    fail("internal compiler error: defer body is missing", currentLine_, currentCol_);
  }
  // The block is not emitted where it appears: it is queued on the enclosing
  // scope and emitted (LIFO) at every point where that scope exits. Registering
  // it here, rather than emitting, is what lets the same source block run on
  // fall-through, `return` and `break`/`continue` alike.
  scopes_.back().defers.push_back(stmt.body);
}

void Compiler::emitDeferredBlocks(size_t minDepth) {
  if (minDepth == 0) minDepth = 1;
  // Snapshot the depth: compiling a deferred body pushes and pops its own
  // scope, so `scopes_` changes size during the loop below.
  const size_t topDepth = scopes_.size();
  for (size_t depth = topDepth; depth >= minDepth; --depth) {
    // Copy the list: compiling a deferred body may append to (and reallocate)
    // `scopes_`, which would invalidate a reference into the scope.
    std::vector<StmtPtr> defers = scopes_[depth - 1].defers;
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
      // Mark the boundary so a `return`/`break`/`continue` that would leave
      // this body is rejected while it is compiled.
      deferStack_.push_back(DeferCtx{loops_.size()});
      compileScopedBlock(**it);
      deferStack_.pop_back();
    }
    if (depth == minDepth) break;  // size_t: stop before it wraps around
  }
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
    emit(OpCode::GETGLOBAL, reg, addString(importBindingName(name)));
  }
  return reg;
}

int Compiler::compileCall(const CallExpr& call, int line, int col) {
  // A call to a declared `native fn` is checked against its signature: arity
  // must match and each argument must be assignable to its parameter type.
  // Types are metadata only, so the emitted code is the same either way.
  if (const auto* callee = std::get_if<VariableExpr>(&call.callee->node)) {
    if (const NativeSig* sig = nativeSigFor(callee->name)) {
      checkNativeCall(callee->name, *sig, call, line, col);
    }
  }

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

    // Type check (PHASE 6.5): a plain assignment must respect the binding's
    // declared or inferred type. Compound assignments (+=, ...) are left alone
    // because their result type is not tracked statically.
    if (assign.op == Tok::Eq) {
      TypeRef targetType;
      if (isLocal) {
        targetType = local.type;
      } else if (upvalueSlot < 0) {
        lookupGlobalType(name, targetType);
      }
      if (targetType.isPresent()) {
        checkAssignable(inferExprType(*assign.value), targetType, "'" + name + "'",
                        assign.value->line, assign.value->col);
      }
    }

    if (assign.op == Tok::Eq) {
      int value = compileExpr(*assign.value);
      if (isLocal) {
        emit(OpCode::SETLOCAL, value, local.slot);
      } else if (upvalueSlot >= 0) {
        emit(OpCode::SETUPVAL, value, upvalueSlot);
      } else {
        emit(OpCode::SETGLOBAL, value, addString(importBindingName(name)));
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
      emit(OpCode::GETGLOBAL, current, addString(importBindingName(name)));
    }
    int right = compileExpr(*assign.value);
    emit(op, current, current, right);
    freeReg();

    if (isLocal) {
      emit(OpCode::SETLOCAL, current, local.slot);
    } else if (upvalueSlot >= 0) {
      emit(OpCode::SETUPVAL, current, upvalueSlot);
    } else {
      emit(OpCode::SETGLOBAL, current, addString(importBindingName(name)));
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

int Compiler::compileTry(const TryExpr& node, int line, int col) {
  // `try` propagates by returning the error from the enclosing function, so it
  // obeys the same rules as `return`: it needs a function to return from, and
  // it is rejected inside a `defer` block (the deferred body is inlined at the
  // scope exit, so returning from it could re-enter the teardown).
  if (!inFunction_) fail("'try' outside of a function", line, col);
  if (!deferStack_.empty()) {
    fail("'try' is not allowed inside a 'defer' block", line, col);
  }

  int result = compileExpr(*node.operand);
  int failed = compileHelperCall("$isError", {result}, line, col);
  // On success the operand is already in `result` and the jump skips the
  // propagation, so a `try` costs one call and one conditional jump.
  int skip = emitJump(OpCode::JMP_IF_FALSE, failed);
  freeReg();  // the helper's result register
  // Leaving the function exits every scope in it, so every deferred block runs
  // (LIFO) before the RETURN, exactly as for an explicit `return`.
  emitDeferredBlocks(1);
  emit(OpCode::RETURN, result);
  patchJump(skip, currentOffset());
  lastWasConditionalReturn_ = true;
  return result;
}

int Compiler::compileCatch(const CatchExpr& node, int line, int col) {
  int result = compileExpr(*node.operand);
  int failed = compileHelperCall("$isError", {result}, line, col);
  // The fallback is compiled only into the failure branch; on success the jump
  // skips it and `result` (already the operand) is returned.
  int skip = emitJump(OpCode::JMP_IF_FALSE, failed);
  int fallback = compileExpr(*node.fallback);
  emit(OpCode::MOVE, result, fallback);
  freeReg();  // the fallback register
  freeReg();  // the helper's result register
  patchJump(skip, currentOffset());
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

// `Point { x: 1, y: 2 }` (PHASE 6.5). A struct value is a plain map at runtime,
// so the literal lowers to the same NEWTABLE/SETTABLE sequence as a map literal
// and adds no opcode. Before lowering, the fields are checked against the
// declaration: an unknown field is a typo, and a missing field would leave the
// map without a key the declaration promises. Field types are checked with the
// same `checkAssignable` rule as every other annotation.
int Compiler::compileStructLiteral(const StructLitExpr& literal, int line, int col) {
  auto found = structDefs_.find(literal.typeName);
  if (found == structDefs_.end()) {
    fail("unknown struct '" + literal.typeName + "'", line, col);
  }
  const std::vector<StructField>& fields = found->second;

  // Every declared field must appear exactly once in the literal. The parser
  // already rejected duplicates, so this only has to catch missing and unknown
  // names.
  std::unordered_map<std::string, const StructLitEntry*> provided;
  for (const StructLitEntry& entry : literal.entries) {
    const StructField* declared = nullptr;
    for (const StructField& field : fields) {
      if (field.name == entry.name) {
        declared = &field;
        break;
      }
    }
    if (declared == nullptr) {
      fail("unknown field '" + entry.name + "' in struct '" + literal.typeName + "'", line, col);
    }
    provided[entry.name] = &entry;
  }
  for (const StructField& field : fields) {
    if (provided.find(field.name) == provided.end()) {
      fail("missing field '" + field.name + "' in struct '" + literal.typeName + "'", line, col);
    }
  }

  // Lower to a map, in declaration order so the runtime table has a stable
  // shape. Each value is checked against its field's annotation when it has
  // one.
  int table = allocReg();
  emit(OpCode::NEWTABLE, table, 0, 1);
  for (const StructField& field : fields) {
    const StructLitEntry& entry = *provided[field.name];
    // The key is interned before the value, matching `compileMap`, so a struct
    // literal is byte-identical to the map literal it lowers to.
    int key = -addString(field.name) - 1;
    int value = compileExpr(*entry.value);
    if (field.type.isPresent()) {
      TypeRef valueType = inferExprType(*entry.value);
      checkAssignable(valueType, field.type, "field '" + field.name + "'", entry.value->line,
                      entry.value->col);
    }
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

int Compiler::compileSlice(const SliceExpr& slice, int line, int col) {
  int object = compileExpr(*slice.object);
  // An omitted bound is passed as nil; the `$slice` helper treats nil as
  // "from the start" / "to the end", so the helper has one fixed arity.
  int start;
  if (slice.start) {
    start = compileExpr(*slice.start);
  } else {
    start = allocReg();
    emit(OpCode::LOADK, start, addConstant(Value()));
  }
  int end;
  if (slice.end) {
    end = compileExpr(*slice.end);
  } else {
    end = allocReg();
    emit(OpCode::LOADK, end, addConstant(Value()));
  }
  int result = compileHelperCall("$slice", {object, start, end}, line, col);
  emit(OpCode::MOVE, object, result);
  freeReg(nextReg_ - object - 1);
  return object;
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
          [&](const StructLitExpr& literal) -> int {
            return compileStructLiteral(literal, expr.line, expr.col);
          },
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

            // `a ?? b` (PHASE 6.5): yields `a` unless it is nil, in which case
            // it evaluates `b`. The test is an explicit `== nil` (not
            // truthiness) so `0`, `false` and `""` are kept, and the right-hand
            // side is skipped entirely when `a` is not nil.
            if (binary.op == Tok::QuestionQuestion) {
              int left = compileExpr(*binary.lhs);
              int nilReg = allocReg();
              emit(OpCode::LOADK, nilReg, addConstant(Value()));
              int isNil = allocReg();
              emit(OpCode::EQ, isNil, left, nilReg);
              int endJump = emitJump(OpCode::JMP_IF_FALSE, isNil);
              int right = compileExpr(*binary.rhs);
              emit(OpCode::MOVE, left, right);
              patchJump(endJump, currentOffset());
              freeReg(nextReg_ - left - 1);
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
          [&](const SliceExpr& slice) -> int {
            return compileSlice(slice, expr.line, expr.col);
          },
          [&](const MemberExpr& member) -> int {
            int object = compileExpr(*member.object);
            if (!member.safe) {
              int key = -addString(member.name) - 1;
              emit(OpCode::GETTABLE, object, object, key);
              return object;
            }
            // `a?.b` (PHASE 6.5): when `a` is nil the member is not looked up
            // and the result is nil (the register already holds nil). The guard
            // is an explicit `== nil`, so a non-nil object such as `0` or `""`
            // is still dereferenced.
            int nilReg = allocReg();
            emit(OpCode::LOADK, nilReg, addConstant(Value()));
            int isNil = allocReg();
            emit(OpCode::EQ, isNil, object, nilReg);
            int skip = emitJump(OpCode::JMP_IF_TRUE, isNil);
            int key = -addString(member.name) - 1;
            emit(OpCode::GETTABLE, object, object, key);
            patchJump(skip, currentOffset());
            freeReg(nextReg_ - object - 1);
            return object;
          },
          [&](const NonNullExpr& assertion) -> int {
            // `a!!` (PHASE 6.5): the `$nonNull` helper raises when the operand
            // is nil and returns it otherwise. Lowered as a call, so no opcode
            // is added to the budget.
            int operand = compileExpr(*assertion.operand);
            int result = compileHelperCall("$nonNull", {operand}, expr.line, expr.col);
            emit(OpCode::MOVE, operand, result);
            freeReg(nextReg_ - operand - 1);
            return operand;
          },
          [&](const RangeExpr& range) -> int {
            return compileRange(range, expr.line, expr.col);
          },
          [&](const TryExpr& node) -> int { return compileTry(node, expr.line, expr.col); },
          [&](const CatchExpr& node) -> int { return compileCatch(node, expr.line, expr.col); },
          [&](const auto&) -> int {
            fail("this expression is not supported by the bytecode compiler yet (Phase 6)",
                 expr.line, expr.col);
          },
      },
      expr.node);
}