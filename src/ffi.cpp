#include "ffi_marshal.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "ffi_types.h"

#ifdef JOLT_HAVE_LIBFFI
// libffi builds the trampoline a C function calls back into. It is only used
// for callbacks, and only in the development engine (AGENT.md §5.1/§6.6).
// `ffi.h` is the system header: the project's own header is `ffi_marshal.h`
// precisely so this include cannot be shadowed by `-Iinclude`.
#include <ffi.h>
#endif

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
// A small struct (<= 16 bytes) is passed and returned in registers as up to two
// *eightbytes*, each classified INTEGER (a general-purpose register) or SSE (an
// xmm register). Rather than carry a distinct C++ type per struct shape, a
// struct argument is flattened into the same scalar alphabet the thunks already
// use: an INTEGER eightbyte is passed bit-for-bit as an `int64_t`, an SSE
// eightbyte as a `double`, so the callee sees exactly the bytes of the struct
// in the registers the ABI assigns. A struct return is captured through a fixed
// POD whose members land in `rax`/`rdx`/`xmm0`/`xmm1` exactly as the six class
// shapes (`I`, `S`, `II`, `SS`, `IS`, `SI`) require.
//
// Bounding a signature by eightbytes (`kMaxExternArgs` = 6) rather than
// parameters is deliberate: it is at most the number of general-purpose
// registers System V passes arguments in, so every flattened eightbyte still
// lands in a register and none spills to the stack, sidestepping the rule that
// a partly-registered aggregate is passed wholly in memory.
//
// The argument list is walked with a compile-time depth (`Index`), not a
// runtime count: the recursion stops at `kMaxExternArgs`, so the compiler
// instantiates a bounded set of thunks instead of recursing on a runtime
// condition (which would instantiate forever).
namespace ffi {
namespace {

// ------------------------------------------------------------------ arguments

// The machine address behind a pointer argument. A `void *` carries an opaque
// token; an owned `cstring`/buffer exposes the address of its bytes, which is
// what a C callee dereferences. A Jolt string is NUL-terminated and stays alive
// for the whole call, so its bytes are a valid `const char *`.
std::int64_t pointerAddress(Runtime& rt, const Value& value, const std::string& typeName) {
  if (value.isNil()) return 0;  // NULL
  if (value.isStr()) return reinterpret_cast<std::int64_t>(value.asStr().c_str());
  if (!value.isPtr()) {
    rt.fail("argument of type '" + std::string(value.typeName()) + "' cannot be passed as '" +
            typeName + "'");
  }
  PtrObj& pointer = value.ptr();
  if (pointer.kind == PtrKind::Void) return pointer.address;
  return reinterpret_cast<std::int64_t>(pointer.bytes.data());
}

// A general-purpose-register argument: an integer, a bool or a pointer. All of
// them travel in the same register; the declaration decides how the Jolt value
// is turned into the 64-bit word.
std::int64_t marshalGpr(Runtime& rt, const Value& value, ParamClass cls,
                        const std::string& typeName) {
  if (cls == ParamClass::Pointer) return pointerAddress(rt, value, typeName);
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

Value wrapPointerValue(const std::string& typeName, std::int64_t raw) {
  if (raw == 0) return Value();  // NULL
  if (typeName == "ptr") return makePtr(PtrKind::Void, {}, raw);
  const char* text = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(raw));
  std::vector<char> bytes(text, text + std::strlen(text));
  bytes.push_back('\0');
  return makePtr(PtrKind::CString, std::move(bytes));
}

// -------------------------------------------------------------------- structs

// Writes one scalar field of a struct argument into `bytes` at `offset`, in the
// C representation of its declared type, using exactly the field's width (the
// target is little-endian, so a narrower store is the low-order bytes).
void writeStructField(std::vector<char>& bytes, std::size_t offset, const std::string& type,
                      const Value& value, Runtime& rt) {
  if (type == "float") {
    const float f = marshalFloat(rt, value, type);
    std::memcpy(bytes.data() + offset, &f, sizeof(f));
    return;
  }
  if (type == "double") {
    const double d = marshalDouble(rt, value, type);
    std::memcpy(bytes.data() + offset, &d, sizeof(d));
    return;
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(type, size, alignment, cls);
  std::int64_t raw = (cls == ParamClass::Pointer) ? pointerAddress(rt, value, type)
                                                  : marshalGpr(rt, value, cls, type);
  std::memcpy(bytes.data() + offset, &raw, size);
}

// Reads one scalar field of a returned struct from `bytes` at `offset` and boxes
// it as a Jolt value, reading only the field's declared width.
Value readStructField(Runtime& rt, const char* bytes, std::size_t offset,
                      const std::string& type) {
  if (type == "float") {
    float f = 0;
    std::memcpy(&f, bytes + offset, sizeof(f));
    return Value::fromFloat(f);
  }
  if (type == "double") {
    double d = 0;
    std::memcpy(&d, bytes + offset, sizeof(d));
    return Value(d);
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(type, size, alignment, cls);
  std::int64_t raw = 0;
  std::memcpy(&raw, bytes + offset, size);
  if (type == "ptr" || isFfiCStringTypeName(type)) return wrapPointerValue(type, raw);
  return wrapInteger(rt, type.empty() ? "int" : type, raw);
}

// The POD types a returned struct is captured through. Each matches one System V
// class shape: the two eightbytes come back in `rax`/`rdx` (INTEGER) and/or
// `xmm0`/`xmm1` (SSE), and a trivially copyable aggregate of these members is
// returned exactly there, so the C++ compiler does the register choreography.
struct PodI { std::int64_t a; };
struct PodS { double a; };
struct PodII { std::int64_t a; std::int64_t b; };
struct PodSS { double a; double b; };
struct PodIS { std::int64_t a; double b; };
struct PodSI { double a; std::int64_t b; };

// Rebuilds the Jolt map of a returned struct from its raw bytes.
Value boxStructResult(Runtime& rt, const StructLayout& layout, const char* bytes) {
  MapPtr result = makeMap();
  for (std::size_t k = 0; k < layout.fields.size(); ++k) {
    result->set(layout.fields[k].name,
                readStructField(rt, bytes, layout.offsets[k], layout.fields[k].type));
  }
  return Value(result);
}

// Runs the returned-struct case for a chosen POD shape, then boxes the bytes.
template <typename Pod, typename... Built>
Value invokeStruct(void* address, const StructLayout& layout, Runtime& rt, Built... built) {
  Pod pod = reinterpret_cast<Pod (*)(Built...)>(address)(built...);
  char bytes[16] = {0};
  std::memcpy(bytes, &pod, sizeof(Pod));
  return boxStructResult(rt, layout, bytes);
}

// Invokes the C function with the fully marshalled argument list. The return
// class selects the register and the width: `void` reads nothing, the integer
// family and pointers read `rax`, `float`/`double` read `xmm0`, and a struct
// reads the registers its shape assigns.
template <typename... Built>
Value invoke(void* address, ReturnClass rc, StructShape shape, const StructLayout& layout,
             const std::string& returnType, Runtime& rt, Built... built) {
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
      return wrapPointerValue(returnType, result);
    }
    case ReturnClass::Struct:
      switch (shape) {
        case StructShape::I1: return invokeStruct<PodI>(address, layout, rt, built...);
        case StructShape::S1: return invokeStruct<PodS>(address, layout, rt, built...);
        case StructShape::II: return invokeStruct<PodII>(address, layout, rt, built...);
        case StructShape::SS: return invokeStruct<PodSS>(address, layout, rt, built...);
        case StructShape::IS: return invokeStruct<PodIS>(address, layout, rt, built...);
        case StructShape::SI: return invokeStruct<PodSI>(address, layout, rt, built...);
        case StructShape::None: break;
      }
      rt.fail("internal error: a struct return has no ABI shape");
    default: {  // Int / Bool: read `rax` and truncate to the declared width
      auto result = reinterpret_cast<std::int64_t (*)(Built...)>(address)(built...);
      return wrapInteger(rt, returnType, result);
    }
  }
}

// Walks the flattened eightbyte argument list, marshalling one per level, then
// calls `invoke` with the argument list typed exactly as the declaration named
// it. `Index` is a template parameter so the recursion is bounded by
// `kMaxExternArgs` and the compiler stops instantiating; the runtime check
// `Index >= classes.size()` ends the walk for a shorter signature.
template <std::size_t Index, typename... Built>
Value dispatch(void* address, ReturnClass rc, StructShape shape, const StructLayout& layout,
               const std::string& returnType, const std::vector<ParamClass>& classes,
               const std::vector<std::string>& types, const std::vector<Value>& values, Runtime& rt,
               Built... built) {
  if constexpr (Index < kMaxExternArgs) {
    if (Index >= classes.size()) {
      return invoke(address, rc, shape, layout, returnType, rt, built...);
    }
    switch (classes[Index]) {
      case ParamClass::Int:
      case ParamClass::Pointer:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built...,
                                   marshalGpr(rt, values[Index], classes[Index], types[Index]));
      case ParamClass::Float:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built..., marshalFloat(rt, values[Index], types[Index]));
      case ParamClass::Double:
        return dispatch<Index + 1>(address, rc, shape, layout, returnType, classes, types, values,
                                   rt, built..., marshalDouble(rt, values[Index], types[Index]));
      case ParamClass::Callback:
        // A callback is marshalled to an `int64` trampoline address before the
        // dispatch walk, so it never reaches here; fail rather than fall
        // through silently if that ever changes.
        break;
    }
    rt.fail("unsupported C parameter class");
  } else {
    return invoke(address, rc, shape, layout, returnType, rt, built...);
  }
}

