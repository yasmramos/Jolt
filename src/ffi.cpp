#include "ffi.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "ffi_types.h"

// Marshalling across the System V AMD64 ABI, without `libffi` (PHASE 6.6).
//
// The call is a compile-time thunk selected by the signature's ABI shape, not
// code generated at run time. Three facts make this a small, closed set:
//
//   - Every integer, bool and pointer argument travels in a general-purpose
//     register; every `float`/`double` in an SSE register. So an argument only
//     needs its register class, not its exact width: the callee reads the
//     sub-register it declared, and the value in the full register matches.
//   - The result, by contrast, is read from `rax`/`xmm0` at the width the
//     declaration named. A C `int` leaves the upper half of `rax` undefined, so
//     the caller truncates to the declared width before boxing it. That is why
//     the return type needs the exact width and an argument does not.
//   - An `extern "C"` declaration is checked at compile time
//     (`ffi::validateSignature`), so a signature reaching the marshalling is
//     always one of the shapes below.
//
// The argument list is walked with a compile-time depth (`Index`), not a
// runtime count: the recursion stops at `kMaxExternArgs`, so the compiler
// instantiates a bounded set of thunks instead of recursing on a runtime
// condition (which would instantiate forever).
namespace ffi {
namespace {

// ------------------------------------------------------------------ arguments

// A general-purpose-register argument: an integer, a bool or a pointer. All of
// them travel in the same register; the declaration decides how the Jolt value
// is turned into the 64-bit word.
std::int64_t marshalGpr(Runtime& rt, const Value& value, ParamClass cls,
                        const std::string& typeName) {
  if (cls == ParamClass::Pointer) {
    if (value.isNil()) return 0;  // NULL
    // A Jolt string is NUL-terminated and the argument stays alive for the
    // whole call, so its bytes are a valid `const char *`.
    if (value.isStr()) {
      return reinterpret_cast<std::int64_t>(value.asStr().c_str());
    }
    if (!value.isPtr()) {
      rt.fail("argument of type '" + std::string(value.typeName()) +
              "' cannot be passed as '" + typeName + "'");
    }
    PtrObj& pointer = value.ptr();
    // A `void *` carries an opaque token; an owned `cstring`/buffer exposes the
    // address of its bytes, which is what a C callee dereferences.
    if (pointer.kind == PtrKind::Void) return pointer.address;
    return reinterpret_cast<std::int64_t>(pointer.bytes.data());
  }
  if (value.isBool()) return value.asBool() ? 1 : 0;
  if (value.isInt()) return value.asInt();
  if (value.isNumber()) return static_cast<std::int64_t>(value.number());
  rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
          typeName + "'");
}

double marshalDouble(Runtime& rt, const Value& value, const std::string& typeName) {
  if (value.isNumber()) return value.number();
  rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
          typeName + "'");
}

float marshalFloat(Runtime& rt, const Value& value, const std::string& typeName) {
  return static_cast<float>(marshalDouble(rt, value, typeName));
}

// -------------------------------------------------------------------- results

// Truncates the raw `rax` to the width the declaration named, so a C `int` is
// read as 32 bits and not as the garbage the upper half of `rax` may hold.
Value wrapInteger(Runtime& rt, const std::string& typeName, std::int64_t raw) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32") {
    return Value(static_cast<std::int64_t>(static_cast<std::int32_t>(raw)));
  }
  if (typeName == "bool") return Value((raw & 0xff) != 0);
  if (typeName == "int8") return Value(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)));
  if (typeName == "int16") return Value(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
  if (typeName == "int64") return Value(raw);
  if (typeName == "uint8") return Value(static_cast<std::int64_t>(static_cast<std::uint8_t>(raw)));
  if (typeName == "uint16") return Value(static_cast<std::int64_t>(static_cast<std::uint16_t>(raw)));
  if (typeName == "uint" || typeName == "uint32") {
    return Value(static_cast<std::int64_t>(static_cast<std::uint32_t>(raw)));
  }
  if (typeName == "uint64" || typeName == "size_t") {
    return Value(static_cast<std::int64_t>(static_cast<std::uint64_t>(raw)));
  }
  // `validateSignature` rejects anything else before a call can reach here.
  rt.fail("unsupported C return type '" + typeName + "'");
}

