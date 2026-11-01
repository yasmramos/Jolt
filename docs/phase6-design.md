# PHASE 6 design — functions, closures, collections and iteration

Status: **approved and in progress**. Slice 6a.0 is implemented; the rest is
queued.

Scope (from `AGENT.md` §4, PHASE 6): `CALL`, `RETURN`, `CLOSURE`, `GETUPVAL`,
`SETUPVAL`, per-function protos, a call stack with per-frame registers,
closures over `Cell`, a `Runtime` interface so builtins can call VM functions
without re-entering the AST, arrays/maps with indexing and members, ranges,
string interpolation and `for-in`.

Non-goals: PHASE 4 (VM as the only engine) and PHASE 7 (bytecode -> C++). They
come after parity is proven.

---

## Progress

| Slice | Status | Notes |
|-------|--------|-------|
| 6a.0 | ✅ done | `Runtime` bridge and `src/ops.{h,cpp}` extraction |
| 6a | next | protos, `CALL`/`RETURN`/`CLOSURE`, 0 upvalues |
| 6b-6f | pending | upvalues, tables, `ITER`, callbacks, parity gate |

The 40-opcode consolidation (D1) is done: `Bytecode::constants` replaces the
`numbers`/`strings` pools and `UNOP` replaces `NEG`/`NOT`/`BNOT`. The PHASE 6
opcodes are declared and the VM rejects them until their slice lands.

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
io        PRINT
end       HALT
```

## 4. Slices (each one shippable, tests green at the end)

| Slice | Content | Exit criterion |
|-------|---------|----------------|
| 6a.0 | `Runtime` + `src/ops` extraction, `NativeFn` change, VM gets natives and error positions (`Bytecode::lines`, frame names) | Existing suites unchanged; VM can call `assertEq`/`print` natively |
| 6a | Protos, `CALL`/`RETURN`/`CLOSURE` (0 upvalues), function declarations, parameters, recursion, arity | `tests/04_functions.jolt` runs on the VM with identical stdout |
| 6b | `GETUPVAL`/`SETUPVAL`, upvalue descriptors, lambdas, closures escaping their scope | `examples/closures.jolt`, `tests/09_lambdas.jolt` parity |
| 6c | `NEWTABLE`/`GETTABLE`/`SETTABLE`, array/map literals, indexing, members, methods, ranges, interpolation, `IN` | `tests/02_strings`, `05_collections`, `06_operators` parity |
| 6d | `ITER` + `for-in` (arrays, strings, maps, lazy ranges), loop-variable cells per iteration | `tests/03_control`, `05_collections` parity |
| 6e | Builtins that take callbacks (`map`, `filter`, `reduce`, `sort`) against VM closures | full suite parity + fuzz harness |
| 6f | Parity harness as CI gate (`tests/parity_test.cpp` over `tests/parity/*.jolt`), then PHASE 4 planning | red/green gate; interpreter demoted to oracle |

The parity harness compares stdout and the error text/position of both
engines for the same file; it grows one directory per slice and is what makes
"parity" measurable instead of aspirational.

## 5. Risks

| Risk | Mitigation |
|------|------------|
| Silent divergence between engines | Parity harness from slice 6a, no slice merges without it |
| Register/frame bugs (the classic VM failure) | Frame invariants asserted in debug builds; `vm_test` cases for arg passing, recursion, early return |
| Native re-entrancy (builtin -> VM -> builtin) | `VM::call` runs a nested frame with a return barrier; test `map` with a VM lambda |
| `Edge: mutation during for-in` | Iterate live containers by index (document it), except ranges which are lazy |
| Performance regression from cells | Accepted in PHASE 6; measured in the benchmark noted in `docs/PROJECT_ANALYSIS.md` |

## 6. What NOT to do

- No `OP_CALL_METHOD_DYNAMIC`, `OP_EVAL_EXPR` or other high-level opcodes.
- No AST execution inside the VM, and no "hybrid" fallback for unsupported
  nodes: unsupported constructs stay a `CompileError` until their slice.
- No JIT, no native compiler, no GC redesign in this phase.
- No new language features beyond what PHASE 6 lists.