#ifdef JOLT_HAVE_LIBFFI
// ------------------------------------------------------------------ callbacks

// Maps an FFI scalar type name to the libffi type the trampoline marshals it
// as. `validateSignature` has already rejected anything unsupported, so the
// fallback (`int`) is only reached for an unannotated scalar.
ffi_type* libffiTypeFor(const std::string& typeName) {
  if (typeName.empty() || typeName == "any" || typeName == "int" || typeName == "int32") {
    return &ffi_type_sint32;
  }
  if (typeName == "bool" || typeName == "uint8") return &ffi_type_uint8;
  if (typeName == "int8") return &ffi_type_sint8;
  if (typeName == "int16") return &ffi_type_sint16;
  if (typeName == "uint16") return &ffi_type_uint16;
  if (typeName == "int64") return &ffi_type_sint64;
  if (typeName == "uint" || typeName == "uint32") return &ffi_type_uint32;
  if (typeName == "uint64" || typeName == "size_t") return &ffi_type_uint64;
  if (typeName == "float") return &ffi_type_float;
  if (typeName == "double") return &ffi_type_double;
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) return &ffi_type_pointer;
  return &ffi_type_sint32;
}

// The state behind one Jolt function passed to C as a callback: the function
// itself, its prepared `ffi_cif`, and the libffi closure C calls. The object is
// kept alive for the whole extern call (the caller holds a `Ref`), so the
// trampoline address stays valid while C runs. A callback C stores for later is
// out of scope for this phase and would need a longer-lived owner.
struct CallbackObj : RefCounted {
  ~CallbackObj() {
    if (closure != nullptr) ffi_closure_free(closure);
  }

