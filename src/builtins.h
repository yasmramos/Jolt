#pragma once

#include <string>

#include "value.h"

class Runtime;

// Registers the standard library in the runtime's global scope.
void registerBuiltins(Runtime& rt);

// Looks up a built-in method on a value (array.push, string.upper, map.keys,
// ...). Returns false when the receiver has no such method.
bool bindMethod(Runtime& rt, const Value& receiver, const std::string& name, Value& out);
