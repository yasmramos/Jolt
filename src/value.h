#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gc.h"

class Runtime;
struct Bytecode;
struct Env;
struct Stmt;

template <typename T>
class Ref;

struct Value;
struct Array;
struct Map;
struct Function;
struct Range;
struct PtrObj;

// Single-threaded reference counting for VM values. The interpreter/VM do not
// share values across threads, so a non-atomic counter is enough and avoids
// the overhead of std::shared_ptr in register transfers.
//
// Destruction is driven by a per-type function pointer installed at creation
// time instead of a virtual destructor. This keeps objects polymorphic-free
// (smaller, no vtable indirection on release) and makes `Ref<T>::release()`
// destroy derived objects correctly even when the static type is `RefCounted`.
struct RefCounted {
  using DestroyFn = void (*)(void*);

  // Strong reference count. `makeRef` seeds it with the single reference
  // owned by the handle it returns. Objects are not polymorphic: cleanup
  // runs through `destroy` below, so there is no virtual destructor.
  mutable std::uint32_t refs = 0;

 private:
  template <typename>
  friend class Ref;
  template <typename T, typename... Args>
  friend Ref<T> makeRef(Args&&... args);

  DestroyFn destroy = nullptr;
};

template <typename T>
class Ref {
 public:
  Ref() noexcept = default;

 private:
  // Adopts `ptr` *without* incrementing its count (used by `makeRef`, which
  // seeds the count itself). The tag type is private on purpose: a public tag
  // would let any caller reach the adopting constructor through implicit
  // pointer-to-member conversions, silently skipping the retain.
  struct Adopt {};
  explicit Ref(Adopt, T* ptr) noexcept : ptr_(ptr) {}

  // Grants `makeRef` access to the adopting constructor without relying on
  // template-argument deduction in the friend declaration.
  template <typename U, typename... Args>
  friend Ref<U> makeRef(Args&&... args);

 public:
  // Creates a handle to an object known only by address and adds a strong
  // reference to it. The cycle collector uses this to pin the objects it is
  // about to clear; everything else gets its handles from `makeRef` or by
  // copying an existing one.
  static Ref retaining(T* ptr) noexcept {
    Ref handle;
    handle.ptr_ = ptr;
    handle.retain();
    return handle;
  }