  Runtime* runtime = nullptr;
  Value function;
  std::string name;  // the extern symbol, for diagnostics
  std::vector<std::string> paramTypes;
  std::string returnType;  // "void" for no result
  std::vector<ffi_type*> argTypes;
  ffi_cif cif{};
  ffi_closure* closure = nullptr;
  void* code = nullptr;
  // A RuntimeError raised inside the Jolt callback cannot unwind through the C
  // frame that called the trampoline, so the trampoline stores it here and the
  // extern call re-raises it once C returns.
  bool hasPendingError = false;
  std::string pendingMessage;
  int pendingLine = 0;
  int pendingCol = 0;
};

// Reads one callback argument from the `void*` libffi hands the trampoline and
// boxes it as a Jolt value, using the declared width so a narrow C `int` is not
// read as 64 bits.
Value readCallbackArg(Runtime& rt, const std::string& typeName, void* slot) {
  if (typeName == "float") return Value::fromFloat(*static_cast<float*>(slot));
  if (typeName == "double") return Value(*static_cast<double*>(slot));
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    auto raw = reinterpret_cast<std::int64_t>(*static_cast<void**>(slot));
    return wrapPointerValue(typeName, raw);
  }
  std::int64_t raw = 0;
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(typeName, size, alignment, cls);
  std::memcpy(&raw, slot, size);
  return wrapInteger(rt, typeName, raw);
}

