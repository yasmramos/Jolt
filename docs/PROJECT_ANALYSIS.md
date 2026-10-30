# Jolt — Project Analysis and Improvement Plan

Date: 2026-10-31 · Branch: `develop` · Base commit analyzed: `4dde99a` (merge of `origin/main`)

This analysis is based on the **actual code in the repository** (~6,900 lines of C++17),
not on assumptions. The project deliberately mirrors the Lua / LuaJIT pipeline:
`source → lexer → parser → AST → compiler → register-based bytecode → VM`.

Everything below was verified by building (`sh build.sh`) and running the full test
suite (`sh tests/run.sh`: 17/17 passing) plus targeted probes described inline.

---

## 1. Current state (verified)

| Area | Status | Evidence |
|------|--------|----------|
| Lexer / Parser / AST | Working, tested | `src/lexer.cpp` (504 L), `src/parser.cpp` (567 L), `tests/01–07*.jolt` |
| Compiler AST→bytecode | Phases 1–5 done | `src/compiler.cpp` (633 L): literals, globals/locals, control flow, bitwise, short-circuit |
| Register-based VM | Phases 1–5 done | `src/vm.cpp` (400 L), `include/bytecode.h` (~40 opcodes as designed) |
| AST interpreter | Full language, still the production engine | `src/interpreter.cpp` (746 L) + `src/builtins.cpp` (1,096 L); `main.cpp` uses `Interpreter` directly |
| Phase 6 (functions/collections in VM) | **Not started** | No `CALL`/`RETURN`/`CLOSURE`/`GETUPVAL` anywhere in `include/` or `src/vm.cpp` |
| Tests | 17 `.jolt` tests + 2 C++ suites | `tests/run.sh`, `run_vm.sh` (584 L), `run_compiler.sh` (443 L) — but see §3.4 |
| CI | None | No `.github/workflows` |

Benchmarks run on this machine (interpreter path, `-O2`):
`fib(24)` recursive: **0.48 s**; array push+sum 200k: **0.10 s**. These numbers are the
baseline any future VM/JIT work must beat — and they also show that today's bottleneck is
the tree-walking interpreter, which reinforces the plan to finish the VM.

---

## 2. High-priority findings (correctness / architecture)

### 2.1 Reference-counting cycle leak (latent, becomes critical with closures)
`RefCounted` has a **virtual destructor** (`src/value.h:28`), so plain `Ref<T>` members do
*not* propagate destruction through cycles. A self-referencing container leaks:

```jolt
let a = [];
a.push(a);   // refs never reach 0 → memory leak
```

Today the impact is small because scopes die per program run, but once phase 6 adds
long-lived closures and REPL sessions, leaks accumulate.

**Recommendation:** remove the virtual destructor (each concrete type knows how to destroy
itself via a function pointer stored once per object, or template the release path), and/or
introduce a `WeakRef` for parent/back pointers. This matches Lua's design where GC handles
cycles outright; if a cycle collector is planned later, document that refcounting only
covers the acyclic case now.

### 2.2 Duplicated arithmetic semantics between interpreter and VM
`intPow` exists twice, byte-for-byte (`src/interpreter.cpp:58` and `src/vm.cpp:39`), and
`vm.cpp` comments say its helpers "replicate interpreter semantics". Replication-by-comment
drifts silently. The differential oracle idea is right, but it needs teeth:

**Recommendation:** extract shared semantics into one header (`src/numeric_ops.h`) used by
both engines, *and* add a randomized differential fuzz test (§3.5) so any divergence fails CI.

### 2.3 Undefined-behavior risk in integer paths
The int fast-paths intentionally wrap via `uint64_t` casts (defined behavior), which is good,
but `intPow(base, negative)` relies on `(uint64_t)exponent` wrapping to a huge loop count —
currently unreachable from the VM/interpreter call sites, yet it is a landmine. Also
`Value(std::fmod(...))` etc. should be guarded against NaN/infinity printing consistency.
Document the chosen overflow semantics (Lua errors on float→int misuse; Jolt wraps silently).

