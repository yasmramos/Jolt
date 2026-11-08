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
// `CString` produce a Jolt pointer handle; `Struct` produces a map rebuilt from
// the returned registers.
enum class ReturnClass {
  Void,
  Bool,
  Int,
  Float,
  Double,
  Ptr,
  CString,
  Struct,
};

// Largest number of *eightbytes* the compile-time thunks cover. A scalar
// argument or return value is one eightbyte; a `struct` counts one per 8 bytes
// it spans (so a 16-byte struct is two). Bounding a signature by eightbytes,
// not parameters, is what keeps the thunk's type alphabet at three C++ types
// (a struct eightbyte is marshalled as a plain `int64_t` or `double`), so the
// template recursion stays a few thousand instantiations instead of exploding.
constexpr std::size_t kMaxExternArgs = 6;

// One field of a struct crossing the C boundary. `type` is an FFI scalar type
// name (the same vocabulary as a parameter annotation); a nested struct is not
// a scalar and is rejected by `buildStructLayout`.
struct FfiField {
  std::string name;
  std::string type;
};

// The C memory layout of a small struct, computed from its field declarations.
// The compiler builds one to validate an `extern "C"` signature and the runtime
// builds the same to marshal the value, so a single function is the source of
// truth for both (the compiler never carries ABI logic of its own).
struct StructLayout {
  std::vector<FfiField> fields;
  std::vector<std::size_t> offsets;  // byte offset of each field
  std::vector<std::size_t> sizes;    // byte size of each field
  std::vector<ParamClass> classes;   // register class of each field
  std::size_t size = 0;              // total size, padded to `alignment`
  std::size_t alignment = 1;
  // System V classification: at most two eightbytes, each `'I'` (INTEGER, a
  // general-purpose register) or `'S'` (SSE, an xmm register). A struct whose
  // size exceeds 16 bytes is class MEMORY and never reaches here.
  char eightbyte[2] = {'I', 'I'};
  int eightbyteCount = 0;
};

// A parameter or return type as written in an `extern "C"` declaration: either
// a scalar (the FFI vocabulary) or a struct (its resolved fields). The compiler
// produces it from the AST, the runtime consumes it, and the AOT backend
// (PHASE 7) will read the same shape off the proto.
struct TypeSpec {
  std::string scalarType;  // scalar spelling when `isStruct` is false ("" = int)
  bool isStruct = false;
  std::vector<FfiField> fields;  // struct shape when `isStruct` is true
  // Callback (PHASE 6.6): a C function pointer. `isCallback` marks it; the
  // signature is stored as scalar type names (a callback parameter cannot
  // itself be a callback or a struct, so no recursion is needed). A callback
  // crosses the ABI as one GPR (the trampoline address).
  bool isCallback = false;
  std::vector<std::string> callbackParams;
  std::string callbackReturn;  // "void" for a callback that returns nothing
};

// Byte size, alignment and register class of a scalar field type. Returns false
// for a type that cannot be a C struct member (a nested struct, a map, ...).
inline bool scalarFieldLayout(const std::string& typeName, std::size_t& size,
                              std::size_t& alignment, ParamClass& cls) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32" ||
      typeName == "uint" || typeName == "uint32") {
    size = 4; alignment = 4; cls = ParamClass::Int; return true;
  }
  if (typeName == "int8" || typeName == "uint8" || typeName == "bool") {
    size = 1; alignment = 1; cls = ParamClass::Int; return true;
  }
  if (typeName == "int16" || typeName == "uint16") {
    size = 2; alignment = 2; cls = ParamClass::Int; return true;
  }
  if (typeName == "int64" || typeName == "uint64" || typeName == "size_t") {
    size = 8; alignment = 8; cls = ParamClass::Int; return true;
  }
  if (typeName == "ptr" || typeName == "cstring") {
    size = 8; alignment = 8; cls = ParamClass::Pointer; return true;
  }
  if (typeName == "float") { size = 4; alignment = 4; cls = ParamClass::Float; return true; }
  if (typeName == "double") { size = 8; alignment = 8; cls = ParamClass::Double; return true; }
  return false;
}