// Writes the Jolt callback's result into the storage libffi provides, in the
// declared C type. `void` writes nothing; the error path writes zero of the
// declared width so C never observes an uninitialised return.
void writeCallbackResult(const std::string& typeName, const Value& value, void* ret) {
  if (typeName.empty() || typeName == "void") return;
  if (typeName == "float") {
    *static_cast<float*>(ret) = value.isNumber() ? static_cast<float>(value.number()) : 0.0f;
    return;
  }
  if (typeName == "double") {
    *static_cast<double*>(ret) = value.isNumber() ? value.number() : 0.0;
    return;
  }
  if (typeName == "ptr" || isFfiCStringTypeName(typeName)) {
    std::int64_t raw = 0;
    if (value.isStr()) {
      raw = reinterpret_cast<std::int64_t>(value.asStr().c_str());
    } else if (value.isPtr()) {
      const PtrObj& pointer = value.ptr();
      raw = pointer.kind == PtrKind::Void
                ? pointer.address
                : reinterpret_cast<std::int64_t>(pointer.bytes.data());
    }
    *static_cast<void**>(ret) = reinterpret_cast<void*>(raw);
    return;
  }
  std::int64_t raw = 0;
  if (value.isBool()) {
    raw = value.asBool() ? 1 : 0;
  } else if (value.isInt()) {
    raw = value.asInt();
  } else if (value.isNumber()) {
    raw = static_cast<std::int64_t>(value.number());
  }
  std::size_t size = 8;
  std::size_t alignment = 8;
  ParamClass cls = ParamClass::Int;
  scalarFieldLayout(typeName, size, alignment, cls);
  std::memcpy(ret, &raw, size);
}

// The C entry point libffi calls for every invocation. It converts the C
// arguments to Jolt values, re-enters the engine through `Runtime::call`, and
// writes the result back. It must never let a C++ exception cross the C frame
// (the caller is C code such as `qsort`), so a Jolt error is captured and
// re-raised by the extern call once C returns.
void joltCallbackTrampoline(ffi_cif* cif, void* ret, void** args, void* userData) {
  (void)cif;
  CallbackObj* callback = static_cast<CallbackObj*>(userData);
  if (callback->hasPendingError) {
    // C may ignore the result, but never leave the return storage with a stale
    // value. A `void` callback writes nothing.
    if (callback->returnType != "void") std::memset(ret, 0, 8);
    return;
  }
  std::vector<Value> joltArgs;
  joltArgs.reserve(callback->paramTypes.size());
  for (std::size_t i = 0; i < callback->paramTypes.size(); ++i) {
    joltArgs.push_back(readCallbackArg(*callback->runtime, callback->paramTypes[i], args[i]));
  }
  Value result;
  try {
    result = callback->runtime->call(callback->function, std::move(joltArgs),
                                     callback->runtime->currentLine, callback->runtime->currentCol);
  } catch (const RuntimeError& error) {
    callback->hasPendingError = true;
    callback->pendingMessage = error.message;
    callback->pendingLine = error.line;
    callback->pendingCol = error.col;
    if (callback->returnType != "void") std::memset(ret, 0, 8);
    return;
  } catch (...) {
    callback->hasPendingError = true;
    callback->pendingMessage = "the callback raised an unknown error";
    if (callback->returnType != "void") std::memset(ret, 0, 8);
    return;
  }
  writeCallbackResult(callback->returnType, result, ret);
}

