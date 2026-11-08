# Jolt Language Reference

Language version: **0.1.0**

This guide describes the syntax and semantics of Jolt as implemented by the interpreter included in this repository.

## Table of contents

1. [Execution model](#1-execution-model)
2. [Lexical structure](#2-lexical-structure)
3. [Data types](#3-data-types)
4. [Variables and scope](#4-variables-and-scope)
   - [4.6 Type annotations](#46-type-annotations)
   - [4.7 `struct`](#47-struct)
5. [Operators](#5-operators)
6. [Statements](#6-statements)
7. [Functions and closures](#7-functions-and-closures)
   - [7.6 `native fn`](#76-native-fn)
   - [7.7 `@MustUse`](#77-mustuse)
8. [Arrays](#8-arrays)
9. [Strings](#9-strings)
10. [Maps](#10-maps)
11. [Ranges](#11-ranges)
12. [Standard library](#12-standard-library)
13. [Errors and diagnostics](#13-errors-and-diagnostics)
    - [13.3 Propagating and recovering: `try` and `catch`](#133-propagating-and-recovering-try-and-catch)
14. [REPL](#14-repl)
15. [Command line](#15-command-line)
16. [Known limitations](#16-known-limitations)
17. [Complete example](#17-complete-example)
18. [Modules and `import`](#18-modules-and-import)
19. [File extensions](#19-file-extensions)

---

A Jolt program is a UTF-8 text file (extension `.jolt`). The pipeline is:

1. **Lexer**: turns the text into tokens.
2. **Parser**: builds an AST with a recursive-descent parser and operator precedence.
3. **Compiler**: translates the AST to bytecode (40 opcodes, register-based).
4. **VM**: executes the bytecode.

The AST is never executed: the `jolt` executable compiles source to bytecode and runs it on the register-based VM, and the compiler and the VM cover functions, closures, control flow, and collections (PHASE 6 done).

The program runs from top to bottom in the global scope. Simple statements end with `;`. Statements with a block (`if`, `while`, `for`, `fn`) close with `}` and optionally accept a `;` after it.

```jolt
let a = 1;
if (a == 1) {
  print("one");
}; // the ';' is optional after the block
```

A file may start with a `#!...` line (shebang); the lexer ignores it.

---

## 2. Lexical structure

### 2.1 Comments

```jolt
// Line comment

/* Block comment.
   /* Block comments can be nested. */
*/
```

`//` always starts a comment; there is no integer division operator.

### 2.2 Identifiers and reserved words

Identifiers start with an ASCII letter or `_`, and continue with ASCII letters, digits or `_`. They are case-sensitive.

Reserved words:

```text
break  case   catch  const  continue  default  defer  do     else  false  fn  for  if
import in     let    native nil       return   struct switch true  try  var
while
```

They cannot be used as names of variables, parameters or unquoted map keys.

The convention is `camelCase` for variables, functions and methods, and the standard library follows it (`startsWith`, `readFile`, `assertEq`).

### 2.3 Numbers

| Form | Example | Value |
| --- | --- | --- |
| Decimal | `123` | `123` |
| Separators | `1_000_000` | `1000000` |
| Hexadecimal | `0x2A` | `42` |
| Binary | `0b101010` | `42` |
| Octal | `0o52` | `42` |
| Double | `1.5`, `0.25` | `1.5`, `0.25` (binary64) |
| Float | `1.5f`, `0.25F` | `1.5`, `0.25` (binary32) |
| Scientific notation | `1.5e3`, `2E-2` | `1500.0`, `0.02` |

- Integers are 64-bit signed. An out-of-range literal is a compile error.
- The `-` sign is a unary operator, not part of the literal.
- A dot is only part of a float when followed by a digit: `1.` is not a valid float; write `1.0`.
- Underscores are ignored inside digits: `1_0` is equivalent to `10`.
- A fractional literal with no suffix is a `double` (binary64); a trailing
  `f`/`F` makes it a `float` (binary32), like Java. The suffix also turns an
  integer-looking literal into a float: `1f` is a `float`.

### 2.4 Strings

Strings are written between double quotes. There are no single quotes or raw strings. They may span several lines:

```jolt
let greeting = "Hello";
let text = "first line
second line";
```

Escape sequences:

| Escape | Meaning |
| --- | --- |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\0` | Null byte |
| `\\` | Backslash |
| `\"` | Double quote |
| `\$` | Literal dollar sign |
| `\xNN` | Hexadecimal byte (two digits) |
| `\uXXXX` | Unicode code point (four digits) |

### 2.5 Interpolation

Inside a string, `${expression}` evaluates an expression and converts it to text:

```jolt
let name = "Ada";
let age = 36;
print("${name} is ${age} years old");
print("2 + 3 = ${2 + 3}");
print("result: ${ {a: 1}.a }");
```

Interpolated expressions may contain strings and nested braces:

```jolt
print("nested: ${"inner ${1 + 1}"}");
```

To write a literal `${`, escape the dollar sign:

```jolt
print("\${not evaluated}"); // ${not evaluated}
```

Strings are UTF-8 and **immutable**. Length and index operations work on code points, not bytes.

---

## 3. Data types

| Type | Values | Notes |
| --- | --- | --- |
| `nil` | `nil` | Absence of a value. |
| `bool` | `true`, `false` | Logical values. |
| `int` | 64-bit signed integers. | Arithmetic wraps around on overflow. |
| `float` | IEEE 754 single-precision floating-point numbers (binary32). | Literal with an `f`/`F` suffix. Can be `inf`, `-inf` or `nan`. |
| `double` | IEEE 754 double-precision floating-point numbers (binary64). | Default for a fractional literal. Can be `inf`, `-inf` or `nan`. |
| `string` | UTF-8 text. | Immutable. |
| `error` | Error values built by `error(message, code?)`. | Immutable. Carries a `message` and an integer `code` (see §13.2). |
| `array` | Ordered sequence of values. | Mutable and by reference. |
| `map` | Collection of string keys. | Preserves insertion order. Mutable and by reference. |
| `function` | Native or user-defined functions. | First-class citizens. |
| `range` | Interval of integers. | Immutable. |
| `void` | (no values) | Return type only: a function that produces no value (see §4.6). Maps to C's `void` in the FFI. |

The `type(value)` function returns the name of the type as a string.

### 3.1 Truthiness

Only two values are falsy:

```jolt
false
nil
```

Everything else is truthy, including `0`, `0.0`, `""`, `[]` and `{}`.

```jolt
if (0) {
  print("0 is truthy in Jolt");
}
```

### 3.2 Equality

- `int`, `float` and `double` are compared numerically: `1 == 1.0` and `1.5f == 1.5` are `true`.
- Strings are compared by content.
- Errors are compared by content (message and code).
- Arrays and maps are compared **structurally**, in depth.
- Ranges compare start, end and inclusiveness.
- Functions are compared by identity (the very same function).
- Values of different types (except the numeric types) are not equal: `0 == false` is `false`.

### 3.3 Mutability and references

Assigning an array or a map to another variable **does not copy it**: both variables point to the same value.

```jolt
let a = [1];
let b = a;
b.push(2);
print(a); // [1, 2]
```

For a shallow copy use `.clone()`. Strings are immutable, so they behave like values.


---

## 4. Variables and scope

### 4.1 Declaration

```jolt
var counter;           // mutable, initialized to nil
var name = "Jolt";     // mutable binding, initializer optional
let LIMIT = 10;        // immutable binding evaluated at runtime
const MAX = 10 * 2;    // immutable binding folded at compile time
```

| Keyword | Reassignable | Initializer required | Evaluated |
| --- | --- | --- | --- |
| `var` | yes | no (defaults to `nil`) | runtime |
| `let` | no | yes | runtime |
| `const` | no | yes | compile time |

An optional **type annotation** may follow the name: `let x: int = 5`. See
[Type annotations](#46-type-annotations) below.

`var` declares a mutable variable. `let` declares an immutable binding: assigning another value to it later is rejected (at runtime in the interpreter, at compile time in the bytecode compiler). Use `let` by default for values that never change, and `var` only when reassignment is needed.

`const` is also immutable, but its value must be a compile-time constant expression: literals, arithmetic (`+ - * / %`), unary minus, comparisons and boolean operators over other constants, and references to other top-level `const` names (including forward references, which the parser resolves to a fixed point). The parser folds each `const` initializer to a literal before the program runs, so uses of the name cost nothing at runtime. If an initializer cannot be folded (for example it calls a function or reads a `var`), the parser reports an error instead of silently falling back to runtime evaluation. Nested blocks fold their `const`s eagerly as well.

### 4.2 Block scope

Every block `{ ... }` creates a new scope. Variables are resolved from the current scope up through the parents.

```jolt
let x = 1;
{
  let x = 2;   // shadows the outer x
  print(x);    // 2
}
print(x);      // 1
```

Redeclaring a name in the same block replaces the previous definition.

### 4.3 Assignment

Assignment looks the variable up in the current scope and, if it does not exist, in the parent scopes. It does not create new variables:

```jolt
let x = 1;
{
  x = 2; // modifies the outer x
}
print(x); // 2

y = 3; // error: undefined variable 'y'
```

### 4.4 Function declarations

`fn name(...) { ... }` declarations are hoisted within their block, so they can be called before the line where they appear. This allows recursion and mutual recursion.

`var` and `let` declarations are **not** hoisted: using a variable before its declaration produces an error.

---

### 4.6 Type annotations

Jolt is dynamically typed, but a declaration, a parameter or a return value may
carry an optional type annotation. Annotations are **compile-time metadata**:
the compiler checks them and then erases them, so the bytecode is identical
with or without them and the VM never sees a type.

```jolt
let count: int = 5;
let ratio: float = 0.5f;
let precise: double = 0.5;
let title: string = "Jolt";
let ok: bool = true;
let items: array = [1, 2, 3];
let table: map = {a: 1};
let span: range = 0..10;
let failure: error = error("not found", 404);

fn add(a: int, b: int): int {
  return a + b;
}
```

The accepted type names are the primitives reported by `type()`: `nil`, `bool`,
`int`, `float`, `double`, `string`, `error`, `array`, `map`, `function` and
`range`, plus `any`. A name that is not one of these is a compile error.
`void` is accepted **only as a return type** (see below).

**`void` return type.** A function may be declared `: void` to say it produces
no value. This maps directly to C's `void` in the FFI (PHASE 6.6), so a native
reads like its C declaration: `native fn free(p): void;`. `void` is
compile-time metadata: the function still returns `nil` at run time, so no
opcode and no runtime type is added.

```jolt
fn logLine(msg: string): void {
  print(msg);
}

fn maybeLog(flag: bool): void {
  if (!flag) {
    return;        // bare return: ok
  }
  print("logged");
}
```

`void` is a return type only: it cannot annotate a variable, a parameter or a
struct field. A `void` function cannot `return` a value (use a bare `return;`),
and a `void` expression cannot be used where a value is expected. A binding
initialized from a `void` call stays untyped, like `var x = nil`. Because an
expression body (`fn f(): void => g();`) is an implicit `return`, a `void`
function uses a block body.

**FFI scalar types.** To write a C signature the way C writes it, the FFI
vocabulary (PHASE 6.6) adds pointers and fixed-width/unsigned integers:

| Jolt | C | Notes |
| --- | --- | --- |
| `ptr` | `void *` | Opaque handle. `nil` is NULL. |
| `cstring` | `const char *` | NUL-terminated byte string. `nil` is NULL. Upcasts to `ptr`. |
| `int8` `int16` `int32` `int64` | `int8_t` … `int64_t` | Signed, fixed width. |
| `uint8` `uint16` `uint32` `uint64` | `uint8_t` … `uint64_t` | Unsigned, fixed width. |
| `uint` | `unsigned int` | Unsigned. |
| `size_t` | `size_t` | Unsigned size. |
| `fn(P...): R` | `R (*)(P...)` | C function pointer (callback). Only scalar types in `P`; `void` return allowed. |

These are **compile-time annotations**: the integer family is erased before the
bytecode (the runtime value is still an `int`) and `type()` keeps reporting the
primitive name. Pointers are the exception: `ptr` and `cstring` are a distinct
runtime value (see **Pointer values** below), because a pointer has to be
recognisable at run time to be marshalled safely. The width and signedness of
the integers are recorded for the FFI marshalling to honour, not for the VM.

```jolt
native fn strlen(s: cstring): size_t;
native fn memcmp(a: ptr, b: ptr, n: size_t): int32;
native fn malloc(n: size_t): ptr;
native fn free(p: ptr): void;

let empty: ptr = nil;      // nil is NULL
```

`nil` is assignable to any pointer (NULL), even without a `?`. A `cstring` is
also a `ptr` (an upcast to `void*`); the reverse needs an explicit cast, so it
is not implicit. The integer family (`int`, the fixed-width types, `uint`,
`size_t`) is mutually assignable at compile time and widens to `float`/`double`,
following the same numeric rule as `int`.

A C function's `const char *` **parameter** may also be spelled `string`: a Jolt
string is NUL-terminated and stays alive for the whole call, so its bytes are a
valid `const char *` (`extern fn strlen(s: string): int;`). A `string` is not a
valid *return* type — a C function that returns text returns a `cstring`.

**Pointer values.** A `ptr`/`cstring` is an **opaque handle**, never a machine
address: the program can pass it to a native and read it back, but it can never
dereference an arbitrary location. This keeps Jolt memory-safe and keeps the
allocation visible to the cycle collector. `type()` reports `ptr` for a `void *`
and `cstring` for a C string, and a `cstring` upcasts to `ptr` without losing
its kind. Pointers compare by identity (like functions), not by their contents.

The development engine owns the storage through the handle and exposes it with
these builtins:

| Builtin | Meaning |
| --- | --- |
| `cstring(s)` | Builds a `cstring` from a Jolt string (bytes plus a NUL). |
| `cstr(p)` | Reads a `cstring`/buffer back as a Jolt string. |
| `buffer(n)` | Allocates `n` zero-initialised bytes and returns a `ptr`. |
| `byteLen(p)` | Bytes the handle owns (`cstring` excludes the NUL; `void *` is 0). |
| `loadByte(p, i)` | Byte `0..255` at index `i` (bounds-checked). |
| `storeByte(p, i, b)` | Writes a byte into a buffer (`cstring` is read-only). |
| `intToPtr(n)` / `ptrToInt(p)` | Wraps and reads the opaque integer token a native returns. |
| `free(p)` | Releases the handle's storage; a later use is a runtime error. |

```jolt
let greeting = cstring("hi");
print(cstr(greeting));      // hi
print(byteLen(greeting));   // 2

let buf = buffer(4);
storeByte(buf, 0, 65);      // 'A'
print(loadByte(buf, 0));    // 65
free(buf);
```

The AOT backend (PHASE 7) lowers the same handles to real C pointers, so a
program that only passes them to natives compiles unchanged.

**`extern "C"` blocks.** A C function is declared with `extern` (PHASE 6.6). Two
spellings are accepted and compile to the same thing:

```jolt
extern "C" { fn sqrt(x: float): float; }   // block: explicit ABI
extern fn sqrt(x: float): float;           // single: implicit "C" ABI
```

The single-line form is sugar for a one-entry block; the ABI is implicitly
`"C"`. The block's ABI is a plain string literal and only `"C"` is supported.
Each entry is a bodyless `fn ...;`; the compiler validates the signature against
the FFI type vocabulary (see §4.6) and records it as compile-time metadata on
the proto, so a declaration emits no bytecode of its own. Declarations are
hoisted, so a function may call one declared later. Like `import`, `struct` and
`native fn`, they are module-level metadata: nesting one is a compile error.

Before a proto runs, the engine resolves each declared name through
`Platform::lookupSymbol` and binds it to a marshalling native. The call passes
its arguments and reads its result across the System V AMD64 ABI without
`libffi`: integers, bools and pointers travel in general-purpose registers and
`float`/`double` in SSE registers, and the result is read back at the exact
width the declaration named (so a C `int` is not read as a 64-bit value). A
symbol the host cannot resolve is a clear runtime error, never a wild call.

```jolt
extern "C" {
  fn strlen(s: cstring): size_t;
  fn sqrt(x: double): double;
  fn free(p: ptr): void;
}

print(sqrt(16.0));         // 4
let n: size_t = strlen("hi");
```

A Jolt `string` is a valid `cstring` argument (`strlen("hi")`), and a parameter
may be spelled `string` directly: a Jolt string is NUL-terminated and stays alive
for the whole call, so its bytes are a valid `const char *`. A `string` is not a
valid *return* type; a C function returns a `cstring`. A `ptr` argument accepts
`nil` (NULL) and an owned `cstring`/buffer, whose bytes are the address the C
callee receives.

**Structs by value.** A `struct` (see §4.7) can also cross the C boundary, both
as a parameter and as a return value. A struct is a map at runtime, so the
compiler records its field layout from the declaration and the marshalling
moves the bytes across the ABI in registers:

```jolt
struct DivResult {
  quot: int32;
  rem: int32;
}
extern "C" {
  fn div(numerator: int, denominator: int): DivResult;
}

let d = div(17, 5);
print(d.quot);   // 3
print(d.rem);    // 2
```

The ABI classifies a small struct as up to two **eightbytes**, each INTEGER (a
general-purpose register) or SSE (an xmm register); a `{int; int}` struct comes
back wholly in `rax`, a `{double; double}` in `xmm0`/`xmm1`, and a mixed
`{int; double}` in `rax` + `xmm0`. A struct argument is flattened to the same
eightbytes and passed in the registers the ABI assigns it, so the callee reads
back the exact bytes of the struct.

Only structs of **16 bytes or fewer** are supported (the register case). A
larger struct is passed in memory, a shape the register marshalling cannot
express, so the declaration is rejected at compile time with a clear message
instead of a wrong call. A struct field must be an FFI scalar type (an integer,
`float`/`double`, `ptr`/`cstring` or `bool`); a nested struct field is rejected
for the same reason.

Structs by value are **System V AMD64 only**. The register classification above
is the System V ABI's; the Microsoft x64 ABI (Windows) passes a struct larger
than 8 bytes through a hidden pointer instead, a shape the register thunks
cannot express. On such a target the declaration is rejected at compile time
with `a struct cannot cross the C boundary by value on this target`. Scalar
arguments and results stay portable: the call goes through a typed function
pointer, so the host compiler applies whichever ABI it targets.

**Callbacks.** A parameter typed `fn(paramTypes): returnType` is a C function
pointer. A Jolt function (or a closure) passed for it is callable from C, and a
callback signature only carries scalar FFI types:

```jolt
extern "C" {
  fn qsort(base: ptr, nmemb: size_t, size: size_t, compar: fn(ptr, ptr): int): void;
  fn memcmp(a: ptr, b: ptr, n: size_t): int;
}

fn ascending(a: ptr, b: ptr): int {
  return memcmp(a, b, 1);
}

let data = buffer(4);
storeByte(data, 0, 50);
storeByte(data, 1, 10);
storeByte(data, 2, 90);
storeByte(data, 3, 30);
qsort(data, 4, 1, ascending);
```

In the development engine the callback is a `libffi` closure: the trampoline
re-enters the VM with the C arguments boxed as Jolt values and writes the Jolt
result back in the declared C type. The callback runs while the C function is on
the stack, so the trampoline is alive for the whole call; storing the function
pointer for a later call is out of scope for this phase. `nil` is the NULL
function pointer.

A callback can only be a **parameter**; a C function that returns a function
pointer is not supported. An error raised inside a callback cannot unwind
through the C frame, so the extern call captures it and re-raises it once C
returns: `pcall` catches it like any other runtime error. The AOT backend
(PHASE 7) will lower callbacks to compile-time stubs and never links `libffi`.

**Local inference.** With no annotation, the compiler infers the type from the
initializer when it is a literal or another typed binding, and then checks
later assignments against it:

```jolt
var n = 0;
n = 1;        // ok
n = "text";   // compile error: cannot assign 'string' to 'int' in 'n'
```

A `nil` initializer does **not** pin a type (`var x = nil;` stays untyped), and
an expression whose type is not evident is left unchecked, so inference never
rejects a program that would have run.

**Widening.** Numeric widening follows Java: `int` → `float` → `double`. The
reverse is not implicit; use an explicit conversion.

```jolt
let a: float = 3;      // ok: int widens to float
let b: double = 3;     // ok: int widens to double
let c: double = 1.5f;  // ok: float widens to double
let d: float = 1.5;    // compile error: double does not narrow to float
let e: int = 1.5;      // compile error
```

**Nullability.** A trailing `?` marks a type that also accepts `nil`. Without
it, `nil` is rejected.

```jolt
let maybe: string? = nil;   // ok
let name: string = nil;     // compile error
```

**Null-safe expressions.** Three operators work on nullable values:

| Operator | Form | Result |
| --- | --- | --- |
| Null coalescing | `a ?? b` | `a` unless it is `nil`, in which case `b` |
| Safe access | `a?.b` | `nil` when `a` is `nil`, otherwise `a.b` |
| Non-null assertion | `a!!` | `a`, or a runtime error when `a` is `nil` |

```jolt
let a: string? = nil;
print(a ?? "default");   // "default"
let b: string? = "hi";
print(b ?? "default");   // "hi"

let m: map? = nil;
print(m?.length);        // nil
let m2: map? = {k: 1};
print(m2?.k);            // 1

let s: string? = "ok";
print(s!!);              // "ok"
let t: string? = nil;
print(t!!);              // runtime error
```

Only `nil` triggers the `??` fallback and the `?.` guard: `0`, `false` and `""`
are kept, so this is not truthiness. `??` short-circuits, so its right-hand side
is evaluated only when the left is `nil`. `a?.b` is read-only and cannot be an
assignment target.

These operators add no opcode: `??` and `?.` lower to `EQ`/`JMP_IF_*`/`GETTABLE`
and `!!` to a call to a hidden helper, so the bytecode budget is unchanged
(AGENT.md §3).

**Union types.** A type may be a union of alternatives, written with `|`. A
value of the union's type may be any one of the alternatives, and the
annotation is checked like every other type and then erased, so it adds no
opcode.

```jolt
let value: int | string = 1;
value = "text";                 // the other alternative is accepted too
let result: int | error = try parse(input);
```

`nil` is not written as a union member: `int | nil` is the same type as `int?`,
so `nil` folds into the nullability of the whole annotation. That keeps the two
spellings interchangeable, and a `?` alone means exactly the same thing.

```jolt
let a: int | nil = nil;     // same type as `int?`
let b: int | string | nil = nil;
```

Assignment follows the same rule as a single type, applied to each alternative:

- A value fits the union when it fits **at least one** alternative. `int`
  widens to `float`, so `let x: float | string = 1;` is accepted.
- A union source fits the target only when **every** member fits, so a union is
  assignable to a wider union that contains all of its members.
- `nil` needs the union to be nullable (written `| nil` or with a `?`).

```jolt
let x: int | string = 1;                 // ok
let y: float | string = 1;               // ok: int widens to float
let wide: int | string | bool = x;       // ok: every member fits
let bad: int | string = true;            // compile error: bool fits neither
let narrow: int = x;                     // compile error: a union is not one type
```

A union is a compile-time-only construct with **no C representation**, so it is
rejected wherever a type must cross the FFI boundary: `extern "C"` signatures,
`native fn` signatures and struct fields (a struct can be passed by value).
`void` is a return-only type, so it cannot be a union member either.

`catch` produces a union: `e catch d` is either `e` or `d`, so its inferred
type is the union of the two. That is what lets a fallback of a different type
type-check without a cast.

```jolt
fn parse(s: string): int | error { ... }
let recovered = parse(input) catch 0;    // int
```

---

### 4.7 `struct`

A `struct` is a named, data-only shape: a fixed set of fields with optional
type annotations. It has no methods and no inheritance (and there never will
be — maps plus closures replace classes). Like `import`, a `struct` is
**module-level metadata**: it may only be declared at the top level of a file,
and the declaration itself emits no bytecode.

```jolt
struct Point {
  x: int;
  y: int;
}

let p = Point { x: 1, y: 2 };
print(p.x, p.y);   // 1 2
```

A struct value **is a map at runtime**, so everything that works on a map works
on a struct: member access, `in`, `len`, `for ... in`, indexing, and structural
equality. `Point { x: 1, y: 2 }` lowers to the same `NEWTABLE`/`SETTABLE`
sequence as `{x: 1, y: 2}`, so `struct` adds no opcode and the register VM and
the future AOT backend never see a struct type. `type(p)` reports `"map"`.

**Field order does not matter** in a literal, but every declared field must be
present exactly once. The compiler rejects an unknown field (a typo), a missing
field (a key the declaration promises), a duplicate field and a duplicate
struct name. A field annotation is optional; when present, the value is checked
with the same rules as every other annotation (including `int` → `float` →
`double` widening).

```jolt
struct Pair { first; second; }          // untyped fields
let a = Pair { first: 1, second: "x" }; // ok: any value fits

struct Vec { x: float; y: float; }
let v = Vec { x: 1, y: 2.5 };           // ok: int widens to float

let bad1 = Vec { x: 1 };                // error: missing field 'y'
let bad2 = Vec { x: 1, z: 2 };          // error: unknown field 'z'
let bad3 = Vec { x: 1, y: "no" };       // error: cannot assign 'string' to 'float'
```

A struct name is a valid type annotation, and — like a hoisted `fn` — a struct
declared later in the file is visible earlier:

```jolt
fn origin(): Point {
  return Point { x: 0, y: 0 };
}

struct Point { x: int; y: int; }   // visible to `origin`, declared above
```

Because `struct` is a reserved word, the sequence `struct Name` is always a
declaration, so `Name { ... }` is unambiguously a struct literal whenever
`Name` is a struct declared in the file (the literal form takes precedence over
reading `Name` as a variable).

A struct is local to its file: its name is **not** exported by `import`. A
module that builds a struct value exports the map, and the importing file sees
a plain map (it cannot write `Point { ... }` unless it declares its own
`struct Point`).

---

## 5. Operators

### 5.1 Precedence

From highest to lowest precedence:

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 | `()` `[]` `.` `?.` `!!` | left |
| 2 | `**` | right |
| 3 | `!` `-` `+` `~` (unary) `try` (prefix) | right |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `..` `..=` | left |
| 7 | `<<` `>>` | left |
| 8 | `<` `<=` `>` `>=` `in` | left |
| 9 | `==` `!=` | left |
| 10 | `&` | left |
| 11 | `^` | left |
| 12 | `\|` | left |
| 13 | `&&` | left |
| 14 | `\|\|` | left |
| 15 | `??` | left |
| 16 | `catch` | left |
| 17 | `?:` | right |
| 18 | `=` `+=` `-=` `*=` `/=` `%=` `**=` `&&=` `\|\|=` `??=` | right |

Notes:

- `**` is right-associative: `2 ** 3 ** 2` is `2 ** (3 ** 2) = 512`.
- Unary minus has lower precedence than `**`: `-2 ** 2` is `-(2 ** 2) = -4`.
- Ranges have higher precedence than shifts and comparisons: `2 in 1..5` is equivalent to `2 in (1..5)`.
- Comparisons do not chain: `1 < 2 < 3` tries to compare a boolean with an integer and produces an error.
- `??` binds looser than `||`: `a || b ?? c` groups as `(a || b) ?? c`.
- `catch` binds looser than `??` and is left-associative: `a() catch b() catch c` recovers at the first failure (see §13.3).
- `!!` is postfix and binds tightest, next to member access: `a?.b!!` asserts on the result of the access.
- Because `!!` is a single token, prefix double negation is not available; write `!(!x)` where a double negation is needed.

### 5.2 Arithmetic

| Operator | Description |
| --- | --- |
| `+` | Addition of numbers, concatenation of strings or arrays. |
| `-` | Subtraction. |
| `*` | Multiplication. |
| `/` | Division. |
| `%` | Remainder. |
| `**` | Power. |

Rules:

- If both operands are `int`, `+ - * / %` produce `int`.
- Integer division truncates toward zero: `7 / 2 == 3`, `-7 / 2 == -3`.
- The integer remainder takes the sign of the dividend: `-7 % 3 == -1`.
- If an operand is `float`, the result is `float`; if an operand is `double`, the result is `double`. A `float` with a `double` promotes to `double`.
- Dividing or taking the remainder with a zero divisor is a runtime error, for floating-point types too.
- `**` returns `int` if base and exponent are integers and the exponent is not negative; otherwise it uses `std::pow` and returns `double`.
- `int` overflow wraps around (two's complement).
- `+` concatenates two strings and also two arrays (creating a new array).

```jolt
assertEq(7 / 2, 3);
assertEq(7 / 2.0, 3.5);
assertEq(type(1.5f + 1.5f), "float");
assertEq(type(1.5f + 1.5), "double");
assertEq(2 ** 10, 1024);
assertEq("a" + "b", "ab");
assertEq([1] + [2], [1, 2]);
```

### 5.3 Comparison

`<`, `<=`, `>`, `>=` work with:

- two numbers (`int`, `float` or `double`, mixed);
- two strings (lexicographic order by UTF-8 bytes);
- two booleans (`false < true`).

Comparing incompatible types is an error.

### 5.4 Equality

`==` and `!=` use the structural equality described in section 3.2.

### 5.5 Logical

| Operator | Description |
| --- | --- |
| `!` | Logical negation; always returns `bool`. |
| `&&` | Logical AND with short-circuiting. |
| `\|\|` | Logical OR with short-circuiting. |

`&&` and `||` **return one of their operands**, not a boolean:

```jolt
assertEq(nil || "default", "default");
assertEq("value" || "default", "value");
assertEq(false && 123, false);
assertEq(true && 123, 123);
assertEq(nil && 123, nil);
```

The right operand is only evaluated if needed.

### 5.6 Bitwise

| Operator | Description |
| --- | --- |
| `&` | Bitwise AND. |
| `\|` | Bitwise OR. |
| `^` | Bitwise XOR. |
| `~` | One's complement (unary). |
| `<<` | Left shift. |
| `>>` | Arithmetic (signed) right shift. |

They only accept `int`. Shifting requires a count between 0 and 63.

```jolt
assertEq(6 & 3, 2);
assertEq(6 | 3, 7);
assertEq(1 << 4, 16);
assertEq(256 >> 3, 32);
assertEq(~0, -1);
```

### 5.7 Membership

The `in` operator tests membership:

| Right side | Behavior |
| --- | --- |
| array | Structural equality with some element. |
| map | The left side must be a string; checks whether the key exists. |
| string | The left side must be a string; checks whether it is a substring. |
| range | The left side must be an integer; checks whether it lies within the range. |

```jolt
assertEq(2 in [1, 2, 3], true);
assertEq("a" in {a: 1}, true);
assertEq("ell" in "hello", true);
assertEq(5 in 1..10, true);
```

### 5.8 Ternary

```jolt
let label = age >= 18 ? "adult" : "minor";
```

Only the chosen branch is evaluated. It can be nested:

```jolt
let size = n > 100 ? "large" : n > 10 ? "medium" : "small";
```

### 5.8.1 Null-safe operators

| Operator | Form | Result |
| --- | --- | --- |
| Null coalescing | `a ?? b` | `a` unless it is `nil`, in which case `b` |
| Safe access | `a?.b` | `nil` when `a` is `nil`, otherwise `a.b` |
| Non-null assertion | `a!!` | `a`, or a runtime error when `a` is `nil` |

```jolt
let name: string? = nil;
print(name ?? "anonymous");   // "anonymous"

let m: map? = {k: 1};
print(m?.k);                  // 1
let empty: map? = nil;
print(empty?.k);              // nil

let s: string? = "ok";
print(s!!);                   // "ok"
```

Only `nil` triggers the fallback and the guard, so `0`, `false` and `""` are
kept: these operators are not truthiness. `??` short-circuits (its right-hand
side is evaluated only when the left is `nil`), and `a?.b` is read-only. See
[4.6 Type annotations](#46-type-annotations) for the full rules.

### 5.9 Assignment

Assignment is an expression and returns the assigned value:

```jolt
var x = 0;
let y = (x = 5);
assertEq(x, 5);
assertEq(y, 5);

var a = 0;
var b = 0;
let c = (a = b = 3); // right-associative
```

Compound operators:

```jolt
x += 1;
x -= 1;
x *= 2;
x /= 2;
x %= 3;
x **= 2;
```

They also work on indexes and members:

```jolt
let data = [1, 2];
data[0] += 10;
data[0] *= 2;

let person = {age: 36};
person.age += 1;
person["age"] *= 2;
```

The logical operators have a compound form too. Each one short-circuits exactly
like its binary counterpart and writes the target back only when it does not,
so the right-hand side is evaluated at most once:

```jolt
var name = nil;
name ??= "anon";  // assigns only when `name` is nil

var count = 0;    // 0 is truthy in Jolt
count ||= 10;     // not assigned: `count` is already truthy

var flag = true;
flag &&= check(); // calls `check()` and assigns its result
```

- `a ??= b` is `a = a ?? b`: assign `b` only when `a` is nil (`false` and `0`
  are kept).
- `a ||= b` is `a = a || b`: assign `b` only when `a` is falsy (`false` or
  `nil`).
- `a &&= b` is `a = a && b`: assign `b` only when `a` is truthy.

Like the other compound operators, they work on indexes and members
(`data[0] ??= 1`, `obj.name ||= "x"`) and yield the target's final value. They
read the target once and lower to the existing opcodes (`EQ`/`NEQ`,
`JMP_IF_FALSE`/`JMP_IF_TRUE`, `MOVE`), so no opcode is added.

There are no `++` or `--`; use `+= 1` and `-= 1`.

---

## 6. Statements

### 6.1 Expression statements

An expression followed by `;` is evaluated and its result is discarded:

```jolt
1 + 2;
print("hello");
```

### 6.2 Blocks

```jolt
{
  let temporary = 1;
  print(temporary);
}
```

Blocks create a new scope. An empty `;` statement is also valid.

### 6.3 `if` / `else`

The condition goes in parentheses. The body is usually a block:

```jolt
if (age >= 18) {
  print("adult");
} else if (age >= 13) {
  print("teenager");
} else {
  print("minor");
}
```

A body may also be a single statement without braces. It is wrapped in a
block, so its bytecode is the same as the brace form, it is a scope of its own
(a `let` in it does not leak) and a `defer` in it runs when the body exits:

```jolt
if (x > 0) print("positive"); else print("non-positive");

if (n >= 90) return "A";
else if (n >= 70) return "B";
else return "C";
```

The dangling `else` binds to the nearest `if`, like C. The same brace-less form
works for `while`, `for` and `for-in` (§6.4, §6.5, §6.6).

### 6.4 `while` and `do`/`while`

`while` tests its condition before each iteration:

```jolt
var i = 0;
while (i < 5) {
  print(i);
  i += 1;
}
```

`do { ... } while (cond);` is the post-test form: the body runs once before the
condition is checked, so it always runs at least once. The trailing `;` after
the condition is optional.

```jolt
var n = 0;
do {
  n += 1;
} while (n < 3);   // runs three times

do { n += 1; } while (false);   // runs once
```

In both loops `break` leaves the loop, `continue` re-tests the condition (in a
`do`/`while` it jumps to the condition, not to the top of the body), and a
`defer` in the body runs on every exit path.

### 6.5 Classic `for`

```jolt
for (var i = 0; i < 5; i += 1) {
  print(i);
}
```

Any of the three clauses may be omitted:

```jolt
for (;;) {
  break;
}
```

The variable declared in the initializer lives in a scope of its own for the loop. The body additionally creates a new scope on every iteration.

### 6.6 `for ... in`

Syntax, with or without parentheses:

```jolt
for value in [10, 20, 30] {
  print(value);
}

for (value in [10, 20, 30]) {
  print(value);
}
```

The two spellings are equivalent. The parenthesized form is told apart from a
classic `for` header by the lookahead `( name in`, so `for (var i = 0; ...)` is
unaffected.

You can iterate over:

- **arrays**: in order, by value;
- **strings**: by code point, each one as a one-character string;
- **maps**: by key, in insertion order;
- **ranges**: ascending or descending depending on the bounds.

```jolt
for c in "abc" {
  print(c.upper());
}

for key in {a: 1, b: 2} {
  print(key);
}

for n in 1..=3 {
  print(n);
}
```

The iterable is evaluated only once. For arrays and maps, a snapshot of their elements or keys is taken when the loop starts. Ranges are iterated lazily: no intermediate array is created, so `for i in 0..1000000000 { ... }` runs in constant memory. The iteration variable gets a fresh binding on every turn, so closures created inside the body capture different values.

### 6.7 `break` and `continue`

```jolt
for n in 0..10 {
  if (n % 2 == 0) {
    continue;
  }
  if (n > 7) {
    break;
  }
  print(n);
}
```

They only affect the innermost loop. They cannot cross a function boundary: a `break` inside a function that is not in a loop is an error, even if the function is called from inside a loop.

### 6.8 `return`

```jolt
fn maximum(a, b) {
  if (a > b) {
    return a;
  }
  return b;
}
```

`return;` returns `nil`. Using `return` outside a function is an error.

### 6.9 Function declarations

```jolt
fn name(parameter1, parameter2) {
  // body
  return value;
}
```

They can be declared in the global scope or inside a block. They are hoisted within their block. See section 7.

### 6.10 `switch`

```jolt
switch (value) {
  case 1: {
    print("one");
  }
  case 2: case 3: {
    print("two or three");
  }
  case 10..=20: {
    print("in the teens");
  }
  default: {
    print("something else");
  }
}
```

A `switch` evaluates its subject **exactly once** and compares it against the case labels in order. A label is either a **literal** (`nil`, `true`, `false`, a number or a string, optionally signed as in `-1`), or an **integer range** (`10..20`, `10..=20`). A literal matches by equality; a range matches by membership, exactly like `value in 10..=20`.

The first matching case runs and then control leaves the switch. **There is no fallthrough**: unlike C or Java, the body of the next case does not run after a match, so a forgotten `break` cannot cause a bug. Several labels may share one body (`case 2: case 3:`), and a `default` clause (at most one) runs only when nothing matched; it is optional, so a switch with no match simply does nothing.

The subject may be any expression. Each case body is a block with its own scope, so a `let` declared there stays local. `break` and `continue` inside a switch bind to the enclosing loop (the switch is not a loop), and `return` works as usual inside a case. Because the bytecode is built from the existing `EQ`, `IN` and `JMP` instructions, `switch` adds no opcode.

```jolt
fn classify(n) {
  switch (n) {
    case 0: { return "zero"; }
    case 1..=9: { return "small"; }
    default: { return "big"; }
  }
}
```

### 6.11 `defer`

```jolt
fn readConfig(path) {
  let file = open(path);
  defer {
    file.close();
  }
  // ... use `file` ...
  return parse(file.read());
}
```

`defer` registers a block to run when the **enclosing scope exits**. The block does not run where it is written: the compiler inlines it at every exit from that scope, so it runs whether the scope is left normally (falling off the end), through `return`, or through the `break`/`continue` that leave it. This makes `defer` the idiomatic way to release a resource, mirroring Go's `defer`.

Several deferred blocks in the same scope run in **LIFO order**: the last one declared runs first. A `defer` inside a loop body runs once per iteration, and once more on the iteration that leaves the loop early. A standalone block is a scope too, so its deferred blocks run when the block ends.

The body must be a `{ ... }` block, and `defer` may only appear inside a function body (at the program top level there is no scope exit to attach it to). A deferred block cannot contain a `return`, `break` or `continue` that would leave the block itself: that would re-enter the teardown that is running it. A loop or a nested function declared *inside* the deferred block is a fresh boundary, so its own `break`/`return` is allowed.

Because the deferred blocks are inlined at the scope exits, `defer` adds no opcode and no VM support.

```jolt
fn lifo() {
  var log = [];
  defer { log.push("first"); }
  defer { log.push("second"); }
  log.push("body");
  return log; // ["body", "second", "first"]
}
```

---

## 7. Functions and closures

### 7.1 Declarations and calls

```jolt
fn add(a, b) {
  return a + b;
}

print(add(2, 3)); // 5
```

A function whose body is a single expression can use the short form with `=>`. It needs a terminating `;`:

```jolt
fn square(x) => x * x;

print(square(4)); // 16
```

The number of arguments must match the number of parameters exactly, unless the
last parameter is **variadic**. A parameter written `rest...` collects every
extra argument into an array; the `...` follows the optional type annotation
(`rest...` or `parts: int...`), which describes the elements, not the array.
A variadic parameter must be the last one, and a function may have at most one.

```jolt
fn sum(first, rest...) {
  var total = first;
  for x in rest {
    total += x;
  }
  return total;
}

print(sum(1));          // 1
print(sum(1, 2, 3));    // 6
print(sum(1, 2, 3, 4)); // 10
```

`rest` is an ordinary array inside the body, so `len(rest)`, indexing and
`for ... in rest` all work. There are no default parameters.

Parameters are local. Arrays and maps are passed by reference; reassigning the parameter inside the function does not change the caller's variable.

```jolt
fn append(arr) {
  arr.push(1); // visible outside
}

fn replace(arr) {
  arr = [9];   // only changes the local parameter
}
```

### 7.2 Lambdas

`fn` always declares a named function. An anonymous function is written as a **lambda** with `=>`, and a function is a value:
```jolt
let double = x => {
  return x * 2;
};

let triple = x => x * 3;

let anonymous = () => {
  print("hello");
};
```

A single parameter needs no parentheses (`x => ...`); zero or several do (`() => ...`, `(a, b) => ...`). The body is either an expression, which becomes the return value, or a `{ ... }` block. Because a `{` after `=>` starts a block, a lambda that returns a map literal wraps it in parentheses: `x => ({value: x})`.

Lambdas can be passed as arguments:

```jolt
let squares = [1, 2, 3].map(x => x * x);
```

### 7.3 Recursive lambdas

A lambda bound to a variable can call itself through that variable, because the name is resolved when the lambda is called:

```jolt
let power = (base, exp) => exp == 0 ? 1 : base * power(base, exp - 1);

print(power(2, 10)); // 1024
```

For anything longer than a one-liner, prefer a declared function (`fn power(base, exp) { ... }`), which is also hoisted.

### 7.4 Closures

Functions capture the environment where they are created and keep it alive:

```jolt
fn makeCounter() {
  var n = 0;
  return () => {
    n += 1;
    return n;
  };
}

let counter = makeCounter();
print(counter()); // 1
print(counter()); // 2
```

Capture is by reference: if the captured variable changes after the function is created, the function sees the new value.

```jolt
fn example() {
  var state = 1;
  let read = () => state;
  state = 2;
  return read;
}

print(example()()); // 2
```

### 7.5 Recursion and depth

Recursion works both in declared functions and in named lambdas. The interpreter limits the call depth to **2000**; exceeding it produces a runtime error.

```jolt
fn factorial(n) {
  if (n <= 1) {
    return 1;
  }
  return n * factorial(n - 1);
}
```

### 7.6 `native fn`

A `native fn` declares a function whose body is **not** written in Jolt: the
name binds to a function the host (the VM or the future AOT runtime) already
provides, such as a standard-library builtin. The declaration is bodyless and
carries only a signature, so it emits no bytecode and adds no opcode — the name
is bound with a plain `GETGLOBAL` of the host function.

```jolt
native fn sqrt(x: double): double;

print(sqrt(9.0));   // 3
print(sqrt(16));    // 4  (int widens to double)
```

The signature is checked at compile time: a call must pass the exact number of
arguments, and each argument must be assignable to its parameter type. A
trailing `rest...` parameter makes the native variadic: the call must pass the
fixed parameters plus zero or more extras, and each extra is checked against the
variadic parameter's element type. The return type is used for inference, so
`let y: double = sqrt(4.0)` is checked like any other annotated binding. An
unannotated parameter (or `any`) turns the check off for that argument, which is
how a builtin with a loose signature is declared:

```jolt
native fn str(v: any): string;
native fn min(first: double, rest: double...): double;   // variadic
native fn len(v);                 // fully untyped parameter
native fn free(p: any): void;     // no return value (maps to C's void)

assertEq(str(42), "42");
assertEq(len([1, 2, 3]), 3);
```

Like `struct`, a `native fn` is **module-level metadata**: it may only be
declared at the top level of a file. Its parameter and return types must be
type names the compiler knows (or `any`). A `void` return type is allowed and
maps directly to C's `void` in the FFI (PHASE 6.6).

A `native fn` binds the name at run time to whatever the host registered under
that name. If the host provides nothing, using the name reports
`undefined variable '<name>'`, so a typo or a missing host function is a clear
runtime error rather than a silent failure. Builtins such as `print`, `len` and
`str` are host functions, so a `native fn` declaration is the typed way to
expose one to the checker.

---

### 7.7 `@MustUse`

A function may be declared `@MustUse`: the caller must do something with the
return value. When a call to it is used as a statement, so its result is thrown
away, the compiler reports a **warning** (never an error), naming the function:

```jolt
@MustUse
fn parse(input: string): int | error { ... }

parse(text);          // warning: the result of '@MustUse' function 'parse' is discarded
let n = parse(text);  // ok: the result is used
```

The annotation is compile-time metadata: it emits no bytecode, and the bytecode
of a program is identical with and without it. It is accepted on `fn` and on
`native fn` declarations, at the top level and inside a function body.

Discarding the result is sometimes intentional. Assigning it to `_` is the
explicit acknowledgement, and it never warns:

```jolt
_ = parse(text);      // ok: the discard is deliberate
```

A warning is a diagnostic, not a rejection. The program still compiles and
runs, and the exit code is unchanged, so a `@MustUse` annotation can never turn
a program that would have run into a failure (AGENT.md §4.6). This mirrors Rust's
`#[must_use]` and Kotlin's `@CheckReturnValue`, and follows Go's
`errcheck` convention that a deliberate discard should be visible.

The check is precise about **which** function is called. A local or upvalue that
shadows a `@MustUse` name is a different function, so it carries its own flag and
does not inherit the annotation:

```jolt
@MustUse
fn parse(input: string): int | error { ... }

fn use() {
  fn parse(input) => 0;   // a different function
  parse("x");             // no warning: this `parse` is not `@MustUse`
}
```

Only `@MustUse` is a known attribute. An unknown attribute is a compile error
rather than being silently ignored, so a typo cannot drop the check; and an
attribute must precede a `fn` or `native fn` declaration. Attribute names are
`PascalCase` (`@MustUse`), like the future `@Dynamic`, so they are never
confused with the `camelCase` of ordinary identifiers.

---

## 8. Arrays

### 8.1 Literals and indexes

```jolt
let empty = [];
let numbers = [1, 2, 3,];
```

A trailing comma is allowed. Indexes are integers; negative ones count from the end:

```jolt
assertEq(numbers[0], 1);
assertEq(numbers[-1], 3);
```

An out-of-range index is a runtime error. Arrays are mutable:

```jolt
numbers[0] = 10;
numbers.push(4);
```

A slice `a[start:end:step]` returns a new array with the half-open range
`[start, end)`, taking every `step`-th element. Every part may be omitted:
`a[:]`, `a[2:]`, `a[:3]`, `a[::2]` and `a[::-1]` all parse. Negative bounds
count from the end and are clamped to the size; a negative `step` walks
backwards, and a `step` of 1 (the default) is the plain range. The bounds
follow Python's rule: with a negative step the defaults are the end and
before the start, so `a[::-1]` reverses. A slice is a copy, so mutating it
never touches the source, and it is not an assignment target.

```jolt
let a = [10, 20, 30, 40, 50];
assertEq(a[1:3], [20, 30]);
assertEq(a[2:], [30, 40, 50]);
assertEq(a[:2], [10, 20]);
assertEq(a[-2:], [40, 50]);
assertEq(a[:], a);
assertEq(a[::2], [10, 30, 50]);
assertEq(a[::-1], [50, 40, 30, 20, 10]);
assertEq(a[3:0:-1], [40, 30, 20]);
```

The same syntax works on strings, slicing by UTF-8 character
(`"hello"[::-1]` is `"olleh"`). A zero step is a runtime error.

### 8.2 Members

`array.length` returns the number of elements. `len(array)` does the same.

### 8.3 Array methods

| Method | Description |
| --- | --- |
| `push(value, ...)` | Appends one or more values at the end and returns the array. |
| `pop()` | Removes and returns the last element; `nil` if empty. |
| `shift()` | Removes and returns the first element; `nil` if empty. |
| `unshift(value)` | Inserts a value at the start and returns the array. |
| `insert(index, value)` | Inserts at `index` (negatives allowed) and returns the array. |
| `removeAt(index)` | Removes and returns the element at `index`. |
| `slice(start?, end?)` | Returns a new array with the range `[start, end)`. |
| `concat(other)` | Returns a new array with both arrays. |
| `join(separator?)` | Joins the elements into a string. |
| `sort(comparator?)` | Sorts the array in place and returns it. |
| `sorted(comparator?)` | Returns a sorted copy. |
| `reversed()` | Returns a reversed copy. |
| `contains(value)` | Checks structural equality. |
| `indexOf(value)` | Returns the first index or `-1`. |
| `map(f)` | Returns an array with the results of `f(element)`. |
| `filter(f)` | Returns the elements for which `f` is truthy. |
| `reduce(f, initial?)` | Accumulates with `f(accumulator, element)`. |
| `each(f)` | Calls `f(element)` for each element and returns the array. |
| `find(f)` | Returns the first element that satisfies `f`, or `nil`. |
| `any(f)` | `true` if any element satisfies `f`. |
| `all(f)` | `true` if all elements satisfy `f`. |
| `sum()` | Sum of an array of numbers. |
| `clear()` | Empties the array and returns it. |
| `clone()` | Shallow copy. |

Details:

- `slice` clamps the indexes to the size of the array and accepts negatives. If `end < start`, it returns an empty array.
- `sort` and `sorted` use ascending order by default. The comparator receives `(a, b)` and must return `true` if `a` should come before `b`; the sort is stable.
- `reduce` without an initial value uses the first element as the accumulator and fails if the array is empty.
- `sum` requires all elements to be numbers; it returns `int` if all are integers and `double` otherwise.
- `clone` is shallow: nested arrays and maps are shared.

```jolt
let nums = [3, 1, 2];
assertEq(nums.sorted(), [1, 2, 3]);
assertEq(nums.sorted((a, b) => a > b), [3, 2, 1]);
assertEq(nums.map(n => n * 2), [6, 2, 4]);
assertEq(nums.reduce((a, n) => a + n, 0), 6);
```

---

## 9. Strings

Strings are immutable and indexed by code point:

```jolt
let s = "áé";
assertEq(s.length, 2);
assertEq(s[0], "á");
assertEq(s[-1], "é");
```

An out-of-range index is an error. You cannot assign to a string index.

Strings also support slicing by code point, with the same rules as arrays
(bounds and step omitted, negatives counted from the end, clamped, negative
step reverses):

```jolt
let s = "hello";
assertEq(s[1:3], "el");
assertEq(s[2:], "llo");
assertEq(s[:-1], "hell");
assertEq(s[::-1], "olleh");
```

### 9.1 String methods

| Method | Description |
| --- | --- |
| `upper()` | Converts ASCII letters to uppercase. |
| `lower()` | Converts ASCII letters to lowercase. |
| `trim()` | Removes ASCII whitespace at the start and end. |
| `trimStart()` | Removes ASCII whitespace at the start. |
| `trimEnd()` | Removes ASCII whitespace at the end. |
| `split(separator?)` | With no argument splits on whitespace; with `""` splits into characters. |
| `lines()` | Splits on `\n` line breaks. |
| `replace(search, replacement)` | Replaces all non-overlapping occurrences. |
| `replaceFirst(search, replacement)` | Replaces the first occurrence. |
| `startsWith(prefix)` | Checks the prefix. |
| `endsWith(suffix)` | Checks the suffix. |
| `contains(text)` | Checks whether it contains the substring. |
| `indexOf(text)` | Code-point index of the first occurrence, or `-1`. |
| `substr(start, length?)` | Substring from `start` (negatives allowed). |
| `chars()` | Array of one-character strings. |
| `repeat(n)` | Repeats the string `n` times. |
| `reversed()` | Reverses the code points. |
| `padStart(width, fill?)` | Pads on the left up to `width`. |
| `padEnd(width, fill?)` | Pads on the right up to `width`. |

Details:

- `upper` and `lower` only transform `A-Z` and `a-z`; the rest of Unicode is unchanged.
- `trim*` considers ASCII whitespace (`std::isspace`), not every Unicode space.
- `lines` does not add an empty trailing line if the string ends with `\n`.
- `indexOf` searches bytes but returns the index in code points.
- `substr` counts code points; a negative `length` produces an empty string.
- `padStart` and `padEnd` pad by code points and trim the fill to the exact width.
- `repeat` has a 64 MB limit on the result.

```jolt
assertEq("  hello  ".trim(), "hello");
assertEq("a,b,c".split(","), ["a", "b", "c"]);
assertEq("abc".substr(1, 2), "bc");
assertEq("5".padStart(3, "0"), "005");
```

---

## 10. Maps

### 10.1 Literals

Keys in a literal can be identifiers, string literals, scalar literals (`int`, `float`, `double`, `true`, `false`, `nil`) or computed expressions in brackets:

```jolt
let person = {
  name: "Ada",
  age: 36,
  "key with spaces": true,
};

// Scalar literals are canonicalized to their string form.
let coded = {1: "one", 1.5: "one and a half", true: "yes", nil: "nothing"};
assertEq(coded["1"], "one");
assertEq(coded["1.5"], "one and a half");

// Computed keys use the [expr] syntax and are evaluated at runtime.
let n = 2;
let dyn = {[n * 10]: "twenty", ["a" + str(n)]: "a2"};
assertEq(dyn["20"], "twenty");
```

All map keys are strings internally: scalar and computed keys are normalized with the value's canonical string form (for example `1` becomes `"1"` and `1.5` becomes `"1.5"`), so lookups always use strings. If a key is repeated, the last value wins, but the position of its first appearance is kept.

### 10.2 Access and assignment

```jolt
assertEq(person.name, "Ada");
assertEq(person["age"], 36);

person.age = 37;
person["city"] = "London";
```

Accessing a missing key returns `nil` (both with `.` and with `[]`). Keys must be strings; using another type as an index is an error.

Maps preserve insertion order. Reassigning an existing key does not move it.

### 10.3 Members and collisions with methods

`map.length` returns the number of keys, unless a `"length"` key exists, in which case its value is returned. Likewise, a key named like a method (for example `"keys"`) takes priority over the method.

### 10.4 Map methods

| Method | Description |
| --- | --- |
| `keys()` | Array of keys in insertion order. |
| `values()` | Array of values in insertion order. |
| `entries()` | Array of `[key, value]` pairs. |
| `has(key)` | `true` if the key exists. |
| `get(key, default?)` | Value of the key, or `default`/`nil`. |
| `set(key, value)` | Assigns and returns the map. |
| `delete(key)` | Removes the key; returns `true` if it existed. |
| `clear()` | Empties the map and returns it. |
| `merge(other)` | Copies the keys of `other` into this map and returns it. |
| `clone()` | Shallow copy. |

```jolt
let m = {a: 1};
m.set("b", 2);
assertEq(m.keys(), ["a", "b"]);
assertEq(m.entries(), [["a", 1], ["b", 2]]);
assertEq(m.get("z", 0), 0);
assertEq(m.delete("a"), true);
assertEq(m, {b: 2});
```

Map equality does not depend on order: two maps with the same keys and values are equal.

Iterating a map produces its keys:

```jolt
for key in {a: 1, b: 2} {
  print(key);
}
```

---

## 11. Ranges

### 11.1 Syntax

| Expression | Meaning |
| --- | --- |
| `a..b` | From `a` up to `b`, excluding `b`. |
| `a..=b` | From `a` up to `b`, including `b`. |

The bounds must be integers. A range can be ascending or descending:

```jolt
assertEq((1..4).toArray(), [1, 2, 3]);
assertEq((1..=3).toArray(), [1, 2, 3]);
assertEq((3..0).toArray(), [3, 2, 1]);
assertEq((3..=0).toArray(), [3, 2, 1, 0]);
```

`5..5` is empty; `5..=5` contains one element.

### 11.2 Operations

- `.length` / `len(range)`: number of elements.
- `.toArray()`: converts to an array (limit of 10,000,000 elements).
- `.contains(value)`: same as `value in range`.
- Equality compares start, end and inclusiveness: `1..3 != 1..=3`.

The `range(end)` function produces `0..end`, and `range(start, end)` produces `start..end` (always exclusive).

Range values are lightweight: they only store the bounds and the inclusiveness flag. Iterating a range with `for ... in` is lazy (constant memory); operations that need all elements at once, such as `toArray()` or passing the range to `map`/`filter`-style builtins, materialize them and are capped at 10,000,000 elements.

```jolt
assertEq(2 in 1..5, true);
assertEq(5 in 1..5, false);
assertEq(5 in 1..=5, true);
assertEq(len(5..0), 5);
```

---

## 12. Standard library

### 12.1 Console and input

| Function | Description |
| --- | --- |
| `print(value, ...)` | Prints the values separated by spaces and appends `\n`. |
| `write(value, ...)` | Like `print`, but without the trailing newline. |
| `eprint(value, ...)` | Like `print`, but to the error output. |
| `input(prompt?)` | Reads a line; returns `nil` at the end of the input. |

### 12.2 Types and conversion

| Function | Description |
| --- | --- |
| `type(value)` | Name of the type as a string. |
| `str(value)` | Text representation. |
| `repr(value)` | Representation with strings in quotes. |
| `int(value)` | Converts to integer (truncates floats and doubles). |
| `float(value)` | Converts to `float` (binary32). |
| `double(value)` | Converts to `double` (binary64). |
| `bool(value)` | Applies Jolt's truthiness. |

`int(string)` accepts the `strtoll` format with base 0 (decimal, `0x...`, `0...` octal) and trims whitespace. `float(string)` and `double(string)` use `strtod` (`float` then rounds to binary32). Converting an empty or badly formatted string is an error.

`str` of an array or map uses the representation of its elements, with inner strings in quotes:

```jolt
assertEq(str(42), "42");
assertEq(str([1, "a"]), "[1, \"a\"]");
assertEq(repr("a"), "\"a\"");
```

### 12.3 Math

Some math functions are host builtins (always in scope); `abs`, `min`, `max`
and `clamp` live in the `std.math` module and are imported. See §18.

Host builtins:

| Function | Description |
| --- | --- |
| `floor(n)` | Rounds down. |
| `ceil(n)` | Rounds up. |
| `round(n)` | Rounds to the nearest integer (halves away from zero). |
| `sqrt(n)` | Square root; error if `n < 0`. |
| `pow(base, exp)` | Power as `double`. |
| `sin(n)`, `cos(n)`, `tan(n)` | Trigonometric functions (radians). |
| `log(n)` | Natural logarithm; error if `n <= 0`. |
| `exp(n)` | `e ** n`. |

`std.math` (import it first):

| Function | Description |
| --- | --- |
| `abs(n)` | Absolute value. |
| `min(a, ...)` | Minimum of at least one argument. |
| `max(a, ...)` | Maximum of at least one argument. |
| `clamp(n, minimum, maximum)` | Limits `n` to the interval. |
| `PI`, `E` | Constants. |

`floor`, `ceil` and `round` return the same `int` if they receive an `int`, and a `double` for a `float` or `double` argument (like Java's `Math.floor`, which takes and returns `double`).

```jolt
import std.math.*;

assertEq(abs(-7), 7);
assertEq(min(3, 1, 2), 1);
assertEq(clamp(15, 0, 10), 10);
assertEq(sqrt(81), 9.0);
```

### 12.4 Randomness and time

| Function | Description |
| --- | --- |
| `random()` | `double` in `[0, 1)`. |
| `random(high)` | `int` in `[0, high)`. |
| `random(low, high)` | `int` in `[low, high)`. |
| `seed(n)` | Reseeds the generator. |
| `clock()` | Seconds elapsed since the process started. |

### 12.5 Utilities

| Function | Description |
| --- | --- |
| `len(value)` | Length of a string, array, map or range. |
| `chr(code)` | Unicode character from a code point. |
| `ord(character)` | Code point of a one-character string. |
| `range(end)` | Range `0..end`. |
| `range(start, end)` | Range `start..end`. |
| `args()` | Script arguments as an array of strings. |

### 12.6 Files

| Function | Description |
| --- | --- |
| `readFile(path)` | Returns the contents of a file as a string. |
| `writeFile(path, contents)` | Writes the contents (exact binary). |
| `fileExists(path)` | Checks whether the file can be opened. |

### 12.7 Assertions and control

| Function | Description |
| --- | --- |
| `assert(condition, message?)` | Fails if the condition is falsy. |
| `assertEq(expected, actual, message?)` | Fails if they are not equal. |
| `panic(message?)` | Raises a runtime error. |
| `pcall(fn, args...)` | Calls `fn` and turns a runtime error into a value (see §13.1). |
| `error(message, code?)` | Builds an error value (see §13.2). |
| `isError(value)` | `true` only for an error value (see §13.2). |
| `exit(code?)` | Ends the process with the given code (0 by default). |

`assertEq` follows the order `expected, actual` and adds both values to the error message.

### 12.8 FFI pointers

Opaque `ptr`/`cstring` handles (PHASE 6.6, see §4.6). They own host storage and
are never a machine address the program can dereference.

| Function | Description |
| --- | --- |
| `cstring(s)` | Builds a `cstring` from a string (bytes plus a NUL). |
| `cstr(p)` | Reads a `cstring`/buffer back as a string. |
| `buffer(n)` | Allocates `n` zero-initialised bytes and returns a `ptr`. |
| `byteLen(p)` | Bytes the handle owns. |
| `loadByte(p, i)` | Byte `0..255` at index `i` (bounds-checked). |
| `storeByte(p, i, b)` | Writes a byte into a buffer. |
| `intToPtr(n)` | Wraps an opaque integer token in a `ptr`. |
| `ptrToInt(p)` | Reads the opaque integer token back. |
| `free(p)` | Releases the handle's storage. |

---

## 13. Errors and diagnostics

Jolt distinguishes two kinds of errors:

- **Compile error**: detected by the lexer or the parser (for example, a missing `;` or a malformed literal).
- **Runtime error**: detected by the interpreter (for example, an undefined variable, a division by zero or an out-of-range index).

Both are shown with the location and a code snippet:

```text
script.jolt:3:11: runtime error: division by zero
  |
3 | let x = 1 / 0;
  |           ^
```

Runtime errors also include a stack trace with the active functions. A deep
call chain (for example an accidental infinite recursion) builds thousands of
frames, so the trace prints the outermost ten and the innermost ten and elides
the middle with a `... N more ...` line; a short trace is printed in full.

A third kind of diagnostic is non-fatal: a **warning**. A warning is reported
with the same snippet form but does not stop the program, which still compiles
and runs and keeps its exit code. The only warning today is the discarded result
of a `@MustUse` function (see §7.7).

```text
script.jolt:4:6: warning: the result of '@MustUse' function 'parse' is discarded; assign it to '_' to acknowledge
  |
4 | parse(text);
  |      ^
```

Process exit codes:

| Code | Meaning |
| --- | --- |
| `0` | Success. |
| `64` | Incorrect command-line usage. |
| `65` | Compile error. |
| `66` | The file could not be read. |
| `70` | Runtime error. |
| other | Code given to `exit(n)`. |

`jolt -c file.jolt` only checks the lexer and the parser; it does not run the program.

### 13.1 Recovering from an error: `pcall`

`pcall(fn, args...)` calls `fn` with the remaining arguments and turns a
runtime error into a value instead of aborting the program. It returns a map
`{ok, value, error}`:

- on success, `ok` is `true`, `value` is the return value and `error` is `nil`;
- on failure, `ok` is `false`, `value` is `nil` and `error` is an error value
  (see §13.2).

```jolt
let result = pcall(() => 1 / 0);
if (!result.ok) {
  print("failed: ${result.error}");            // failed: division by zero
  print(result.error.code);                    // 0
}

// Arguments are forwarded to the callee.
let sum = pcall((a, b) => a + b, 2, 3);
print(sum.value);                              // 5
```

Every runtime error is catchable, including `panic(...)`, an out-of-range
index, an undefined variable and an error raised deep in the call stack. After
`pcall` returns, the program keeps running and the engine stays consistent.

`pcall` is a function, not a keyword, so it compiles to an ordinary call: no
new syntax and no new bytecode. Passing a non-function as the first argument is
a caller bug, not a catchable error, and fails loudly.

### 13.2 Errors as values

An error is a first-class value with its own runtime type, not a map that
happens to have the right shape. `error(message, code?)` builds one and
`isError(value)` detects it:

```jolt
let failure = error("not found", 404);
print(type(failure));        // error
print(failure.message);      // not found
print(failure.code);         // 404
print("${failure}");         // not found
print(isError(failure));     // true
```

The message is the display form, so `print(err)` and `"${err}"` render the
text; `repr` shows the constructor form `error("not found", 404)`. Two errors
are equal when their message and code match.

`isError` is exact: a user map such as `{ok: false, error: "x"}` is never
mistaken for an error. This is why the type exists instead of reusing a map —
a helper that returns an error must be distinguishable from one that returns
data with the same field names.

`error(...)` builds a value; it does **not** raise. To abort the program use
`panic(...)`. To catch a raised error use `pcall` (§13.1). Because errors are
ordinary values, they can be stored, returned, compared and carried through
containers like any other value.

### 13.3 Propagating and recovering: `try` and `catch`

`pcall` turns a raised error into a value, but it is awkward for the common
case where a function wants to hand a failure back to its caller. Two
operators cover that, and both work on error **values** (the result of
`error(...)` or of a function that returns one):

- **`try expr`** — if `expr` is an error value, the enclosing function returns
  it immediately; otherwise `expr` is yielded unchanged. It is Rust's `?`.
- **`expr catch default`** — if `expr` is an error value, the whole expression
  is `default`; otherwise it is `expr`. It is Zig's `catch`.

```jolt
fn parse(s) {
  if (len(s) == 0) { return error("empty input"); }
  return 42;
}

// `catch` recovers locally, with a default.
print(parse("") catch 0);     // 0
print(parse("x") catch 0);    // 42

// `try` propagates to the caller.
fn double(s) {
  let n = try parse(s);       // returns the error from `double` if parse fails
  return n * 2;
}
let r = double("");           // the error value, not a crash
print(isError(r));            // true
print(r.message);             // empty input
print(double("x"));           // 84
```

Precedence and associativity:

- `try` is a prefix operator, binding like `-`, `!` and `~`, so `try f().x`
  propagates the result of the member access.
- `catch` binds looser than `??` and is left-associative, so
  `a() catch b() catch c` recovers at the first failure, and `a + b catch 0`
  is `(a + b) catch 0`.

Both are pure sugar over `isError` plus a jump, so they add **no opcode**:
`try` lowers to `CALL $isError` + `JMP_IF_FALSE` + `RETURN`, and `catch` to
`CALL $isError` + `JMP_IF_FALSE` + `MOVE`.

Two rules mirror `return`:

- `try` needs a function to return from: it is a compile error outside a
  function.
- `try` is rejected inside a `defer` block, because the block is inlined at the
  scope exit and returning from it would re-enter the teardown.

A propagating `try` leaves the function like a `return`, so every `defer` in
the function runs (LIFO) before the caller sees the error.

`catch` only recovers error **values**. A raised runtime error (for example
`1 / 0` or `panic(...)`) is not a value and is not caught by `catch`; use
`pcall` (§13.1) to turn those into values.

---

## 14. REPL

Running `jolt` with no arguments starts the REPL. Features:

- Expressions are evaluated and shown with `repr`; `nil` is not printed.
- `_` holds the last evaluated value.
- The `...>` prompt means continuation; a blank line cancels the incomplete input.
- The trailing `;` of each entry may be omitted.
- Definitions persist until `:clear`.

The session is semantic, not just a bag of values. An entry sees the
definitions of the entries before it exactly as if they were one accumulating
program: a `let` stays immutable, a `var` keeps its type annotation, a
`struct` stays usable as a literal or an annotation, and an `import` is bound
once. Redefining a name in a later entry is allowed and replaces the old
binding. An entry that fails to compile leaves no trace: it commits nothing,
so an import it started can be imported again cleanly. `:clear` drops the
whole session state with the definitions. A file run directly (`jolt
file.jolt`) always starts from an empty session; `:load` instead runs the file
in the current session, so it sees the session's definitions and merges its
own into it.

Commands:

| Command | Description |
| --- | --- |
| `:help`, `:?` | Help. |
| `:quit`, `:q`, `:exit` | Quit. |
| `:clear` | Delete the user definitions. |
| `:load <file>` | Run a script in the current session. |
| `:vars` | List the user's global variables. |

Example:

```text
jolt> let x = 10
jolt> let double = n => n * 2
jolt> double(x)
20
jolt> _
20
jolt> :vars
  _: int
  double: function
  x: int
```

---

## 15. Command line

```text
jolt                      starts the REPL
jolt <file.jolt>          runs a script
jolt -e "<code>"          runs code from the command line
jolt -c <file.jolt>       checks the syntax without running
```

| Option | Description |
| --- | --- |
| `-h`, `--help` | Help. |
| `-v`, `--version` | Version. |
| `-e`, `--eval <code>` | Code to run. The remaining arguments go to the script. |
| `-c`, `--check` | Syntax check only. |
| `--` | End of options; the next argument is the file. |

Options must come before the file. Everything after the file is considered an argument of the script and is obtained with `args()`.

```sh
jolt program.jolt one two
```

```jolt
for argument in args() {
  print(argument);
}
```
**Planned (PHASE 7)**:

```sh
joltc <file.jolt> -o app    compiles to a native binary (AOT, musl static)
joltc <file.jolt> -c        compiles to bytecode (.jolc)
joltc -pkg ... -o app.jpkg  packages a .jpkg
```

`joltc` is the host compiler. It reads sources and writes the ELF, so it links libc; the freestanding part is what it emits, not the compiler itself. 
`jolt` (the development engine) may use AsmJIT JIT; joltc uses AsmJIT Assembler (no JIT).

---

## 16. Known limitations

- Dynamic typing. The `jolt` executable compiles source to bytecode and runs it on the register-based VM; the AST is never executed. The compiler and the VM cover functions, closures, control flow, and collections (PHASE 6 done).
- No JIT. A JIT (AsmJIT) is planned for `jolt` (development engine) in PHASE 8+, not for `joltc` (product).
- No classes, interfaces or inheritance (and there never will be — maps + closures replace them). A `struct` (data only, no methods) is supported (see §4.7): it is a named map shape checked at compile time, not a class.
- No `try`/`catch` or catchable exceptions. Error handling uses `nil` (not found), `pcall(fn, ...)` for recoverable runtime errors (see §13.1) and `panic()` for unrecoverable ones. There are no plans to add catchable exceptions; `pcall` and errors as values are the model.
- Modules use Java-style `import` (see §18). `import` is only allowed at the top level of a module: nesting an import inside a function or block is a compile error. `require` (Lua-style) is dropped — `import` is the only module form. A module is loaded once, eagerly, when the program is compiled, and a cyclic import is a compile error rather than a crash.
- No `++`, `--` or default parameters. A trailing `rest...` parameter makes a function variadic (see §7.1). `switch` on literals and ranges is supported (see §6.10), and so is the post-test loop `do { ... } while (cond);` (see §6.4).
- Optional types and nullability are implemented for declarations and expressions (PHASE 6.5). Type annotations (`let x: int = 5`), local inference, the nullable `T?` annotation, and the null-safe expressions `?.`, `??` and `!!` are checked at compile time. `struct` is implemented (see §4.7) and so is `native fn` (see §7.6), the typed declaration of a host-provided function.
- FFI with C is implemented for the development engine (PHASE 6.6). C functions are declared with `extern "C" { ... }` blocks or the single-line `extern fn ...;` form (implicit `"C"` ABI); both desugar to the same representation. The FFI type vocabulary is documented in §4.6: the compiler validates each signature, records it on the proto, and the engine resolves the symbol through `Platform::lookupSymbol` and binds it to a marshalling native. The call marshals its arguments and result across the System V AMD64 ABI without `libffi` for scalars and structs by value. A `fn(P...): R` parameter is a callback: the development engine builds a `libffi` closure that re-enters the VM, while the AOT backend (PHASE 7) lowers callbacks to compile-time stubs and never links `libffi`.
- No UI library yet. Skia + SDL2 + declarative UI is planned for post-1.0.
- No async/await, generics, full pattern matching, generators, or transactions. These are out of scope (see `AGENT.md`).
- Map keys are always strings. Literals accept identifier, string, scalar (`int`, `float`, `double`, `true`, `false`, `nil`) and computed `[expr]` keys; non-string keys are canonicalized to their string form.
- A body of `if`, `while`, `for` or `for-in` may be a block or a single statement without braces; a brace-less body is wrapped in a block, so its bytecode and scoping match the brace form. `switch` arms always require braces.
- `for ... in` accepts the loop variable and the iterable with or without parentheses (`for x in xs` and `for (x in xs)`).
- Slicing syntax `s[start:end:step]` works for strings and arrays, with an optional step (`s[::-1]` reverses); the methods `substr`/`slice` remain available.
- `upper` and `lower` only work with ASCII.
- Maximum call depth: 2000.
- Memory is managed with reference counting plus a cycle collector. Unreachable cycles (a function stored in the scope it closes over, an array that contains itself, **a bound method stored in its own receiver such as `a.f = a.push`**) are reclaimed.
- Sorting with custom comparators relies on the comparator defining a strict order.

---

## 17. Complete example

```jolt
// Counts words and shows the most frequent ones.

fn countWords(text) {
  let counts = {};
  for word in text.lower().split() {
    counts[word] = counts.get(word, 0) + 1;
  }
  return counts;
}

let text = "one two two three three three four four four four";
let counts = countWords(text);

let pairs = counts.entries();
pairs.sort((a, b) => a[1] > b[1]);

for pair in pairs {
  print("${pair[0]}: ${pair[1]}");
}
```

Output:

```text
four: 4
three: 3
two: 2
one: 1
```

## 18. Modules and `import`

Jolt imports are Java-style: the module is named by a **dotted path without quotes**, and `std.` is reserved for the standard library. Four forms exist.

```jolt
import std.math;                  // qualified: access is std.math.sqrt
import std.math.*;                // wildcard: access is sqrt
import std.math as m;             // alias: access is m.sqrt
import { sqrt, pow } from std.math; // selective: access is sqrt and pow
```

`import` is a declaration, not a function call, and it is only allowed at the **top level of a module**. Nesting an import inside a function or a block is a compile error.

### 18.1 The four forms

| Form | Meaning |
| --- | --- |
| `import a.b;` | Load `a/b.jolt` and bind it to the namespace `a.b`. Access is `a.b.name`. |
| `import a.b.*;` | Load `a/b.jolt` and add all its public names to the current scope. Access is `name`. |
| `import a.b as c;` | Load `a/b.jolt` and bind it to `c`. Access is `c.name`. |
| `import { x, y } from a.b;` | Load `a/b.jolt` and bind only `x` and `y`. Access is `x` and `y`. |

### 18.2 What a module exports

A module is a `.jolt` file. Its top-level declarations become the module's public names. A binding whose name starts with `_` is **private** and is not exported.

```jolt
// std/math.jolt
const PI = 3.141592653589793;

fn sqrt(x) {
  return x ** 0.5;
}

fn _helper() {   // private: not exported
  return 1;
}
```

A module's own top-level bindings are **local to the module frame**: importing a module never injects its internals into the importing program's globals. Only the names you import become visible.

The same holds for a module's own imports: the namespaces, aliases and wildcard names a module creates with `import` are private to that module. A module that imports `a.b` makes `a.b` visible to its own functions, but the importing program does not see `a.b` unless it imports it too. Likewise, a wildcard import inside a module never collides with a wildcard import in the importing program.

A module body runs **exactly once**, no matter how many times it is imported, and its exported table is shared by every import.

### 18.3 Conflict detection

Wildcard imports are safe because conflicts are caught at compile time. If two imports bind the same name, the compiler reports an error instead of silently letting one shadow the other:

```text
'sqrt' is ambiguous between std.math and std.stats; use 'std.math.sqrt' or 'std.stats.sqrt' to disambiguate
```

Conflict detection covers every pair of forms, not only two `.*` imports: a wildcard name that collides with an alias or with a namespace root is reported too, and so is an alias that collides with a namespace root. When both sides are unqualified names the error suggests the qualified form; otherwise it asks you to rename one of the imports.

Two cases are *not* conflicts:

- Importing the same module twice (the same binding, whatever the form).
- Two dotted imports with the same root (`import a.b;` then `import a.c;`): they are sibling modules of one package and share the namespace table.

A name the importing module declares itself (`let`, `var` or `fn`) wins over an imported one, whatever the order of the declarations: an import never silently shadows a local binding. The two cases are distinct — a local binding is deliberate and wins; two imports colliding is a mistake and is reported.

### 18.4 Module resolution

A dotted path `a.b.c` maps to the file `a/b/c.jolt`, looked up in this order:

| Path | Resolves to |
| --- | --- |
| `std.*` | the standard library directory (`std/math.jolt`). `std.` is reserved: a user module under that prefix is a compile error. |
| anything else | the project directory (the directory of the file being compiled), then the installed packages (`$JOLT_PACKAGES`, or `$HOME/.jolt/packages`). |

The standard library directory is found by looking in the working directory, then walking up from the running executable's directory (so `std/` is found whether `jolt` sits in the source tree, in `build/`, or in an install prefix), then `../share/jolt/std` next to the binary. `$JOLT_STDLIB` overrides the search. `cmake --install` ships `std/` to `share/jolt/std`, so an installed `jolt` resolves `import std.math` from any working directory.

A path that resolves nowhere is a compile error: `module 'X' not found`.

Nested imports (a module that imports another module) resolve from the same project root. A module that imports itself, directly or through a chain (`a -> b -> a`), is a compile error: `cyclic import detected: a -> b -> a`.

### 18.5 Example

```jolt
import std.math;            // std.math.sqrt, std.math.pow, ...
import { sqrt } from std.math;

print(std.math.sqrt(16));   // 4
print(sqrt(25));            // 5
```

---

## 19. File extensions
| Extension | What it is | Java analog |
|-----------|------------|-------------|
| `.jolt` | Source code | `.java` |
| `.jolc` | Bytecode | `.class` |
| `.jpkg` | Package (ZIP) | `.jar` |
| `.jlib` | Native library | `.dll` / `.so` / `.dylib` |
| `.jdoc` | Documentation | `.javadoc` |
| `myapp` | Native binary (Linux/macOS) | (GraalVM) `.exe` |
| `myapp.exe` | Native binary (Windows) | (GraalVM) `.exe` |

**`.jpkg` format** (ZIP):

```text
myapp.jpkg
├── META-INF/MANIFEST.MF
├── bin/main.jolc
├── lib/mylib.jlib
├── resources/icon.png
└── README.md
```
