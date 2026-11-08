#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "value.h"

// <windows.h> defines `IN` as a macro (the input-control parameter annotation),
// which would rewrite the `OpCode::IN` enumerator below. The bytecode header is
// included after <windows.h> in the CLI, so drop the macro here rather than
// depending on include order in every translation unit.
#ifdef IN
#undef IN
#endif

// Low-level bytecode for Jolt's register-based VM.
//
// Every instruction works on registers and has exactly one direct native
// lowering in PHASE 7: either a short inline instruction sequence or one call
// to a named runtime primitive (AGENT.md §3.2). The set is deliberately small:
// literals share one tagged constant pool, unary operators share one opcode
// whose `c` field selects the operation, and collections share the GET/SET
// table pair.
//
// PHASE 6 adds functions, collections and iteration. PHASE 6f retires the
// temporary `PRINT` opcode (every `print` arity compiles to `CALL` to the
// builtin), leaving 39 opcodes. 40 is a hard ceiling; the retirement ladder
// for a new opcode is in AGENT.md §3.3.

enum class OpCode : std::uint8_t {
  LOADK,   // R[a] = constants[b]  (nil, bool, int, float or string)
  MOVE,    // R[a] = R[b]

  ADD,     // R[a] = R[b] + R[c]
  SUB,     // R[a] = R[b] - R[c]
  MUL,     // R[a] = R[b] * R[c]
  DIV,     // R[a] = R[b] / R[c]
  MOD,     // R[a] = R[b] % R[c]
  POW,     // R[a] = R[b] ** R[c]
  UNOP,    // R[a] = unary(UnaryOp(c), R[b])

  BAND,    // R[a] = R[b] & R[c]
  BOR,     // R[a] = R[b] | R[c]
  BXOR,    // R[a] = R[b] ^ R[c]
  SHL,     // R[a] = R[b] << R[c]
  SHR,     // R[a] = R[b] >> R[c]

  EQ,      // R[a] = R[b] == R[c]
  NEQ,     // R[a] = R[b] != R[c]
  LT,      // R[a] = R[b] <  R[c]
  LTE,     // R[a] = R[b] <= R[c]
  GT,      // R[a] = R[b] >  R[c]
  GTE,     // R[a] = R[b] >= R[c]

  // Control flow. `jump` is an absolute index into `code`.
  JMP,          // pc = jump
  JMP_IF_FALSE, // if !R[a] then pc = jump
  JMP_IF_TRUE,  // if R[a] then pc = jump

  // Variables. Globals are resolved by name (a string constant in the pool);
  // locals by cell index in the current frame.
  SETGLOBAL,  // globals[constants[b]] = R[a]
  GETGLOBAL,  // R[a] = globals[constants[b]]
  DECLLOCAL,  // locals[b] = new cell holding R[a]
  SETLOCAL,   // locals[b]->value = R[a]
  GETLOCAL,   // R[a] = locals[b]->value

  // Functions (PHASE 6). `CALL` uses a contiguous argument window starting at
  // `b + 1`, so a call needs no separate argument-array allocation.
  CALL,       // R[a] = call(R[a], R[a+1 .. a+b]); `b` is the argument count
  RETURN,     // return R[a] from the current frame
  CLOSURE,    // R[a] = closure of child proto `b` (captures in proto->upvalues)
  GETUPVAL,   // R[a] = upvalues[b]->value
  SETUPVAL,   // upvalues[b]->value = R[a]

  // Collections and iteration (PHASE 6).
  //
  // NEWTABLE creates an array when `c == 0` (with `b` pre-sized nil slots) and
  // an insertion-ordered map when `c == 1`.
  //
  // GETTABLE/SETTABLE cover both dynamic indices and constant member names.
  // `c >= 0` means the key lives in register `c` (array index, map key, or the
  // string key produced for a computed map-literal entry). `c < 0` means the
  // key is the string constant at index `-(c + 1)`: a member access/assignment
  // for GETTABLE/SETTABLE, or a literal map key when building a map.
  //
  // ITER reads the iterable from R[b] and keeps its cursor in R[c]. The
  // compiler initializes R[c] to nil; ITER starts the iteration on the first
  // call. R[a] receives the next item, or nil when the sequence is exhausted.
  // `instruction.jump` holds a destination register index for the done flag
  // (0 while items remain, 1 when exhausted), so a nil item is not mistaken
  // for the end.
  NEWTABLE,   // R[a] = new table (array if c == 0, map if c == 1)
  GETTABLE,   // R[a] = R[b][R[c]] or R[b].<constants[-c-1]>
  SETTABLE,   // R[b][R[c]] = R[a] or R[b].<constants[-c-1]> = R[a]
  ITER,       // R[a] = next item of R[b]; R[jump] = done flag
  IN,         // R[a] = R[b] in R[c]