// Builds the Jolt value for a pointer result. A `void *` is an opaque token (it
// is never dereferenced); a `cstring` copies the bytes up to the NUL, so the
// handle owns them and a later `cstr()` can read them safely.
Value wrapPointer(ReturnClass cls, std::int64_t raw) {
  if (raw == 0) return Value();  // NULL
  if (cls == ReturnClass::Ptr) {
    return makePtr(PtrKind::Void, {}, raw);
  }
  const char* text = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw));
  std::vector<char> bytes(text, text + std::strlen(text));
  bytes.push_back('\0');
  return makePtr(PtrKind::CString, std::move(bytes));
}

// Invokes the C function with the fully marshalled argument list. The return
// class selects the register and the width: `void` reads nothing, the integer
// family and pointers read `rax`, `float`/`double` read `xmm0`.
template <typename... Built>
Value invoke(void* address, ReturnClass rc, const std::string& returnType, Runtime& rt,
             Built... built) {
  switch (rc) {
    case ReturnClass::Void:
      reinterpret_cast<void (*)(Built...)>(address)(built...);
      return Value();
    case ReturnClass::Float:
      return Value::fromFloat(reinterpret_cast<float (*)(Built...)>(address)(built...));
    case ReturnClass::Double:
      return Value(reinterpret_cast<double (*)(Built...)>(address)(built...));
    case ReturnClass::Ptr:
    case ReturnClass::CString: {
      auto result = reinterpret_cast<std::int64_t (*)(Built...)>(address)(built...);
      return wrapPointer(rc, result);
    }
    default: {  // Int / Bool: read `rax` and truncate to the declared width
      auto result = reinterpret_cast<std::int64_t (*)(Built...)>(address)(built...);
      return wrapInteger(rt, returnType, result);
    }
  }
}

// Walks the declared parameters, marshalling one per level, then calls `invoke`
// with the argument list typed exactly as the declaration named it. `Index` is
// a template parameter so the recursion is bounded by `kMaxExternArgs` and the
// compiler stops instantiating; the runtime check `Index >= classes.size()`
// ends the walk for a shorter signature.
template <std::size_t Index, typename... Built>
Value dispatch(void* address, ReturnClass rc, const std::string& returnType,
               const std::vector<ParamClass>& classes, const std::vector<std::string>& types,
               const std::vector<Value>& values, Runtime& rt, Built... built) {
  if constexpr (Index < kMaxExternArgs) {
    if (Index >= classes.size()) {
      return invoke(address, rc, returnType, rt, built...);
    }
    switch (classes[Index]) {
      case ParamClass::Int:
      case ParamClass::Pointer:
        return dispatch<Index + 1>(address, rc, returnType, classes, types, values, rt, built...,
                                   marshalGpr(rt, values[Index], classes[Index], types[Index]));
      case ParamClass::Float:
        return dispatch<Index + 1>(address, rc, returnType, classes, types, values, rt, built...,
                                   marshalFloat(rt, values[Index], types[Index]));
      case ParamClass::Double:
        return dispatch<Index + 1>(address, rc, returnType, classes, types, values, rt, built...,
                                   marshalDouble(rt, values[Index], types[Index]));
    }
    rt.fail("unsupported C parameter class");
  } else {
    return invoke(address, rc, returnType, rt, built...);
  }
}

}  // namespace

Value makeExternNative(Runtime& rt, const std::string& name, void* address,
                       const std::vector<std::string>& paramTypes,
                       const std::string& returnType) {
  (void)rt;
  std::vector<ParamClass> classes;
  classes.reserve(paramTypes.size());
  for (const std::string& type : paramTypes) {
    ParamClass cls = ParamClass::Int;
    classifyParam(type, cls);
    classes.push_back(cls);
  }
  ReturnClass rc = ReturnClass::Int;
  classifyReturn(returnType, rc);

  // The address and the signature are captured by value, so the native is
  // self-contained and the call site only supplies the arguments.
  return makeNative(name, [name, address, rc, returnType, classes, paramTypes](
                              Runtime& in, std::vector<Value>& args) -> Value {
    if (args.size() != classes.size()) {
      in.fail("'" + name + "' expects " + std::to_string(classes.size()) + " argument" +
              (classes.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
    }
    return dispatch<0>(address, rc, returnType, classes, paramTypes, args, in);
  });
}

}  // namespace ffi
