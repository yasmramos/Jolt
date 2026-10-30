#include "interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "builtins.h"
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

int64_t normalizeIndex(Interpreter& interp, int64_t index, int64_t length, const char* kind) {
  int64_t original = index;
  if (index < 0) index += length;
  if (index < 0 || index >= length) {
    interp.fail(std::string(kind) + " index " + std::to_string(original) + " out of range (length " +
                std::to_string(length) + ")");
  }
  return index;
}

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

int64_t intPow(int64_t base, int64_t exponent) {
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

bool inRange(const Range& range, int64_t value) {
  if (range.start <= range.end) {
    if (range.inclusive) return value >= range.start && value <= range.end;
    return value >= range.start && value < range.end;
  }
  if (range.inclusive) return value >= range.end && value <= range.start;
  return value > range.end && value <= range.start;
}

}  // namespace

// ----------------------------------------------------------------------- Env

Binding* Env::find(const std::string& name) {
  for (Env* current = this; current != nullptr; current = current->parent.get()) {
    auto it = current->vars.find(name);
    if (it != current->vars.end()) return &it->second;
  }
  return nullptr;
}

void Env::define(const std::string& name, Value value, bool isConst) {
  Binding binding;
  binding.value = std::move(value);
  binding.isConst = isConst;
  vars[name] = std::move(binding);
}

Env::Assign Env::assign(const std::string& name, Value value) {
  for (Env* current = this; current != nullptr; current = current->parent.get()) {
    auto it = current->vars.find(name);
    if (it != current->vars.end()) {
      if (it->second.isConst) return Env::Assign::Const;
      it->second.value = std::move(value);
      return Env::Assign::Ok;
    }
  }
  return Env::Assign::NotFound;
}

// --------------------------------------------------------------- Interpreter

Interpreter::Interpreter(std::string name)
    : sourceName(std::move(name)), globals(std::make_shared<Env>()) {
  env = globals;
  registerBuiltins(*this);
}

void Interpreter::defineGlobal(const std::string& name, Value value, bool isConst) {
  globals->define(name, std::move(value), isConst);
}

void Interpreter::addNative(const std::string& name, NativeFn fn) {
  defineGlobal(name, makeNative(name, std::move(fn)));
}

