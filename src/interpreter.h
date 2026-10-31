#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "errors.h"
#include "value.h"

struct Binding {
  Value value;
  bool isConst = false;
};

struct Env : std::enable_shared_from_this<Env> {
  // The parent link is weak on purpose. Reference counting cannot collect
  // cycles, and child -> parent -> function -> child is a cycle: hoisted
  // function declarations bind themselves inside their own defining scope
  // (so recursion works), which would keep every environment alive forever.
  // A closure therefore holds its definition environment with a strong
  // reference while scopes only *link* upward without owning their parent.
  // An environment stays alive exactly while something reachable owns it
  // (a closure or the interpreter's current chain), so `lock()` succeeds
  // whenever the parent still matters; otherwise the variable lookup simply
  // behaves like an undefined name.
  std::weak_ptr<Env> parent;
  std::unordered_map<std::string, Binding> vars;

  // Call/loop scopes use this instead of `parent`: a flat list of shared
  // pointers that keeps the whole lexical chain alive for as long as this
  // scope exists. Closures created inside such a scope capture it strongly,
  // so free variables remain resolvable after the scope "returns" (closures
  // over locals). Because nothing points back from ancestors to this scope,
  // the list cannot form a reference cycle: when the last closure dies, the
  // scope dies and each ancestor is released down to whoever else owns it.
  std::vector<std::shared_ptr<Env>> strongAncestors;

  explicit Env(std::shared_ptr<Env> parent = nullptr) : parent(std::move(parent)) {}

  // Builds a scope whose entire visible chain is kept alive strongly. Used
  // for function calls and loop bodies, where closures may outlive the scope.
  static std::shared_ptr<Env> createLinked(std::shared_ptr<Env> outer) {
    auto scope = std::make_shared<Env>();
    if (outer) {
      std::unordered_set<const Env*> seen;
      outer->collectAncestors(seen);
      for (const Env* ancestor : seen) {
        if (auto alive = const_cast<Env*>(ancestor)->shared_from_this()) {
          scope->strongAncestors.push_back(std::move(alive));
        }
      }
    }
    return scope;
  }

  // Walks up the scope chain looking for `name`. Returns nullptr when the
  // variable is not visible anymore (including when a parent environment has
  // already been collected through its weak link).
  Binding* find(const std::string& name);
  void define(const std::string& name, Value value, bool isConst = false);
  enum class Assign { Ok, NotFound, Const };
  Assign assign(const std::string& name, Value value);

  // Removes every binding (used when resetting the interpreter between runs).
  void clear() { vars.clear(); }

  // Adds `env` and all of its ancestors to `seen`. Used by call scopes to
  // keep their lexical chain alive for as long as closures created inside
  // them exist (see `Interpreter::callFunction`).
  void collectAncestors(std::unordered_set<const Env*>& seen) const {
    if (!seen.insert(this).second) return;  // already visited: stop recursing
    // Call/loop scopes store their chain flattened in `strongAncestors`;
    // block scopes use the single weak `parent` link.
    for (const auto& ancestor : strongAncestors) ancestor->collectAncestors(seen);
    if (auto parent_ = parent.lock()) parent_->collectAncestors(seen);
  }
};

// Visits every environment reachable from `start` through either kind of
// upward link (`parent` or `strongAncestors`) and returns the ones that
// contain at least one function value. Those environments must stay alive
// while hoisting, because the hoisted functions close over them.
inline std::vector<std::shared_ptr<Env>> envsHoldingFunctions(const std::shared_ptr<Env>& start) {
  std::vector<std::shared_ptr<Env>> result;
  std::unordered_map<const Env*, std::shared_ptr<Env>> owned;
  std::vector<std::shared_ptr<Env>> pending{start};
  while (!pending.empty()) {
    std::shared_ptr<Env> current = std::move(pending.back());
    pending.pop_back();
    const Env* key = current.get();
    if (owned.count(key)) continue;  // already visited
    owned.emplace(key, current);
    bool hasFunction = false;
    for (const auto& entry : current->vars) {
      if (entry.second.value.isFunc()) {
        hasFunction = true;
        break;
      }
    }
    if (hasFunction) result.push_back(current);
    for (const auto& ancestor : current->strongAncestors) pending.push_back(ancestor);
    if (auto parent_ = current->parent.lock()) pending.push_back(std::move(parent_));
  }
  return result;
}

struct BreakSignal {
  int line = 0;
  int col = 0;
};
struct ContinueSignal {
  int line = 0;
  int col = 0;
};
struct ReturnSignal {
  Value value;
};

class Interpreter {
 public:
  explicit Interpreter(std::string sourceName = "<script>");

