# PHASE 6 design — functions, closures, collections and iteration

Status: **approved and in progress**. Slices 6a.0 and 6a are implemented; the
rest is queued.

Scope (from `AGENT.md` §4, PHASE 6): `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`,
`SETUPVAL`, per-function protos, a call stack with per-frame registers,
closures over `Cell`, a `Runtime` interface so builtins can call VM functions
without re-entering the AST, arrays/maps with indexing and members, ranges,
string interpolation and `for-in`.

Non-goals: PHASE 4 (VM as the dev engine) and PHASE 7 (bytecode -> native AOT
backend). They come after parity is proven.

---

## Progress

| Slice | Status | Notes |
|-------|--------|-------|
| 6a.0 | ✅ done | `Runtime` bridge and `src/ops.{h,cpp}` extraction |
| 6a | ✅ done | protos, `CALL`/`RETURN`/`CLOSURE`, 0 upvalues |
| 6b | ✅ done | `GETUPVAL`/`SETUPVAL`, upvalue descriptors, lambdas, escaping closures |
| 6c-6f | pending | tables, `ITER`, callbacks, parity gate |

The 40-opcode consolidation (D1) is done: `Bytecode::constants` replaces the
`numbers`/`strings` pools and `UNOP` replaces `NEG`/`NOT`/`BNOT`. `CALL`,
`RETURN` and `CLOSURE` execute in the VM; the table, upvalue, `ITER` and `IN`
cases still reject with "opcode not implemented yet" until their slice.

Decisions taken while implementing 6a.0, superseding the text below:

- Error positions live in `Instruction::line`/`col` instead of a parallel
  `Bytecode::lines` vector. A parallel vector would have to be kept in sync
  by every emitter; a field cannot go out of sync.
- `VM` does not register the standard library in its constructor. The CLI
  calls `registerBuiltins(vm)` explicitly, so `vm_test` and future parity
  tools can link the VM without the AST interpreter.
- The shared operator diagnostics use the real symbol (`'+'`, `'in'`, ...).
  The interpreter previously printed `'operator'` on the fallback path, which
  the VM never did; both now produce identical text, which the parity harness
  needs.
- `ops::memberGet` still calls `bindMethod` from `builtins.h`, so anything
  that links `ops.cpp` must link `builtins.cpp` too.

Decisions taken while implementing 6a, superseding the text below:

- `CALL` copies the callee's arguments into a fresh register window
  (`regBase = registers_.size()`) instead of aliasing the caller's registers at
  `a + 1`. The argument registers are reused by the caller after the call, and
  the frame shares one growable `Value` stack, so a copy keeps the two windows
  independent and makes the return path a simple resize. Correctness first;
  PHASE 7 can remove the copy when it specializes the calling convention.
- `Bytecode` owns its children as `shared_ptr<Bytecode>`; a `Function` holds a
  `shared_ptr<const Bytecode>` proto. The root chunk is referenced by a
  non-owning `shared_ptr` during `run`, so a closure still keeps its proto
  alive after the root `Bytecode` is gone (REPL entries, successive runs).
- 6a only compiles top-level `fn` declarations. Local (nested) declarations
  are a targeted `CompileError` until 6b, because without upvalues a function
  could not see the locals it should close over. Top-level declarations are
  hoisted before any statement, matching `Interpreter::execBlock`.
- A function that refers to an outer local resolves it as a global until 6b.
  This is a deliberate 6a limitation: it keeps recursion and top-level helpers
  working without upvalues, and 6b replaces the resolution with
  `GETUPVAL`/`SETUPVAL`.
- 6a's exit criterion is a VM test subset (calls, parameters, return, early
  return, recursion, mutual recursion, native re-entry, arity and depth
  errors), not full `tests/04_functions.oskl` parity: that file also uses
  lambdas, closures and collections, so its parity lands with 6b/6c.
- `print` keeps the `PRINT` opcode only for exactly one unshadowed argument.
  Other arities compile to a normal `CALL`, so they need `registerBuiltins`
  like any other native.


---

## 1. Verified starting point

| Fact | Evidence |
|------|----------|
| The compiler emits one flat chunk | `Compiler::compile`, `src/compiler.cpp:256` |
| Locals are already heap `Cell`s | `DECLLOCAL` in `src/vm.cpp:365`, `struct Cell` in `src/value.h:366` |
| `Function` is AST + native today | `src/value.h:394` (`body`, `closure`, `native`) |
| The interpreter resolves names at call time through `Env` | `Env::find`, `src/interpreter.h:62` |
| Builtins only re-enter user code in two places | `invoke` and the sort comparator, `src/builtins.cpp:77` and `:110` |
| Two copies of the arithmetic semantics | `src/interpreter.cpp:58`, `src/vm.cpp:39` |
| The VM reports errors without line/column or stack | `failRuntime`, `src/vm.cpp:14` |
| 35 opcodes today | `include/bytecode.h:14` |

