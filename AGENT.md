# AGENT.md — Jolt Project

Instructions for AI agents assisting in the development of **Jolt**, a compiled
programming language with a Lua-style internal architecture.

---

## 1. Project identity

| Field | Value |
|-------|-------|
| **Name** | Jolt |
| **Extension** | `.jolt` |
| **Philosophy** | Simple like Python, structured like Java, fast like Lua |
| **Implementation** | C++17 (no external dependencies) |
| **Initial platform** | Windows (g++/MinGW), then cross-platform |
| **Architecture** | Register-based bytecode + VM (Lua 5.4 style) |

---

## 2. Mandatory architecture

```text
.jolt → LEXER → PARSER → AST → COMPILER → BYTECODE ─┬─→ VM (dev engine)
                                                     └─→ AOT BACKEND → freestanding static native binary
```

The VM is a development tool (REPL, fast iteration, differential testing).
`jolt build` produces a self-contained freestanding binary (no libc, no
runtime to install); end users never install Jolt or any runtime.

### Modules and responsibilities

| Module | Input | Output | Status |
|--------|-------|--------|--------|
| `lexer.cpp` | source text | `Token[]` | ✅ existing |
| `parser.cpp` | `Token[]` | `Node*` (AST) | ✅ existing |
| `bytecode.cpp` | `Bytecode` | basic operations | ✅ PHASE 1 |
| `compiler.cpp` | `Node*` | `Bytecode*` | ✅ PHASE 5 |
| `vm.cpp` | `Bytecode*` | execution | ✅ PHASE 5 |
| `main.cpp` | CLI | full pipeline | ⬜ PHASE 4 (VM as the engine) |
| `interpreter.cpp` | `Node*` | reference execution | ⚠️ test oracle only |
| `value.cpp` / `env.cpp` | `Value`, scope chain | shared runtime: values and lexical `Env` | ✅ PHASE 6 groundwork |
| `gc.cpp` | tracked `GcNode`s | reclamation of reference cycles (arrays, maps, functions, scopes, captured cells) for the interpreter and the VM | ✅ PHASE 6 |
| `native/codegen.cpp` | x86-64 instructions | machine-code bytes | 🔶 PHASE 7a (emitter only) |

### Toolchain binaries and the compiler boundary

`jolt` ships as a single binary until PHASE 7d. Split the toolchain only once
`jolt build` exists and something actually consumes it (CI, third-party
builds); end users never install Jolt (PHASE 7), so an early split adds
packaging and build cost without removing any user-facing surface.

The split, when it happens:

- `jolt` keeps run, REPL and `-c`: the development engine.
- `joltc` becomes the host compiler (`joltc file.jolt -o app`). It reads
  sources and writes the ELF, so it links libc; the freestanding part is what
  it emits, not the compiler itself.
- `jolt build` stays a thin alias over the same library, never a second
  implementation of the pipeline.

Boundary rule, in force before the split: `src/compiler.*` and `src/native/*`
must not include `interpreter.h` or `vm.h`. Both are libraries that any entry
point may link (the `compiler_test` target already builds the compiler without
the interpreter), which is what makes the later split a second `main` instead
of a refactor. The `compiler_boundary` CTest case
(tests/check_compiler_boundary.cmake) enforces the rule on every run.

Rationale: it keeps the PHASE 7 anti-goal (no compiler and no eval reachable
from the AOT runtime) enforceable mechanically, and it keeps the compiler
reusable by a leaner entry point.

---

## 3. Bytecode rules (CRITICAL)

These rules are **not negotiable**. If a proposal violates them, reject it
and propose an alternative.

### 3.1 Representation

1. **Register-based**, NOT stack-based. Registers are an array of dynamic
   values in the VM (`Value regs[256]`). Do not use `double` alone: Jolt also
   needs to represent strings, booleans, `nil`, arrays and maps. All
   registers, global variables, frames and temporaries use the same `Value`
   type. `Bytecode::constants` is a single tagged constant pool shared by the
   compiler and the VM (numbers, strings and global names); the VM does not
   keep separate registers per type.
   Arithmetic operations must include fast paths for `int` and `float`.
   In C++17, `Value` is a 16-byte tagged union with non-atomic reference
   counting (the interpreter/VM is single-threaded). Strings are immutable
   and shared, so `MOVE` does not duplicate text.

### 3.2 Lowering tiers (the native backend contract)

2. **Every opcode has exactly one direct lowering**, but not every lowering is
   a single machine instruction. There are two tiers:

   | Tier | Lowering | Opcodes |
   |------|----------|---------|
   | **Inline** | 1–3 instructions that read/write registers and frame cells, no call | `LOADK`, `MOVE`, `ADD`–`POW` (int/float fast paths), `UNOP`, `BAND`–`SHR`, `EQ`–`GTE`, `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`, `GETLOCAL`, `SETLOCAL` |
   | **Runtime primitive** | Load arguments into ABI registers and `call` one named helper in the embedded runtime | `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`, `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`, `ITER`, `PRINT`, `DECLLOCAL`, `GETGLOBAL`, `SETGLOBAL` (until globals are resolved to fixed slots) |

   A runtime primitive still satisfies the anti-goal: it is one small typed
   operation (e.g. `jolt_table_get`), never the compiler, the AST or `eval`.
   **Do not claim 1:1 ASM for the second tier.** An earlier version of this
   document did, which contradicted the PHASE 7c plan; the table in
   "Expected bytecode → native lowering" is now the normative one.

   Some inline opcodes also fall back to a runtime primitive on the generic
   path (`ADD` with strings, comparisons of non-numbers). That fallback is
   part of the same opcode's lowering, not a second opcode.