  void run(const StmtPtr& program);
  void runStatement(const Stmt& stmt);
  void exec(const Stmt& stmt);
  Value eval(const Expr& expr);
  Value evalRepl(const ExprPtr& expr);

  Value call(const Value& callee, std::vector<Value> args, int line, int col);

  void defineGlobal(const std::string& name, Value value, bool isConst = false);
  void addNative(const std::string& name, NativeFn fn);


  // RAII guard that installs the strong receiver pin for the duration of one
  // native call frame (see `Interpreter::call`). Nesting works naturally:
  // each guard saves the outer slot on construction and restores it on
  // destruction, so a reference adopted from the pin always has its count
  // properly incremented while the innermost frame is running.
  struct ActiveReceiverGuard {
    explicit ActiveReceiverGuard(Interpreter& target, Ref<RefCounted> pin) noexcept
        : interpreter(target), saved(std::move(target.activeReceiver_)) {
      target.activeReceiver_ = std::move(pin);
    }
    ~ActiveReceiverGuard() { interpreter.activeReceiver_ = std::move(saved); }
    ActiveReceiverGuard(const ActiveReceiverGuard&) = delete;
    ActiveReceiverGuard& operator=(const ActiveReceiverGuard&) = delete;

    Interpreter& interpreter;
    Ref<RefCounted> saved;
  };

 private:
  // Transient strong reference to the receiver of the native method currently
  // being invoked (see `Function::boundReceiver`). Set by `call()` right
  // before the native runs through `ActiveReceiverGuard`, and restored
  // afterwards, so nested calls inside a native keep their own receiver.
  // Holding the object strongly here guarantees it stays alive for the whole
  // duration of the native even when the last external owner dies mid-call:
  // bound methods capture their receiver weakly to break ownership cycles
  // (`a.push(a)`), and `receiverOf` below re-adopts this pinned reference
  // instead of resurrecting the object from the (possibly expired) weak
  // handle.
  Ref<RefCounted> activeReceiver_;

 public:
  // Returns an owning reference to the live receiver of a bound array/map
  // method. Bound methods hold only a `WeakRef` to their container, which
  // breaks the ownership cycle that a strong capture would create when a
  // method is stored inside its own receiver (`a.push(a)`). At call time
  // `Interpreter::call` pins the receiver in `activeReceiver_`, so during the
  // native the live object is guaranteed strongly reachable; when its
  // identity matches the binding-time capture, the pin is converted into a
  // typed owning reference with a proper `retain()`. If the pin does not
  // match (or no native frame is active), the weak handle is locked as a
  // fallback. A bound method used after its receiver died fails with a clear
  // error instead of touching freed memory.
  template <typename T>
  Ref<T> receiverOf(Interpreter& in, const WeakRef<T>& captured) {
    if (in.activeReceiver_.get() && captured.matches(in.activeReceiver_.get())) {
      return Ref<T>::retaining(static_cast<T*>(in.activeReceiver_.get()));
    }
    auto strong = captured.lock();
    if (strong) return strong;
    in.fail("internal error: use of a bound method whose receiver was destroyed");
  }

  [[noreturn]] void fail(const std::string& message, int line = 0, int col = 0) const;
  [[noreturn]] void failHere(const std::string& message) const;

  // Helpers shared with the builtin library.
  Value indexGet(const Value& object, const Value& index, int line, int col);
  void indexSet(const Value& object, const Value& index, Value value, int line, int col);
  Value memberGet(const Value& object, const std::string& name, int line, int col);
  void memberSet(const Value& object, const std::string& name, Value value, int line, int col);
  Value binary(Tok op, const Value& left, const Value& right, int line, int col);
  bool lessThan(const Value& left, const Value& right, int line, int col);

  std::string sourceName;
  std::vector<std::string> scriptArgs;
  std::shared_ptr<Env> globals;
  std::shared_ptr<Env> env;
  std::vector<Frame> stack;
  int maxDepth = 2000;
  int depth = 0;
  int currentLine = 1;
  int currentCol = 1;

 private:
  // Anchors that keep hoisted-function environment chains alive for the
  // duration of the innermost block that created them (see execBlock). The
  // interpreter owns one anchor vector; blocks push their owning references
  // here and pop exactly what they pushed when they finish, so captured
  // subgraphs are released as soon as no outside owner remains.
  std::vector<std::shared_ptr<Env>> keepAlive_;

  void execBlock(const Stmt& block, const std::shared_ptr<Env>& target, bool newScope);
  Value unary(Tok op, const Value& operand, int line, int col);
  Value assignTo(const Expr& target, Value value);
  FuncPtr makeFunction(const std::string& name, const std::vector<std::string>& params,
                       const StmtPtr& body, std::shared_ptr<Env> closure);
};
