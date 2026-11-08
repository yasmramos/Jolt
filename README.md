# Jolt

**Jolt** is a dynamic, general-purpose programming language implemented from scratch in **C++17**. The project includes the lexer, the parser, the AST, the bytecode compiler, the register-based virtual machine and the standard library, with no external dependencies beyond the C++ standard library.

Its goal is to be small, readable and easy to study: a single executable, a Lua-style bytecode architecture and a familiar syntax with braces and semicolons.

```jolt
fn greet(name) {
  return "Hello, ${name}!";
}

let languages = ["C++", "Jolt"];
for language in languages {
  print(greet(language));
}
```

## Features

- Dynamic types: `nil`, `bool`, `int` (64-bit), `float` (binary32), `double` (binary64), `string`, `array`, `map`, `function` and `range`.
- Variables with `var` (mutable), `let` (immutable, runtime) and `const` (immutable, folded at compile time), block scope and real closures.
- First-class functions, recursion and `=>` lambdas (`x => x * 2`).
- String interpolation with `"${expression}"` and UTF-8 support.
- Ordered arrays and maps that preserve insertion order.
- Ascending and descending ranges: `1..5`, `0..=10`, `5..0`.
- Arithmetic, logical, bitwise, comparison, ternary and compound assignment operators.
- Control flow: `if`/`else`, `while`, classic `for` and `for ... in`.
- Built-in standard library: strings, arrays, maps, math, files, console and utilities.
- Interactive REPL with session state, commands and expression evaluation.
- Diagnostics with file, line, column, code snippet and stack trace.

## Status

