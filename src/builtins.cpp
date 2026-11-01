#include "builtins.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "runtime.h"
#include "util.h"

using Args = std::vector<Value>&;
namespace {

// ------------------------------------------------------------ arg checking

[[noreturn]] void arityError(Runtime& in, const std::string& fn, size_t expected, Args args) {
  in.fail(fn + "() expects " + std::to_string(expected) + " argument" +
          (expected == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
}

void need(Runtime& in, const std::string& fn, Args args, size_t expected) {
  if (args.size() != expected) arityError(in, fn, expected, args);
}

void needAtLeast(Runtime& in, const std::string& fn, Args args, size_t least) {
  if (args.size() < least) {
    in.fail(fn + "() expects at least " + std::to_string(least) + " argument" +
            (least == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
  }
}

void needBetween(Runtime& in, const std::string& fn, Args args, size_t least, size_t most) {
  if (args.size() < least || args.size() > most) {
    in.fail(fn + "() expects between " + std::to_string(least) + " and " + std::to_string(most) +
            " arguments, got " + std::to_string(args.size()));
  }
}

int64_t wantInt(Runtime& in, const std::string& fn, const Value& value,
                const std::string& what = "argument") {
  if (!value.isInt()) {
    in.fail(fn + "() expects an integer " + what + ", got '" + value.typeName() + "'");
  }
  return value.asInt();
}

double wantNumber(Runtime& in, const std::string& fn, const Value& value,
                  const std::string& what = "argument") {
  if (!value.isNumber()) {
    in.fail(fn + "() expects a number " + what + ", got '" + value.typeName() + "'");
  }
  return value.number();
}

std::string wantString(Runtime& in, const std::string& fn, const Value& value,
                       const std::string& what = "argument") {
  if (!value.isStr()) {
    in.fail(fn + "() expects a string " + what + ", got '" + value.typeName() + "'");
  }
  return value.asStr();
}

Value invoke(Runtime& in, const Value& fn, std::vector<Value> args,
             const std::string& who = "callback") {
  if (!fn.isFunc()) {
    in.fail(who + " must be a function, got '" + fn.typeName() + "'");
  }
  return in.call(fn, std::move(args), in.currentLine, in.currentCol);
}

int64_t clampIndex(int64_t index, int64_t length) {
  if (index < 0) index += length;
  if (index < 0) return 0;
  if (index > length) return length;
  return index;
}

uint32_t decodeFirstCodePoint(const std::string& ch) {
  unsigned char b0 = static_cast<unsigned char>(ch[0]);
  auto byteAt = [&](size_t i) {
    return i < ch.size() ? static_cast<unsigned char>(ch[i]) : static_cast<unsigned char>(0);
  };
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0) return ((b0 & 0x1F) << 6) | (byteAt(1) & 0x3F);
  if ((b0 & 0xF0) == 0xE0) {
    return ((b0 & 0x0F) << 12) | ((byteAt(1) & 0x3F) << 6) | (byteAt(2) & 0x3F);
  }
  if ((b0 & 0xF8) == 0xF0) {
    return ((b0 & 0x07) << 18) | ((byteAt(1) & 0x3F) << 12) | ((byteAt(2) & 0x3F) << 6) |
           (byteAt(3) & 0x3F);
  }
  return b0;
}

void sortItems(Runtime& in, std::vector<Value>& items, const Value* comparator,
               const std::string& who) {
  int line = in.currentLine;
  int col = in.currentCol;
  if (comparator != nullptr) {
    std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
      Value result = in.call(*comparator, {a, b}, line, col);
      if (!result.isBool()) in.fail(who + " comparator must return a bool");
      return result.asBool();
    });
  } else {
    std::stable_sort(items.begin(), items.end(), [&](const Value& a, const Value& b) {
      return in.lessThan(a, b, line, col);
    });
  }
}


// --------------------------------------------------------------- array methods

bool bindArray(Runtime&, const ArrayPtr& strongArray, const std::string& name, Value& out) {
  // Bound methods own their receiver, so a method called on a temporary
  // (`[1, 2].concat(...)`) keeps it alive for the whole call. The only cycle
  // this creates is a method stored inside its own receiver (`a.f = a.push`).
  ArrayPtr array = strongArray;

  if (name == "push") {
    out = makeNative("array.push", [array](Runtime&, Args args) -> Value {
      ArrayPtr self = array;
      for (auto& arg : args) self->items.push_back(arg);
      return Value(self);
    });
    return true;
  }
  if (name == "pop") {
    out = makeNative("array.pop", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "pop", args, 0);
      if (self->items.empty()) return Value();
      Value value = std::move(self->items.back());
      self->items.pop_back();
      return value;
    });
    return true;
  }
  if (name == "shift") {
    out = makeNative("array.shift", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "shift", args, 0);
      if (self->items.empty()) return Value();
      Value value = std::move(self->items.front());
      self->items.erase(self->items.begin());
      return value;
    });
    return true;
  }
  if (name == "unshift") {
    out = makeNative("array.unshift", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "unshift", args, 1);
      self->items.insert(self->items.begin(), args[0]);
      return Value(self);
    });
    return true;
  }
  if (name == "insert") {
    out = makeNative("array.insert", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "insert", args, 2);
      int64_t index = wantInt(in, "insert", args[0], "index");
      int64_t length = static_cast<int64_t>(self->items.size());
      if (index < 0) index += length;
      if (index < 0 || index > length) {
        in.fail("insert() index out of range (length " + std::to_string(length) + ")");
      }
      self->items.insert(self->items.begin() + index, args[1]);
      return Value(self);
    });
    return true;
  }
  if (name == "removeAt") {
    out = makeNative("array.removeAt", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "removeAt", args, 1);
      int64_t index = wantInt(in, "removeAt", args[0], "index");
      int64_t length = static_cast<int64_t>(self->items.size());
      if (index < 0) index += length;
      if (index < 0 || index >= length) {
        in.fail("removeAt() index out of range (length " + std::to_string(length) + ")");
      }
      Value value = std::move(self->items[static_cast<size_t>(index)]);
      self->items.erase(self->items.begin() + index);
      return value;
    });
    return true;
  }
  if (name == "slice") {
    out = makeNative("array.slice", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      needBetween(in, "slice", args, 0, 2);
      int64_t length = static_cast<int64_t>(self->items.size());
      int64_t start = args.size() > 0 ? wantInt(in, "slice", args[0], "start") : 0;
      int64_t end = args.size() > 1 ? wantInt(in, "slice", args[1], "end") : length;
      start = clampIndex(start, length);
      end = clampIndex(end, length);
      if (end < start) end = start;
      std::vector<Value> items;
      for (int64_t i = start; i < end; ++i) items.push_back(self->items[static_cast<size_t>(i)]);
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "concat") {
    out = makeNative("array.concat", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "concat", args, 1);
      if (!args[0].isArray()) in.fail("concat() expects an array");
      std::vector<Value> items = self->items;
      items.insert(items.end(), args[0].arr().items.begin(), args[0].arr().items.end());
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "join") {
    out = makeNative("array.join", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      needBetween(in, "join", args, 0, 1);
      std::string separator = args.empty() ? "" : args[0].toString();
      std::string result;
      for (size_t i = 0; i < self->items.size(); ++i) {
        if (i) result += separator;
        result += self->items[i].toString();
      }
      return Value(std::move(result));
    });
    return true;
  }
  if (name == "sort") {
    out = makeNative("array.sort", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      needBetween(in, "sort", args, 0, 1);
      sortItems(in, self->items, args.empty() ? nullptr : &args[0], "sort");
      return Value(self);
    });
    return true;
  }
  if (name == "sorted") {
    out = makeNative("array.sorted", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      needBetween(in, "sorted", args, 0, 1);
      std::vector<Value> items = self->items;
      sortItems(in, items, args.empty() ? nullptr : &args[0], "sorted");
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "reversed") {
    out = makeNative("array.reversed", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "reversed", args, 0);
      std::vector<Value> items = self->items;
      std::reverse(items.begin(), items.end());
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "contains") {
    out = makeNative("array.contains", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "contains", args, 1);
      for (const auto& item : self->items) {
        if (item.equals(args[0])) return Value(true);
      }
      return Value(false);
    });
    return true;
  }
  if (name == "indexOf") {
    out = makeNative("array.indexOf", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "indexOf", args, 1);
      for (size_t i = 0; i < self->items.size(); ++i) {
        if (self->items[i].equals(args[0])) return Value(static_cast<int64_t>(i));
      }
      return Value(static_cast<int64_t>(-1));
    });
    return true;
  }
  if (name == "map") {
    out = makeNative("array.map", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "map", args, 1);
      std::vector<Value> result;
      result.reserve(self->items.size());
      for (const auto& item : self->items) result.push_back(invoke(in, args[0], {item}, "map()"));
      return Value(makeArray(std::move(result)));
    });
    return true;
  }
  if (name == "filter") {
    out = makeNative("array.filter", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "filter", args, 1);
      std::vector<Value> result;
      for (const auto& item : self->items) {
        if (invoke(in, args[0], {item}, "filter()").truthy()) result.push_back(item);
      }
      return Value(makeArray(std::move(result)));
    });
    return true;
  }
  if (name == "reduce") {
    out = makeNative("array.reduce", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      needBetween(in, "reduce", args, 1, 2);
      size_t index = 0;
      Value accumulator;
      if (args.size() == 2) {
        accumulator = args[1];
      } else {
        if (self->items.empty()) in.fail("reduce() of an empty array needs an initial value");
        accumulator = self->items[0];
        index = 1;
      }
      for (; index < self->items.size(); ++index) {
        accumulator = invoke(in, args[0], {accumulator, self->items[index]}, "reduce()");
      }
      return accumulator;
    });
    return true;
  }
  if (name == "each") {
    out = makeNative("array.each", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "each", args, 1);
      for (const auto& item : self->items) invoke(in, args[0], {item}, "each()");
      return Value(self);
    });
    return true;
  }
  if (name == "find") {
    out = makeNative("array.find", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "find", args, 1);
      for (const auto& item : self->items) {
        if (invoke(in, args[0], {item}, "find()").truthy()) return item;
      }
      return Value();
    });
    return true;
  }
  if (name == "any") {
    out = makeNative("array.any", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "any", args, 1);
      for (const auto& item : self->items) {
        if (invoke(in, args[0], {item}, "any()").truthy()) return Value(true);
      }
      return Value(false);
    });
    return true;
  }
  if (name == "all") {
    out = makeNative("array.all", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "all", args, 1);
      for (const auto& item : self->items) {
        if (!invoke(in, args[0], {item}, "all()").truthy()) return Value(false);
      }
      return Value(true);
    });
    return true;
  }
  if (name == "sum") {
    out = makeNative("array.sum", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "sum", args, 0);
      bool allInts = true;
      int64_t intTotal = 0;
      double floatTotal = 0;
      for (const auto& item : self->items) {
        if (!item.isNumber()) in.fail("sum() expects an array of numbers");
        if (item.isInt() && allInts) {
          intTotal = static_cast<int64_t>(static_cast<uint64_t>(intTotal) +
                                          static_cast<uint64_t>(item.asInt()));
        } else {
          if (allInts) floatTotal = static_cast<double>(intTotal);
          allInts = false;
          floatTotal += item.number();
        }
      }
      return allInts ? Value(intTotal) : Value(floatTotal);
    });
    return true;
  }
  if (name == "clear") {
    out = makeNative("array.clear", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "clear", args, 0);
      self->items.clear();
      return Value(self);
    });
    return true;
  }
  if (name == "clone") {
    out = makeNative("array.clone", [array](Runtime& in, Args args) -> Value {
      ArrayPtr self = array;
      need(in, "clone", args, 0);
      return Value(makeArray(self->items));
    });
    return true;
  }
  return false;
}

