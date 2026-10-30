#pragma once

#include <string>

#include "errors.h"

void printCompileError(const std::string& sourceName, const std::string& source,
                       const CompileError& error);
void printRuntimeError(const std::string& sourceName, const std::string& source,
                       const RuntimeError& error);