Version **0.1.0**. The language is functional and covered by a test suite, but it is not yet considered stable or complete. See the [limitations](#limitations-and-roadmap) section.

## Requirements

- A C++17 compiler: `g++`, `clang++` or MSVC.
- `cmake` 3.15 or newer (optional, only if you prefer building with CMake).
- On Windows it has been tested with **llvm-mingw** (clang 21 with the `g++` interface).

## Building

### Windows (script)

```bat
build.bat
build\jolt.exe examples\hello.jolt
```

### Linux, macOS or MSYS2 (script)

```sh
chmod +x build.sh tests/run.sh
./build.sh
./build/jolt examples/hello.jolt
```

You can choose the compiler and its options:

```sh
CXX=clang++ CXXFLAGS="-O0 -g" ./build.sh
```

### CMake

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
./build-cmake/jolt examples/hello.jolt
```

### Manual build

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build/jolt src/*.cpp
```

On Windows, with `cmd.exe`:

```bat
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Isrc -o build\jolt.exe src\*.cpp
```

## Usage

```text
jolt                      starts the REPL
jolt <file.jolt>          runs a script
jolt -e "<code>"          runs code from the command line
jolt -c <file.jolt>       checks the script without running it
```

Options:

| Option | Description |
| --- | --- |
| `-h`, `--help` | Shows the help. |
| `-v`, `--version` | Shows the version. |
| `-e`, `--eval <code>` | Runs the given code. |
| `-c`, `--check` | Only checks the syntax, without running. |
| `--` | Marks the end of the options; the next argument is the file. |

Arguments that follow the file (or the `-e` code) are passed to the script and can be read with `args()`:

```sh
jolt greet.jolt Ada Grace
```

```jolt
for name in args() {
  print("Hello, ${name}");
}
```

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Successful execution. |
| `64` | Command-line usage error. |
| `65` | Compile error (lexer or parser). |
| `66` | The file could not be read. |
| `70` | Runtime error. |
| other | Value given to `exit(code)`. |

### REPL

Running `jolt` with no arguments opens the REPL:

```text
Jolt 0.1.0 - a small scripting language written in C++
Type :help for help, :quit to exit.
jolt> let x = 21
jolt> x * 2
42
jolt> let double = n => n * 2
jolt> double(x)
42
```

- Expressions print their result in `repr` format.
- `_` holds the last evaluated value.
- The `...>` prompt means the input continues on the next line.
- A blank line cancels an incomplete input.
- The trailing `;` of each REPL entry may be omitted.
- Definitions stay in the session until `:clear` is used.

Available commands:

| Command | Description |
| --- | --- |
| `:help`, `:?` | Shows the REPL help. |
| `:quit`, `:q`, `:exit` | Leaves the REPL. |
| `:clear` | Forgets all user definitions. |
| `:load <file>` | Runs a script inside the current session. |
| `:vars` | Lists the user-defined global variables. |

## Examples

In the `examples/` directory:

| File | Contents |
| --- | --- |
| `hello.jolt` | Greeting and string interpolation. |
| `fizzbuzz.jolt` | Loops, conditionals and modulo. |
| `fibonacci.jolt` | Recursion and an iterative version. |
| `closures.jolt` | Closures and functions that remember state. |
| `collections.jolt` | Arrays, maps, filtering, reduction and sorting. |

```sh
jolt examples/fizzbuzz.jolt
```

## The language in a minute

```jolt
// Variables: var is mutable, let is immutable, const is a compile-time constant
let name = "world";
const VERSION = "0.1.0";

// Functions and closures
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

// Conditionals and loops
if (counter() > 2) {
  print("going fast");
} else {
  print("going slow");
}

for i in 1..=3 {
  print("i = ${i}");
}

// Collections
let numbers = [1, 2, 3, 4, 5];
let evenSquares = numbers
  .filter(n => n % 2 == 0)
  .map(n => n * n);
print(evenSquares); // [4, 16]

let person = {name: "Ada", age: 36};
person.age += 1;
print("${person.name} is ${person.age} years old");
```

The full reference is in **[docs/language.md](docs/language.md)**.

## Tests

The simplest way to run everything (all `.jolt` scripts, the error cases, the
examples and both C++ suites) is CTest:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

With a multi-config generator (for example Visual Studio) add
`-C Release` to the `ctest` invocation. The same tests are available through
the helper scripts below.

Windows:

```bat
powershell -ExecutionPolicy Bypass -File tests\run.ps1
powershell -ExecutionPolicy Bypass -File tests\run_compiler.ps1
powershell -ExecutionPolicy Bypass -File tests\run_vm.ps1
powershell -ExecutionPolicy Bypass -File tests\run_gc.ps1
```

Linux, macOS or MSYS2:

```sh
./tests/run.sh
./tests/run_compiler.sh
./tests/run_vm.sh
./tests/run_gc.sh
```

The positive tests use `assert` and `assertEq`; a script exits with a non-zero code if any assertion fails. The tests in `tests/errors/` check compile-time and runtime errors: they declare the expected exit code on their first line with `// expect-exit: N`. The bytecode compiler tests (`compiler_test.cpp`) verify the instruction sequence, the constant pools, the `nil`/boolean literals, the unary and compound operators, the comparisons, the `if`/`while`/`for` jumps with backpatching, the local slots and the rejection of assignments to immutable `let` bindings. The VM tests (`vm_test.cpp`) compile snippets to bytecode and check the execution of integer and float arithmetic, `%`, `**`, comparisons, unary operators, globals, strings, compound assignment, `if`/`else`, loops, `break`/`continue`, block shadowing, bitwise operators, `&&`/`||` short-circuiting, the ternary operator and runtime errors.

## Limitations and roadmap

Current limitations:

- Dynamic typing. The product model is Lua's: source code is always compiled to bytecode and run on the VM. The CLI and the REPL both compile to bytecode and run on the register-based VM; the AST is never executed. There is no JIT.
- There are no classes, structs, interfaces or inheritance.
- There is no `try`/`catch` *statement* or catchable exceptions. Errors are values: `error(message, code?)` builds a first-class error value and `isError(value)` detects it. `pcall(fn, ...)` turns a raised runtime error into a `{ok, value, error}` map instead of aborting. Two operators work on error values: `try expr` propagates one to the caller (Rust's `?`) and `expr catch default` recovers locally (Zig's `catch`). `panic()` is for unrecoverable errors.
- Modules use Java-style `import` (`import std.math;`, `import std.math.*;`, `import std.math as m;`, `import { sqrt } from std.math;`) with compile-time conflict detection for `.*` and a reserved `std.` prefix. There is no `require`; modules are loaded eagerly and a cyclic import is a compile error.
- There is no `++`/`--` or default parameters. `switch` on literals and integer ranges is supported, with no fallthrough, and so is the post-test loop `do { ... } while (cond);`.
- `defer { ... }` runs a block when its scope exits, in LIFO order, on fall-through, `return` and `break`/`continue` alike; it compiles to no new opcode.
- Map keys are always strings. Map literals accept identifier, string, scalar (`int`, `true`, `false`, `nil`) and computed `[expr]` keys; non-string keys are canonicalized to their string form.
- Braces are mandatory in the bodies of `if`, `while`, `for` and `switch`.
- Slicing syntax works for strings and arrays (`s[1:3]`, `a[2:]`, `a[:-1]`), with negative bounds counted from the end and an optional step (`a[::2]`, `a[::-1]` reverses); `substr`/`slice` remain available.
- `upper` and `lower` only transform ASCII letters.
- The maximum call depth is 2000.
- Memory is managed with reference counting plus a cycle collector (`src/gc.h`) that reclaims unreachable cycles, such as a function stored in the scope it closes over. A cycle that runs through state captured by a native method (`a.f = a.push`) is never collected.

Ideas for future versions:

- Union types (`T | E`), `@MustUse` warnings and richer error handling sugar (PHASE 6.7, slices 4-5).
- User-defined structs and methods.
- Better Unicode support (upper/lower case and graphemes).
- More tooling: formatter and debugger.

Editor support: a VS Code syntax-highlighting extension lives in
[`editors/vscode`](editors/vscode).

## License

Distributed under the **MIT** license. See [LICENSE](LICENSE).

---

Made with C++17, with no external dependencies.