// Builds the closure for one Jolt function bound to a callback parameter.
Ref<CallbackObj> makeCallback(Runtime& rt, const std::string& name, const TypeSpec& spec,
                              const Value& function) {
  auto callback = makeRef<CallbackObj>();
  callback->runtime = &rt;
  callback->function = function;
  callback->name = name;
  callback->paramTypes = spec.callbackParams;
  callback->returnType = spec.callbackReturn.empty() ? "void" : spec.callbackReturn;
  callback->argTypes.reserve(callback->paramTypes.size());
  for (const std::string& type : callback->paramTypes) {
    callback->argTypes.push_back(libffiTypeFor(type));
  }
  ffi_type* returnType =
      callback->returnType == "void" ? &ffi_type_void : libffiTypeFor(callback->returnType);
  if (ffi_prep_cif(&callback->cif, FFI_DEFAULT_ABI,
                   static_cast<unsigned>(callback->argTypes.size()), returnType,
                   callback->argTypes.data()) != FFI_OK) {
    rt.fail("cannot build a callback for '" + name + "'");
  }
  void* code = nullptr;
  callback->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &code));
  if (callback->closure == nullptr) {
    rt.fail("cannot allocate a callback for '" + name + "'");
  }
  if (ffi_prep_closure_loc(callback->closure, &callback->cif, joltCallbackTrampoline,
                           callback.get(), code) != FFI_OK) {
    ffi_closure_free(callback->closure);
    callback->closure = nullptr;
    rt.fail("cannot prepare a callback for '" + name + "'");
  }
  callback->code = code;
  return callback;
}
#endif  // JOLT_HAVE_LIBFFI

}  // namespace

