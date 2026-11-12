# Jolt — Project Analysis

Date: 2026-11-08 · Branch: `develop` · Language version: **0.1.0**

This analysis reflects the **actual code in the repository** (about 12,300
lines of engine C++ under `src/` and `include/`, plus about 6,300 lines for the
AOT backend under `src/native/` and `include/native/`). The project mirrors the
Lua / LuaJIT pipeline: `source → lexer → parser → AST → compiler →
register-based bytecode → VM`, with an AOT backend in progress (PHASE 7).

It supersedes the 2026-10-31 analysis, which described a state the tree has
since left behind (a tree-walking AST interpreter as the production engine, 17
tests and no CI).

---

## 1. Current state (verified)

| Area | Status | Evidence |
|------|--------|----------|
| Lexer / Parser / AST | Working, tested | `src/lexer.cpp`, `src/parser.cpp` (1,742 L), `src/ast.h` |
| Compiler AST→bytecode | PHASE 6.7 complete | `src/compiler.cpp` (2,853 L): literals, locals/globals, control flow, closures, collections, types, modules |
| Register-based VM | PHASE 6 complete | `src/vm.cpp` (815 L); **39/40 opcodes** (`static_assert(kOpcodeCount <= 40)`) |
| AST interpreter | **Retired** | no `src/interpreter.cpp`; the VM is the only engine |
| Shared runtime | Working | `src/value.cpp`, `src/ops.cpp` (operators), `src/builtins.cpp` (1,524 L), `src/env.cpp`, `src/gc.cpp` |
| AOT backend | PHASE 7 in progress (7a, 7b, 7c.0–7c.2) | `src/native/*` (codegen, lower, runtime, dtoa, elf, pe, arena, freestanding platform, entry) |
| Tests | 50 `.jolt` + 100 error `.jolt` + 4 check `.jolt` + 9 examples + 12 C++ suites | `tests/`, `tests/errors/`, `tests/check/`, `examples/`, wired into CTest |
| CI | Two workflows | `.github/workflows/{cmake-multi-platform,artifacts}.yml` (Linux, Windows, macOS) |

Build (`sh build.sh`) and the full CTest suite run clean on Linux x86-64, and
`tests/run.sh` now mirrors CTest: it runs the same `.jolt` scripts (including
`examples/` and the `-c` check tests) and skips `32_ffi_callback.jolt` when
`libffi` is absent (see §3.1).

---

## 2. Findings

Each finding records its current status, verified against the tree.

### 2.1 Reference-counting cycle leak — **resolved**
The old report feared that `RefCounted`'s virtual destructor would leave cycles
uncollected. The design changed: `RefCounted` no longer has a virtual
destructor; it stores a per-type `destroy` hook and objects are freed through
`destroyRefCounted`, which walks a work list iteratively so a deep structure is
released with constant C++ stack depth. A cycle collector (`src/gc.cpp`)
reclaims the cycles counting cannot, and `Cell`s register lazily. Covered by
`tests/11_cycles.jolt` and the `gc_test`/`vm_test` suites.

### 2.2 Duplicated arithmetic semantics — **resolved**
Arithmetic now lives in one place, `src/ops.cpp` (`intPow`, `add`, comparisons,
…), shared by the VM and the builtins. The retired interpreter's copy is gone.
Integer `const`-fold overflow is now reported with a warning instead of wrapping
silently (see `docs/language.md` §4.1).

### 2.3 Integer-overflow semantics — **documented, bounded**
`intPow` wraps through `uint64_t` (defined behavior). The negative-exponent
"huge loop count" landmine the old report flagged is not reachable: `intPow` is
only called when the integer exponent is `>= 0`; a negative exponent takes the
`std::pow` float path (`src/ops.cpp`). The chosen semantics (Jolt wraps silently
on `int` overflow; `const` folding warns) are documented.

### 2.4 Braceless single-statement bodies — **resolved**
`if`, `while`, `for` and `for-in` accept a block or a single statement without
braces (the brace-less body is wrapped in a block, so bytecode and scoping
match). `switch` arms still require braces. See `docs/language.md` §6 and
`tests/38_braceless_bodies.jolt`.