// -------------------------------------------------------------- string methods

std::string asciiLower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string asciiUpper(std::string text) {
  for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

bool bindString(Runtime& in, StringPtr receiver, const std::string& name, Value& out) {
  // The captured object is a RefCounted string; work on its payload directly.
  const std::string& text = receiver->value;
  (void)in;

  if (name == "upper") {
    out = makeNative("string.upper", [text](Runtime& in, Args args) -> Value {
      need(in, "upper", args, 0);
      return Value(asciiUpper(text));
    });
    return true;
  }
  if (name == "lower") {
    out = makeNative("string.lower", [text](Runtime& in, Args args) -> Value {
      need(in, "lower", args, 0);
      return Value(asciiLower(text));
    });
    return true;
  }
  if (name == "trim") {
    out = makeNative("string.trim", [text](Runtime& in, Args args) -> Value {
      need(in, "trim", args, 0);
      return Value(trim(text));
    });
    return true;
  }
  if (name == "trimStart") {
    out = makeNative("string.trimStart", [text](Runtime& in, Args args) -> Value {
      need(in, "trimStart", args, 0);
      size_t start = 0;
      while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
      return Value(text.substr(start));
    });
    return true;
  }
  if (name == "trimEnd") {
    out = makeNative("string.trimEnd", [text](Runtime& in, Args args) -> Value {
      need(in, "trimEnd", args, 0);
      size_t end = text.size();
      while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
      return Value(text.substr(0, end));
    });
    return true;
  }
  if (name == "split") {
    out = makeNative("string.split", [text](Runtime& in, Args args) -> Value {
      needBetween(in, "split", args, 0, 1);
      std::vector<Value> items;
      if (args.empty()) {
        std::istringstream stream(text);
        std::string word;
        while (stream >> word) items.emplace_back(word);
      } else {
        std::string separator = wantString(in, "split", args[0], "separator");
        if (separator.empty()) {
          for (auto& ch : utf8Chars(text)) items.emplace_back(std::move(ch));
        } else {
          size_t start = 0;
          while (true) {
            size_t found = text.find(separator, start);
            if (found == std::string::npos) {
              items.emplace_back(text.substr(start));
              break;
            }
            items.emplace_back(text.substr(start, found - start));
            start = found + separator.size();
          }
        }
      }
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "lines") {
    out = makeNative("string.lines", [text](Runtime& in, Args args) -> Value {
      need(in, "lines", args, 0);
      std::vector<Value> items;
      std::string current;
      for (char c : text) {
        if (c == '\n') {
          if (!current.empty() && current.back() == '\r') current.pop_back();
          items.emplace_back(current);
          current.clear();
        } else {
          current += c;
        }
      }
      if (!current.empty()) {
        if (current.back() == '\r') current.pop_back();
        items.emplace_back(current);
      }
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "replace") {
    out = makeNative("string.replace", [text](Runtime& in, Args args) -> Value {
      need(in, "replace", args, 2);
      std::string from = wantString(in, "replace", args[0], "search text");
      std::string to = wantString(in, "replace", args[1], "replacement text");
      if (from.empty()) in.fail("replace() search text cannot be empty");
      std::string result = text;
      size_t position = 0;
      while ((position = result.find(from, position)) != std::string::npos) {
        result.replace(position, from.size(), to);
        position += to.size();
      }
      return Value(std::move(result));
    });
    return true;
  }
  if (name == "replaceFirst") {
    out = makeNative("string.replaceFirst", [text](Runtime& in, Args args) -> Value {
      need(in, "replaceFirst", args, 2);
      std::string from = wantString(in, "replaceFirst", args[0], "search text");
      std::string to = wantString(in, "replaceFirst", args[1], "replacement text");
      if (from.empty()) in.fail("replaceFirst() search text cannot be empty");
      size_t position = text.find(from);
      if (position == std::string::npos) return Value(text);
      std::string result = text;
      result.replace(position, from.size(), to);
      return Value(std::move(result));
    });
    return true;
  }
  if (name == "startsWith") {
    out = makeNative("string.startsWith", [text](Runtime& in, Args args) -> Value {
      need(in, "startsWith", args, 1);
      std::string prefix = wantString(in, "startsWith", args[0], "prefix");
      return Value(startsWith(text, prefix));
    });
    return true;
  }
  if (name == "endsWith") {
    out = makeNative("string.endsWith", [text](Runtime& in, Args args) -> Value {
      need(in, "endsWith", args, 1);
      std::string suffix = wantString(in, "endsWith", args[0], "suffix");
      return Value(text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0);
    });
    return true;
  }
  if (name == "contains") {
    out = makeNative("string.contains", [text](Runtime& in, Args args) -> Value {
      need(in, "contains", args, 1);
      std::string needle = wantString(in, "contains", args[0], "text");
      return Value(text.find(needle) != std::string::npos);
    });
    return true;
  }
  if (name == "indexOf") {
    out = makeNative("string.indexOf", [text](Runtime& in, Args args) -> Value {
      need(in, "indexOf", args, 1);
      std::string needle = wantString(in, "indexOf", args[0], "text");
      size_t found = text.find(needle);
      if (found == std::string::npos) return Value(static_cast<int64_t>(-1));
      return Value(utf8Length(text.substr(0, found)));
    });
    return true;
  }
  if (name == "substr") {
    out = makeNative("string.substr", [text](Runtime& in, Args args) -> Value {
      needBetween(in, "substr", args, 1, 2);
      auto chars = utf8Chars(text);
      int64_t length = static_cast<int64_t>(chars.size());
      int64_t start = wantInt(in, "substr", args[0], "start");
      if (start < 0) start += length;
      if (start < 0) start = 0;
      if (start > length) start = length;
      int64_t count = length - start;
      if (args.size() > 1) {
        count = wantInt(in, "substr", args[1], "length");
        if (count < 0) count = 0;
      }
      std::vector<std::string> slice;
      for (int64_t i = start; i < start + count && i < length; ++i) {
        slice.push_back(chars[static_cast<size_t>(i)]);
      }
      return Value(utf8Join(slice));
    });
    return true;
  }
  if (name == "chars") {
    out = makeNative("string.chars", [text](Runtime& in, Args args) -> Value {
      need(in, "chars", args, 0);
      std::vector<Value> items;
      for (auto& ch : utf8Chars(text)) items.emplace_back(std::move(ch));
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "repeat") {
    out = makeNative("string.repeat", [text](Runtime& in, Args args) -> Value {
      need(in, "repeat", args, 1);
      int64_t count = wantInt(in, "repeat", args[0], "count");
      if (count < 0) in.fail("repeat() count cannot be negative");
      if (count > 0 && text.size() > 0 &&
          static_cast<uint64_t>(count) > (64ULL * 1024ULL * 1024ULL) / text.size()) {
        in.fail("repeat() result is too large");
      }
      std::string result;
      result.reserve(text.size() * static_cast<size_t>(count));
      for (int64_t i = 0; i < count; ++i) result += text;
      return Value(std::move(result));
    });
    return true;
  }
  if (name == "reversed") {
    out = makeNative("string.reversed", [text](Runtime& in, Args args) -> Value {
      need(in, "reversed", args, 0);
      auto chars = utf8Chars(text);
      std::reverse(chars.begin(), chars.end());
      return Value(utf8Join(chars));
    });
    return true;
  }
  if (name == "padStart" || name == "padEnd") {
    bool atStart = name == "padStart";
    std::string methodName = name;
    out = makeNative("string." + methodName, [text, atStart, methodName](Runtime& in,
                                                                        Args args) -> Value {
      needBetween(in, methodName, args, 1, 2);
      int64_t width = wantInt(in, methodName, args[0], "width");
      std::string fill = args.size() > 1 ? wantString(in, methodName, args[1], "fill") : " ";
      if (fill.empty()) in.fail(methodName + "() fill cannot be empty");
      int64_t current = static_cast<int64_t>(utf8Length(text));
      if (current >= width) return Value(text);
      std::string padding;
      while (static_cast<int64_t>(utf8Length(padding)) < width - current) padding += fill;
      int64_t keep = width - current;
      auto chars = utf8Chars(padding);
      std::string clipped;
      for (int64_t i = 0; i < keep && i < static_cast<int64_t>(chars.size()); ++i) {
        clipped += chars[static_cast<size_t>(i)];
      }
      return Value(atStart ? clipped + text : text + clipped);
    });
    return true;
  }
  return false;
}

// ----------------------------------------------------------------- map methods

bool bindMap(Runtime&, const MapPtr& strongMap, const std::string& name, Value& out) {
  // Bound methods own their receiver, see bindArray above.
  MapPtr map = strongMap;
  if (name == "keys") {
    out = makeNative("map.keys", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "keys", args, 0);
      std::vector<Value> items;
      for (const auto& entry : self->entries) items.emplace_back(entry.first);
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "values") {
    out = makeNative("map.values", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "values", args, 0);
      std::vector<Value> items;
      for (const auto& entry : self->entries) items.push_back(entry.second);
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "entries") {
    out = makeNative("map.entries", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "entries", args, 0);
      std::vector<Value> items;
      for (const auto& entry : self->entries) {
        items.push_back(Value(makeArray({Value(entry.first), entry.second})));
      }
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "has") {
    out = makeNative("map.has", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "has", args, 1);
      std::string key = wantString(in, "has", args[0], "key");
      return Value(self->has(key));
    });
    return true;
  }
  if (name == "get") {
    out = makeNative("map.get", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      needBetween(in, "get", args, 1, 2);
      std::string key = wantString(in, "get", args[0], "key");
      const Value* found = self->get(key);
      if (found) return *found;
      return args.size() > 1 ? args[1] : Value();
    });
    return true;
  }
  if (name == "set") {
    out = makeNative("map.set", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "set", args, 2);
      std::string key = wantString(in, "set", args[0], "key");
      self->set(key, args[1]);
      return Value(self);
    });
    return true;
  }
  if (name == "delete") {
    out = makeNative("map.delete", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "delete", args, 1);
      std::string key = wantString(in, "delete", args[0], "key");
      return Value(self->erase(key));
    });
    return true;
  }
  if (name == "clear") {
    out = makeNative("map.clear", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "clear", args, 0);
      self->clear();
      return Value(self);
    });
    return true;
  }
  if (name == "merge") {
    out = makeNative("map.merge", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "merge", args, 1);
      if (!args[0].isMap()) in.fail("merge() expects a map");
      MapPtr other = args[0].mapPtr();
      for (const auto& entry : other->entries) self->set(entry.first, entry.second);
      return Value(self);
    });
    return true;
  }
  if (name == "clone") {
    out = makeNative("map.clone", [map](Runtime& in, Args args) -> Value {
      MapPtr self = map;
      need(in, "clone", args, 0);
      auto copy = makeMap();
      for (const auto& entry : self->entries) copy->set(entry.first, entry.second);
      return Value(std::move(copy));
    });
    return true;
  }
  return false;
}

