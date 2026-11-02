#pragma once

#include <cstddef>
#include <memory>

// Cycle collector for reference-counted objects.
//
// Reference counting frees an object as soon as nothing owns it, but it cannot
// free a group of objects that only own each other: a function stored in the
// scope it closes over, an array that contains itself, and so on. This
// collector finds such groups and clears them so that ordinary reference
// counting can finish the job.
//
// Objects that can take part in a cycle derive from GcNode. It links them in a
// registry and tells the collector how to inspect them. A collection:
//   1. starts each object's trial count at its reference count,
//   2. subtracts one for every reference held by another tracked object,
//   3. treats objects whose count stays positive as reachable from outside
//      (a C++ variable, the call stack, a native closure...), and everything
//      reachable from those as alive,
//   4. clears whatever is left: it is only reachable from itself.
//
// References the collector cannot see (for example a value captured inside a
// std::function) just look like references from outside. Missing an edge can
// make the collector keep garbage, but it can never free a live object.
struct GcNode {
  // Called once for every strong reference an object holds to another tracked
  // object, with the `context` that was passed to `visit`.
  using Visitor = void (*)(GcNode& child, void* context);

  // How the collector inspects one kind of object. Plain function pointers
  // instead of virtual functions: the tracked types are deliberately free of
  // vtables (see RefCounted).
  struct Ops {
    // Strong references currently held to the object, from anywhere.
    std::size_t (*refCount)(const GcNode& node);
    // Reports every strong reference the object holds to a tracked object.
    void (*visit)(const GcNode& node, Visitor visitor, void* context);
    // Drops everything the object owns. Only called on unreachable objects.
    void (*clear)(GcNode& node);
    // Returns a handle that keeps the object alive until it is destroyed.
    std::shared_ptr<void> (*pin)(GcNode& node);
  };

  explicit GcNode(const Ops& ops);
  // A copy is a new object: it registers itself instead of sharing the links.
  GcNode(const GcNode& other);
  GcNode& operator=(const GcNode&) noexcept { return *this; }
  ~GcNode();

  const Ops* ops;

  // Registry links and per-collection scratch data; only gc.cpp touches them.
  GcNode* prev = nullptr;
  GcNode* next = nullptr;
  long trial = 0;
  bool reachable = false;

 private:
  void link();
  void unlink();
};

// Frees every unreachable cycle among the tracked objects.
void gcCollect();

// Collects when enough tracked objects were created since the last collection.
// Cheap enough to call at every safe point. The threshold grows with the
// number of live objects, so the cost per allocation stays constant.
void gcCollectIfNeeded();

// Number of tracked objects that are currently alive.
std::size_t gcTrackedCount();
