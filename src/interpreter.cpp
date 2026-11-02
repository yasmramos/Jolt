#include "interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "builtins.h"
#include "ops.h"
#include "util.h"

namespace {

struct EnvGuard {
  Interpreter& interp;
  std::shared_ptr<Env> saved;
  explicit EnvGuard(Interpreter& i) : interp(i), saved(i.env) {}
  ~EnvGuard() { interp.env = std::move(saved); }
};

struct CallGuard {
  Interpreter& interp;
  explicit CallGuard(Interpreter& i) : interp(i) {}
  ~CallGuard() {
    if (!interp.stack.empty()) interp.stack.pop_back();
    --interp.depth;
  }
};

Tok baseAssignOp(Tok op) {
  switch (op) {
    case Tok::PlusEq:
      return Tok::Plus;
    case Tok::MinusEq:
      return Tok::Minus;
    case Tok::StarEq:
      return Tok::Star;
    case Tok::SlashEq:
      return Tok::Slash;
    case Tok::PercentEq:
      return Tok::Percent;
    case Tok::StarStarEq:
      return Tok::StarStar;
    default:
      return op;
  }
}

}  // namespace

// --------------------------------------------------------------- Interpreter

Interpreter::Interpreter(std::string name) : globals(std::make_shared<Env>()) {
  sourceName = std::move(name);
  env = globals;
  registerBuiltins(*this);
}

Interpreter::~Interpreter() {
  // Release everything the interpreter owns, then reclaim the cycles left among
  // objects that nothing else references.
  env.reset();
  globals->clear();
  globals.reset();
  gcCollect();
}

void Interpreter::defineGlobal(const std::string& name, Value value, bool isImmutable) {
  globals->define(name, std::move(value), isImmutable);
}

void Interpreter::run(const StmtPtr& program) {
  // Drop user definitions from a previous run() so that repeated executions
  // (for example one-per-line in the REPL) do not keep every intermediate
  // value alive until process exit. Builtins are registered in the
  // constructor and are re-installed by clearGlobals().
  if (env != globals) env = globals;
  depth = 0;
  stack.clear();
  globals->clear();
  registerBuiltins(*this);

  try {
    execBlock(*program, globals, false);
  } catch (const BreakSignal& signal) {
    fail("'break' outside of a loop", signal.line, signal.col);
  } catch (const ContinueSignal& signal) {
    fail("'continue' outside of a loop", signal.line, signal.col);
  } catch (const ReturnSignal&) {
    fail("'return' outside of a function");
  }

  // Top-level functions close over the global scope that also holds them.
  // Releasing the globals here breaks that cycle at the end of the run, and the
  // collector reclaims whatever cycles the program left behind.
  globals->clear();
  registerBuiltins(*this);
  gcCollect();
}

void Interpreter::runStatement(const Stmt& stmt) {
  try {
    exec(stmt);
  } catch (const BreakSignal& signal) {
    fail("'break' outside of a loop", signal.line, signal.col);
  } catch (const ContinueSignal& signal) {
    fail("'continue' outside of a loop", signal.line, signal.col);
  } catch (const ReturnSignal&) {
    fail("'return' outside of a function");
  }
}

