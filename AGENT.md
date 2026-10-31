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
.jolt → LEXER → PARSER → AST → COMPILER → BYTECODE → VM
                                                  ↓
                                   (future) NATIVE COMPILER
```

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

---

## 3. Bytecode rules (CRITICAL)

These rules are **not negotiable**. If a proposal violates them, reject it
and propose an alternative.

1. **Register-based**, NOT stack-based. Registers are an array of dynamic
   values in the VM (`Value regs[256]`). Do not use `double` alone: Jolt also
   needs to represent strings, booleans, `nil`, arrays and maps. All
   registers, global variables, frames and temporaries use the same `Value`
   type. `Bytecode::numbers` and `Bytecode::strings` are only constant pools
   for the compiler; the VM does not keep separate registers per type.
   Arithmetic operations must include fast paths for `int` and `float`.
   In C++17, `Value` is a 16-byte tagged union with non-atomic reference
   counting (the interpreter/VM is single-threaded). Strings are immutable
   and shared, so `MOVE` does not duplicate text.
2. **Each opcode maps to 1 x86-64 ASM instruction** (or close to it). If it
   does not map, it is badly designed.
3. **At most ~40 opcodes** in total. If more are needed, redesign.
4. **Forbidden instructions** (too high level):
   - `OP_EVAL_EXPR`, `OP_EXECUTE_STATEMENT`, `OP_CALL_METHOD_DYNAMIC`
5. **Allowed instructions** (low level):
   - Load: `LOADK` (number), `LOADKS` (string), `LOADNIL`, `LOADBOOL`, `MOVE`
   - Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `NEG`, `NOT`
   - Bits: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
   - Comparison: `EQ`, `NEQ`, `LT`, `GT`, `LTE`, `GTE`
   - Variables: `GETGLOBAL`, `SETGLOBAL`, `DECLLOCAL`, `GETLOCAL`, `SETLOCAL`
   - Control: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
   - Functions: `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
   - I/O: `PRINT`
   - End: `HALT`

### Explicit anti-goal

> **Do NOT repeat Java's mistake**: high-level bytecode that later turns out
> to be impossible to compile to native code efficiently. Jolt's bytecode
> must be born with future native compilation in mind.

### Expected bytecode → ASM mapping

| Bytecode | x86-64 ASM |
|----------|-----------|
| `LOADK R0, 10` | `movq $10, %rax` |
| `ADD R2, R0, R1` | `addq %rbx, %rax` |
| `JMP_IF_FALSE R2, +2` | `testq %rax, %rax; jz .label` |
| `SETGLOBAL R2, "x"` | `movq %rax, x(%rip)` |
| `PRINT R3` | `call printf` |

---

## 4. Development phases (strict order)

You cannot move on to the next phase without completing and testing the
previous one.

### PHASE 1 — `include/bytecode.h` ✅
- Define `enum class OpCode` with: `LOADK`, `LOADKS`, `LOADNIL`, `LOADBOOL`,
  `MOVE`, `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `POW`, `NEG`, `NOT`, `EQ`, `NEQ`,
  `LT`, `LTE`, `GT`, `GTE`, `SETGLOBAL`, `GETGLOBAL`, `PRINT`, `HALT`
- Define `struct Instruction { OpCode op; int a, b, c; int jump; }`
- Define `struct Bytecode` with `code`, `numbers`, `strings` and `num_registers`
- `numbers` is a `std::vector<Value>` to preserve the difference between `int` and `float`
- `LOADK` loads numeric constants and `LOADKS` string constants (separate
  opcodes, without using `c` as a discriminator)
- Functions: `bytecode_init`, `bytecode_emit`, `bytecode_free` (declared in
  `include/bytecode.h`, implemented in `src/bytecode.cpp`)

### PHASE 2 — `src/compiler.cpp` (AST → bytecode) ✅
- Literals: `nil` → `LOADNIL`, booleans → `LOADBOOL`, numbers → `LOADK`,
  strings → `LOADKS`
- `VariableExpr` → `GETGLOBAL`
- `BinaryExpr` with `+`, `-`, `*`, `/`, `%`, `**` and comparisons (`==`, `!=`,
  `<`, `<=`, `>`, `>=`) → compile left, compile right, emit the opcode
- `UnaryExpr`: `-` → `NEG`, `!` → `NOT`, `+` emits no instruction, `~` stays
  reserved for PHASE 5
- `&&`, `||`, `in` and bitwise operators → PHASE 5
- `AssignExpr` on a global variable → compile the expression, emit `SETGLOBAL`
- Compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`) → `GETGLOBAL`,
  operation, `SETGLOBAL`
- `LetStmt` → compile the initializer; if there is none, emit an explicit
  `LOADNIL`; then `SETGLOBAL`
- Compile-time `const` tracking (`consts_`): reassigning a constant throws a
  `CompileError` with the same message as the interpreter
