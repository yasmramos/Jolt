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

class Interpreter;
struct Env;
struct Stmt;

struct Value;
struct Array;
struct Map;
struct Function;
struct Range;

// Single-threaded reference counting for VM values. The interpreter/VM do not
// share values across threads, so a non-atomic counter is enough and avoids
// the overhead of std::shared_ptr in register transfers.
struct RefCounted {
  mutable std::uint32_t refs = 1;
  virtual ~RefCounted() = default;
};

template <typename T>
class Ref {
 public:
  Ref() noexcept = default;
  explicit Ref(T* ptr) noexcept : ptr_(ptr) {}

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
    if (ptr_ && --ptr_->refs == 0) delete ptr_;
  }

  RefCounted* ptr_ = nullptr;
};

template <typename T, typename... Args>
Ref<T> makeRef(Args&&... args) {
  return Ref<T>(new T(std::forward<Args>(args)...));
}

struct StringObj : RefCounted {
  std::string value;
  explicit StringObj(std::string s) : value(std::move(s)) {}
};

using StringPtr = Ref<StringObj>;
using ArrayPtr = Ref<Array>;
using MapPtr = Ref<Map>;
using FuncPtr = Ref<Function>;
using RangePtr = Ref<Range>;
using NativeFn = std::function<Value(Interpreter&, std::vector<Value>&)>;

enum class VType : std::uint8_t { Nil, Bool, Int, Float, Str, Array, Map, Func, Range };

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
    ArrayPtr array;
    MapPtr map;
    FuncPtr function;
    RangePtr range;

    Storage() {}
    ~Storage() {}
  } data;

  Value() noexcept : tag(VType::Nil) {}
  Value(std::nullptr_t) noexcept : tag(VType::Nil) {}
  Value(bool b) noexcept : tag(VType::Bool) { new (&data.boolean) bool(b); }
  Value(int i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  Value(int64_t i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  Value(size_t i) noexcept : tag(VType::Int) { new (&data.integer) int64_t(i); }
  Value(double d) noexcept : tag(VType::Float) { new (&data.number) double(d); }
  Value(const char* s) : Value(std::string(s)) {}
  Value(std::string s) : tag(VType::Str) {
    new (&data.string) StringPtr(makeString(std::move(s)));
  }
  Value(ArrayPtr a) noexcept : tag(VType::Array) { new (&data.array) ArrayPtr(std::move(a)); }
  Value(MapPtr m) noexcept : tag(VType::Map) { new (&data.map) MapPtr(std::move(m)); }
  Value(FuncPtr f) noexcept : tag(VType::Func) { new (&data.function) FuncPtr(std::move(f)); }
  Value(RangePtr r) noexcept : tag(VType::Range) { new (&data.range) RangePtr(std::move(r)); }

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
  bool isStr() const { return tag == VType::Str; }
  bool isArray() const { return tag == VType::Array; }
  bool isMap() const { return tag == VType::Map; }
  bool isFunc() const { return tag == VType::Func; }
  bool isRange() const { return tag == VType::Range; }
  bool isNumber() const { return isInt() || isFloat(); }

  bool truthy() const;
  bool asBool() const { return data.boolean; }
  int64_t asInt() const { return data.integer; }
  double asFloat() const { return data.number; }
  double number() const { return isInt() ? static_cast<double>(asInt()) : asFloat(); }

  // Jolt strings are immutable. The shared object is never exposed as mutable.
  const std::string& asStr() const { return data.string->value; }

  ArrayPtr arrayPtr() const { return data.array; }
  MapPtr mapPtr() const { return data.map; }
  FuncPtr funcPtr() const { return data.function; }
  RangePtr rangePtr() const { return data.range; }

  Array& arr() const;
  Map& map() const;
  Function& func() const;
  Range& range() const;

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
        new (&data.number) double(other.data.number);
        break;
      case VType::Str:
        new (&data.string) StringPtr(other.data.string);
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
        new (&data.number) double(other.data.number);
        break;
      case VType::Str:
        new (&data.string) StringPtr(std::move(other.data.string));
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
    }
  }

  void destroy() noexcept {
    switch (tag) {
      case VType::Nil:
      case VType::Bool:
      case VType::Int:
      case VType::Float:
        break;
      case VType::Str:
        destroyMember(data.string);
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
    }
  }
};

// Celda de una variable local. Las variables locales viven en celdas para que
// una closure pueda capturar la variable (no solo su valor) y compartir sus
// mutaciones. Se usara en FASE 6; en FASE 5 ya fija el modelo.
struct Cell : RefCounted {
  Value value;
};

using CellPtr = Ref<Cell>;

struct Array : RefCounted {
  std::vector<Value> items;
};

// Insertion-ordered string-keyed map.
struct Map : RefCounted {
  std::vector<std::pair<std::string, Value>> entries;
  std::unordered_map<std::string, size_t> index;

  bool has(const std::string& key) const;
  const Value* get(const std::string& key) const;
  Value* get(const std::string& key);
  void set(const std::string& key, Value value);
  bool erase(const std::string& key);
  void clear();
  void rebuildIndex();
  size_t size() const { return entries.size(); }
};

struct Function : RefCounted {
  std::string name;
  std::vector<std::string> params;
  std::shared_ptr<Stmt> body;
  std::shared_ptr<Env> closure;
  NativeFn native;

  bool isNative() const { return static_cast<bool>(native); }
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

ArrayPtr makeArray(std::vector<Value> items = {});
MapPtr makeMap();
RangePtr makeRange(int64_t start, int64_t end, bool inclusive);
Value makeNative(std::string name, NativeFn fn);

std::string quoteString(const std::string& s);

// UTF-8 helpers. Jolt strings are byte strings, but these functions treat
// them as sequences of code points so that accented text behaves sanely.
size_t utf8Length(const std::string& s);
std::vector<std::string> utf8Chars(const std::string& s);
std::string utf8Join(const std::vector<std::string>& chars);
std::string utf8Substr(const std::string& s, int64_t start, int64_t count);
void appendUtf8(std::string& out, uint32_t codePoint);