void Interpreter::exec(const Stmt& stmt) {
  std::visit(
      Overloaded{
          [&](const ExprStmt& s) { eval(*s.expr); },
          [&](const LetStmt& s) {
            Value value = s.init ? eval(*s.init) : Value();
            env->define(s.name, std::move(value), s.kind != Tok::Var);
          },
          [&](const FnDeclStmt& s) {
            env->define(s.name, Value(makeFunction(s.name, s.params, s.body, env)), false);
          },
          [&](const BlockStmt&) { execBlock(stmt, env, true); },
          [&](const IfStmt& s) {
            if (eval(*s.condition).truthy()) {
              execBlock(*s.thenBranch, env, true);
            } else if (s.elseBranch) {
              exec(*s.elseBranch);
            }
          },
          [&](const WhileStmt& s) {
            while (eval(*s.condition).truthy()) {
              try {
                execBlock(*s.body, env, true);
              } catch (const ContinueSignal&) {
                continue;
              } catch (const BreakSignal&) {
                break;
              }
            }
          },
          [&](const ForStmt& s) {
            auto loopEnv = std::make_shared<Env>(env);
            EnvGuard guard(*this);
            env = loopEnv;
            if (s.init) exec(*s.init);
            while (s.condition == nullptr || eval(*s.condition).truthy()) {
              try {
                execBlock(*s.body, env, true);
              } catch (const ContinueSignal&) {
                // fall through to the step expression
              } catch (const BreakSignal&) {
                break;
              }
              if (s.step) eval(*s.step);
            }
          },
          [&](const ForInStmt& s) {
            Value iterable = eval(*s.iterable);
            // Non-range iterables are realized eagerly (arrays/strings/maps
            // already store their elements, so this is just a view/copy of
            // existing data). Ranges are the exception: they are stepped
            // lazily from their bounds so `for i in 0..N {}` stays O(1) in
            // memory regardless of N.
            std::vector<Value> sequence;
            if (iterable.isArray()) {
              sequence = iterable.arr().items;
            } else if (iterable.isStr()) {
              for (auto& ch : utf8Chars(iterable.asStr())) sequence.emplace_back(std::move(ch));
            } else if (iterable.isMap()) {
              for (auto& entry : iterable.map().entries) sequence.push_back(Value(entry.first));
            } else if (!iterable.isRange()) {
              fail(std::string("cannot iterate over a value of type '") + iterable.typeName() + "'",
                   s.iterable->line, s.iterable->col);
            }

            auto runIteration = [&](const Value& item) {
              // Each iteration gets its own binding of the loop variable, so
              // closures created in the body capture a different one per turn.
              // The body runs directly in this scope (no nested block scope).
              auto iterationEnv = std::make_shared<Env>(env);
              iterationEnv->define(s.var, item, false);
              EnvGuard guard(*this);
              env = iterationEnv;
              try {
                execBlock(*s.body, env, false);
              } catch (const ContinueSignal&) {
                // continue to the next item
              } catch (const BreakSignal&) {
                return false;
              }
              return true;
            };

            if (iterable.isRange()) {
              const Range& range = iterable.range();
              if (range.start <= range.end) {
                for (int64_t i = range.start; range.inclusive ? i <= range.end : i < range.end;
                     ++i) {
                  if (!runIteration(Value(i))) break;
                }
              } else {
                for (int64_t i = range.start; range.inclusive ? i >= range.end : i > range.end;
                     --i) {
                  if (!runIteration(Value(i))) break;
                }
              }
            } else {
              for (auto& item : sequence) {
                if (!runIteration(item)) break;
              }
            }
          },
          [&](const BreakStmt&) { throw BreakSignal{stmt.line, stmt.col}; },
          [&](const ContinueStmt&) { throw ContinueSignal{stmt.line, stmt.col}; },
          [&](const ReturnStmt& s) {
            ReturnSignal signal;
            if (s.value) signal.value = eval(*s.value);
            throw signal;
          },
      },
      stmt.node);
}

void Interpreter::execBlock(const Stmt& block, const std::shared_ptr<Env>& target, bool newScope) {
  // A safe point: every object reachable from the running program is held by
  // a variable, the call stack or a scope, so only unreachable cycles are freed.
  gcCollectIfNeeded();

  const auto& body = std::get<BlockStmt>(block.node);
  std::shared_ptr<Env> scope = newScope ? std::make_shared<Env>(target) : target;

  EnvGuard guard(*this);
  env = scope;

  // Hoist function declarations so that recursion and mutual recursion work.
  for (const auto& stmt : body.stmts) {
    if (auto* fn = std::get_if<FnDeclStmt>(&stmt->node)) {
      scope->define(fn->name, Value(makeFunction(fn->name, fn->params, fn->body, scope)), false);
    }
  }

  for (const auto& stmt : body.stmts) exec(*stmt);
}

FuncPtr Interpreter::makeFunction(const std::string& name, const std::vector<std::string>& params,
                                  const StmtPtr& body, std::shared_ptr<Env> closure) {
  auto function = makeRef<Function>();
  function->name = name;
  function->params = params;
  function->body = body;
  function->closure = std::move(closure);
  return function;
}

// ---------------------------------------------------------------- evaluation

