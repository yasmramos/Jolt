# AGENT.md — Jolt Project

Instructions for AI agents assisting in the development of **Jolt**, a compiled
programming language with a Lua-style internal architecture.

---

## 1. Project identity

| Field | Value |
|-------|-------|
| **Name** | Jolt |
| **Extension** | `.jolt` |
| **Philosophy** | Simple like Python, structured like Java, fast like Lua, safe like Kotlin |
| **Implementation** | C++17 (standard library only, optional AsmJIT for JIT) |
| **Initial platform** | Linux x86-64 (musl static), then Windows/macOS |
| **Architecture** | Register-based bytecode + VM (dev engine) + AOT backend (production) |
| **Niche** | Desktop apps, CLI, UI (not systems, not backend, not data science) |

**Jolt is a general-purpose language** with a clear focus: desktop apps, CLI,
and UI. It **can** do backend, data science, games, or mobile (with FFI), but
those are not priorities.

---

## 2. Mandatory architecture

```text
.jolt → LEXER → PARSER → AST → COMPILER → BYTECODE ─┬─→ VM (dev engine)
                                                     └─→ AOT BACKEND → native binary
```

The VM is a development tool (REPL, fast iteration, differential testing).
joltc produces a self-contained native binary (musl static by default,
no runtime to install); end users never install Jolt or any runtime.

The bytecode is designed from day one to be compiled to native. It is
low-level (40 opcodes, 1:1 or 1:2 lowering to x86-64) so the AOT backend can
generate efficient native code. Do NOT repeat Java's mistake (high-level
bytecode that later turns out to be impossible to compile efficiently).

### Modules and responsibilities

| Module | Input | Output | Status |
|--------|-------|--------|--------|
| `lexer.cpp` | source text | `Token[]` | ✅ |
| `parser.cpp` | `Token[]` | `Node*` (AST) | ✅ |
| `bytecode.cpp` | `Bytecode` | basic operations | ✅ |
| `compiler.cpp` | `Node*` | `Bytecode*` | ✅ |
| `module_resolver.cpp` | dotted module path | source file path | ✅ |
| `vm.cpp` | `Bytecode*` | execution | ✅ |
| `main.cpp` | CLI | full pipeline | ⬜ (VM as dev engine, PHASE 4) |
| `interpreter.cpp` | `Node*` | reference execution | ⚠️ test oracle only |
| `value.cpp` / `env.cpp` | `Value`, scope chain | shared runtime | ✅ |
| `gc.cpp` | tracked `GcNode`s | cycle reclamation | ✅ |
| `native/codegen.cpp` | x86-64 instructions | machine-code bytes | 🔶 PHASE 7a |
| `native/lower.cpp` | bytecode | x86-64 | 🔶 PHASE 7b |
| `native/elf.cpp` | text + rodata | static ELF64 bytes | ✅ |
| `native/pe.cpp` | text + rdata | static PE32+ bytes | ✅ |
| `native/arena.cpp` | block + size | aligned allocations | ✅ PHASE 7c.0 |
| `native/platform_freestanding.cpp` | `Platform` ops | direct Linux syscalls | ✅ PHASE 7c.0 |
| `native/entry.cpp` | program entry label | `_start` + runtime prelude | ✅ PHASE 7c.0 |

### Toolchain binaries and the compiler boundary

`jolt` ships as a single binary until PHASE 7d. After the split:

- `jolt` development engine (run, REPL, -c). Includes VM, interpreter
(test oracle), and JIT (AsmJIT).
- `joltc` host compiler (joltc file.jolt -o app). Reads sources and
writes the ELF. Uses AsmJIT Assembler (no JIT). Links against musl static.
- `jolt build` thin alias over `joltc`