  HALT,       // stop execution
};

// Hard budget gate (AGENT.md §3.3). HALT must stay the last enumerator: adding
// an opcode without first executing a retirement rung trips this assertion at
// compile time, so the ceiling cannot drift silently.
inline constexpr int kOpcodeCount = static_cast<int>(OpCode::HALT) + 1;
static_assert(kOpcodeCount <= 40,
              "opcode budget exceeded: execute a retirement rung first (AGENT.md §3.3)");

// Selector stored in `Instruction::c` for `UNOP`. Keeping the operator in the
// instruction lets PHASE 7 emit one native instruction per case without a
// second dispatch table.
enum class UnaryOp : std::uint8_t {
  Negate = 0,  // -x
  Not = 1,     // !x
  BitNot = 2,  // ~x
};

struct Instruction {
  OpCode op = OpCode::HALT;
  int a = 0;
  int b = 0;
  int c = 0;
  int jump = 0;
  // Source position, used only for runtime diagnostics. Native codegen in
  // PHASE 7 ignores these fields.
  int line = 0;
  int col = 0;
};

// One entry of a proto's capture list. `index` is a local slot in the parent
// when `inParentLocal` is true, or one of the parent's upvalues otherwise.
// PHASE 6a only builds protos with an empty list; 6b fills it in.
struct UpvalDesc {
  bool inParentLocal = true;
  int index = 0;
};

// One C function declared in an `extern "C"` block (PHASE 6.6). The
// declaration carries the C symbol name and the signature, in the FFI
// vocabulary. It is pure link metadata: the block emits no bytecode, and the
// development engine resolves `name` through `Platform::lookupSymbol` when the
// proto starts. The AOT backend (PHASE 7) reads the same entries to emit the
// relocations of the generated binary.
struct ExternSymbol {
  std::string name;                     // the C symbol, e.g. "strlen"
  std::vector<std::string> paramTypes;  // one type name per parameter, in order
  // The declared return type, or an empty string when it was not annotated
  // (which the FFI marshalling reads as C's implicit `int`). `void` is stored
  // literally.
  std::string returnType;
};

// A function prototype. The root chunk is the `<main>` proto and nested
// functions are its `children`, instantiated by `CLOSURE` at runtime. Protos
// are shared pointers so a closure keeps its code alive after the chunk that
// produced it is gone (REPL entries, `:load`, dynamically returned functions).
struct Bytecode {
  std::string name = "<main>";
  int numParams = 0;
  // True when the last parameter is variadic (`fn min(a, rest...)`). The frame
  // then accepts `numParams - 1` or more arguments and binds the last slot to
  // an array of the extras; the VM packs them, so no opcode is needed.
  bool variadic = false;
  std::vector<Instruction> code;
  // Tagged constant pool. Uses `Value` so the difference between integers and
  // floats survives, and so strings and global names live in the same pool.
  std::vector<Value> constants;
  std::vector<std::shared_ptr<Bytecode>> children;
  std::vector<UpvalDesc> upvalues;
  // C symbols declared in `extern "C"` blocks in this proto (PHASE 6.6). The
  // development engine resolves each one before the proto runs; the AOT backend
  // reads them to emit the binary's relocations.
  std::vector<ExternSymbol> externs;
  // No per-proto capture storage: upvalue cells live in the `Function` value
  // produced by `CLOSURE` (`Function::upvalues`), so closures created from the
  // same proto in different frames never overwrite each other's captures.
  int numRegisters = 0;
  // Number of local cells the frame needs.
  int numLocals = 0;
};

// Resets the bytecode while keeping the capacity of its containers.
void bytecodeInit(Bytecode& bc);

// Appends an instruction and returns its index.
int bytecodeEmit(Bytecode& bc, const Instruction& instruction);

// Releases the bytecode contents. In C++ the containers manage their own
// memory, but the function is kept for symmetry with the original design.
void bytecodeFree(Bytecode& bc);