Value Interpreter::eval(const Expr& expr) {
  currentLine = expr.line;
  currentCol = expr.col;

  return std::visit(
      Overloaded{
          [&](const LiteralExpr& e) { return e.value; },
          [&](const InterpExpr& e) {
            std::string result;
            for (const auto& part : e.parts) {
              if (part.isText()) {
                result += part.text();
              } else {
                result += eval(*part.expr()).toString();
              }
            }
            return Value(std::move(result));
          },
          [&](const VariableExpr& e) -> Value {
            Binding* binding = env->find(e.name);
            if (!binding) fail("undefined variable '" + e.name + "'", expr.line, expr.col);
            return binding->value;
          },
          [&](const ArrayExpr& e) {
            std::vector<Value> items;
            items.reserve(e.elements.size());
            for (const auto& element : e.elements) items.push_back(eval(*element));
            return Value(makeArray(std::move(items)));
          },
          [&](const MapExpr& e) {
            auto map = makeMap();
            for (const auto& entry : e.entries) {
              // Literal keys are already normalized strings; computed keys
              // evaluate to a Value that is canonicalized via toString() so
              // it matches the runtime's string-keyed map representation.
              std::string key = entry.keyExpr ? eval(*entry.keyExpr).toString() : entry.literalKey;
              map->set(key, eval(*entry.value));
            }
            return Value(std::move(map));
          },
          [&](const UnaryExpr& e) { return unary(e.op, eval(*e.operand), expr.line, expr.col); },
          [&](const BinaryExpr& e) -> Value {
            Value left = eval(*e.lhs);
            if (e.op == Tok::AmpAmp) {
              return left.truthy() ? eval(*e.rhs) : left;
            }
            if (e.op == Tok::PipePipe) {
              return left.truthy() ? left : eval(*e.rhs);
            }
            Value right = eval(*e.rhs);
            return binary(e.op, left, right, e.opLine, e.opCol);
          },
          [&](const AssignExpr& e) -> Value {
            if (e.op == Tok::Eq) {
              Value value = eval(*e.value);
              assignTo(*e.target, value);
              return value;
            }

            Tok op = baseAssignOp(e.op);
            Value right = eval(*e.value);

            if (auto* var = std::get_if<VariableExpr>(&e.target->node)) {
              Binding* binding = env->find(var->name);
              if (!binding) fail("undefined variable '" + var->name + "'", e.target->line, e.target->col);
              if (binding->isImmutable) {
                fail("cannot assign to immutable variable '" + var->name + "'", e.target->line, e.target->col);
              }
              Value result = binary(op, binding->value, right, e.opLine, e.opCol);
              binding->value = result;
              return result;
            }
            if (auto* index = std::get_if<IndexExpr>(&e.target->node)) {
              Value object = eval(*index->object);
              Value key = eval(*index->index);
              Value current = indexGet(object, key, e.target->line, e.target->col);
              Value result = binary(op, current, right, e.opLine, e.opCol);
              indexSet(object, key, result, e.target->line, e.target->col);
              return result;
            }
            if (auto* member = std::get_if<MemberExpr>(&e.target->node)) {
              Value object = eval(*member->object);
              Value current = memberGet(object, member->name, e.target->line, e.target->col);
              Value result = binary(op, current, right, e.opLine, e.opCol);
              memberSet(object, member->name, result, e.target->line, e.target->col);
              return result;
            }
            fail("invalid assignment target", expr.line, expr.col);
          },
          [&](const TernaryExpr& e) -> Value {
            return eval(*e.condition).truthy() ? eval(*e.thenExpr) : eval(*e.elseExpr);
          },
          [&](const CallExpr& e) -> Value {
            Value callee = eval(*e.callee);
            std::vector<Value> args;
            args.reserve(e.args.size());
            for (const auto& arg : e.args) args.push_back(eval(*arg));
            return call(callee, std::move(args), expr.line, expr.col);
          },
          [&](const IndexExpr& e) {
            Value object = eval(*e.object);
            Value index = eval(*e.index);
            return indexGet(object, index, expr.line, expr.col);
          },
          [&](const MemberExpr& e) {
            Value object = eval(*e.object);
            return memberGet(object, e.name, expr.line, expr.col);
          },
          [&](const RangeExpr& e) -> Value {
            Value start = eval(*e.start);
            Value end = eval(*e.end);
            if (!start.isInt() || !end.isInt()) {
              fail("range bounds must be integers", expr.line, expr.col);
            }
            return Value(makeRange(start.asInt(), end.asInt(), e.inclusive));
          },
          [&](const LambdaExpr& e) {
            return Value(makeFunction("<lambda>", e.params, e.body, env));
          },
      },
      expr.node);
}

Value Interpreter::evalRepl(const ExprPtr& expr) {
  try {
    return eval(*expr);
  } catch (const BreakSignal& signal) {
    fail("'break' outside of a loop", signal.line, signal.col);
  } catch (const ContinueSignal& signal) {
    fail("'continue' outside of a loop", signal.line, signal.col);
  } catch (const ReturnSignal&) {
    fail("'return' outside of a function");
  }
}

Value Interpreter::unary(Tok op, const Value& operand, int line, int col) {
  return ops::unary(*this, op, operand, line, col);
}

bool Interpreter::lessThan(const Value& left, const Value& right, int line, int col) {
  return ops::lessThan(*this, left, right, line, col);
}