### 2.4 Grammar friction: no braceless single-statement bodies
`if (cond) return x;` fails to parse ("expected '{'"). Every branch/example must use braces.
Either support expression-bodied statements (like Lua's absence of braces entirely) or keep
braces mandatory — but then say so in `docs/language.md` and emit a *targeted* diagnostic
("did you mean `{ ... }`?") instead of the generic "expected '{'".

### 2.5 API inconsistency for collections
Maps expose method syntax `m.keys() / m.get(k) / m.set(k,v)` but have **no** `m.len()` /
`m.size()` (only global `len(m)` works); arrays mix `push` with global `len`. One convention
(method-first, like Lua's `#t` + methods, or builtin-first) should be picked and completed.

---

## 3. Medium-priority improvements

### 3.1 Finish phase 6 before anything else (per AGENT.md ordering)
Scope already defined in AGENT.md §FASE 6: protos per function, `CALL/RETURN/CLOSURE/
GETUPVAL/SETUPVAL`, per-frame registers, upvalue lists with open→closed migration (Lua 5.4
model), `Runtime` interface so builtins can call VM functions without re-entering the AST.
Design notes worth adopting from Lua now, while cheap:
- Store upvalues as a single linked/open list per closure (`lua_UpVal` style) rather than
  vectors of cells copied at closure creation — avoids O(n²) capture cost.
- Keep argument count / varargs in the proto header (`numparams`, `is_vararg`) to make
  `wrong_argument_count` a VM-level check, matching existing interpreter error text.
- Reserve opcode space for `NEWTABLE`/`GETTABLE`/`SETTABLE`/`ITER`-style ops *now* in the
  enum comments so the ~40-opcode budget isn't blown when collections land.

### 3.2 Then phase 4: make the VM the only engine
`main.cpp` still runs everything through `Interpreter` (REPL included). Once parity is
proven by the differential tests, flip `execute()`/`repl()` to compile→VM, keep `--ast`
temporarily behind a flag, then delete the interpreter from the binary (AGENT.md §10).

### 3.3 Instruction encoding
`Instruction{op,a,b,c,jump}` = 20 bytes with an always-present `jump`. Lua packs everything
into one 32-bit word (`iABC/iAsBx/iAx`). Suggested middle ground for Jolt: a tagged union or
two variants (`ABC` / `sJmp`) cutting size ~2× and removing the dead field; improves fetch
density before any native-compilation work (phase 7).

### 3.4 Wire the C++ test suites into the main runner
`tests/run.sh` executes only `.jolt` files; `run_vm.sh` and `run_compiler.sh` exist but are
never invoked together, and nothing checks examples. Add `tests/run_all.sh` (build + 3 suites
+ examples smoke) and make it the single entry point.

### 3.5 Differential fuzzing (the real value of keeping two engines)
Generate random programs (same grammar subset both engines support), run interpreter vs VM,
compare stdout/exit codes. This is exactly how the Lua team validated 5.4 and catches the
§2.2 drift class permanently. Start with arithmetic/control-flow templates; extend after
phase 6.

### 3.6 CI
Add GitHub Actions: linux (gcc+clang) build, `run_all.sh`, and ASan/UBSan variant. Cheap,
catches the refcount bugs above immediately.

---

## 4. Low-priority / hygiene

1. **Language policy conflict.** Repo rules (`CONVENTIONS.md`, user instruction) require
   code/comments/docs in **English**, but `AGENT.md`, README and `docs/language.md` are in
   Spanish and AGENT.md rule 6 even mandates Spanish comments. Decide: translate AGENT.md
   and inline comments progressively (start with touched files), or amend the convention.
   Until resolved, new code follows English.
2. **Executable bits lost.** `build.sh`/`tests/*.sh` ship non-executable (`./build.sh` →
   Permission denied). Fix with `git update-index --chmod=+x`.
3. **Header placement split.** `vm.h`/`bytecode.h`/`compiler.h` live in `include/` while
   their siblings (`value.h`, `lexer.h`) live in `src/`; AGENT.md rule 7 says one place.
   Consolidate public headers in `include/`.
4. **Stack-trace flood.** Infinite recursion prints ~2,000 identical frames; cap the trace
   (e.g. first/last 10 + "... N more") as Lua does.
5. **Version/build metadata.** `kVersion` hardcoded in `main.cpp`; derive from CMake
   project version to avoid drift.

---

## 5. Suggested slice order (each independently shippable)

1. `fix(memory)`: non-virtual RefCounted destruction (or weak refs) + regression test. *(§2.1)*
2. `refactor(numeric)`: shared `numeric_ops.h` + differential fuzz harness v1. *(§2.2, §3.5)*
3. `chore(repo)`: exec bits, CI workflow, `run_all.sh`, header consolidation. *(§3.4, §3.6, §4)*
4. `feat(bytecode)`: compact instruction encoding + reserved collection opcodes. *(§3.3)*
5. `feat(vm)`: phase 6 — protos, CALL/RETURN/CLOSURE/upvalues (split: calls → closures →
   collections → for-in). *(§3.1)*
6. `feat(cli)`: phase 4 — VM as sole engine, `--ast` oracle flag, persistent-VM REPL. *(§3.2)*
7. Language/API cleanups: braceless bodies decision, map `len` consistency, trace cap. *(§2.4, §2.5, §4.4)*

Commits follow Conventional Commits in English (see `CONVENTIONS.md`).