void Interpreter::run(const StmtPtr& program) {
  try {
    execBlock(*program, globals, false);
  } catch (const BreakSignal& signal) {
    fail("'break' outside of a loop", signal.line, signal.col);
  } catch (const ContinueSignal& signal) {
    fail("'continue' outside of a loop", signal.line, signal.col);
  } catch (const ReturnSignal&) {
    fail("'return' outside of a function");
  }
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
            env->define(s.name, std::move(value), s.isConst);
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
            std::vector<Value> sequence;
            if (iterable.isArray()) {
              sequence = iterable.arr().items;
            } else if (iterable.isStr()) {
              for (auto& ch : utf8Chars(iterable.asStr())) sequence.emplace_back(std::move(ch));
            } else if (iterable.isMap()) {
              for (auto& entry : iterable.map().entries) sequence.push_back(Value(entry.first));
            } else if (iterable.isRange()) {
              const Range& range = iterable.range();
              if (range.start <= range.end) {
                for (int64_t i = range.start; range.inclusive ? i <= range.end : i < range.end; ++i) {
                  sequence.push_back(Value(i));
                }
              } else {
                for (int64_t i = range.start; range.inclusive ? i >= range.end : i > range.end; --i) {
                  sequence.push_back(Value(i));
                }
              }
            } else {
              fail(std::string("cannot iterate over a value of type '") + iterable.typeName() + "'",
                   s.iterable->line, s.iterable->col);
            }

            for (auto& item : sequence) {
              auto iterationEnv = std::make_shared<Env>(env);
              iterationEnv->define(s.var, item, false);
              EnvGuard guard(*this);
              env = iterationEnv;
              try {
                execBlock(*s.body, env, true);
              } catch (const ContinueSignal&) {
                continue;
              } catch (const BreakSignal&) {
                break;
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
              if (part.expr) {
                result += eval(*part.expr).toString();
              } else {
                result += part.text;
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
            for (const auto& entry : e.entries) map->set(entry.key, eval(*entry.value));
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
              if (binding->isConst) {
                fail("cannot assign to constant '" + var->name + "'", e.target->line, e.target->col);
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
            std::string name = e.name.empty() ? "<lambda>" : e.name;
            return Value(makeFunction(name, e.params, e.body, env));
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
  switch (op) {
    case Tok::Bang:
      return Value(!operand.truthy());
    case Tok::Minus:
      if (operand.isInt()) return Value(static_cast<int64_t>(0ULL - static_cast<uint64_t>(operand.asInt())));
      if (operand.isFloat()) return Value(-operand.asFloat());
      break;
    case Tok::Plus:
      if (operand.isNumber()) return operand;
      break;
    case Tok::Tilde:
      if (operand.isInt()) return Value(~operand.asInt());
      break;
    default:
      break;
  }
  fail(std::string("unsupported unary operator for type '") + operand.typeName() + "'", line, col);
}

bool Interpreter::lessThan(const Value& left, const Value& right, int line, int col) {
  if (left.isNumber() && right.isNumber()) {
    if (left.isInt() && right.isInt()) return left.asInt() < right.asInt();
    return left.number() < right.number();
  }
  if (left.isStr() && right.isStr()) return left.asStr() < right.asStr();
  if (left.isBool() && right.isBool()) return !left.asBool() && right.asBool();
  fail(std::string("cannot compare '") + left.typeName() + "' with '" + right.typeName() + "'", line,
       col);
}

Value Interpreter::binary(Tok op, const Value& left, const Value& right, int line, int col) {
  switch (op) {
    case Tok::Plus: {
      if (left.isInt() && right.isInt()) {
        return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) +
                                          static_cast<uint64_t>(right.asInt())));
      }
      if (left.isNumber() && right.isNumber()) return Value(left.number() + right.number());
      if (left.isStr() && right.isStr()) return Value(left.asStr() + right.asStr());
      if (left.isArray() && right.isArray()) {
        std::vector<Value> items = left.arr().items;
        items.insert(items.end(), right.arr().items.begin(), right.arr().items.end());
        return Value(makeArray(std::move(items)));
      }
      fail("unsupported operand types for '+': '" + std::string(left.typeName()) + "' and '" +
               right.typeName() + "'",
           line, col);
    }
    case Tok::Minus: {
      if (left.isInt() && right.isInt()) {
        return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) -
                                          static_cast<uint64_t>(right.asInt())));
      }
      if (left.isNumber() && right.isNumber()) return Value(left.number() - right.number());
      break;
    }
    case Tok::Star: {
      if (left.isInt() && right.isInt()) {
        return Value(static_cast<int64_t>(static_cast<uint64_t>(left.asInt()) *
                                          static_cast<uint64_t>(right.asInt())));
      }
      if (left.isNumber() && right.isNumber()) return Value(left.number() * right.number());
      break;
    }
    case Tok::Slash: {
      if (left.isNumber() && right.isNumber()) {
        if (right.number() == 0) fail("division by zero", line, col);
        if (left.isInt() && right.isInt()) {
          int64_t a = left.asInt();
          int64_t b = right.asInt();
          if (a == INT64_MIN && b == -1) return Value(a);
          return Value(a / b);
        }
        return Value(left.number() / right.number());
      }
      break;
    }
    case Tok::Percent: {
      if (left.isNumber() && right.isNumber()) {
        if (right.number() == 0) fail("division by zero", line, col);
        if (left.isInt() && right.isInt()) {
          int64_t a = left.asInt();
          int64_t b = right.asInt();
          if (a == INT64_MIN && b == -1) return Value(static_cast<int64_t>(0));
          return Value(a % b);
        }
        return Value(std::fmod(left.number(), right.number()));
      }
      break;
    }
    case Tok::StarStar: {
      if (left.isNumber() && right.isNumber()) {
        if (left.isInt() && right.isInt() && right.asInt() >= 0) {
          return Value(intPow(left.asInt(), right.asInt()));
        }
        return Value(std::pow(left.number(), right.number()));
      }
      break;
    }
    case Tok::EqEq:
      return Value(left.equals(right));
    case Tok::BangEq:
      return Value(!left.equals(right));
    case Tok::Lt:
      return Value(lessThan(left, right, line, col));
    case Tok::LtEq:
      return Value(!lessThan(right, left, line, col));
    case Tok::Gt:
      return Value(lessThan(right, left, line, col));
    case Tok::GtEq:
      return Value(!lessThan(left, right, line, col));
    case Tok::Amp:
    case Tok::Pipe:
    case Tok::Caret:
    case Tok::Shl:
    case Tok::Shr: {
      if (!left.isInt() || !right.isInt()) break;
      int64_t a = left.asInt();
      int64_t b = right.asInt();
      if (op == Tok::Amp) return Value(a & b);
      if (op == Tok::Pipe) return Value(a | b);
      if (op == Tok::Caret) return Value(a ^ b);
      if (b < 0 || b > 63) fail("shift count must be between 0 and 63", line, col);
      if (op == Tok::Shl) {
        return Value(static_cast<int64_t>(static_cast<uint64_t>(a) << static_cast<uint64_t>(b)));
      }
      return Value(a >> b);
    }
    case Tok::In: {
      if (right.isArray()) {
        for (const auto& item : right.arr().items) {
          if (item.equals(left)) return Value(true);
        }
        return Value(false);
      }
      if (right.isMap()) {
        if (!left.isStr()) fail("map membership requires a string key", line, col);
        return Value(right.map().has(left.asStr()));
      }
      if (right.isStr()) {
        if (!left.isStr()) fail("string membership requires a string operand", line, col);
        return Value(right.asStr().find(left.asStr()) != std::string::npos);
      }
      if (right.isRange()) {
        if (!left.isInt()) fail("range membership requires an integer", line, col);
        return Value(inRange(right.range(), left.asInt()));
      }
      fail(std::string("operator 'in' is not supported for '") + left.typeName() + "' and '" +
               right.typeName() + "'",
           line, col);
    }
    default:
      break;
  }

  fail(std::string("unsupported operand types for '") + std::string("operator") + "': '" +
           left.typeName() + "' and '" + right.typeName() + "'",
       line, col);
}

