#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "gc.h"
#include "value.h"

// A lexical scope. Kept apart from the interpreter so that value.cpp, which
// has to know how a function owns its scope, does not depend on it.

extern const GcNode::Ops envGcOps;

struct Binding {
  Value value;
  bool isImmutable = false;
};

struct Env : std::enable_shared_from_this<Env>, GcNode {
  // The link to the enclosing scope is strong: a closure that captures a
  // scope keeps its whole lexical chain alive, so free variables stay
  // resolvable after the scope that created the closure has returned.
  //
  // Reference counting alone cannot free the cycles this allows (a function
  // stored in the very scope it closes over); those are reclaimed by the
  // cycle collector in gc.h.
  std::shared_ptr<Env> parent;
  std::unordered_map<std::string, Binding> vars;

  explicit Env(std::shared_ptr<Env> parent = nullptr)
      : GcNode(envGcOps), parent(std::move(parent)) {}

  // Walks up the scope chain looking for `name`; nullptr when it is not visible.
  Binding* find(const std::string& name);
  void define(const std::string& name, Value value, bool isImmutable = false);
  enum class Assign { Ok, NotFound, Immutable };
  Assign assign(const std::string& name, Value value);

  // Removes every binding (used when resetting the interpreter between runs).
  void clear() { vars.clear(); }
};
