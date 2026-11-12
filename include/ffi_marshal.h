#pragma once

#include <string>
#include <vector>

#include "ffi_types.h"
#include "runtime.h"
#include "value.h"

// Runtime half of the `extern "C"` FFI (PHASE 6.6).
//
// A C function is declared in an `extern "C"` block, the compiler records the
// declaration as `Bytecode::externs`, and the engine resolves the symbol
// through `Platform::lookupSymbol` before the proto runs. This module turns the
// resolved address plus the declared signature into a Oskel native, marshalling
// the arguments and the result across the System V AMD64 ABI.
//
// No `libffi`: the call goes through a compile-time thunk selected by the
// signature's ABI shape. Arguments are passed in the register class the ABI
// assigns them (general-purpose or SSE), and the result is read back with the
// exact C type the declaration named, so a narrow `int` return is not read as a
// 64-bit value.
namespace ffi {

// Builds the native function that stands in for a resolved `extern "C"` symbol.
//
// `address` is the resolved symbol; `paramTypes` and `returnType` are the
// declared FFI types (a scalar or a resolved struct), in order. The returned
// `Value` is a native ready to be defined as a global. `Runtime::fail` is used
// for a wrong argument type or arity, so a bad call is a clean runtime error,
// never a wild C call.
//
// The caller validates the signature with `ffi::validateSignature` first; a
// signature that reaches here unsupported is a programming error.
Value makeExternNative(Runtime& rt, const std::string& name, void* address,
                       const std::vector<TypeSpec>& paramTypes, const TypeSpec& returnType);

}  // namespace ffi