// --------------------------------------------------------------- range methods

bool bindRange(Runtime& in, RangePtr range, const std::string& name, Value& out) {
  (void)in;

  if (name == "toArray") {
    out = makeNative("range.toArray", [range](Runtime& in, Args args) -> Value {
      need(in, "toArray", args, 0);
      if (range->length() > 10000000) in.fail("range is too large to convert to an array");
      std::vector<Value> items;
      if (range->start <= range->end) {
        for (int64_t i = range->start; range->inclusive ? i <= range->end : i < range->end; ++i) {
          items.emplace_back(i);
        }
      } else {
        for (int64_t i = range->start; range->inclusive ? i >= range->end : i > range->end; --i) {
          items.emplace_back(i);
        }
      }
      return Value(makeArray(std::move(items)));
    });
    return true;
  }
  if (name == "contains") {
    out = makeNative("range.contains", [range](Runtime& in, Args args) -> Value {
      need(in, "contains", args, 1);
      Value receiver = Value(range);
      return in.binary(Tok::In, args[0], receiver, in.currentLine, in.currentCol);
    });
    return true;
  }
  return false;
}

}  // namespace

bool bindMethod(Runtime& interp, const Value& receiver, const std::string& name, Value& out) {
  // Every binder captures its receiver by value, so the produced method owns
  // it and stays valid even when the receiver was a temporary.
  if (receiver.isArray()) return bindArray(interp, receiver.arrayPtr(), name, out);
  if (receiver.isStr()) return bindString(interp, receiver.strPtr(), name, out);
  if (receiver.isMap()) return bindMap(interp, receiver.mapPtr(), name, out);
  if (receiver.isRange()) return bindRange(interp, receiver.rangePtr(), name, out);
  return false;
}