  Ref(const Ref& other) noexcept : ptr_(other.ptr_) { retain(); }
  Ref(Ref&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

  ~Ref() { release(); }

  Ref& operator=(const Ref& other) noexcept {
    if (this != &other) {
      release();
      ptr_ = other.ptr_;
      retain();
    }
    return *this;
  }

  Ref& operator=(Ref&& other) noexcept {
    if (this != &other) {
      release();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  T* get() const noexcept { return static_cast<T*>(ptr_); }
  T& operator*() const noexcept { return *get(); }
  T* operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  bool operator==(const Ref& other) const noexcept { return ptr_ == other.ptr_; }
  bool operator!=(const Ref& other) const noexcept { return ptr_ != other.ptr_; }

 private:
  void retain() noexcept {
    if (ptr_) ++ptr_->refs;
  }

  void release() noexcept {
    if (ptr_ && --ptr_->refs == 0) {
      // Last strong reference: destroy through the per-type hook installed by
      // `makeRef`. `delete` through a base (`RefCounted*`) pointer would be
      // undefined behavior without a vtable, and objects reachable only as
      // `RefCounted` (such as the inner Value of a self-referencing
      // container) must still die correctly.
      ptr_->destroy(ptr_);
    }
  }

  RefCounted* ptr_ = nullptr;
};

template <typename T, typename... Args>
Ref<T> makeRef(Args&&... args) {
  static_assert(std::is_base_of<RefCounted, T>::value,
                "Ref<T> targets must derive from RefCounted");
  T* object = new T(std::forward<Args>(args)...);
  // The hook receives the `RefCounted` subobject and converts it back to `T`
  // in two steps, so it stays correct even when `T` has other base classes
  // (such as GcNode). It deletes through the concrete type, so members are
  // destroyed properly however the object is later referenced.
  object->destroy = [](void* p) { delete static_cast<T*>(static_cast<RefCounted*>(p)); };
  // The adopting constructor does not retain, so seed the count with the
  // single strong reference owned by the returned handle. Without this the
  // first copy of the handle brings the count to 1 and the original's
  // destructor drops it to 0, freeing an object that is still referenced.
  object->refs = 1;
  return Ref<T>(typename Ref<T>::Adopt{}, object);
}

struct StringObj : RefCounted {
  std::string value;
  explicit StringObj(std::string s) : value(std::move(s)) {}
};

// A first-class error value (PHASE 6.7 slice 2). It carries a human-readable
// message and an integer code, and it is a distinct runtime type
// (`type(err) == "error"`), so `isError` is exact: a user map that happens to
// have an `ok` field can never be mistaken for an error. It holds no
// references, so like a string it cannot sit in a cycle and is not a GcNode.
struct ErrorObj : RefCounted {
  std::string message;
  int64_t code = 0;

  ErrorObj(std::string m, int64_t c) : message(std::move(m)), code(c) {}
};

using StringPtr = Ref<StringObj>;
using ErrorPtr = Ref<ErrorObj>;
using ArrayPtr = Ref<Array>;
using MapPtr = Ref<Map>;
using FuncPtr = Ref<Function>;
using RangePtr = Ref<Range>;
using NativeFn = std::function<Value(Runtime&, std::vector<Value>&)>;

// FFI pointer (PHASE 6.6). A `ptr`/`cstring` is an *opaque handle*, never a raw
// address: exposing a machine address to Jolt would let a program dereference
// arbitrary memory (Jolt is meant to be memory-safe) and would hide the
// allocation from the cycle collector, which only understands `Value` edges.
// The handle owns the C-side storage; the host is the only code that ever sees
// the address.
enum class PtrKind : std::uint8_t {
  Void,     // `ptr` (C `void *`): opaque, not dereferenceable
  CString,  // `cstring` (C `const char *`): NUL-terminated bytes, read-only
  Buffer,   // owned byte buffer: the storage `loadByte`/`storeByte` operate on
};

// The object behind a pointer value. It is reference counted but, like a string
// or an error, it is *not* a `GcNode`: it owns only raw bytes, so it can never
// be part of a reference cycle. The host owns the storage through this handle,
// and releasing the last handle frees the bytes.
struct PtrObj : RefCounted {
  PtrObj(PtrKind k, std::vector<char> storage) : kind(k), bytes(std::move(storage)) {}

  PtrKind kind;
  // `CString` and `Buffer` own their bytes; `Void` keeps them empty and carries
  // no storage. A released handle drops its bytes, so using it afterwards is a
  // detectable error rather than a use-after-free.
  std::vector<char> bytes;
  // Opaque integer token, only meaningful to the host. A `Void` handle is just
  // this token (the result of a native such as `malloc`); an owned buffer keeps
  // it at 0. It is never an address the program may dereference.
  int64_t address = 0;
  bool released = false;
};

using PtrPtr = Ref<PtrObj>;

enum class VType : std::uint8_t {
  Nil,
  Bool,
  Int,
  Float,
  Double,
  Str,
  Error,
  Array,
  Map,
  Func,
  Range,
  Ptr,
};

// A dynamically typed value optimized for VM registers.
//
// It is a compact tagged union (16 bytes on 64-bit platforms): numeric values
// live inline, while strings and container values are reference counted.
// Strings are immutable, so copying a Value only bumps a reference count
// instead of duplicating the text. This keeps MOVE and register transfers
// cheap, which is the common case in a register-based bytecode VM.
struct Value {
  VType tag = VType::Nil;

  union Storage {
    bool boolean;
    int64_t integer;
    double number;
    StringPtr string;
    ErrorPtr error;
    ArrayPtr array;
    MapPtr map;
    FuncPtr function;
    RangePtr range;
    PtrPtr pointer;

    Storage() {}
    ~Storage() {}
  } data;

  Value() noexcept : tag(VType::Nil) {}
  Value(std::nullptr_t) noexcept : tag(VType::Nil) {}
  Value(bool b) noexcept : tag(VType::Bool) { new (&data.boolean) bool(b); }
  Value(int i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  Value(int64_t i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  Value(size_t i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  // Both binary floating-point types share the same storage: a `float` (binary32)
  // is stored widened to `double` and rounded back on read, so it always holds
  // exactly the value a C++ `float` would. `double` is the default for a
  // fractional literal with no suffix (Java-style).
  Value(double d) noexcept : tag(VType::Double) { new (&data.number) double(d); }
  static Value fromFloat(float f) noexcept {
    Value v;
    v.tag = VType::Float;
    new (&v.data.number) double(static_cast<double>(f));
    return v;
  }
  Value(const char* s) : Value(std::string(s)) {}
  Value(std::string s) : tag(VType::Str) {
    new (&data.string) StringPtr(makeString(std::move(s)));
  }
  Value(ErrorPtr e) noexcept : tag(VType::Error) { new (&data.error) ErrorPtr(std::move(e)); }
  Value(ArrayPtr a) noexcept : tag(VType::Array) { new (&data.array) ArrayPtr(std::move(a)); }
  Value(MapPtr m) noexcept : tag(VType::Map) { new (&data.map) MapPtr(std::move(m)); }
  Value(FuncPtr f) noexcept : tag(VType::Func) { new (&data.function) FuncPtr(std::move(f)); }
  Value(RangePtr r) noexcept : tag(VType::Range) { new (&data.range) RangePtr(std::move(r)); }
  Value(PtrPtr p) noexcept : tag(VType::Ptr) { new (&data.pointer) PtrPtr(std::move(p)); }

  Value(const Value& other) : tag(other.tag) { copyFrom(other); }
  Value(Value&& other) noexcept : tag(other.tag) {
    moveFrom(other);
    other.destroy();
    other.tag = VType::Nil;
  }

  Value& operator=(const Value& other) {
    if (this != &other) {
      destroy();
      tag = other.tag;
      copyFrom(other);
    }
    return *this;
  }

  Value& operator=(Value&& other) noexcept {
    if (this != &other) {
      destroy();
      tag = other.tag;
      moveFrom(other);
      other.destroy();
      other.tag = VType::Nil;
    }
    return *this;
  }

  ~Value() { destroy(); }

  VType type() const { return tag; }
  const char* typeName() const;

  bool isNil() const { return tag == VType::Nil; }
  bool isBool() const { return tag == VType::Bool; }
  bool isInt() const { return tag == VType::Int; }
  bool isFloat() const { return tag == VType::Float; }
  bool isDouble() const { return tag == VType::Double; }
  bool isStr() const { return tag == VType::Str; }
  bool isError() const { return tag == VType::Error; }
  bool isArray() const { return tag == VType::Array; }
  bool isMap() const { return tag == VType::Map; }
  bool isFunc() const { return tag == VType::Func; }
  bool isRange() const { return tag == VType::Range; }
  bool isPtr() const { return tag == VType::Ptr; }
  bool isNumber() const { return isInt() || isFloat() || isDouble(); }

  bool truthy() const;
  bool asBool() const { return data.boolean; }
  int64_t asInt() const { return data.integer; }
  // A `float` is stored as a `double` holding a binary32 value; rounding back to
  // `float` recovers exactly the original value, so `asFloat()` is lossless.
  double asFloat() const { return static_cast<double>(static_cast<float>(data.number)); }
  double asDouble() const { return data.number; }
  double number() const { return isInt() ? static_cast<double>(asInt()) : data.number; }

  // Jolt strings are immutable. The shared object is never exposed as mutable.
  const std::string& asStr() const { return data.string->value; }

  StringPtr strPtr() const { return data.string; }
  ErrorPtr errorPtr() const { return data.error; }
  ArrayPtr arrayPtr() const { return data.array; }
  MapPtr mapPtr() const { return data.map; }
  FuncPtr funcPtr() const { return data.function; }
  RangePtr rangePtr() const { return data.range; }
  PtrPtr ptrPtr() const { return data.pointer; }

  // Mutable access to the stored function reference, used by `bindMethod` to
  // stamp the receiver identity on freshly created bound methods.
  FuncPtr& funcRef() { return data.function; }

  Array& arr() const;
  Map& map() const;
  Function& func() const;
  Range& range() const;
  ErrorObj& err() const;
  PtrObj& ptr() const;

  // Display form: strings are rendered raw.
  std::string toString() const;
  // Debug form: strings are quoted and escaped.
  std::string repr() const;

  bool equals(const Value& other) const;

 private:
  static StringPtr makeString(std::string s) {
    return makeRef<StringObj>(std::move(s));
  }

  template <typename T>
  static void destroyMember(T& member) noexcept {
    member.~T();
  }

  void copyFrom(const Value& other) {
    switch (tag) {
      case VType::Nil:
        break;
      case VType::Bool:
        new (&data.boolean) bool(other.data.boolean);
        break;
      case VType::Int:
        new (&data.integer) int64_t(other.data.integer);
        break;
      case VType::Float:
      case VType::Double:
        new (&data.number) double(other.data.number);
        break;
      case VType::Str:
        new (&data.string) StringPtr(other.data.string);
        break;
      case VType::Error:
        new (&data.error) ErrorPtr(other.data.error);
        break;
      case VType::Array:
        new (&data.array) ArrayPtr(other.data.array);
        break;
      case VType::Map:
        new (&data.map) MapPtr(other.data.map);
        break;
      case VType::Func:
        new (&data.function) FuncPtr(other.data.function);
        break;
      case VType::Range:
        new (&data.range) RangePtr(other.data.range);
        break;
      case VType::Ptr:
        new (&data.pointer) PtrPtr(other.data.pointer);
        break;
    }
  }

  void moveFrom(Value& other) noexcept {
    switch (tag) {
      case VType::Nil:
        break;
      case VType::Bool:
        new (&data.boolean) bool(other.data.boolean);
        break;
      case VType::Int:
        new (&data.integer) int64_t(other.data.integer);
        break;
      case VType::Float:
      case VType::Double:
        new (&data.number) double(other.data.number);
        break;
      case VType::Str:
        new (&data.string) StringPtr(std::move(other.data.string));
        break;
      case VType::Error:
        new (&data.error) ErrorPtr(std::move(other.data.error));
        break;
      case VType::Array:
        new (&data.array) ArrayPtr(std::move(other.data.array));
        break;
      case VType::Map:
        new (&data.map) MapPtr(std::move(other.data.map));
        break;
      case VType::Func:
        new (&data.function) FuncPtr(std::move(other.data.function));
        break;
      case VType::Range:
        new (&data.range) RangePtr(std::move(other.data.range));
        break;
      case VType::Ptr:
        new (&data.pointer) PtrPtr(std::move(other.data.pointer));
        break;
    }
  }

  void destroy() noexcept {
    switch (tag) {
      case VType::Nil:
      case VType::Bool:
      case VType::Int:
      case VType::Float:
      case VType::Double:
        break;
      case VType::Str:
        destroyMember(data.string);
        break;
      case VType::Error:
        destroyMember(data.error);
        break;
      case VType::Array:
        destroyMember(data.array);
        break;
      case VType::Map:
        destroyMember(data.map);
        break;
      case VType::Func:
        destroyMember(data.function);
        break;
      case VType::Range:
        destroyMember(data.range);
        break;
      case VType::Ptr:
        destroyMember(data.pointer);
        break;
    }
  }
};

// Cell holding a local variable. Locals live in cells so a closure can capture
// the variable itself (not just its value) and share its mutations. It will be
// used in PHASE 6; in PHASE 5 it already fixes the model.
//
// A cell can be part of a cycle (a closure stored in a variable that it also
// captures), so it is a GcNode. Every local variable gets a cell and almost none
// are ever captured, so a cell only registers with the collector once a closure
// captures it (see GcNode::track).
extern const GcNode::Ops cellGcOps;

struct Cell : RefCounted, GcNode {
  Cell() : GcNode(cellGcOps, /*tracked=*/false) {}

  Value value;
};

using CellPtr = Ref<Cell>;

// Containers and functions can end up owning themselves, so they are tracked
// by the cycle collector (gc.h). Their inspection tables live in value.cpp.
extern const GcNode::Ops arrayGcOps;
extern const GcNode::Ops mapGcOps;
extern const GcNode::Ops functionGcOps;

struct Array : RefCounted, GcNode {
  std::vector<Value> items;

  Array() : GcNode(arrayGcOps) {}
  explicit Array(std::vector<Value> init) : GcNode(arrayGcOps), items(std::move(init)) {}
};

// Insertion-ordered string-keyed map.
struct Map : RefCounted, GcNode {
  std::vector<std::pair<std::string, Value>> entries;
  std::unordered_map<std::string, size_t> index;

  Map() : GcNode(mapGcOps) {}

  bool has(const std::string& key) const;
  const Value* get(const std::string& key) const;
  Value* get(const std::string& key);
  void set(const std::string& key, Value value);
  bool erase(const std::string& key);
  void clear();
  void rebuildIndex();
  size_t size() const { return entries.size(); }
};

struct Function : RefCounted, GcNode {
  Function() : GcNode(functionGcOps) {}

  std::string name;
  std::vector<std::string> params;
  std::shared_ptr<Stmt> body;
  // Lexical scope captured at definition time. `Env` (declared in
  // interpreter.h) is only ever used through `std::shared_ptr`, which supports
  // incomplete pointed-to types here, so value.h stays free of interpreter
  // dependencies. The destructor instantiated in translation units that do
  // see the full `Env` type destroys the chain correctly.
  std::shared_ptr<Env> closure;
  NativeFn native;
  // Bytecode function (PHASE 6). A VM function has a proto and no native; an
  // AST function has neither. `upvalues` keeps the captured cells alive and
  // is empty until PHASE 6b.
  std::shared_ptr<const Bytecode> proto;
  std::vector<CellPtr> upvalues;
  std::vector<std::shared_ptr<void>> capturedGcNodes;
  bool isNative() const { return static_cast<bool>(native); }
  bool isVm() const { return static_cast<bool>(proto); }
};

// Start/end are integers. `0..5` is exclusive, `0..=5` is inclusive.
// Descending ranges (5..0) are iterated downwards.
struct Range : RefCounted {
  int64_t start = 0;
  int64_t end = 0;
  bool inclusive = false;
  int64_t length() const;
};

inline Array& Value::arr() const { return *data.array; }
inline Map& Value::map() const { return *data.map; }
inline Function& Value::func() const { return *data.function; }
inline Range& Value::range() const { return *data.range; }
inline ErrorObj& Value::err() const { return *data.error; }
inline PtrObj& Value::ptr() const { return *data.pointer; }

// Reports the tracked object that `value` owns, if any, to a collector visitor.
// Strings, errors and ranges cannot hold references, so they are not tracked.
// Cells will be added with the PHASE 6 closures, the first place they can be
// part of a cycle.
inline void gcVisit(const Value& value, GcNode::Visitor visitor, void* context) {
  if (value.isArray()) {
    visitor(value.arr(), context);
  } else if (value.isMap()) {
    visitor(value.map(), context);
  } else if (value.isFunc()) {
    visitor(value.func(), context);
  }
}

ArrayPtr makeArray(std::vector<Value> items = {});
MapPtr makeMap();
RangePtr makeRange(int64_t start, int64_t end, bool inclusive);
// Builds a pointer handle. `Void` carries no bytes (only the opaque `address`
// token); `CString`/`Buffer` own the storage they are given. The host is the
// only code that ever sees an address.
Value makePtr(PtrKind kind, std::vector<char> bytes = {}, int64_t address = 0);
// Builds an error value from a message and an optional code (0 by default).
Value makeError(std::string message, int64_t code = 0);
Value makeNative(std::string name, NativeFn fn);

template <typename T>
std::shared_ptr<void> pinGcNode(const Ref<T>& ref) {
  static_assert(std::is_base_of<GcNode, T>::value,
                "pinGcNode requires a collector-tracked type");
  // Keep the strong reference alive through an aliasing handle whose stored
  // pointer is the GcNode subobject. `functionVisit` recovers that pointer and
  // hands it to the collector, so it must not point at the Ref<T> holder.
  auto holder = std::make_shared<Ref<T>>(ref);
  return std::shared_ptr<void>(holder, static_cast<GcNode*>(ref.get()));
}

std::string quoteString(const std::string& s);

// UTF-8 helpers. Jolt strings are byte strings, but these functions treat
// them as sequences of code points so that accented text behaves sanely.
size_t utf8Length(const std::string& s);
std::vector<std::string> utf8Chars(const std::string& s);
std::string utf8Join(const std::vector<std::string>& chars);
std::string utf8Substr(const std::string& s, int64_t start, int64_t count);
void appendUtf8(std::string& out, uint32_t codePoint);