### 3.3 Opcode budget and retirement ladder

3. **At most 40 opcodes** in total. **40 is a hard ceiling, not a target.**
   PHASE 6 sits exactly at it (40/40), so there is no free slot. The policy for
   the next opcode is fixed now:

   - A proposal that needs a new opcode must name the rung it consumes **and**
     land that consolidation in the same slice, with all suites green.
   - The ladder is ordered; do not skip a rung to protect a favorite opcode.
     A rung may only be skipped if it has already been executed.
   - Never emit a 41st opcode, not even temporarily.
   - If the feature can be expressed with the current set (`switch` and
     pattern matching lower to `JMP_IF_FALSE`/`JMP`), it gets no opcode.

   The ceiling is enforced at compile time: `include/bytecode.h` has
   `static_assert(kOpcodeCount <= 40)`, so a 41st enumerator fails the build
   before it can land. `PRINT`'s deletion (39) is the only planned decrease.

   | Rung | Consolidation | Saves | Notes |
   |------|---------------|-------|-------|
   | 1 | `EQ` `NEQ` `LT` `LTE` `GT` `GTE` → `CMP R[a], R[b], R[c]` with the operator in `c` | -5 | Same selector trick already used by `UNOP`; native code still lowers to `cmp` + `setcc`. **Pre-approved default answer** to "which opcode is dropped". |
   | 2 | `JMP_IF_FALSE` + `JMP_IF_TRUE` → `JMP_IF` with polarity in `c` | -1 | One `test` + one `jcc` either way. |
   | 3 | `BAND` `BOR` `BXOR` `SHL` `SHR` → `BITOP` with the operator in `c` | -4 | The ASM already selects the operation. |
   | 4 | `ADD` `SUB` `MUL` `DIV` `MOD` `POW` → `ARITH` with the operator in `c` | -5 | Last resort: one dispatch in the hot path instead of six. |

   At 40/40, **any** feature that wants a new opcode starts at rung 1. If a
   future review prefers 20–25 opcodes, this ladder is the route there; it is
   not a redesign.

### 3.4 Forbidden instructions

4. **Forbidden instructions** (too high level):
   - `OP_EVAL_EXPR`, `OP_EXECUTE_STATEMENT`, `OP_CALL_METHOD_DYNAMIC`

### 3.5 Allowed instructions (the complete 40)

5. **Allowed instructions** (low level):
   - Load: `LOADK` (tagged constant: nil, bool, int, float, string), `MOVE`
   - Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `UNOP` (`c` selects
     negate/not/bit-not)
   - Bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`
   - Comparison: `EQ`, `NEQ`, `LT`, `GT`, `LTE`, `GTE`
   - Variables: `GETGLOBAL`, `SETGLOBAL`, `DECLLOCAL`, `GETLOCAL`, `SETLOCAL`
   - Control: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
   - Functions: `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
   - Collections: `NEWTABLE`, `GETTABLE`, `SETTABLE`, `IN`
   - Iteration: `ITER`
   - I/O: `PRINT` (**temporary**, see §3.6)
   - End: `HALT`

### 3.6 `PRINT` is temporary

`PRINT` is the only opcode that touches the outside world. It is a deliberate,
time-boxed exception to the rule that the core knows no std, and it is
**scheduled for deletion**, not kept:

- 6c/6e: the compiler emits `CALL` to the `print` builtin for every arity, so
  no new code contains `PRINT`.
- 6f: the `OpCode::PRINT` case and its VM implementation are removed. This is
  part of the parity gate (D9 in `docs/phase6-design.md`), not an optional
  cleanup.
- 7c: the freestanding runtime provides the same `print` builtin on
  `PlatformFreestanding`; I/O goes through `Platform::write`, never through an
  opcode.

Removing `PRINT` frees one slot (39/40). It does **not** postpone rung 1 of
§3.3: the ladder applies to any opcode requested before 6f, and `PRINT` is
already promised to the gate.

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
- Memory in the VM: closures that capture each other's variables form cycles through `Cell`s, which reference counting cannot free. A `Cell` registers with the cycle collector when a closure captures it, `Function`s report their upvalue cells to it, and the VM runs `gcCollectIfNeeded()` at `CLOSURE` (the place where such cycles are created). Regression tests: `testCapturedCellsAreNotRetainedByTheVM` and `testClosureCyclesAreCollected` in `tests/vm_test.cpp`; build it with `-DJOLT_GC_STRESS` to collect at every safe point.

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
  - **7b** — lowering of opcodes that need no runtime (int/float arithmetic,
    jumps, comparisons).
  - **7c** — freestanding embedded runtime calls (strings, the `print`
    builtin, collections on top of the custom allocator).
  - **7d** — `jolt build` wired into the CLI.