## 2. Decisions

### D1 — Opcode budget (approved)

35 opcodes before PHASE 6; the phase needs 9 more (5 for functions, 3 for
tables, 1 for iteration) plus `IN`. 45 breaks the "~40" rule in `AGENT.md` §3.

Approved result: **exactly 40 opcodes**, by folding instructions whose operand
is a compile-time constant (so native codegen in PHASE 7 specializes them back
to one instruction each):

| Change | Delta |
|--------|-------|
| `LOADK` + `LOADKS` + `LOADNIL` + `LOADBOOL` -> single tagged constant pool (`LOADK R[a], K[b]`) | -3 |
| `NEG` + `NOT` + `BNOT` -> `UNOP R[a], R[b], op` (op is a byte in `c`) | -2 |
| `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`, `SETUPVAL` | +5 |
| `NEWTABLE`, `GETTABLE`, `SETTABLE` | +3 |
| `ITER` | +1 |
| `IN` | +1 |
| **Total** | **40** |

`GETTABLE`/`SETTABLE` take a mode bit (index vs member) in `c`; like `UNOP`
and `NEWTABLE`'s kind bit it is a compiler constant, so it becomes a
specialized instruction later.

The ceiling is hard, not a target (`AGENT.md` §3.3). Any later slice or
PHASE 7 feature that needs a new opcode must first execute the next rung of
the retirement ladder: comparisons → `CMP` (-5), conditional jumps → `JMP_IF`
(-1), bitwise → `BITOP` (-4), arithmetic → `ARITH` (-5). Never emit a 41st
opcode. `PRINT` is already scheduled for deletion at the 6f gate, so the
effective budget after PHASE 6 is 39.

### D2 — A proto is a `Bytecode`

`Bytecode` gains `name`, `numParams`, `std::vector<ProtoPtr> children` and
`std::vector<UpvalDesc> upvalues` (`{bool inParentLocal; int index;}`). The
root chunk is the `<main>` proto. `CLOSURE R[a], b` instantiates
`children[b]`. Protos are `shared_ptr`, so a closure keeps its proto alive
after the chunk that produced it is gone (REPL, `:load`).

### D3 — One callable `Value`, two engines

`Function` keeps its AST fields (interpreter, retired in PHASE 4) and gains
`std::shared_ptr<const Bytecode> proto` + `std::vector<CellPtr> upvalues`.
`isNative()` / `isVm()` are explicit predicates. No new `VType`, so `equals`,
`toString`, `repr` and every builtin keep working unchanged.

### D4 — Frames share one register stack

```cpp
struct CallFrame {
  const Bytecode* proto;
  size_t pc;
  size_t callSite;   // caller register that receives the result
  size_t regBase;    // frame R0 == caller's R[callSite + 1]
  size_t localBase;  // first slot in the shared Cell vector
};
std::vector<Value> stack_;   // registers of every live frame
std::vector<CellPtr> locals_;
```

`CALL a, b`: the callee is `R[a]`, arguments are `R[a+1 .. a+b]`, the new
frame's `regBase` is `a + 1` (the arguments are already the parameters, no
copy), and `RETURN` writes the result into the caller's `R[callSite]`. Arity
is checked from `proto->numParams` with the interpreter's exact message
(`fn() expects N argument(s), got M`). Call depth is capped like
`Interpreter::maxDepth` and the stack trace mirrors `RuntimeError::stack`.

### D5 — Closures capture cells, not values

Because every local is already a `Cell`, `CLOSURE` only copies `CellPtr`s:
`inParentLocal ? frame.locals[index] : frame.upvalues[index]`. No
open/close-upvalue machinery is needed, which is the main simplification
Lua cannot make. Cost: one allocation per local per call (already true today).
Escape analysis in PHASE 7 is what removes it.

Parameters are boxed at function entry by emitting one `DECLLOCAL` per
parameter (reuses the existing opcode, keeps cells uniform).

### D6 — Lexical resolution and hoisting must keep interpreter parity

The compiler resolves locals statically; the interpreter resolves at call
time. Two cases diverge and need an explicit rule:

1. `let x = x;` — interpreter evaluates the initializer before defining `x`
   (reads the outer/global `x`). We keep that order: the compiler declares
   the local *after* compiling the initializer, as it already does.