// ------------------------------------------------------------------- builtins

void registerBuiltins(Runtime& in) {
  static std::mt19937_64 engine(std::random_device{}());
  static auto startTime = std::chrono::steady_clock::now();

  in.addNative("print", [](Runtime&, Args args) -> Value {
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) std::cout << ' ';
      std::cout << args[i].toString();
    }
    std::cout << '\n';
    std::cout.flush();
    return Value();
  });

  in.addNative("write", [](Runtime&, Args args) -> Value {
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) std::cout << ' ';
      std::cout << args[i].toString();
    }
    std::cout.flush();
    return Value();
  });

  in.addNative("eprint", [](Runtime&, Args args) -> Value {
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) std::cerr << ' ';
      std::cerr << args[i].toString();
    }
    std::cerr << '\n';
    std::cerr.flush();
    return Value();
  });

  in.addNative("input", [](Runtime& in, Args args) -> Value {
    needBetween(in, "input", args, 0, 1);
    if (!args.empty()) {
      std::cout << args[0].toString();
      std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) return Value();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return Value(std::move(line));
  });

  in.addNative("len", [](Runtime& in, Args args) -> Value {
    need(in, "len", args, 1);
    const Value& value = args[0];
    if (value.isStr()) return Value(utf8Length(value.asStr()));
    if (value.isArray()) return Value(value.arr().items.size());
    if (value.isMap()) return Value(value.map().size());
    if (value.isRange()) return Value(value.range().length());
    in.fail("len() is not defined for values of type '" + std::string(value.typeName()) + "'");
  });

  in.addNative("type", [](Runtime& in, Args args) -> Value {
    need(in, "type", args, 1);
    return Value(std::string(args[0].typeName()));
  });

  in.addNative("str", [](Runtime& in, Args args) -> Value {
    need(in, "str", args, 1);
    return Value(args[0].toString());
  });

  in.addNative("repr", [](Runtime& in, Args args) -> Value {
    need(in, "repr", args, 1);
    return Value(args[0].repr());
  });

  in.addNative("int", [](Runtime& in, Args args) -> Value {
    need(in, "int", args, 1);
    const Value& value = args[0];
    if (value.isInt()) return value;
    if (value.isBool()) return Value(static_cast<int64_t>(value.asBool() ? 1 : 0));
    if (value.isFloat()) {
      double number = value.asFloat();
      if (!std::isfinite(number)) in.fail("cannot convert a non-finite float to int");
      if (number < -9223372036854775808.0 || number > 9223372036854775807.0) {
        in.fail("float is out of the integer range");
      }
      return Value(static_cast<int64_t>(number));
    }
    if (value.isStr()) {
      std::string text = trim(value.asStr());
      if (text.empty()) in.fail("cannot convert an empty string to int");
      errno = 0;
      char* end = nullptr;
      long long result = std::strtoll(text.c_str(), &end, 0);
      if (errno == ERANGE || end == nullptr || *end != '\0') {
        in.fail("cannot convert '" + value.asStr() + "' to int");
      }
      return Value(static_cast<int64_t>(result));
    }
    in.fail("cannot convert '" + std::string(value.typeName()) + "' to int");
  });

  in.addNative("float", [](Runtime& in, Args args) -> Value {
    need(in, "float", args, 1);
    const Value& value = args[0];
    if (value.isFloat()) return value;
    if (value.isInt()) return Value(static_cast<double>(value.asInt()));
    if (value.isBool()) return Value(value.asBool() ? 1.0 : 0.0);
    if (value.isStr()) {
      std::string text = trim(value.asStr());
      if (text.empty()) in.fail("cannot convert an empty string to float");
      char* end = nullptr;
      double result = std::strtod(text.c_str(), &end);
      if (end == nullptr || *end != '\0') {
        in.fail("cannot convert '" + value.asStr() + "' to float");
      }
      return Value(result);
    }
    in.fail("cannot convert '" + std::string(value.typeName()) + "' to float");
  });

  in.addNative("bool", [](Runtime& in, Args args) -> Value {
    need(in, "bool", args, 1);
    return Value(args[0].truthy());
  });

  in.addNative("chr", [](Runtime& in, Args args) -> Value {
    need(in, "chr", args, 1);
    int64_t code = wantInt(in, "chr", args[0]);
    if (code < 0 || code > 0x10FFFF) in.fail("chr() code point out of range");
    std::string text;
    appendUtf8(text, static_cast<uint32_t>(code));
    return Value(std::move(text));
  });

  in.addNative("ord", [](Runtime& in, Args args) -> Value {
    need(in, "ord", args, 1);
    std::string text = wantString(in, "ord", args[0]);
    auto chars = utf8Chars(text);
    if (chars.size() != 1) in.fail("ord() expects a single character");
    return Value(static_cast<int64_t>(decodeFirstCodePoint(chars[0])));
  });

  in.addNative("abs", [](Runtime& in, Args args) -> Value {
    need(in, "abs", args, 1);
    if (args[0].isInt()) {
      int64_t value = args[0].asInt();
      return Value(value < 0 ? static_cast<int64_t>(0ULL - static_cast<uint64_t>(value)) : value);
    }
    if (args[0].isFloat()) return Value(std::fabs(args[0].asFloat()));
    in.fail("abs() expects a number");
  });

  in.addNative("floor", [](Runtime& in, Args args) -> Value {
    need(in, "floor", args, 1);
    if (args[0].isInt()) return args[0];
    return Value(std::floor(wantNumber(in, "floor", args[0])));
  });

  in.addNative("ceil", [](Runtime& in, Args args) -> Value {
    need(in, "ceil", args, 1);
    if (args[0].isInt()) return args[0];
    return Value(std::ceil(wantNumber(in, "ceil", args[0])));
  });

  in.addNative("round", [](Runtime& in, Args args) -> Value {
    need(in, "round", args, 1);
    if (args[0].isInt()) return args[0];
    return Value(std::round(wantNumber(in, "round", args[0])));
  });

  in.addNative("sqrt", [](Runtime& in, Args args) -> Value {
    need(in, "sqrt", args, 1);
    double value = wantNumber(in, "sqrt", args[0]);
    if (value < 0) in.fail("sqrt() of a negative number");
    return Value(std::sqrt(value));
  });

  in.addNative("pow", [](Runtime& in, Args args) -> Value {
    need(in, "pow", args, 2);
    double base = wantNumber(in, "pow", args[0]);
    double exponent = wantNumber(in, "pow", args[1]);
    return Value(std::pow(base, exponent));
  });

  in.addNative("min", [](Runtime& in, Args args) -> Value {
    needAtLeast(in, "min", args, 1);
    Value best = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      if (in.lessThan(args[i], best, in.currentLine, in.currentCol)) best = args[i];
    }
    return best;
  });

  in.addNative("max", [](Runtime& in, Args args) -> Value {
    needAtLeast(in, "max", args, 1);
    Value best = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      if (in.lessThan(best, args[i], in.currentLine, in.currentCol)) best = args[i];
    }
    return best;
  });

  in.addNative("clamp", [](Runtime& in, Args args) -> Value {
    need(in, "clamp", args, 3);
    if (in.lessThan(args[0], args[1], in.currentLine, in.currentCol)) return args[1];
    if (in.lessThan(args[2], args[0], in.currentLine, in.currentCol)) return args[2];
    return args[0];
  });

  in.addNative("random", [](Runtime& in, Args args) -> Value {
    needBetween(in, "random", args, 0, 2);
    if (args.empty()) {
      std::uniform_real_distribution<double> distribution(0.0, 1.0);
      return Value(distribution(engine));
    }
    if (args.size() == 1) {
      int64_t high = wantInt(in, "random", args[0]);
      if (high <= 0) in.fail("random() bound must be positive");
      std::uniform_int_distribution<int64_t> distribution(0, high - 1);
      return Value(distribution(engine));
    }
    int64_t low = wantInt(in, "random", args[0]);
    int64_t high = wantInt(in, "random", args[1]);
    if (high <= low) in.fail("random() needs an upper bound greater than the lower bound");
    std::uniform_int_distribution<int64_t> distribution(low, high - 1);
    return Value(distribution(engine));
  });

  in.addNative("seed", [](Runtime& in, Args args) -> Value {
    need(in, "seed", args, 1);
    engine.seed(static_cast<uint64_t>(wantInt(in, "seed", args[0])));
    return Value();
  });

  in.addNative("clock", [](Runtime& in, Args args) -> Value {
    need(in, "clock", args, 0);
    auto now = std::chrono::steady_clock::now();
    return Value(std::chrono::duration<double>(now - startTime).count());
  });

  in.addNative("assert", [](Runtime& in, Args args) -> Value {
    needBetween(in, "assert", args, 1, 2);
    if (!args[0].truthy()) {
      in.fail(args.size() > 1 ? args[1].toString() : "assertion failed");
    }
    return Value();
  });

  in.addNative("assertEq", [](Runtime& in, Args args) -> Value {
    needBetween(in, "assertEq", args, 2, 3);
    if (!args[0].equals(args[1])) {
      std::string message = args.size() > 2 ? args[2].toString() : "assertion failed";
      in.fail(message + ": expected " + args[0].repr() + ", got " + args[1].repr());
    }
    return Value();
  });

  in.addNative("panic", [](Runtime& in, Args args) -> Value {
    needBetween(in, "panic", args, 0, 1);
    in.fail(args.empty() ? "panic" : args[0].toString());
  });

  in.addNative("exit", [](Runtime& in, Args args) -> Value {
    needBetween(in, "exit", args, 0, 1);
    int code = args.empty() ? 0 : static_cast<int>(wantInt(in, "exit", args[0]));
    std::exit(code);
  });

  in.addNative("args", [](Runtime& in, Args args) -> Value {
    need(in, "args", args, 0);
    std::vector<Value> items;
    for (const auto& arg : in.scriptArgs) items.emplace_back(arg);
    return Value(makeArray(std::move(items)));
  });

  in.addNative("readFile", [](Runtime& in, Args args) -> Value {
    need(in, "readFile", args, 1);
    std::string path = wantString(in, "readFile", args[0], "path");
    std::string contents;
    if (!readSourceFile(path, contents)) in.fail("cannot read file '" + path + "'");
    return Value(std::move(contents));
  });

  in.addNative("writeFile", [](Runtime& in, Args args) -> Value {
    need(in, "writeFile", args, 2);
    std::string path = wantString(in, "writeFile", args[0], "path");
    std::string contents = wantString(in, "writeFile", args[1], "contents");
    std::ofstream file(path, std::ios::binary);
    if (!file) in.fail("cannot write file '" + path + "'");
    file << contents;
    if (!file) in.fail("cannot write file '" + path + "'");
    return Value();
  });

  in.addNative("fileExists", [](Runtime& in, Args args) -> Value {
    need(in, "fileExists", args, 1);
    std::string path = wantString(in, "fileExists", args[0], "path");
    std::ifstream file(path, std::ios::binary);
    return Value(static_cast<bool>(file));
  });

  in.addNative("range", [](Runtime& in, Args args) -> Value {
    needBetween(in, "range", args, 1, 2);
    int64_t start = 0;
    int64_t end = 0;
    if (args.size() == 1) {
      end = wantInt(in, "range", args[0], "end");
    } else {
      start = wantInt(in, "range", args[0], "start");
      end = wantInt(in, "range", args[1], "end");
    }
    return Value(makeRange(start, end, false));
  });
}