Boundary rule: src/compiler.* and src/native/* must not include
interpreter.h or vm.h. Enforced by tests/check_compiler_boundary.cmake.

---

## 3. Bytecode rules (CRITICAL)

### 3.1 Representation

1. **Register-based**, NOT stack-based. Registers are an array of dynamic
   values. All registers, globals, frames and temporaries use the same `Value`
   type. `Bytecode::constants` is a single tagged constant pool shared by the
   compiler and the VM. Arithmetic operations include fast paths for `int` and
   `float`. `Value` is a 16-byte tagged union with non-atomic reference
   counting (single-threaded). Strings are immutable and shared.

### 3.2 Lowering tiers

2. **Every opcode has exactly one direct lowering**, but not every lowering is
   a single machine instruction. Two tiers:

   | Tier | Lowering | Opcodes |
   |------|----------|---------|
   | **Inline** | 1–3 instructions, no call | `LOADK`, `MOVE`, `ADD`–`POW` (int/float fast paths), `UNOP`, `BAND`–`SHR`, `EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`, `GETLOCAL`, `SETLOCAL` |
   | **Runtime primitive** | 1 call to a named helper | `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`, `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`, `ITER`, `PRINT`, `DECLLOCAL`, `GETGLOBAL`, `SETGLOBAL` |

   **Rule 1:1 / 1:2**: every opcode lowers to **1 instruction** (inline) or
   **2 instructions** (inline + fallback) or **1 call** (runtime primitive).
   **Never 3+ instructions.**

   A runtime primitive is **one small typed operation** (e.g.
   `jolt_table_get`), never the compiler, the AST or `eval`.

### 3.3 Opcode budget

3. **At most 40 opcodes** in total. **40 is a hard ceiling, not a target.**
   Enforced at compile time: `static_assert(kOpcodeCount <= 40)`.

   **Retirement ladder** (if a new opcode is needed):
   | Rung | Consolidation | Saves |
   |------|---------------|-------|
   | 1 | `EQ` `NEQ` `LT` `LTE` `GT` `GTE` → `CMP` | -5 |
   | 2 | `JMP_IF_FALSE` + `JMP_IF_TRUE` → `JMP_IF` | -1 |
   | 3 | `BAND` `BOR` `BXOR` `SHL` `SHR` → `BITOP` | -4 |
   | 4 | `ADD` `SUB` `MUL` `DIV` `MOD` `POW` → `ARITH` | -5 |

   **`PRINT` deletion (6f) frees one slot (39/40).** It does not postpone
   rung 1.

### 3.4 Forbidden instructions

4. **Forbidden**: `OP_EVAL_EXPR`, `OP_EXECUTE_STATEMENT`,
   `OP_CALL_METHOD_DYNAMIC`, `OP_IF`, `OP_WHILE`.

### 3.5 Allowed instructions (the complete 40)

5. **Allowed**:
   - Load: `LOADK`, `MOVE`
   - Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `UNOP`
   - Bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`
   - Comparison: `EQ`, `NEQ`, `LT`, `GT`, `LTE`, `GTE`
   - Variables: `GETGLOBAL`, `SETGLOBAL`, `DECLLOCAL`, `GETLOCAL`, `SETLOCAL`
   - Control: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
   - Functions: `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
   - Collections: `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`
   - Iteration: `ITER`
   - I/O: `PRINT` (**temporary**, see §3.6)
   - End: `HALT`

### 3.6 `PRINT` is retired

`PRINT` was the only opcode that touched the outside world. It was retired in
PHASE 6f: every `print` arity (including the single-argument form) compiles to
`CALL` to the `print` builtin. The opcode is gone from the enum, the VM and the
compiler.

Removing `PRINT` freed one slot (39/40). The next opcode request (if any) starts
at rung 1 of the §3.3 ladder.

### Explicit anti-goal

> **Do NOT repeat Java's mistake**: high-level bytecode that later turns out
> to be impossible to compile to native code efficiently. Jolt's bytecode
> must be born with future native compilation in mind.

### Design rules for the AOT backend

1. **No VM-only features.** Every language feature must be compilable by the
   AOT backend. Runtime `eval`/`load` of bytecode is forbidden unless the
   produced binary embeds the compiler itself (GraalVM pitfall).
2. **The bytecode must carry enough information for AOT.** Instructions stay
   annotatable with provable types so the backend can emit typed fast paths.
3. **The embeddable runtime cannot call libc.** The runtime that ships inside
   AOT binaries may only reach the outside world through the `Platform`
   interface. **Unless the user explicitly requests musl static** (the
   default), in which case libc is statically linked and the binary is still
   self-contained.

### Explicit anti-goal

> **Do NOT repeat Java's mistake**: high-level bytecode that later turns out
> to be impossible to compile to native code efficiently. Jolt's bytecode
> must be born with future native compilation in mind.

### Design rules for the AOT backend

1. **No VM-only features.** Every language feature must be compilable by the
   AOT backend. Runtime `eval`/`load` of bytecode is forbidden unless the
   produced binary embeds the compiler itself (same pitfall as GraalVM: a
   dynamic escape hatch that defeats static compilation).
2. **The bytecode must carry enough information for AOT.** Instructions stay
   annotatable with provable types so the backend can emit typed fast paths
   instead of falling back to full dynamic dispatch.
3. **The embeddable runtime cannot call libc.** The runtime that ships inside
   AOT binaries may only reach the outside world through the `Platform`
   interface (see PHASE 7): write, read, alloc, exit and file I/O go through
   it, never through libc or libstdc++ symbols.

### Expected bytecode → native lowering

This table is normative for PHASE 7. **Sequence** means inline instructions;
**call** means one call into the embedded runtime. The instruction count is a
budget, not a promise that every opcode is one instruction.

| Bytecode | Lowering |
|----------|----------|
| `LOADK R0, K1` | sequence: write tag + payload (2–3 insns) |
| `MOVE R0, R1` | sequence: tagged move (2 insns) |
| `ADD R2, R0, R1` | sequence: int/float fast path; generic path calls `jolt_add` |
| `SUB`–`POW` | sequence: same fast-path shape as `ADD`; generic path calls `jolt_arith` |
| `UNOP R0, R1, op` | sequence: 1–2 insns (negate/not/bit-not) |
| `BAND`–`SHR` | sequence: 1–2 insns (integer only) |
| `EQ`–`GTE` | sequence: `cmp` + `setcc`; generic path calls `jolt_compare` |
| `JMP +n` | sequence: `jmp rel32` |
| `JMP_IF_FALSE R2, +2` | sequence: `test`; `jz .label` |
| `JMP_IF_TRUE R2, +2` | sequence: `test`; `jnz .label` |
| `GETGLOBAL R0, K1` | call: `jolt_global_get` until globals are fixed slots |
| `SETGLOBAL R2, K3` | call: `jolt_global_set` until globals are fixed slots |
| `DECLLOCAL R0, 3` | call: `jolt_cell_new` (arena alloc) |
| `GETLOCAL R1, 10` | sequence: cell load + tagged load |
| `SETLOCAL R0, 10` | sequence: tagged store into the cell |
| `CALL R0, 2` | call: `jolt_call` (frame setup + dispatch) |
| `RETURN R0` | call: `jolt_return` (frame teardown) |
| `CLOSURE R0, 1` | call: `jolt_closure` (capture cells) |
| `GETUPVAL R0, 0` | call: `jolt_upval_get` (shared cell deref) |
| `SETUPVAL R0, 0` | call: `jolt_upval_set` (write barrier + cell store) |
| `NEWTABLE R0, 8` | call: `jolt_table_new` (arena alloc) |
| `GETTABLE R0, R1, R2` | call: `jolt_table_get` (bounds/hash/type checks) |
| `SETTABLE R1, R2, R0` | call: `jolt_table_set` (grow + write barrier) |
| `IN R0, R1, R2` | call: `jolt_contains` |
| `ITER R0, R1` | call: `jolt_iter_next` |
| `PRINT R3` | call: `jolt_print` → `Platform::write` (temporary, §3.6) |
| `HALT` | sequence: frame epilogue + `ret` |


---

## 4. Development phases (strict order)

You cannot move on to the next phase without completing and testing the
previous one.

### PHASE 1 — `include/bytecode.h` ✅
- Define `enum class OpCode` with the 40 opcodes listed in §3.5
- Define `struct Instruction { OpCode op; int a, b, c; int jump; int line, col; }`
- Define `struct Bytecode` with `code`, a tagged `constants` pool and `numRegisters`
- `constants` is a `std::vector<Value>` so `int`, `float`, strings, booleans and
  nil keep their type
- `LOADK` loads any constant (`nil`, booleans, numbers and strings)
- Functions: `bytecodeInit`, `bytecodeEmit`, `bytecodeFree` (declared in
  `include/bytecode.h`, implemented in `src/bytecode.cpp`)

### PHASE 2 — `src/compiler.cpp` (AST → bytecode) ✅
- Literals: every literal → `LOADK` with the value in the tagged pool
- `VariableExpr` → `GETGLOBAL`
- `BinaryExpr` with `+`, `-`, `*`, `/`, `%`, `**` and comparisons (`==`, `!=`,
  `<`, `<=`, `>`, `>=`) → compile left, compile right, emit the opcode
- `UnaryExpr`: `-` → `UNOP` negate, `!` → `UNOP` not, `+` emits no
  instruction, `~` → `UNOP` bit-not
- `&&`, `||`, `in` and bitwise operators → PHASE 5
- `AssignExpr` on a global variable → compile the expression, emit `SETGLOBAL`
- Compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`) → `GETGLOBAL`,
  operation, `SETGLOBAL`
- `LetStmt` → compile the initializer; if there is none, load an explicit nil
  constant; then `SETGLOBAL`
- Compile-time immutability tracking (`isImmutable` on locals and globals):
  assigning to a `let` binding throws a
  `CompileError` with the same message as the interpreter
- `print(expr)` → compile the expression, emit `PRINT` (temporary; see §3.6;
  every arity becomes a `CALL` to the builtin at 6c/6e); in the current AST
  `print` is a call, so it is detected as a `CallExpr`
- Register allocation with a counter that goes up and down
- Update `numRegisters` when reserving registers
- Reuse of constants in the tagged pool; `int` and `float` are distinct
  constants
- Nodes not yet supported throw `CompileError` with line/column

### PHASE 3 — `src/vm.cpp` (run bytecode) ✅
- Loop `while (pc < count)` + `switch` on the opcode
- `std::vector<Value> registers_` sized by `numRegisters`
- Global variable table `std::unordered_map<std::string, Value>`
- Numeric fast paths in `ADD`, `SUB`, `MUL`, `DIV` without walking variants
- `ADD` also concatenates strings; `DIV` and `MOD` detect division by zero
- `POW` replicates `intPow` for `int ** int` with a non-negative exponent and
  uses `std::pow` in every other case
- `UNOP` respects the interpreter's semantics (`0` is truthy) for negate, not
  and bit-not
- Comparisons with `EQ`/`NEQ`/`LT`/`LTE`/`GT`/`GTE` and the same ordering
  relation as `Interpreter::lessThan`
- `PRINT` uses `Value::toString()` (temporary; removed at the 6f gate, see
  §3.6)
- Runtime errors through `RuntimeError`
- `run()` resets the registers and keeps the globals between executions
- Each opcode: 2-3 lines of C++

### PHASE 4 — `src/main.cpp` (VM as the dev engine) ⬜ (runs after PHASE 6)
- Lua/LuaJIT model: `jolt`, `jolt -e` and the REPL **always** compile to
  bytecode and run on the VM as the **development mode**. The AST is never
  executed in production.
- REPL with **a single persistent VM**: each input is compiled to a chunk and
  run on the same globals.
- Temporary `--ast` only for differential tests (comparing AST vs VM); it is
  removed from the binary once parity is reached.
- `jolt -c file.jolt` compiles without running.
- `jolt build file.jolt` → PHASE 7 AOT backend: freestanding static native
  binary (no libc), no Jolt installation needed by end users.

### PHASE 5 — Control flow and locals ✅
- Opcodes: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
- Jumps with backpatching (absolute target inside `code`)
- `if`/`else` (including `else if`), `while`, classic `for`, `break` and
  `continue` (compile-time error outside a loop)
- Block scopes and local variables with slots (`DECLLOCAL`, `GETLOCAL`,
  `SETLOCAL`); `Bytecode::numLocals`
- Locals live in reference-counted `Cell`s so the PHASE 6 closures capture the
  variable, not just its value. A closure owns a reference to every cell it
  captures (`Function::upvalues`), so a cell lives exactly as long as its frame
  or a closure that captured it: the VM keeps no registry of captured cells
- Top-level `var`/`let` remain globals (they persist in the REPL); `const`
  bindings are folded to literals by the parser and never reach the bytecode
- `&&`, `||` with real short-circuiting (jumps), ternary with jumps
- Bitwise operators: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
- `+x` emits no instruction; `~x` requires an integer
- Do **NOT** add `OP_IF` or `OP_WHILE` (high level)
- `in` and `for-in` are left for PHASE 6 (they depend on collections)

### PHASE 6 — Functions, collections and library 🔶 in progress ← NEXT
- `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
- Call stack in the VM and registers per frame
- `Bytecode` evolves into **per-function protos** with child protos (Lua style)
- Closures with upvalues over `Cell` (locals are already cells)
- Shared `Runtime` interface for the VM and the AST interpreter so that the
  builtins (`map`, `filter`, `sort`) can invoke VM functions without
  re-entering the AST
- Arrays, maps, indexing, members, ranges, interpolation and `for-in`
- `in` for arrays, maps, strings and ranges
- Retire the temporary `PRINT` opcode: from 6c/6e every `print` arity compiles
  to `CALL`, and the opcode is deleted at the 6f gate (§3.6)
- Already in the tree: the shared `Value` and lexical `Env` (`value.cpp`, `env.cpp`) plus the cycle collector in `gc.cpp`. Slice 6a is done: top-level `fn` declarations, parameters, `return`, recursion and general calls compile and run on the VM. Slice 6b is done: `GETUPVAL`/`SETUPVAL`, upvalue descriptors, lambdas and closures that escape their defining scope (verified by `examples/closures.jolt` and `tests/09_lambdas.jolt`). The compiler still rejects collections and `for-in` with a `(Phase 6)` message; those land in 6c.
- Memory in the VM: closures that capture each other's variables, and tables that contain themselves (directly or through a closure stored in them), form cycles that reference counting cannot free. A `Cell` registers with the cycle collector when a closure captures it, and `Function` must report its `upvalues` (the captured cells) as well as `capturedGcNodes` from `visit`; a missing edge makes every cycle through a cell immortal. The VM runs `gcCollectIfNeeded()` at `CLOSURE` and `NEWTABLE`, the instructions that create the objects cycles are made of, so a program that never creates a closure is still collected. Regression tests: `testCapturedCellsAreNotRetainedByTheVM`, `testClosureCyclesAreCollected`, `testTableCyclesAreCollectedWithoutClosures` and `testClosureStoredInTheTableItCapturesIsCollected` in `tests/vm_test.cpp`; build it with `-DJOLT_GC_STRESS` to collect at every safe point.
- **6f done**: `PRINT` retired. Every `print` arity compiles to `CALL`. The
  opcode count is now 39/40.

### PHASE 6.5 — Types and nullability (1.0)

- Optional types: `let x: int = 5` — **done**: annotations on declarations,
  parameters and return types, checked at compile time and then **erased**
  (identical bytecode with or without the annotation, so the VM and the AOT
  backend never see a type). Accepted names are the `type()` primitives plus
  `any`; `int` widens to `float`. See `docs/language.md` §4.6 and
  `tests/16_types.jolt`.
- Local type inference: `let x = 5` (x: int) — **done**: the type is inferred
  from literal initializers and from other typed bindings, and later
  assignments are checked. A `nil` initializer stays untyped; an expression
  whose type is not evident is left unchecked.
- Nullability: `string?` — **done** as an annotation (a `?` type accepts
  `nil`). The expression forms are **done** too: `a ?? b` (null coalescing,
  short-circuiting), `a?.b` (safe access, read-only) and `a!!` (non-null
  assertion). Only `nil` triggers them (not truthiness), and none adds an
  opcode: `??`/`?.` lower to `EQ`/`JMP_IF_*`/`GETTABLE`/`MOVE`, `!!` to a call
  to the hidden `$nonNull` helper. See `docs/language.md` §4.6 and §5.8.1 and
  `tests/17_nullability.jolt`.
- `defer { ... }` — **done**: the deferred block is inlined at every exit from
  its scope (fall-through, `return`, `break`/`continue`), LIFO order, no new
  opcode and no VM support. `defer` is rejected outside a function, and a
  `return`/`break`/`continue` that would leave the deferred block itself is
  rejected. See `docs/language.md` §6.11 and `tests/15_defer.jolt`.
- **Modules: Java-style `import`** (`import std.math;`, `import std.math.*;`,
  `import std.math as m;`, `import { sqrt } from std.math;`) with compile-time
  conflict detection for `.*` and a reserved `std.` prefix. A module is loaded
  once, eagerly; a cyclic import (`a -> b -> a`, including a self-import) is a
  compile error rather than a stack overflow. `require` (Lua-style) is
  **dropped**: `import` is the only module form.
- **`struct`** (data only, no methods) — **done**: a named, module-level map
  shape with optional field types. The declaration emits no bytecode and adds
  no opcode; a `Point { x: 1, y: 2 }` literal is checked against the
  declaration (unknown, missing, duplicate or mistyped field is a compile
  error) and then lowered to the same `NEWTABLE`/`SETTABLE` sequence as a map
  literal, so a struct value is a plain map at runtime. A struct name is a
  valid annotation and is visible to the whole file (like a hoisted `fn`). See
  `docs/language.md` §4.7 and `tests/18_structs.jolt`.
- `switch` simple (literals and ranges) — **done**: no fallthrough, labels are
  literals or integer ranges, optional single `default`; compiles to the
  existing `EQ`/`IN`/`JMP` opcodes (no new opcode). See `docs/language.md` §6.10.
- **`native fn`** for internal FFI (stdlib in Jolt) — **done**: a bodyless,
  module-level declaration (`native fn sqrt(x: float): float;`) that binds a
  name to a function the host provides. It emits no bytecode and adds no
  opcode (the binding is a `GETGLOBAL` of the host function); the signature is
  checked at call sites (arity and argument types) and the declared return type
  feeds inference. A parameter with no annotation (or `any`) turns the check
  off for that argument. See `docs/language.md` §7.6 and
  `tests/19_native_fn.jolt`.
- **Variadic parameters** — **done**: a trailing `rest...` parameter
  (`fn sum(first, rest...)`) binds an array holding every extra argument; the
  `...` follows the optional element type (`rest...` or `parts: int...`) and
  must be the last parameter. A variadic function accepts `n - 1` or more
  arguments; the VM packs the extras into an array, so no opcode is added and
  the bytecode count stays 39/40. The same rule applies to `native fn`
  signatures. See `docs/language.md` §7.1 and `tests/20_variadic.jolt`.
- **Standard library: `std.math`** — **done**: the primitives that need C++
  (`sqrt`, `pow`, `sin`, `cos`, `tan`, `log`, `exp`, `floor`, `ceil`, `round`)
  are declared `native fn` in `std/math.jolt` and bind to the host builtins of
  the same name; `abs`, `min`, `max` (variadic) and `clamp` are plain Jolt, and
  `PI`/`E` are constants. Moving the four C++ builtins out keeps the host
  registry small while the standard library stays mostly Jolt. See
  `docs/language.md` §12.3/§18 and `tests/21_std_math.jolt`.

### PHASE 6.6 — FFI with C (1.0)

- `extern "C" { ... }` blocks.
- Compile-time stubs (like Zig/Rust).
- **No `libffi`**.
- Support for ints, floats, strings, structs, callbacks.

### PHASE 7 — Bytecode → native backend (AOT, freestanding) 🔶 in progress
- New module `src/native/` + `include/native/`: lowering of every opcode to
  x86-64 following the bytecode → native lowering table of §3.
- **Own machine-code and static ELF executable emitter** (~500-1000 lines:
  ELF header, program headers, `.text`, `.rodata`, entry point), keeping the
  zero-dependency rule of §5.1/§8. LIEF, AsmJit and LLVM were evaluated and
  rejected: LIEF is built to parse/modify binaries (not generate them), and
  AsmJit/LLVM break the no-external-dependencies rule.
- **Freestanding**: its own `_start` entry point (argc/argv read from the
  stack, `exit` via syscall), direct syscalls (`write`, `read`, `openat`,
  `close`, `mmap`, `brk`, `exit_group`), a custom arena/bump allocator on top
  of `mmap`, and no libc or libstdc++ linked into the generated binary —
  `ldd` must report "not a dynamic executable". Custom dtoa/formatting for
  printing floats in the `print` builtin.
- **Platform layer**: extract a minimal `Platform` interface from
  `vm.cpp`/runtime (write, read, alloc, exit, file I/O) with two
  implementations: `PlatformPosix` (dev engine; may use libc) and
  `PlatformFreestanding` (AOT; direct syscalls only). The `Runtime` of slice
  6a.0 (PHASE 6) must be built on this interface from the start.
- **Target descriptor from day one**: `include/native/target.h` defines
  `Target { arch, os, abi }` and every backend entry point takes one.
  `CodeGen` validates it and currently accepts only `x86_64-linux-sysv`, so
  adding Windows, macOS or WebAssembly is a new backend behind the same
  descriptor instead of a driver rewrite.
- Initial target: Linux x86-64 only (direct syscalls = one concrete ABI).
- Subslices:
  - **7a** — "hello world" spike: hand-written `_start` + `write` syscall →
    static freestanding ELF → `./hello` runs, no libc.
  - **7b** — lowering of the opcodes that need no runtime, integer-only:
    `LOADK` (int/bool), `MOVE`, `ADD`/`SUB`/`MUL`, `BAND`/`BOR`/`BXOR`,
    `UNOP`, `EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE` and `HALT`.
    R0–R11 map to x86-64 registers (r15 is the scratch); comparisons and
    `!x` take three instructions (`cmp`/`test` + `setcc` + `movzx`) because
    x86-64 cannot materialize a full-width 0/1 in one instruction — the
    documented exception to the 1:1/1:2 budget. Floats need value tags and
    move to 7c; `SHL`/`SHR` need `cl` (R1) and would clobber the window;
    `DIV`/`MOD`/`POW` need the error path and libm.
  - **7c** — freestanding embedded runtime calls (strings, the `print`
    builtin, collections on top of the custom allocator) and the boxed
    `Value` representation that unlocks floats and the rest of the opcodes.
    - **7c.0** (done) — the freestanding substrate: the static ELF64 emitter
      (`include/native/elf.h`, `src/native/elf.cpp`) and the PE32+ emitter
      (`include/native/pe.h`, `src/native/pe.cpp`); the bump allocator
      (`include/native/arena.h`, `src/native/arena.cpp`); `PlatformFreestanding`
      (`include/native/platform_freestanding.h`,
      `src/native/platform_freestanding.cpp`), the second `Platform`
      implementation, reaching the kernel through direct Linux x86-64 syscalls
      (`write`, `read`, `openat`, `close`, `lseek`, `mmap`, `exit_group`) with
      no libc; and the entry point (`include/native/entry.h`,
      `src/native/entry.cpp`), which appends the `_start` and the runtime
      prelude (`jolt_write`, `jolt_exit`) to the same code buffer as the
      program so every `call` resolves through the emitter's labels, with no
      relocations and no linker. Verified end to end in
      `tests/freestanding_test.cpp`: the test emits a program, wraps it in an
      ELF, writes it to disk, runs it and checks its stdout, exit code and
      that `ldd` reports no dynamic dependencies.
    - **7c.1+** — boxed `Value` over the arena, `print`/`toString` (own dtoa),
      collections and functions. **Open architectural question**: the hosted
      `Value` is reference counted and cycle collected (`gc.cpp`), which cannot
      live in a no-libc binary; the AOT runtime needs its own boxed value and
      its own arena-backed objects, sharing *semantics* with `ops.h` but not
      code. `PlatformFreestanding` currently pulls `operator delete` through
      `Platform`'s virtual destructor, so linking it into a `-nostdlib` binary
      needs either a tiny shim or a `-fno-exceptions`-friendly base — to be
      settled when 7c.1 first links the runtime into a generated binary.
  - **7d** — `jolt build` wired into the CLI.
- Deferred: per-instruction type annotations for fast paths without dynamic
  dispatch (bridge toward the speculative work in
  `docs/ARCHITECTURE_ANALYSIS.md`); other targets (macOS/Windows via libc or
  stubs).
- Afterwards, LuaJIT's JIT as a reference.
- **In the tree now**: the `Target` descriptor (`include/native/target.h`) and
  the x86-64 instruction emitter (`include/native/codegen.h`,
  `src/native/codegen.cpp`): labels with backpatching, `mov`/`lea`, integer
  ALU, shifts, compares, `setcc`/`movzx`, `jmp`/`jcc`, SSE2 float ops,
  `push`/`pop`, `call rel32`, `syscall` and `ret`, covered by the byte-exact
  tests in `tests/codegen_test.cpp` (the mmap/execution checks run on Linux
  x86-64 only). The 7b lowering (`include/native/lower.h`,
  `src/native/lower.cpp`) turns the runtime-free, integer-only opcodes into
  machine code, tested byte-exactly and by execution in `tests/lower_test.cpp`.
  7c.0 adds the freestanding substrate: the static ELF64 and PE32+ emitters,
  the arena, `PlatformFreestanding` (direct syscalls, no libc) and the
  `_start`/prelude, exercised end to end (a real generated binary is run) in
  `tests/freestanding_test.cpp`. Still missing: the boxed `Value` runtime
  (7c.1), the embedded runtime calls (7c.2/7c.3) and `jolt build` (7d).

### Post-1.0 — UI library

- **Skia** as rendering backend.
- **SDL2** for windows and events.
- **Declarative UI** (SwiftUI/Flutter style): `Window`, `Column`, `Row`,
  `Text`, `Button`, `Input`, `Checkbox`, `Slider`, etc.

### Post-1.0 — Other features

- **Cosmopolitan (APE)** for CLI multi-platform (optional).
- **Interfaces/traits** (for generic code without full generics).
- **Dynamic FFI** (`dlopen`, `dlsym`, runtime stubs with AsmJIT JIT).
- **Hot reload** (1.1+).
- **Extended stdlib**: JSON, dates, bytes, regex, networking.
- **Package manager** (`jolt install foo`) and registry.
- **Windows and macOS** (1.2+).
- **Embedded VM** (`--embed-vm`): AOT binary + VM for plugins, hot reload.

### Future — Static with `@Dynamic` (2.0)

- Static by default, `@Dynamic` for dynamic parts.
- Type inference, monomorphization.
- Requires: types (6.5), FFI (6.6), AOT (7).

### Current status

Phase 1: ✅ completed and reviewed
Phase 2: ✅ completed
Phase 3: ✅ completed
Phase 4: ⬜ pending (VM as the dev engine; requires PHASE 6)
Phase 5: ✅ completed and tested
Phase 6: ✅ completed (6a, 6b, 6c, 6d, 6f done)
Phase 6.5: ✅ completed (Java-style `import`, `switch`, `defer`, type annotations/inference/nullable `T?` and nullability expressions `?.`/`??`/`!!`, `struct`, `native fn`, variadic `rest...` parameters and the `std.math` module)
Phase 6.6: ⬜ pending (FFI with C)
Phase 7: 🔶 in progress (7a, 7b and 7c.0 done; 7c.1+, 7d pending)
Post-1.0: ⬜ pending (UI, Cosmopolitan, modules, package manager, VM embedded)
2.0: ⬜ future (static with `@Dynamic`)

**12 bugs fixed** (see `docs/BUGS.md` for the full list).

Distribution model: musl static by default (self-contained, no runtime).
Optional: newlib (bare-metal), freestanding (no libc).

VM = dev engine only. The AST interpreter leaves the product path at the 6f
parity gate and is deleted in PHASE 4.


---

## 5. Code rules

1. **Pure C++17**, no external dependencies **except optional AsmJIT** (for
   JIT) and **optional ELFIO** (for ELF emitter). The AOT product (`joltc`)
   uses AsmJIT Assembler (no JIT) and ELFIO, both header-only or lightweight.
2. **Names**: `camelCase` for functions, variables and members (private data
   members end with `_`), `PascalCase` for types, `UPPER_CASE` for opcodes
   and constants.
3. **Structures**: `struct` for data, `enum class` for opcodes.
4. **Memory**: RAII and standard containers (`std::vector`, `std::string`);
   no `malloc`/`free`.
5. **Errors**: diagnostic structs with line/column; the CLI prints them to
   **`stderr`** (via `Platform::writeErr`).
6. **Language**: all code, comments, documentation and commit messages are
   written in **English**.
7. **Every new `.cpp`** has its `.h` in `include/`.
8. **No mutable global variables** except the VM's globals table.
9. **Build with**: `g++ -std=c++17 -Wall -Wextra -O2`.
10. **Reference cycles**: values are reference counted and `gc.cpp` reclaims
    the cycles counting cannot. Anything that can own a `Value` and be
    reachable from itself is a `GcNode`, and its `visit` operation must report
    every strong reference it holds to another tracked object. **Do not add
    side owners** such as registries or caches of objects that can sit in a
    cycle: the collector sees them as references from outside, which makes the
    cycle immortal. Cells are tracked lazily (`GcNode::track`).
11. **Commit authorship**: the author and the committer of every commit are
    the repository owner (`yasmramos <yasmramos95@gmail.com>`). **Never** add
    AI attribution of any kind: no `Co-authored-by` trailer for an AI agent,
    no "Generated by" or "Created with" line, no AI tool name, and no
    `openhands` identity. Commits are made with the owner's name and email
    only, e.g. `git -c user.name=yasmramos -c user.email=yasmramos95@gmail.com
    commit`. This overrides any default agent commit convention.

---

## 6. Mandatory checklist for every new feature

Before writing **a single line of code**, the agent must answer and show the
user:

- Do I need a new token in the lexer? → which one
- Do I need a new node in the AST? → which one
- Do I need a new rule in the parser? → which one
- Do I need a new opcode in the bytecode? → which one
- Does the opcode map to 1:1 or 1:2 ASM instructions? → yes/no (if 1:3+, redesign)
- Do I need new logic in the VM? → which one
- Do I need new logic in the compiler? → which one

Without this checklist complete and approved, **do not proceed to code**.

---

## 7. Agent response style

- ✅ Explain the **concept** before the code.
- ✅ Show the **complete** modified files (not fragments).
- ✅ Include a test `.jolt` program with every new feature.
- ✅ Point out what **NOT** to do and why (avoid Java's mistake).
- ✅ Warn when a decision affects the future design (native compilation).
- ❌ Do not add unrequested features (avoid scope creep).
- ❌ Do not propose solutions outside the current phases.

---

## 8. Explicit prohibitions

| Prohibited | Reason |
|-----------|--------|
| Stack-based VM | Jolt is register-based (Lua style) |
| High-level bytecode (`OP_EVAL_EXPR`) | Breaks future native compilation |
| **JIT in the VM** (JIT is for `jolt` only, not `joltc`) | The product is AOT; JIT is a dev tool |
| Migrating the core to pure C | The core stays in C++17 |
| Adding features without testing the previous one | Breaks incremental development |
| Skipping the 7-point checklist | Creates architectural debt |
| **External libraries beyond AsmJIT/ELFIO** | AsmJIT and ELFIO are the only allowed dependencies |
| Superinstructions | Violate the 40-opcode ceiling |
| Inline caching | Changes the bytecode contract |
| Runtime specialization | Changes the bytecode contract |
| `async`/`await`, generics, full pattern matching, generators, transactions | Out of scope |

**Note**: AsmJIT and ELFIO are **optional** dependencies. The AOT product
(`joltc`) uses AsmJIT Assembler (no JIT, `ASMJIT_NO_JIT`) and ELFIO. The
development VM (`jolt`) may use AsmJIT JIT. **The end-user binary (`hello.exe`)
contains neither.**

---

## 9. Commands the agent must recognize

| User command | Agent action |
|---------------------|-------------------|
| `start` | Begin PHASE 1: show the complete `bytecode.h` and wait for approval |
| `phase N done` | Verify, update the status, propose PHASE N+1 |
| `add <feature>` | Apply the 7-point checklist, then code |
| `review <file>` | Verify against the bytecode rules, point out violations |
| `status` | Show a summary of the phases and the checklist |
| `summary` | Summarize what was done in the session and update the status |

---

## 10. Historical context

- There is a **previous AST interpreter** (MiniLang, renamed Jolt) that is
  kept only as a **differential test oracle** while parity with the VM is
  being reached.
- The product model is Lua/LuaJIT's: source → lexer → parser → AST → compiler
  → bytecode → VM. The AST is **not executed** in production.
- The "dual" approach (AST interpreter as an alternative engine) is **NOT**
  preserved. Once the VM covers the language (PHASE 4), the AST interpreter
  leaves the binary and stays only in tests, and is retired afterwards.
- The phases run in order: 5 ✅, 6, then 4 (VM as the dev engine) and 7 (AOT
  native backend).

---
## 11. File extensions and packaging

| Extension | What it is | Java analog |
|-----------|------------|-------------|
| `.jolt` | Source code | `.java` |
| `.jolc` | Bytecode | `.class` |
| `.jpkg` | Package (ZIP) | `.jar` |
| `.jlib` | Native library | `.dll`/`.so`/`.dylib` |
| `.jdoc` | Documentation | `.javadoc` |
| `myapp` | Native binary (Linux/macOS) | (GraalVM) `.exe` |
| `myapp.exe` | Native binary (Windows) | (GraalVM) `.exe` |

**`.jpkg` format** (ZIP):
myapp.jpkg
├── META-INF/MANIFEST.MF
├── bin/main.jolc
├── lib/mylib.jlib
├── resources/icon.png
└── README.md


**`.jolc` format** (binary):
+--------+--------+--------+--------+
| 'J' | 'O' | 'L' | 'C' | magic
+--------+--------+--------+--------+
| version (u32) |
+--------+--------+--------+--------+
| flags (u32) |
+--------+--------+--------+--------+
| name_len (u32) + name |
+--------+--------+--------+--------+
| numParams, numRegisters, numLocals|
+--------+--------+--------+--------+
| code_size + code |
+--------+--------+--------+--------+
| constants_size + constants |
+--------+--------+--------+--------+
| children_size + children |
+--------+--------+--------+--------+
| upvalues_size + upvalues |
+--------+--------+--------+--------+
| checksum (u32) |
+--------+--------+--------+--------+


**Bytecode verification** (on `.jolc` load):
- **Level 1 (structural)**: mandatory. Magic, version, ranges, valid opcodes,
  operands in range, checksum.
- **Level 2 (semantic)**: optional. Jumps to valid instructions, registers
  initialized.
- **Digital signature**: post-1.0, if the threat model requires it.

---

## 12. Reference resources (for the agent)

- *Crafting Interpreters* (Robert Nystrom) — fundamentals
- *The Implementation of Lua 5.0* — register VM design
- LuaJIT source code (Mike Pall) — performance reference
- System V ABI AMD64 — calling convention (PHASE 7)
- AsmJIT documentation — JIT and Assembler
- ELFIO documentation — ELF emitter
- AXL documentation — arena allocator and low-level utilities
- Cosmopolitan Libc — multi-platform CLI (optional)

---

## 13. First task

When the user types **`start`**, the agent must:

1. Confirm the design of `include/bytecode.h` with the minimum opcodes.
2. Show the **complete file**.
3. Explain why each opcode maps to 1:1 or 1:2 ASM (or 1 call).
4. Wait for **explicit approval** before moving on to the next phase.