2. A hoisted `fn` that refers to a local declared later in the same block —
   the interpreter sees it at call time. Rule: **compile function bodies
   after the enclosing block's statements**, then patch the `CLOSURE`
   child index. Upvalue descriptors are read by the VM at `CLOSURE` runtime,
   so the late patch is safe, and both engines agree.

A parity test (`hoisted_forward_local`) pins this down. If the rule turns out
to be impractical, the fallback is to reject the construct with a targeted
compile error and document it.

### D7 — `in` and interpolation

`in` is not a new opcode unless we spend the free slot: implement it through
the shared `contains()` helper called from `GETTABLE`-like dispatch, or add
`IN R[a], R[b], R[c]`. Recommendation: **opcode `IN`** (40th) — it mirrors
`binary(Tok::In)` exactly, and `40` is still within budget. Interpolation
needs no opcode: the compiler emits the text parts as constants and joins
them with `ADD`/`toString` (the interpreter's `InterpExpr` semantics).

### D8 — `Runtime` bridge (prerequisite, no language change)

```cpp
class Runtime {                       // implemented by Interpreter and VM
 public:
  virtual Value call(const Value&, std::vector<Value>, int line, int col) = 0;
  virtual Value indexGet(...); virtual void indexSet(...);
  virtual Value memberGet(...); virtual void memberSet(...);
  virtual Value binary(...);    virtual bool lessThan(...);
  virtual void fail(...) = 0;
  int currentLine = 1, currentCol = 1;
};
```

`NativeFn` becomes `std::function<Value(Runtime&, std::vector<Value>&)>` and
`registerBuiltins` takes a `Runtime&`. `getBuiltins` keeps working for both
engines, `map`/`filter`/`sort` call back through `Runtime::call`, and
`VM::call` executes a nested frame. The per-type semantics
(`indexGet/indexSet/memberGet/memberSet`, `contains`, `toString` of ranges)
move to `src/ops.{h,cpp}` and both engines call it, which also removes the
duplicated arithmetic in `vm.cpp`.

> **PHASE 7 note:** the `Runtime` introduced in slice 6a.0 must be designed as
> a linkable, embeddable component (no dependency on the compiler front-end or
> the VM) and must reach the outside world only through the minimal `Platform`
> interface (write, read, alloc, exit, file I/O). The AOT backend statically
> links this same runtime into every generated binary, which is **freestanding
> — no libc**: there it runs on `PlatformFreestanding` (direct syscalls), while
> the dev engine uses `PlatformPosix`. Anything that keeps the runtime tied to
> the VM or to libc would have to be refactored later. See PHASE 7 in
> `AGENT.md`. The AOT backend also takes the `Target { arch, os, abi }`
> descriptor from `include/native/target.h` from the first slice; only
> `x86_64-linux-sysv` is implemented, but no entry point hardcodes it.

### D9 — 6f parity gate (objective, normative)

The AST interpreter is the oracle only until the gate below is green on a
single commit. "Parity" is not a judgement call; it is these checks, run by
`tests/parity_test.cpp` in CTest and in CI:

**Corpus.** Every script matched by the existing CTest globs:
`tests/*.oskl`, `tests/parity/**/*.oskl` and `examples/*.oskl`, plus the
committed grammar-fuzzer corpus from 6e. `tests/errors/**` are included as
diagnostics cases. The list is generated by the glob, never hand-picked; a
feature slice adds its script in the same commit.

**Pass criteria — all mandatory, zero tolerance.**

| # | Surface | Criterion |
|---|---------|-----------|
| 1 | stdout | Byte-for-byte identical: same bytes, newlines, float formatting and ordering. No normalization, no trimming. |
| 2 | exit code | Identical for success (`0`) and for every failure (non-zero). |
| 3 | diagnostics | Identical message text and identical `line`/`col` for compile and runtime errors. The harness compares structured `{message, line, col}`, not a rendered prefix. |
| 4 | stack traces | Identical `function@line:col` frame sequence, innermost first. Empty or engine-specific prefixes are not allowed. |
| 5 | observable state | After each run, a canonical globals dump (keys sorted, values as `repr`, cycles rendered as `<cycle>`, no addresses) is identical. |
| 6 | fuzz | The 6e grammar-fuzzer corpus completes with no crash and satisfies 1–5; non-terminating programs are bounded by a per-case timeout committed with the corpus. |

**Not part of parity:** memory addresses, allocation counts, GC timing,
register values, bytecode dumps and instruction counts. Those are
implementation-private; comparing them would freeze the VM to the interpreter
instead of to the language.

**Exclusions.** `tests/parity/excluded.txt` names each excluded script with a
reason and the slice that removes the exclusion. The gate fails if the file is
non-empty at 6f. A "known divergence" without an entry is a failing test.

**Conflict rule.** If the engines disagree, `docs/language.md` decides. If the
interpreter is wrong, fix the interpreter and its spec test first; never
change the VM to copy a bug. If the interpreter cannot be fixed cheaply, move
the script to `excluded.txt` with the reason, and 6f cannot pass until the
entry is gone.

**Oracle expiry.** Once the gate is green, `interpreter.cpp` is linked only
into `parity_test`, never into `oskel`. PHASE 4 deletes it together with the
`--ast` flag once the VM is the dev engine. There is no state in which the
interpreter remains a supported execution mode.

## 3. Final opcode set (39 + `IN` = 40)

```
load      LOADK MOVE
arith     ADD SUB MUL DIV MOD POW
unary     UNOP                (neg | not | bnot)
bits      BAND BOR BXOR SHL SHR
compare   EQ NEQ LT LTE GT GTE
control   JMP JMP_IF_FALSE JMP_IF_TRUE
vars      GETGLOBAL SETGLOBAL DECLLOCAL SETLOCAL GETLOCAL
functions CALL RETURN CLOSURE GETUPVAL SETUPVAL
tables    NEWTABLE GETTABLE SETTABLE
iteration ITER
membership IN
io        PRINT             (temporary; deleted at 6f, AGENT.md §3.6)
end       HALT
```

## 4. Slices (each one shippable, tests green at the end)

| Slice | Content | Exit criterion |
|-------|---------|----------------|
| 6a.0 | `Runtime` + `src/ops` extraction, `NativeFn` change, VM gets natives and error positions (`Bytecode::lines`, frame names) | Existing suites unchanged; VM can call `assertEq`/`print` natively |
| 6a | Protos, `CALL`/`RETURN`/`CLOSURE` (0 upvalues), top-level `fn`, parameters, recursion, arity | VM test subset: calls/return, early return, recursion, mutual recursion, native re-entry, arity/depth errors. Full `tests/04_functions.oskl` parity needs 6b/6c (lambdas, closures, collections) |
| 6b | `GETUPVAL`/`SETUPVAL`, upvalue descriptors, lambdas, closures escaping their scope | `examples/closures.oskl`, `tests/09_lambdas.oskl` parity |
| 6c | `NEWTABLE`/`GETTABLE`/`SETTABLE`, array/map literals, indexing, members, methods, ranges, interpolation, `IN` | `tests/02_strings`, `05_collections`, `06_operators` parity |
| 6d | `ITER` + `for-in` (arrays, strings, maps, lazy ranges), loop-variable cells per iteration | `tests/03_control`, `05_collections` parity |
| 6e | Builtins that take callbacks (`map`, `filter`, `reduce`, `sort`) against VM closures; every `print` arity compiles to `CALL` | full suite parity + fuzz harness |
| 6f | Parity harness as CI gate (`tests/parity_test.cpp` over the D9 corpus), delete `PRINT`, then PHASE 4 planning | D9 green with an empty exclusion list; interpreter demoted to oracle |

The gate is defined in D9: stdout bytes, exit code, structured diagnostics,
structured stack traces, a canonical globals dump and the 6e fuzz corpus, all
compared by `tests/parity_test.cpp`. That is what makes "parity" measurable
instead of aspirational, and it is the expiry date for `interpreter.cpp`.

## 5. Risks

| Risk | Mitigation |
|------|------------|
| Silent divergence between engines | Parity harness from slice 6a, no slice merges without it |
| Register/frame bugs (the classic VM failure) | Frame invariants asserted in debug builds; `vm_test` cases for arg passing, recursion, early return |
| Native re-entrancy (builtin -> VM -> builtin) | `VM::call` runs a nested frame with a return barrier; test `map` with a VM lambda |
| `Edge: mutation during for-in` | Iterate live containers by index (document it), except ranges which are lazy |
| Performance regression from cells | Accepted in PHASE 6; measured in the benchmark noted in `docs/PROJECT_ANALYSIS.md` |
| Closures over `Cell` form cycles (a recursive local function: cell -> closure -> cell) | Make `Cell` and the VM closure a `GcNode` and add their hooks next to the others in `src/value.cpp`; `src/gc.h` is engine-agnostic, so this is not a GC redesign. Add the cases to `tests/gc_test.cpp` |

## 6. What NOT to do

- No `OP_CALL_METHOD_DYNAMIC`, `OP_EVAL_EXPR` or other high-level opcodes.
- No AST execution inside the VM, and no "hybrid" fallback for unsupported
  nodes: unsupported constructs stay a `CompileError` until their slice.
- No JIT, no native compiler, no GC redesign in this phase.
- No new language features beyond what PHASE 6 lists.