// ------------------------------------------------------------------- lvalues

Value Interpreter::assignTo(const Expr& target, Value value) {
  if (auto* var = std::get_if<VariableExpr>(&target.node)) {
    switch (env->assign(var->name, value)) {
      case Env::Assign::Ok:
        return value;
      case Env::Assign::Const:
        fail("cannot assign to constant '" + var->name + "'", target.line, target.col);
      case Env::Assign::NotFound:
        fail("undefined variable '" + var->name + "' (declare it with 'let')", target.line,
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
  if (object.isArray()) {
    if (!index.isInt()) fail("array index must be an integer", line, col);
    const auto& items = object.arr().items;
    int64_t i = normalizeIndex(*this, index.asInt(), static_cast<int64_t>(items.size()), "array");
    return items[static_cast<size_t>(i)];
  }
  if (object.isStr()) {
    if (!index.isInt()) fail("string index must be an integer", line, col);
    auto chars = utf8Chars(object.asStr());
    int64_t i = normalizeIndex(*this, index.asInt(), static_cast<int64_t>(chars.size()), "string");
    return Value(chars[static_cast<size_t>(i)]);
  }
  if (object.isMap()) {
    if (!index.isStr()) fail("map keys must be strings", line, col);
    const Value* found = object.map().get(index.asStr());
    return found ? *found : Value();
  }
  fail(std::string("cannot index a value of type '") + object.typeName() + "'", line, col);
}

void Interpreter::indexSet(const Value& object, const Value& index, Value value, int line, int col) {
  if (object.isArray()) {
    if (!index.isInt()) fail("array index must be an integer", line, col);
    auto& items = object.arr().items;
    int64_t i = normalizeIndex(*this, index.asInt(), static_cast<int64_t>(items.size()), "array");
    items[static_cast<size_t>(i)] = std::move(value);
    return;
  }
  if (object.isStr()) fail("strings are immutable", line, col);
  if (object.isMap()) {
    if (!index.isStr()) fail("map keys must be strings", line, col);
    object.map().set(index.asStr(), std::move(value));
    return;
  }
  fail(std::string("cannot assign to an index of a value of type '") + object.typeName() + "'", line,
       col);
}

Value Interpreter::memberGet(const Value& object, const std::string& name, int line, int col) {
  if (object.isMap()) {
    if (const Value* found = object.map().get(name)) return *found;
    if (name == "length") return Value(object.map().size());
    Value method;
    if (bindMethod(*this, object, name, method)) return method;
    return Value();
  }
  if (object.isArray()) {
    if (name == "length") return Value(object.arr().items.size());
  } else if (object.isStr()) {
    if (name == "length") return Value(utf8Length(object.asStr()));
  } else if (object.isRange()) {
    if (name == "length") return Value(object.range().length());
  }

  Value method;
  if (bindMethod(*this, object, name, method)) return method;

  fail(std::string("type '") + object.typeName() + "' has no member '" + name + "'", line, col);
}

void Interpreter::memberSet(const Value& object, const std::string& name, Value value, int line,
                            int col) {
  if (object.isMap()) {
    object.map().set(name, std::move(value));
    return;
  }
  fail(std::string("cannot set a field on a value of type '") + object.typeName() + "'", line, col);
}

// ---------------------------------------------------------------------- calls

Value Interpreter::call(const Value& callee, std::vector<Value> args, int line, int col) {
  if (!callee.isFunc()) {
    fail(std::string("cannot call a value of type '") + callee.typeName() + "'", line, col);
  }

  auto function = callee.funcPtr();
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
