#pragma once

#include <string>

#include "value.h"

class Interpreter;

// Registers the standard library in the interpreter's global scope.
void registerBuiltins(Interpreter& interp);

// Looks up a built-in method on a value (array.push, string.upper, map.keys,
// ...). Returns false when the receiver has no such method.
bool bindMethod(Interpreter& interp, const Value& receiver, const std::string& name, Value& out);
