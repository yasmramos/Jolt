#include "value.h"

#include "env.h"

#include <cmath>
#include <charconv>
#include <utility>

const char* Value::typeName() const {
  switch (type()) {
    case VType::Nil:
      return "nil";
    case VType::Bool:
      return "bool";
    case VType::Int:
      return "int";
    case VType::Float:
      return "float";
    case VType::Double:
      return "double";
    case VType::Str:
      return "string";
    case VType::Error:
      return "error";
    case VType::Array:
      return "array";
    case VType::Map:
      return "map";
    case VType::Func:
      return "function";
    case VType::Range:
      return "range";
    case VType::Ptr:
      // The handle's kind is the FFI type it was built as, so `type()` keeps
      // reporting `ptr` or `cstring` after a `cstring` upcasts to `ptr`.
      return data.pointer && data.pointer->kind == PtrKind::CString ? "cstring" : "ptr";
  }
  return "unknown";
}

bool Value::truthy() const {
  if (isBool()) return asBool();
  return !isNil();
}

bool Value::equals(const Value& other) const {
  std::vector<std::pair<const RefCounted*, const RefCounted*>> visited;
  return equalsImpl(other, visited);
}

bool Value::equalsImpl(
    const Value& other,
    std::vector<std::pair<const RefCounted*, const RefCounted*>>& visited) const {
  if (isNumber() && other.isNumber()) {
    if (isInt() && other.isInt()) return asInt() == other.asInt();
    return number() == other.number();
  }
  if (type() != other.type()) return false;

  switch (type()) {
    case VType::Nil:
      return true;
    case VType::Bool:
      return asBool() == other.asBool();
    case VType::Int:
    case VType::Float:
    case VType::Double:
      return false;  // handled above
    case VType::Str:
      return asStr() == other.asStr();
    case VType::Error:
      // Errors compare by value (message and code), like strings. This keeps
      // `assertEq` and `==` useful for error results.
      return err().message == other.err().message && err().code == other.err().code;
    case VType::Array: {
      // Two containers already being compared are assumed equal, so a cyclic
      // structure compares equal to an isomorphic one instead of recursing
      // forever.
      const RefCounted* self = data.array.get();
      const RefCounted* that = other.data.array.get();
      for (const auto& pair : visited) {
        if (pair.first == self && pair.second == that) return true;
      }
      visited.emplace_back(self, that);
      const auto& a = arr().items;
      const auto& b = other.arr().items;
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i].equalsImpl(b[i], visited)) return false;
      }
      return true;
    }
    case VType::Map: {
      const RefCounted* self = data.map.get();
      const RefCounted* that = other.data.map.get();
      for (const auto& pair : visited) {
        if (pair.first == self && pair.second == that) return true;
      }
      visited.emplace_back(self, that);
      const auto& a = map();
      const auto& b = other.map();
      if (a.size() != b.size()) return false;
      for (const auto& entry : a.entries) {
        const Value* v = b.get(entry.first);
        if (!v || !entry.second.equalsImpl(*v, visited)) return false;
      }
      return true;
    }
    case VType::Func:
      return data.function == other.data.function;
    case VType::Range: {
      const auto& a = range();
      const auto& b = other.range();
      return a.start == b.start && a.end == b.end && a.inclusive == b.inclusive;
    }
    case VType::Ptr:
      // Two pointer values are equal when they are the same handle. The bytes
      // are not compared: a pointer is an identity, like a function.
      return data.pointer == other.data.pointer;
  }
  return false;
}

static std::string formatFloat(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buffer[64];
  // Shortest text that round-trips through binary64 (Java-style): `1.0/3.0`
  // prints "0.3333333333333333", not a truncated "0.333333333333333" that would
  // read back as a different value.
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), d);
  if (result.ec != std::errc()) return "nan";
  return std::string(buffer, result.ptr);
}

// A `float` prints with the shortest text that round-trips through binary32
// (Java-style: `0.1f` prints "0.1", not the binary64 value "0.1"). `to_chars`
// with the `float` overload produces exactly that.
static std::string formatFloat32(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<float>(d));
  if (result.ec != std::errc()) return "nan";
  return std::string(buffer, result.ptr);
}