### 2.5 Collection API consistency — **open (minor)**
Maps expose `keys`/`values`/`entries`/`has`/`get`/`set`/`delete` and arrays
expose `push`/`pop`/`shift`/`unshift`/`insert`/`removeAt`/`slice`/`concat`/
`join`/`sort`/`sorted`/`reversed`/`contains`/`indexOf`/`map`/`filter`/`reduce`/
`each`/`find`/`any`/`all`. Neither exposes a `len`/`size`/`length` method; the
global `len(x)` is the only length accessor. Picking one convention (for example
a `len` method mirroring the global) and applying it to both types would remove
the asymmetry. Low priority.

---

## 3. Medium-priority improvements

### 3.1 Local/CI test parity — **resolved**
`tests/run.sh` used to run only `tests/*.jolt` and `tests/errors/*.jolt`, so it
skipped `examples/*.jolt` and the `tests/check/*.jolt` compile-only tests that
CMake/CTest does run, and it failed `32_ffi_callback.jolt` when `libffi` was
absent while CTest skipped it. The runner now mirrors CTest: it executes the
same four groups (run `tests/` and `examples/`, error tests under
`tests/errors/`, and `-c` check tests under `tests/check/`, requiring an empty
stdout there), and skips a test whose `// requires:` capability is missing.

### 3.2 Benchmarks
The old report quoted `fib(24)` timings with no harness in the tree. There is no
`benchmarks/` directory. A minimal, reproducible benchmark script (recursion,
array push+sum, string building) gives a baseline for the VM and a target for
the AOT backend. Medium priority.

### 3.3 Instruction encoding
`Instruction` is a flat struct (`op, a, b, c, jump, line, col`, 28 bytes) with
an always-present `jump`. Lua packs an instruction into one 32-bit word. A
tagged-union or two-variant encoding would cut size and improve fetch density
before the native backend leans on it. This changes the bytecode contract, so it
is a deliberate PHASE-level decision, not a drive-by edit.

### 3.4 Header placement
Public headers live in `include/` (`bytecode.h`, `compiler.h`, `ffi_*.h`,
`platform*.h`, `vm.h`) while siblings (`value.h`, `lexer.h`, `parser.h`, …) live
in `src/`. AGENT.md rule 7 says every new `.cpp` has its `.h` in `include/`.
Consolidating the public headers in `include/` would make the rule uniform. Low
priority; touches many includes.

### 3.5 CI hardening — resolved.
The two workflows build and test on three platforms. A `sanitizers` job in
`cmake-multi-platform.yml` now also runs the suite under
`-fsanitize=address,undefined -fno-sanitize-recover=all`, so a memory-safety
regression in the refcount/GC paths fails the build instead of hiding in a
Release run. Landing it surfaced three undefined behaviors (two in the tests,
one in the AOT string-constant pool), all fixed.

---

## 4. Low-priority / hygiene

1. **Version metadata.** `kVersion` is a string literal in `src/main.cpp` and
   `CMakeLists.txt` declares `project(jolt VERSION 0.1.0)`. Generating the
   header from CMake avoids drift. Low priority.
2. **Stack-trace flood — resolved.** `src/diagnostic.cpp` caps the trace to a
   head and a tail with an "… N more …" line.
3. **Executable bits — resolved.** `build.sh` and `tests/*.sh` are executable in
   the index.
4. **Language policy — resolved.** All code, comments and docs are English
   (AGENT.md rule 6).

---

## 5. Suggested order (each independently shippable)

1. `chore(bench)`: add a benchmark harness and record a baseline. *(§3.2)*
2. `ci`: add an ASan/UBSan job. *(§3.5 — done.)*
3. `refactor(build)`: derive `kVersion` from the CMake project version. *(§4.1)*
4. Collection-API consistency (`len` method) and header consolidation are
   optional cleanups that touch the language surface or many includes; schedule
   them deliberately. *(§2.5, §3.4)*

Commits follow Conventional Commits in English (see `CONVENTIONS.md`).
