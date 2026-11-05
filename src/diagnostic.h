#pragma once

#include <string>

#include "errors.h"
#include "platform.h"

// `platform` defaults to the development implementation so existing call sites
// (and the host toolchain) keep working; freestanding callers pass their own.
void printCompileError(const std::string& sourceName, const std::string& source,
                       const CompileError& error, Platform& platform);
void printRuntimeError(const std::string& sourceName, const std::string& source,
                       const RuntimeError& error, Platform& platform);