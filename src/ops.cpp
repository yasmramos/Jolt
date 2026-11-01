#include "ops.h"

#include <cmath>
#include <cstdint>
#include <string>

#include "builtins.h"
#include "runtime.h"

namespace ops {
namespace {

int64_t normalizeIndex(Runtime& rt, int64_t index, int64_t length, const char* kind, int line,
                       int col) {
  int64_t original = index;
  if (index < 0) index += length;
  if (index < 0 || index >= length) {
    rt.fail(std::string(kind) + " index " + std::to_string(original) + " out of range (length " +
                std::to_string(length) + ")",
            line, col);
  }
  return index;
}

// Symbol of an operator, for diagnostics. Only the operators that reach
// `binary()` are needed; anything else is an internal compiler bug.
const char* opSymbol(Tok op) {
  switch (op) {
    case Tok::Plus:
      return "+";
    case Tok::Minus:
      return "-";
    case Tok::Star:
      return "*";
    case Tok::Slash:
      return "/";
    case Tok::Percent:
      return "%";
    case Tok::StarStar:
      return "**";
    case Tok::Amp:
      return "&";
    case Tok::Pipe:
      return "|";
    case Tok::Caret:
      return "^";
    case Tok::Shl:
      return "<<";
    case Tok::Shr:
      return ">>";
    case Tok::EqEq:
      return "==";
    case Tok::BangEq:
      return "!=";
    case Tok::Lt:
      return "<";
    case Tok::LtEq:
      return "<=";
    case Tok::Gt:
      return ">";
    case Tok::GtEq:
      return ">=";
    case Tok::In:
      return "in";
    default:
      return "?";
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

bool lessThan(Runtime& rt, const Value& left, const Value& right, int line, int col) {
  if (left.isNumber() && right.isNumber()) {
    if (left.isInt() && right.isInt()) return left.asInt() < right.asInt();
    return left.number() < right.number();
  }
  if (left.isStr() && right.isStr()) return left.asStr() < right.asStr();
  if (left.isBool() && right.isBool()) return !left.asBool() && right.asBool();
  rt.fail(std::string("cannot compare '") + left.typeName() + "' with '" + right.typeName() + "'",
          line, col);
}

bool contains(Runtime& rt, const Value& needle, const Value& haystack, int line, int col) {
  if (haystack.isArray()) {
    for (const auto& item : haystack.arr().items) {
      if (item.equals(needle)) return true;
    }
    return false;
  }
  if (haystack.isMap()) {
    if (!needle.isStr()) rt.fail("map membership requires a string key", line, col);
    return haystack.map().has(needle.asStr());
  }
  if (haystack.isStr()) {
    if (!needle.isStr()) rt.fail("string membership requires a string operand", line, col);
    return haystack.asStr().find(needle.asStr()) != std::string::npos;
  }
  if (haystack.isRange()) {
    if (!needle.isInt()) rt.fail("range membership requires an integer", line, col);
    return inRange(haystack.range(), needle.asInt());
  }
  rt.fail(std::string("operator 'in' is not supported for '") + needle.typeName() + "' and '" +
              haystack.typeName() + "'",
          line, col);
}

Value unary(Runtime& rt, Tok op, const Value& operand, int line, int col) {
  switch (op) {
    case Tok::Bang:
      return Value(!operand.truthy());
    case Tok::Minus:
      if (operand.isInt()) {
        return Value(static_cast<int64_t>(0ULL - static_cast<uint64_t>(operand.asInt())));
      }
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
  rt.fail(std::string("unsupported unary operator for type '") + operand.typeName() + "'", line,
          col);
}

Value binary(Runtime& rt, Tok op, const Value& left, const Value& right, int line, int col) {
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
      break;
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
        if (right.number() == 0) rt.fail("division by zero", line, col);
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
        if (right.number() == 0) rt.fail("division by zero", line, col);
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
      return Value(lessThan(rt, left, right, line, col));
    case Tok::LtEq:
      return Value(!lessThan(rt, right, left, line, col));
    case Tok::Gt:
      return Value(lessThan(rt, right, left, line, col));
    case Tok::GtEq:
      return Value(!lessThan(rt, left, right, line, col));
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
      if (b < 0 || b > 63) rt.fail("shift count must be between 0 and 63", line, col);
      if (op == Tok::Shl) {
        return Value(static_cast<int64_t>(static_cast<uint64_t>(a) << static_cast<uint64_t>(b)));
      }
      return Value(a >> b);
    }
    case Tok::In:
      return Value(contains(rt, left, right, line, col));
    default:
      break;
  }

  rt.fail(std::string("unsupported operand types for '") + opSymbol(op) + "': '" +
              left.typeName() + "' and '" + right.typeName() + "'",
          line, col);
}

Value indexGet(Runtime& rt, const Value& object, const Value& index, int line, int col) {
  if (object.isArray()) {
    if (!index.isInt()) rt.fail("array index must be an integer", line, col);
    const auto& items = object.arr().items;
    int64_t i = normalizeIndex(rt, index.asInt(), static_cast<int64_t>(items.size()), "array", line,
                               col);
    return items[static_cast<size_t>(i)];
  }
  if (object.isStr()) {
    if (!index.isInt()) rt.fail("string index must be an integer", line, col);
    auto chars = utf8Chars(object.asStr());
    int64_t i =
        normalizeIndex(rt, index.asInt(), static_cast<int64_t>(chars.size()), "string", line, col);
    return Value(chars[static_cast<size_t>(i)]);
  }
  if (object.isMap()) {
    if (!index.isStr()) rt.fail("map keys must be strings", line, col);
    const Value* found = object.map().get(index.asStr());
    return found ? *found : Value();
  }
  rt.fail(std::string("cannot index a value of type '") + object.typeName() + "'", line, col);
}

void indexSet(Runtime& rt, const Value& object, const Value& index, Value value, int line, int col) {
  if (object.isArray()) {
    if (!index.isInt()) rt.fail("array index must be an integer", line, col);
    auto& items = object.arr().items;
    int64_t i = normalizeIndex(rt, index.asInt(), static_cast<int64_t>(items.size()), "array", line,
                               col);
    items[static_cast<size_t>(i)] = std::move(value);
    return;
  }
  if (object.isStr()) rt.fail("strings are immutable", line, col);
  if (object.isMap()) {
    if (!index.isStr()) rt.fail("map keys must be strings", line, col);
    object.map().set(index.asStr(), std::move(value));
    return;
  }
  rt.fail(std::string("cannot assign to an index of a value of type '") + object.typeName() + "'",
          line, col);
}

Value memberGet(Runtime& rt, const Value& object, const std::string& name, int line, int col) {
  if (object.isMap()) {
    if (const Value* found = object.map().get(name)) return *found;
    if (name == "length") return Value(object.map().size());
    Value method;
    if (bindMethod(rt, object, name, method)) return method;
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
  if (bindMethod(rt, object, name, method)) return method;

  rt.fail(std::string("type '") + object.typeName() + "' has no member '" + name + "'", line, col);
}

void memberSet(Runtime& rt, const Value& object, const std::string& name, Value value, int line,
               int col) {
  if (object.isMap()) {
    object.map().set(name, std::move(value));
    return;
  }
  rt.fail(std::string("cannot set a field on a value of type '") + object.typeName() + "'", line,
          col);
}

}  // namespace ops
