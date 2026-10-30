#include "value.h"

#include <cmath>
#include <cstdio>

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
    case VType::Str:
      return "string";
    case VType::Array:
      return "array";
    case VType::Map:
      return "map";
    case VType::Func:
      return "function";
    case VType::Range:
      return "range";
  }
  return "unknown";
}

bool Value::truthy() const {
  if (isBool()) return asBool();
  return !isNil();
}

bool Value::equals(const Value& other) const {
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
      return false;  // handled above
    case VType::Str:
      return asStr() == other.asStr();
    case VType::Array: {
      const auto& a = arr().items;
      const auto& b = other.arr().items;
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (!a[i].equals(b[i])) return false;
      }
      return true;
    }
    case VType::Map: {
      const auto& a = map();
      const auto& b = other.map();
      if (a.size() != b.size()) return false;
      for (const auto& entry : a.entries) {
        const Value* v = b.get(entry.first);
        if (!v || !entry.second.equals(*v)) return false;
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
  }
  return false;
}

static std::string formatFloat(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.15g", d);
  return buffer;
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
      out += formatFloat(value.asFloat());
      break;
    case VType::Str:
      out += repr ? quoteString(value.asStr()) : value.asStr();
      break;
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
  int64_t distance = end >= start ? end - start : start - end;
  return inclusive ? distance + 1 : distance;
}

ArrayPtr makeArray(std::vector<Value> items) {
  auto array = makeRef<Array>();
  array->items = std::move(items);
  return array;
}

MapPtr makeMap() { return makeRef<Map>(); }

RangePtr makeRange(int64_t start, int64_t end, bool inclusive) {
  auto range = makeRef<Range>();
  range->start = start;
  range->end = end;
  range->inclusive = inclusive;
  return range;
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
          char buffer[8];
          std::snprintf(buffer, sizeof buffer, "\\x%02x", c);
          out += buffer;
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