Value makeExternNative(Runtime& rt, const std::string& name, void* address,
                       const std::vector<TypeSpec>& paramTypes, const TypeSpec& returnType) {
  (void)rt;
  // Precompute the return disposition once: its class, its shape (for a struct)
  // and its resolved layout, so the call site only marshals arguments.
  ReturnClass rc = ReturnClass::Int;
  StructShape shape = StructShape::None;
  StructLayout layout;
  std::string returnScalar;
  if (returnType.isStruct) {
    if (!kStructByValueSupported) {
      rt.fail("a struct cannot cross the C boundary by value on this target");
    }
    std::string reason;
    if (!buildStructLayout(returnType.fields, layout, reason)) {
      rt.fail("invalid extern return struct: " + reason);
    }
    rc = ReturnClass::Struct;
    shape = structShapeOf(layout);
  } else {
    returnScalar = returnType.scalarType;
    classifyReturn(returnScalar, rc);
  }

  // The address, the signature and the (flattened) parameter classes are
  // captured by value, so the native is self-contained.
  return makeNative(name, [name, address, rc, shape, layout, returnScalar, paramTypes](
                              Runtime& in, std::vector<Value>& args) -> Value {
    if (args.size() != paramTypes.size()) {
      in.fail("'" + name + "' expects " + std::to_string(paramTypes.size()) + " argument" +
              (paramTypes.size() == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
    }
    // Flatten the declared arguments to eightbytes: a scalar is one entry in the
    // register class the ABI assigns it; a struct is one entry per eightbyte, an
    // INTEGER eightbyte passed bit-for-bit as an `int64_t` and an SSE eightbyte
    // as a `double`, so the callee sees the struct's exact bytes.
    std::vector<ParamClass> classes;
    std::vector<std::string> types;
    std::vector<Value> values;
#ifdef JOLT_HAVE_LIBFFI
    // Callbacks built for this call, kept alive until C returns so the
    // trampoline address stays valid while the callee runs.
    std::vector<Ref<CallbackObj>> callbacks;
#endif
    for (std::size_t i = 0; i < paramTypes.size(); ++i) {
      const TypeSpec& spec = paramTypes[i];
      if (spec.isCallback) {
#ifdef JOLT_HAVE_LIBFFI
        // A callback is one GPR: the trampoline address (or NULL for `nil`).
        std::int64_t code = 0;
        if (!args[i].isNil()) {
          if (!args[i].isFunc()) {
            in.fail("argument " + std::to_string(i + 1) + " of '" + name +
                    "' must be a function or nil, got '" + args[i].typeName() + "'");
          }
          auto callback = makeCallback(in, name, spec, args[i]);
          code = reinterpret_cast<std::int64_t>(callback->code);
          callbacks.push_back(std::move(callback));
        }
        classes.push_back(ParamClass::Int);
        types.push_back("int64");
        values.push_back(Value(code));
#else
        in.fail("a callback argument to '" + name +
                "' needs libffi, which this build does not have");
#endif
        continue;
      }
      if (!spec.isStruct) {
        ParamClass cls = ParamClass::Int;
        classifyParam(spec.scalarType, cls);
        classes.push_back(cls);
        types.push_back(spec.scalarType);
        values.push_back(args[i]);
        continue;
      }
      StructLayout structLayout;
      std::string reason;
      if (!kStructByValueSupported) {
        in.fail("a struct cannot cross the C boundary by value on this target");
      }
      if (!buildStructLayout(spec.fields, structLayout, reason)) in.fail(reason);
      if (!args[i].isMap()) {
        in.fail("argument " + std::to_string(i + 1) + " of '" + name +
                "' must be a struct value");
      }
      // The buffer is padded to whole eightbytes so the eightbyte extraction
      // below never reads past the end of a struct smaller than 8 bytes; the
      // padding bytes are zero and a C callee ignores them.
      std::vector<char> bytes(static_cast<std::size_t>(structLayout.eightbyteCount) * 8, 0);
      for (std::size_t k = 0; k < structLayout.fields.size(); ++k) {
        const Value* field = args[i].map().get(structLayout.fields[k].name);
        if (field == nullptr) {
          in.fail("argument " + std::to_string(i + 1) + " of '" + name + "' is missing field '" +
                  structLayout.fields[k].name + "'");
        }
        writeStructField(bytes, structLayout.offsets[k], structLayout.fields[k].type, *field, in);
      }
      for (int e = 0; e < structLayout.eightbyteCount; ++e) {
        const char* chunk = bytes.data() + e * 8;
        if (structLayout.eightbyte[e] == 'I') {
          std::int64_t raw = 0;
          std::memcpy(&raw, chunk, sizeof(raw));
          classes.push_back(ParamClass::Int);
          types.push_back("int64");
          values.push_back(Value(raw));
        } else {
          double bits = 0;
          std::memcpy(&bits, chunk, sizeof(bits));
          classes.push_back(ParamClass::Double);
          types.push_back("double");
          values.push_back(Value(bits));
        }
      }
    }
    Value result = dispatch<0>(address, rc, shape, layout, returnScalar, classes, types, values, in);
#ifdef JOLT_HAVE_LIBFFI
    // A Jolt error raised inside a callback could not unwind through the C
    // frame; re-raise it now that C has returned.
    for (const Ref<CallbackObj>& callback : callbacks) {
      if (callback->hasPendingError) {
        in.fail(callback->pendingMessage, callback->pendingLine, callback->pendingCol);
      }
    }
#endif
    return result;
  });
}

}  // namespace ffi