- Deferred: per-instruction type annotations for fast paths without dynamic
  dispatch (bridge toward the speculative work in
  `docs/ARCHITECTURE_ANALYSIS.md`); other targets (macOS/Windows via libc or
  stubs).
- Afterwards, LuaJIT's JIT as a reference.
- **In the tree now**: the `Target` descriptor (`include/native/target.h`) and
  the x86-64 instruction emitter (`include/native/codegen.h`,
  `src/native/codegen.cpp`): labels with backpatching, `mov`/`lea`, integer
  ALU, shifts, compares, `jmp`/`jcc`, SSE2 float ops, `push`/`pop`, `call
  rel32`, `syscall` and `ret`, covered by the byte-exact tests in
  `tests/codegen_test.cpp` (the mmap/execution checks run on Linux x86-64
  only). Still missing: opcode→emitter lowering, the ELF/freestanding emitter,
  `_start`, the arena allocator, the `Platform` layer and `jolt build`.

### Current status
Phase 1: ✅ completed and reviewed
Phase 2: ✅ completed
Phase 3: ✅ completed
Phase 4: ⬜ pending (VM as the dev engine; requires PHASE 6)
Phase 5: ✅ completed and tested
Phase 6: 🔶 in progress ← NEXT (6a.0, 6a and 6b done: shared `Runtime`, per-function protos, `CALL`/`RETURN`/`CLOSURE`, top-level functions, recursion, upvalues, lambdas and closures, with closure cycles reclaimed by the collector; collections and `for-in` are still missing)
Phase 7: 🔶 in progress (7a: target descriptor + x86-64 instruction emitter with tests; no ELF emitter, freestanding runtime or `jolt build` yet)
Distribution model: freestanding AOT native binary (PHASE 7, Linux x86-64).
VM = dev engine only. The AST interpreter leaves the product path at the 6f
parity gate (D9 in `docs/phase6-design.md`) and is deleted in PHASE 4.


---

## 5. Code rules

1. **Pure C++17**, no external dependencies (standard library only).
2. **Names**: `camelCase` for functions, variables and members (private data
   members end with `_`), `PascalCase` for types, and `UPPER_CASE` for opcodes
   and constants. The Jolt standard library follows the same rule
   (`startsWith`, `readFile`, `assertEq`).
3. **Structures**: `struct` for data and `enum class` for opcodes.
4. **Memory**: RAII and standard containers (`std::vector`, `std::string`); no `malloc`/`free`.
5. **Errors**: diagnostic structs with line/column; the CLI prints them to `stderr`.
6. **Language**: all code, comments, documentation and commit messages are
   written in English (see `CONVENTIONS.md`). Comments are concise and only
   where they add value.
7. **Every new `.cpp`** has its `.h` in `include/`.
8. **No mutable global variables** except the VM's globals table.
9. **Build with**: `g++ -std=c++17 -Wall -Wextra -O2`
10. **Reference cycles**: values are reference counted and `gc.cpp` reclaims the
    cycles counting cannot. Anything that can own a `Value` and be reachable from
    itself is a `GcNode`, and its `visit` operation must report every strong
    reference it holds to another tracked object (a missing edge keeps garbage
    alive, it never frees live data). Do not add side owners such as registries
    or caches of objects that can sit in a cycle: the collector sees them as
    references from outside, which makes the cycle immortal. Cells are tracked
    lazily (`GcNode::track`) because every local variable has one.

---

## 6. Mandatory checklist for every new feature

Before writing **a single line of code**, the agent must answer and show the
user:

Do I need a new token in the lexer? → which one

Do I need a new node in the AST? → which one

Do I need a new rule in the parser? → which one

Do I need a new opcode in the bytecode? → which one

Does the opcode map to 1 ASM instruction? → yes/no (if not, redesign)

Do I need new logic in the VM? → which one

Do I need new logic in the compiler? → which one

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

The agent **must NOT** propose or implement:

| Prohibited | Reason |
|-----------|--------|
| Stack-based VM | Jolt is register-based (Lua style) |
| High-level bytecode (`OP_EVAL_EXPR`) | Breaks future native compilation |
| JIT (hot compiler) | Level 6, years of work, not a current goal |
| Migrating the core to pure C | The core stays in C++17 |
| Adding features without testing the previous one | Breaks incremental development |
| Skipping the 7-point checklist | Creates architectural debt |
| External libraries | C++ standard library only |

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

## 11. Reference resources (for the agent)

- *Crafting Interpreters* (Robert Nystrom) — fundamentals
- *The Implementation of Lua 5.0* — register VM design
- LuaJIT source code (Mike Pall) — performance reference
- System V ABI AMD64 — calling convention (PHASE 7)

---

## 12. First task

When the user types **`start`**, the agent must:

1. Confirm the design of `include/bytecode.h` with the minimum opcodes.
2. Show the **complete file**.
3. Explain why each opcode maps to ASM.
4. Wait for **explicit approval** before moving on to the next phase.