// Computes the C layout of a struct from its fields, following the natural
// alignment of each scalar member. On failure it fills `reason` and returns
// false: a nested/unsupported field type, an empty struct, or a struct larger
// than 16 bytes (which System V passes in memory, a shape the register-based
// marshalling does not support).
inline bool buildStructLayout(const std::vector<FfiField>& fields, StructLayout& out,
                              std::string& reason) {
  out = StructLayout{};
  out.fields = fields;
  std::size_t offset = 0;
  std::size_t maxAlignment = 1;
  for (const FfiField& field : fields) {
    std::size_t size = 0;
    std::size_t alignment = 1;
    ParamClass cls = ParamClass::Int;
    if (!scalarFieldLayout(field.type, size, alignment, cls)) {
      reason = "field '" + field.name + "' has type '" +
               (field.type.empty() ? "unannotated" : field.type) +
               "', which cannot cross the C boundary";
      return false;
    }
    offset = (offset + alignment - 1) / alignment * alignment;
    out.offsets.push_back(offset);
    out.sizes.push_back(size);
    out.classes.push_back(cls);
    offset += size;
    if (alignment > maxAlignment) maxAlignment = alignment;
  }
  if (offset == 0) {
    reason = "an empty struct cannot cross the C boundary";
    return false;
  }
  const std::size_t total = (offset + maxAlignment - 1) / maxAlignment * maxAlignment;
  if (total > 16) {
    reason = "a struct of " + std::to_string(total) +
             " bytes is returned/passed in memory (> 16), which the extern marshalling "
             "does not support";
    return false;
  }
  out.size = total;
  out.alignment = maxAlignment;
  out.eightbyteCount = static_cast<int>((total + 7) / 8);
  // An eightbyte is SSE only when every field touching it is floating point;
  // an integer/pointer field anywhere in it makes the whole eightbyte INTEGER.
  bool anyInteger[2] = {false, false};
  for (std::size_t k = 0; k < fields.size(); ++k) {
    const bool integer = out.classes[k] != ParamClass::Float &&
                         out.classes[k] != ParamClass::Double;
    if (!integer) continue;
    for (std::size_t b = out.offsets[k]; b < out.offsets[k] + out.sizes[k]; ++b) {
      anyInteger[b / 8] = true;
    }
  }
  for (int i = 0; i < out.eightbyteCount; ++i) out.eightbyte[i] = anyInteger[i] ? 'I' : 'S';
  return true;
}

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

// The register shape of a small struct: at most two eightbytes, each INTEGER
// (`I`, a general-purpose register) or SSE (`S`, an xmm register). `None` is a
// scalar, not a struct. The runtime dispatches on this to pick the fixed POD
// type it passes to, or reads the result from, the C function.
enum class StructShape { None, I1, S1, II, SS, IS, SI };

inline StructShape structShapeOf(const StructLayout& layout) {
  if (layout.eightbyteCount == 1) {
    return layout.eightbyte[0] == 'I' ? StructShape::I1 : StructShape::S1;
  }
  const char first = layout.eightbyte[0];
  const char second = layout.eightbyte[1];
  if (first == 'I') return second == 'I' ? StructShape::II : StructShape::IS;
  return second == 'I' ? StructShape::SI : StructShape::SS;
}

// Checks one resolved parameter/return spec against what the marshalling can
// honour. Returns an empty string when the type is fine, or a message naming
// the problem. A scalar goes through `classifyParam`/`classifyReturn`; a struct
// must have a layout `buildStructLayout` accepts.
inline std::string validateTypeSpec(const TypeSpec& spec, bool isReturn) {
  if (spec.isCallback) {
    // A callback is a C function pointer. It crosses the ABI as one GPR, so it
    // is only meaningful as a parameter: a C function that *returns* a
    // function pointer is not part of the surface this phase supports.
    if (isReturn) {
      return "a callback cannot be a return type";
    }
    for (const std::string& param : spec.callbackParams) {
      ParamClass ignored;
      // A callback parameter is a scalar; a nested callback or a struct by
      // value in a callback signature is out of scope.
      if (!classifyParam(param, ignored)) {
        return "callback parameter type " + unsupportedTypeMessage(param, false);
      }
    }
    if (!spec.callbackReturn.empty() && spec.callbackReturn != "void") {
      ReturnClass ignored;
      if (!classifyReturn(spec.callbackReturn, ignored)) {
        return "callback " + unsupportedTypeMessage(spec.callbackReturn, true);
      }
    }
    return "";
  }
  if (!spec.isStruct) {
    if (isReturn) {
      ReturnClass ignored;
      if (!classifyReturn(spec.scalarType, ignored)) {
        return unsupportedTypeMessage(spec.scalarType, true);
      }
    } else {
      ParamClass ignored;
      if (!classifyParam(spec.scalarType, ignored)) {
        return unsupportedTypeMessage(spec.scalarType, false);
      }
    }
    return "";
  }
  StructLayout layout;
  std::string reason;
  if (!buildStructLayout(spec.fields, layout, reason)) return reason;
  return "";
}

// Checks one `extern "C"` signature against what the marshalling can honour.
// Returns an empty string when the signature is fine, or a message naming the
// first problem. The compiler calls this so an unsupported declaration fails
// at compile time with a clear error instead of at the first call.
inline std::string validateSignature(const std::vector<TypeSpec>& params,
                                     const TypeSpec& returnType) {
  // The bound is on flattened eightbytes, not parameters: a struct contributes
  // one per 8 bytes it spans. Keeping the total at or below the number of
  // general-purpose argument registers guarantees every eightbyte lands in a
  // register, so none spills to the stack (a partly-registered aggregate is
  // passed wholly in memory, which the register thunks cannot express).
  std::size_t eightbytes = 0;
  for (const TypeSpec& spec : params) {
    if (spec.isStruct) {
      StructLayout layout;
      std::string reason;
      if (!buildStructLayout(spec.fields, layout, reason)) return reason;
      eightbytes += static_cast<std::size_t>(layout.eightbyteCount);
    } else {
      eightbytes += 1;
    }
    std::string problem = validateTypeSpec(spec, /*isReturn=*/false);
    if (!problem.empty()) return problem;
  }
  if (eightbytes > kMaxExternArgs) {
    return "an 'extern' function may pass at most " + std::to_string(kMaxExternArgs) +
           " register-sized values (a struct counts one per 8 bytes)";
  }
  return validateTypeSpec(returnType, /*isReturn=*/true);
}

}  // namespace ffi