static void writeValue(std::string& out, const Value& value, bool repr, int depth) {
  if (depth > 8) {
    out += "...";
    return;
  }
  switch (value.type()) {
    case VType::Nil:
      out += "nil";
      break;
    case VType::Bool:
      out += value.asBool() ? "true" : "false";
      break;
    case VType::Int:
      out += std::to_string(value.asInt());
      break;
    case VType::Float:
      out += formatFloat32(value.asFloat());
      break;
    case VType::Double:
      out += formatFloat(value.asDouble());
      break;
    case VType::Str:
      out += repr ? quoteString(value.asStr()) : value.asStr();
      break;
    case VType::Error: {
      const auto& e = value.err();
      if (repr) {
        // Debug form, used at the top level and inside containers:
        // `error("message")`, or `error("message", code)` when the code is set.
        out += "error(";
        out += quoteString(e.message);
        if (e.code != 0) {
          out += ", ";
          out += std::to_string(e.code);
        }
        out += ')';
      } else {
        // Display form: the message alone, so `${err}` and `print(err)` read
        // like the error text.
        out += e.message;
      }
      break;
    }
    case VType::Array: {
      const auto& items = value.arr().items;
      out += '[';
      for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        writeValue(out, items[i], true, depth + 1);
      }
      out += ']';
      break;
    }
    case VType::Map: {
      const auto& entries = value.map().entries;
      out += '{';
      for (size_t i = 0; i < entries.size(); ++i) {
        if (i) out += ", ";
        out += quoteString(entries[i].first);
        out += ": ";
        writeValue(out, entries[i].second, true, depth + 1);
      }
      out += '}';
      break;
    }
    case VType::Func: {
      const auto& fn = value.func();
      if (fn.isNative()) {
        out += "<native fn " + fn.name + ">";
      } else {
        out += "<fn " + fn.name + ">";
      }
      break;
    }
    case VType::Range: {
      const auto& r = value.range();
      out += std::to_string(r.start);
      out += r.inclusive ? "..=" : "..";
      out += std::to_string(r.end);
      break;
    }
    case VType::Ptr: {
      const PtrObj& p = value.ptr();
      // A `cstring` reads as the text it points at (that is what the user
      // passed in); any other handle is opaque, so it prints as `<ptr>` rather
      // than leaking an address the program must never see.
      if (p.kind == PtrKind::CString) {
        // C sees a `cstring` up to the first NUL; display matches that view.
        auto end = p.bytes.begin();
        while (end != p.bytes.end() && *end != '\0') ++end;
        std::string text(p.bytes.begin(), end);
        out += repr ? quoteString(text) : text;
      } else {
        out += "<ptr>";
      }
      break;
    }
  }
}

std::string Value::toString() const {
  std::string out;
  writeValue(out, *this, false, 0);
  return out;
}

std::string Value::repr() const {
  std::string out;
  writeValue(out, *this, true, 0);
  return out;
}

bool Map::has(const std::string& key) const { return index.find(key) != index.end(); }

const Value* Map::get(const std::string& key) const {
  auto it = index.find(key);
  if (it == index.end()) return nullptr;
  return &entries[it->second].second;
}

Value* Map::get(const std::string& key) {
  auto it = index.find(key);
  if (it == index.end()) return nullptr;
  return &entries[it->second].second;
}

void Map::set(const std::string& key, Value value) {
  auto it = index.find(key);
  if (it != index.end()) {
    entries[it->second].second = std::move(value);
    return;
  }
  index.emplace(key, entries.size());
  entries.emplace_back(key, std::move(value));
}

bool Map::erase(const std::string& key) {
  auto it = index.find(key);
  if (it == index.end()) return false;
  entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(it->second));
  rebuildIndex();
  return true;
}

void Map::clear() {
  entries.clear();
  index.clear();
}

void Map::rebuildIndex() {
  index.clear();
  for (size_t i = 0; i < entries.size(); ++i) index.emplace(entries[i].first, i);
}

int64_t Range::length() const {
  // The bounds are int64 and their difference can overflow for a span wider
  // than INT64_MAX (for example INT64_MIN..INT64_MAX), which is undefined
  // behaviour and used to return a negative count. Compute the magnitude in
  // uint64 and saturate at INT64_MAX: a range longer than that reports as
  // INT64_MAX, which still trips the "too large to convert to an array" guard.
  uint64_t distance = end >= start
                          ? static_cast<uint64_t>(end) - static_cast<uint64_t>(start)
                          : static_cast<uint64_t>(start) - static_cast<uint64_t>(end);
  if (inclusive) {
    if (distance == UINT64_MAX) return INT64_MAX;
    ++distance;
  }
  if (distance > static_cast<uint64_t>(INT64_MAX)) return INT64_MAX;
  return static_cast<int64_t>(distance);
}

ArrayPtr makeArray(std::vector<Value> items) {
  // Construct in a single step: `array->items = std::move(items)` would leave
  // the source vector alive with its elements still holding references, which
  // keeps self-referencing containers (cycles) from ever reaching refs == 0.
  return makeRef<Array>(std::move(items));
}

MapPtr makeMap() { return makeRef<Map>(); }

Value makeError(std::string message, int64_t code) {
  return Value(makeRef<ErrorObj>(std::move(message), code));
}

RangePtr makeRange(int64_t start, int64_t end, bool inclusive) {
  auto range = makeRef<Range>();
  range->start = start;
  range->end = end;
  range->inclusive = inclusive;
  return range;
}

Value makePtr(PtrKind kind, std::vector<char> bytes, int64_t address) {
  auto pointer = makeRef<PtrObj>(kind, std::move(bytes));
  pointer->address = address;
  return Value(std::move(pointer));
}

Value makeNative(std::string name, NativeFn fn) {
  auto function = makeRef<Function>();
  function->name = std::move(name);
  function->native = std::move(fn);
  return Value(std::move(function));
}