- `print(expr)` → compile the expression, emit `PRINT`; in the current AST
  `print` is a call, so it is detected as a `CallExpr`
- Register allocation with a counter that goes up and down
- Update `num_registers` when reserving registers
- Reuse of constants in the number and string pools; `int` and `float` are
  distinct constants
- Nodes not yet supported throw `CompileError` with line/column

### PHASE 3 — `src/vm.cpp` (run bytecode) ✅
- Loop `while (pc < count)` + `switch` on the opcode
- `std::vector<Value> registers_` sized by `num_registers`
- Global variable table `std::unordered_map<std::string, Value>`
- Numeric fast paths in `ADD`, `SUB`, `MUL`, `DIV` without walking variants
- `ADD` also concatenates strings; `DIV` and `MOD` detect division by zero
- `POW` replicates `intPow` for `int ** int` with a non-negative exponent and
  uses `std::pow` in every other case
- `NEG` and `NOT` respect the interpreter's semantics (`0` is truthy)
- Comparisons with `EQ`/`NEQ`/`LT`/`LTE`/`GT`/`GTE` and the same ordering
  relation as `Interpreter::lessThan`
- `PRINT` uses `Value::toString()`
- Runtime errors through `RuntimeError`
- `run()` resets the registers and keeps the globals between executions
- Each opcode: 2-3 lines of C++

### PHASE 4 — `src/main.cpp` (VM as the only engine) ⬜ (runs after PHASE 6)
- Lua/LuaJIT model: `jolt`, `jolt -e` and the REPL **always** compile to
  bytecode and run on the VM. The AST is never executed in production.
- REPL with **a single persistent VM**: each input is compiled to a chunk and
  run on the same globals.
- Temporary `--ast` only for differential tests (comparing AST vs VM); it is
  removed from the binary once parity is reached.
- `jolt -c file.jolt` compiles without running.
- `jolt build file.jolt` (future) → generate C++ + g++

### PHASE 5 — Control flow and locals ✅
- Opcodes: `JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`
- Jumps with backpatching (absolute target inside `code`)
- `if`/`else` (including `else if`), `while`, classic `for`, `break` and
  `continue` (compile-time error outside a loop)
- Block scopes and local variables with slots (`DECLLOCAL`, `GETLOCAL`,
  `SETLOCAL`); `Bytecode::num_locals`
- Locals live in reference-counted `Cell`s so the PHASE 6 closures capture the
  variable, not just its value
- Top-level `let`/`const` remain globals (they persist in the REPL)
- `&&`, `||` with real short-circuiting (jumps), ternary with jumps
- Bitwise operators: `BAND`, `BOR`, `BXOR`, `SHL`, `SHR`, `BNOT`
- `+x` emits no instruction; `~x` requires an integer
- Do **NOT** add `OP_IF` or `OP_WHILE` (high level)
- `in` and `for-in` are left for PHASE 6 (they depend on collections)

### PHASE 6 — Functions, collections and library ⬜ ← NEXT
- `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL`
- Call stack in the VM and registers per frame
- `Bytecode` evolves into **per-function protos** with child protos (Lua style)
- Closures with upvalues over `Cell` (locals are already cells)
- Shared `Runtime` interface for the VM and the AST interpreter so that the
  builtins (`map`, `filter`, `sort`) can invoke VM functions without
  re-entering the AST
- Arrays, maps, indexing, members, ranges, interpolation and `for-in`
- `in` for arrays, maps, strings and ranges

### PHASE 7 — Bytecode → C++ compiler (future) ⬜
- Translate each instruction into one line of C++
- `g++ -O2` produces the native binary
- Evaluate `union`/`variant` for `Instruction` and drop `jump` when unused
- Afterwards, LuaJIT's JIT as a reference

### Current status
Phase 1: ✅ completed and reviewed
Phase 2: ✅ completed
Phase 3: ✅ completed
Phase 4: ⬜ pending (VM as the only engine; requires PHASE 6)
Phase 5: ✅ completed and tested
Phase 6: ⬜ pending ← NEXT
Phase 7: ⬜ pending


---

## 5. Code rules

1. **Pure C++17**, no external dependencies (standard library only).
2. **Names**: `snake_case` for functions and variables.
3. **Structures**: `struct` for data and `enum class` for opcodes.
4. **Memory**: RAII and standard containers (`std::vector`, `std::string`); no `malloc`/`free`.
5. **Errors**: diagnostic structs with line/column; the CLI prints them to `stderr`.
6. **Language**: all code, comments, documentation and commit messages are
   written in English (see `CONVENTIONS.md`). Comments are concise and only
   where they add value.
7. **Every new `.cpp`** has its `.h` in `include/`.
8. **No mutable global variables** except the VM's globals table.
9. **Build with**: `g++ -std=c++17 -Wall -Wextra -O2`

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
- The phases run in order: 5 ✅, 6, then 4 (VM as the only engine) and 7.

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
