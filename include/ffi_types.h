#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ast.h"

// The FFI type vocabulary, classified for the System V AMD64 ABI (PHASE 6.6).
//
// This header is deliberately free of any runtime dependency: the compiler uses
// it to validate an `extern "C"` signature and reject one the marshalling
// cannot honour, and the runtime uses the same tables to build the call. That
// keeps a single source of truth while respecting the toolchain boundary
// (AGENT.md §2: `src/compiler.*` must not pull in the VM or the interpreter).
//
// The ABI passes every integer and pointer in a general-purpose register and
// every floating-point value in an SSE register, so a declaration only needs
// to distinguish those two register classes; `float` and `double` share the
// SSE class but need distinct C++ types to marshal the value.
namespace ffi {

// The register class a declared parameter maps to.
enum class ParamClass {
  Int,      // integer, bool or an unannotated/`any` parameter (GPR)
  Pointer,  // `ptr`/`cstring` (GPR, but marshalled from a handle)
  Float,    // C `float` (SSE)
  Double,   // C `double` (SSE)
};

// The class of a declared return type. `Void` produces no value; `Ptr` and
// `CString` produce a Jolt pointer handle.
enum class ReturnClass {
  Void,
  Bool,
  Int,
  Float,
  Double,
  Ptr,
  CString,
};

// Largest arity the compile-time thunks cover. C functions in the standard
// library rarely exceed this; a wider signature is rejected with a clear
// message instead of silently mis-calling. Raising it costs template
// instantiations, so it stays a documented, deliberate bound.
constexpr std::size_t kMaxExternArgs = 5;

// Maps a declared Jolt parameter type to its FFI class. An empty name or `any`
// is treated as C's implicit `int` (the GPR class). Returns false when the type
// cannot cross the C boundary.
inline bool classifyParam(const std::string& typeName, ParamClass& out) {
  if (typeName.empty() || typeName == "any" || typeName == "bool" ||
      isIntegerTypeName(typeName)) {
    out = ParamClass::Int;
    return true;
  }
  if (typeName == "ptr" || typeName == "cstring") {
    out = ParamClass::Pointer;
    return true;
  }
  if (typeName == "float") {
    out = ParamClass::Float;
    return true;
  }
  if (typeName == "double") {
    out = ParamClass::Double;
    return true;
  }
  return false;
}

// Maps a declared return type to its FFI class. An empty name is C's implicit
// `int`. Returns false when the type cannot be produced by a C function.
inline bool classifyReturn(const std::string& typeName, ReturnClass& out) {
  if (typeName.empty() || typeName == "any" || isIntegerTypeName(typeName)) {
    out = ReturnClass::Int;
    return true;
  }
  if (typeName == "void") {
    out = ReturnClass::Void;
    return true;
  }
  if (typeName == "bool") {
    out = ReturnClass::Bool;
    return true;
  }
  if (typeName == "float") {
    out = ReturnClass::Float;
    return true;
  }
  if (typeName == "double") {
    out = ReturnClass::Double;
    return true;
  }
  if (typeName == "ptr") {
    out = ReturnClass::Ptr;
    return true;
  }
  if (typeName == "cstring") {
    out = ReturnClass::CString;
    return true;
  }
  return false;
}

// Human-readable reason a type is not marshallable, for a diagnostic. The
// caller prefixes it with the symbol it belongs to.
inline std::string unsupportedTypeMessage(const std::string& typeName, bool isReturn) {
  std::string shown = typeName.empty() ? "unannotated" : "'" + typeName + "'";
  return (isReturn ? "return type " : "parameter type ") + shown +
         " cannot cross the C boundary";
}

// Checks one `extern "C"` signature against what the marshalling can honour.
// Returns an empty string when the signature is fine, or a message naming the
// first problem. The compiler calls this so an unsupported declaration fails
// at compile time with a clear error instead of at the first call.
inline std::string validateSignature(const std::vector<std::string>& paramTypes,
                                     const std::string& returnType) {
  if (paramTypes.size() > kMaxExternArgs) {
    return "an 'extern' function may declare at most " + std::to_string(kMaxExternArgs) +
           " parameters";
  }
  for (const std::string& type : paramTypes) {
    ParamClass ignored;
    if (!classifyParam(type, ignored)) return unsupportedTypeMessage(type, /*isReturn=*/false);
  }
  ReturnClass ignoredReturn;
  if (!classifyReturn(returnType, ignoredReturn)) {
    return unsupportedTypeMessage(returnType, /*isReturn=*/true);
  }
  return "";
}

}  // namespace ffi