std::string quoteString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (c < 0x20) {
          const char* hex = "0123456789abcdef";
          out += "\\x";
          out += hex[(c >> 4) & 0x0F];
          out += hex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

size_t utf8Length(const std::string& s) {
  size_t count = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::vector<std::string> utf8Chars(const std::string& s) {
  std::vector<std::string> chars;
  std::string current;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80 && !current.empty()) {
      chars.push_back(current);
      current.clear();
    }
    current += static_cast<char>(c);
  }
  if (!current.empty()) chars.push_back(current);
  return chars;
}

std::string utf8Join(const std::vector<std::string>& chars) {
  std::string out;
  for (const auto& ch : chars) out += ch;
  return out;
}

std::string utf8Substr(const std::string& s, int64_t start, int64_t count) {
  auto chars = utf8Chars(s);
  const int64_t n = static_cast<int64_t>(chars.size());
  if (start < 0) start += n;
  if (start < 0) start = 0;
  if (start > n) start = n;
  if (count < 0) count = 0;
  int64_t end = start + count;
  if (end > n) end = n;
  std::vector<std::string> slice;
  for (int64_t i = start; i < end; ++i) slice.push_back(chars[static_cast<size_t>(i)]);
  return utf8Join(slice);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// ------------------------------------------------------- cycle collector hooks

namespace {

template <typename T>
std::shared_ptr<void> pinObject(GcNode& node) {
  Ref<T> handle = Ref<T>::retaining(&static_cast<T&>(node));
  return std::shared_ptr<void>(new Ref<T>(std::move(handle)),
                               [](void* p) { delete static_cast<Ref<T>*>(p); });
}

std::size_t arrayRefCount(const GcNode& node) { return static_cast<const Array&>(node).refs; }

void arrayVisit(const GcNode& node, GcNode::Visitor visitor, void* context) {
  for (const Value& item : static_cast<const Array&>(node).items) gcVisit(item, visitor, context);
}

void arrayClear(GcNode& node) { std::vector<Value>().swap(static_cast<Array&>(node).items); }

std::size_t mapRefCount(const GcNode& node) { return static_cast<const Map&>(node).refs; }

void mapVisit(const GcNode& node, GcNode::Visitor visitor, void* context) {
  for (const auto& entry : static_cast<const Map&>(node).entries) {
    gcVisit(entry.second, visitor, context);
  }
}

void mapClear(GcNode& node) { static_cast<Map&>(node).clear(); }

std::size_t functionRefCount(const GcNode& node) { return static_cast<const Function&>(node).refs; }

// A function owns its defining scope, the cells it captured, and the objects its
// native captured state keeps alive. Anything it owns that this does not report
// looks like a reference from outside, which keeps cycles through it alive.
void functionVisit(const GcNode& node, GcNode::Visitor visitor, void* context) {
  const Function& function = static_cast<const Function&>(node);
  if (function.closure) visitor(*function.closure, context);
  // Captured cells are shared between closures, so each one is a node of its own.
  // The VM tracks a cell as soon as a closure captures it.
  for (const CellPtr& cell : function.upvalues) {
    if (cell && cell->isTracked()) visitor(*cell, context);
  }
  for (const auto& captured : function.capturedGcNodes) {
    if (captured) visitor(*static_cast<GcNode*>(captured.get()), context);
  }
}

void functionClear(GcNode& node) {
  Function& function = static_cast<Function&>(node);
  function.closure.reset();
  function.native = nullptr;
  function.upvalues.clear();
  function.capturedGcNodes.clear();
}

std::size_t cellRefCount(const GcNode& node) { return static_cast<const Cell&>(node).refs; }

void cellVisit(const GcNode& node, GcNode::Visitor visitor, void* context) {
  gcVisit(static_cast<const Cell&>(node).value, visitor, context);
}

void cellClear(GcNode& node) { static_cast<Cell&>(node).value = Value(); }

// Environments are owned through std::shared_ptr, so their reference count is
// the shared count. `parent` and every bound value are strong references.
std::size_t envRefCount(const GcNode& node) {
  return const_cast<Env&>(static_cast<const Env&>(node)).weak_from_this().use_count();
}

void envVisit(const GcNode& node, GcNode::Visitor visitor, void* context) {
  const Env& env = static_cast<const Env&>(node);
  if (env.parent) visitor(*env.parent, context);
  for (const auto& entry : env.vars) gcVisit(entry.second.value, visitor, context);
}

void envClear(GcNode& node) {
  Env& env = static_cast<Env&>(node);
  env.vars.clear();
  env.parent.reset();
}

std::shared_ptr<void> envPin(GcNode& node) { return static_cast<Env&>(node).shared_from_this(); }

}  // namespace

const GcNode::Ops arrayGcOps = {arrayRefCount, arrayVisit, arrayClear, pinObject<Array>};
const GcNode::Ops mapGcOps = {mapRefCount, mapVisit, mapClear, pinObject<Map>};
const GcNode::Ops functionGcOps = {functionRefCount, functionVisit, functionClear,
                                   pinObject<Function>};
const GcNode::Ops envGcOps = {envRefCount, envVisit, envClear, envPin};
const GcNode::Ops cellGcOps = {cellRefCount, cellVisit, cellClear, pinObject<Cell>};
