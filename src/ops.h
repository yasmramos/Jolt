#pragma once

#include <string>

#include "lexer.h"
#include "value.h"

class Runtime;

// Per-type value semantics, shared by the AST interpreter and the bytecode VM.
//
// Both engines call these functions instead of carrying their own copy of the
// rules, so they cannot drift apart: `+` concatenates strings in one engine
// exactly when it does in the other, and an error message is produced in a
// single place. Every function reports errors through `Runtime::fail`.
namespace ops {

Value unary(Runtime& rt, Tok op, const Value& operand, int line, int col);
Value binary(Runtime& rt, Tok op, const Value& left, const Value& right, int line, int col);
bool lessThan(Runtime& rt, const Value& left, const Value& right, int line, int col);

// The `in` operator for arrays, maps, strings and ranges.
bool contains(Runtime& rt, const Value& needle, const Value& haystack, int line, int col);

Value indexGet(Runtime& rt, const Value& object, const Value& index, int line, int col);
void indexSet(Runtime& rt, const Value& object, const Value& index, Value value, int line, int col);
Value memberGet(Runtime& rt, const Value& object, const std::string& name, int line, int col);
void memberSet(Runtime& rt, const Value& object, const std::string& name, Value value, int line, int col);

}  // namespace ops