Value Interpreter::binary(Tok op, const Value& left, const Value& right, int line, int col) {
  return ops::binary(*this, op, left, right, line, col);
}

// ------------------------------------------------------------------- lvalues

Value Interpreter::assignTo(const Expr& target, Value value) {
  if (auto* var = std::get_if<VariableExpr>(&target.node)) {
    switch (env->assign(var->name, value)) {
      case Env::Assign::Ok:
        return value;
      case Env::Assign::Immutable:
        fail("cannot assign to immutable variable '" + var->name + "'", target.line, target.col);
      case Env::Assign::NotFound:
        fail("undefined variable '" + var->name + "' (declare it with 'var' or 'let')", target.line,
             target.col);
    }
  }
  if (auto* index = std::get_if<IndexExpr>(&target.node)) {
    Value object = eval(*index->object);
    Value key = eval(*index->index);
    indexSet(object, key, value, target.line, target.col);
    return value;
  }
  if (auto* member = std::get_if<MemberExpr>(&target.node)) {
    Value object = eval(*member->object);
    memberSet(object, member->name, value, target.line, target.col);
    return value;
  }
  fail("invalid assignment target", target.line, target.col);
}

Value Interpreter::indexGet(const Value& object, const Value& index, int line, int col) {
  return ops::indexGet(*this, object, index, line, col);
}

void Interpreter::indexSet(const Value& object, const Value& index, Value value, int line, int col) {
  ops::indexSet(*this, object, index, std::move(value), line, col);
}

Value Interpreter::memberGet(const Value& object, const std::string& name, int line, int col) {
  return ops::memberGet(*this, object, name, line, col);
}

void Interpreter::memberSet(const Value& object, const std::string& name, Value value, int line,
                            int col) {
  ops::memberSet(*this, object, name, std::move(value), line, col);
}

// ---------------------------------------------------------------------- calls

Value Interpreter::call(const Value& callee, std::vector<Value> args, int line, int col) {
  if (!callee.isFunc()) {
    fail(std::string("cannot call a value of type '") + callee.typeName() + "'", line, col);
  }

  // Keep the callee object strongly alive for the entire call. A temporary
  // bound method (`items.push(1)` on a transient array) owns the only strong
  // reference to its `Function`; letting it die mid-call would destroy the
  // closure — including its captured state and receiver handle — while the
  // native is still running.
  FuncPtr function = callee.funcPtr();
  Function& fn = *function;

  if (fn.isNative()) {
    int savedLine = currentLine;
    int savedCol = currentCol;
    currentLine = line;
    currentCol = col;
    try {
      Value result = fn.native(*this, args);
      currentLine = savedLine;
      currentCol = savedCol;
      return result;
    } catch (RuntimeError& error) {
      if (error.line == 0) {
        error.line = line;
        error.col = col;
      }
      currentLine = savedLine;
      currentCol = savedCol;
      throw;
    }
  }

  if (args.size() != fn.params.size()) {
    fail(fn.name + "() expects " + std::to_string(fn.params.size()) + " argument" +
             (fn.params.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()),
         line, col);
  }
  if (depth >= maxDepth) {
    fail("maximum call depth exceeded (possible infinite recursion)", line, col);
  }

  auto callEnv = std::make_shared<Env>(fn.closure);
  for (size_t i = 0; i < fn.params.size(); ++i) {
    callEnv->define(fn.params[i], std::move(args[i]), false);
  }
  // Named function expressions can refer to themselves recursively.
  if (!fn.name.empty() && fn.name != "<lambda>") {
    callEnv->define(fn.name, Value(function), false);
  }

  EnvGuard envGuard(*this);
  env = callEnv;
  ++depth;
  stack.push_back(Frame{fn.name, line, col});
  CallGuard callGuard(*this);

  try {
    execBlock(*fn.body, callEnv, false);
  } catch (const ReturnSignal& signal) {
    return signal.value;
  } catch (const BreakSignal& signal) {
    // 'break' cannot cross a function boundary.
    fail("'break' outside of a loop", signal.line, signal.col);
  } catch (const ContinueSignal& signal) {
    // 'continue' cannot cross a function boundary.
    fail("'continue' outside of a loop", signal.line, signal.col);
  }
  return Value();
}

// --------------------------------------------------------------------- errors

void Interpreter::fail(const std::string& message, int line, int col) const {
  RuntimeError error;
  error.message = message;
  error.line = line > 0 ? line : currentLine;
  error.col = col > 0 ? col : currentCol;
  error.stack = stack;
  throw error;
}

void Interpreter::failHere(const std::string& message) const {
  fail(message, currentLine, currentCol);
}
