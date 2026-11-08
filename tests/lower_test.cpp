// Tests for the x86-64 bytecode lowering (PHASE 7c.1).
//
// The lowering no longer maps bytecode registers to physical CPU registers:
// each register is a 16-byte slot in a register file on the stack, holding a
// boxed `AotValue`. The tests therefore run the generated code (Linux x86-64
// only) and read the `AotValue` the chunk returns in rax:rdx, instead of
// comparing bytes. Byte-exact coverage of the encoders lives in
// `codegen_test.cpp`; here the subject is the lowering's behaviour.
//
// Every opcode the slice does not lower must raise a LoweringError rather than
// emit wrong code, so the deferred cases are tested too.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bytecode.h"
#include "native/codegen.h"
#include "native/lower.h"
#include "native/target.h"
#include "native/value.h"

#if defined(__linux__) && defined(__x86_64__)
#define LOWER_EXEC_TESTS 1
#include <sys/mman.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

static Bytecode makeProto(const std::vector<Instruction>& code,
                          const std::vector<Value>& constants, int numRegisters) {
  Bytecode proto;
  proto.name = "<test>";
  proto.code = code;
  proto.constants = constants;
  proto.numRegisters = numRegisters;
  return proto;
}

static Instruction instr(OpCode op, int a = 0, int b = 0, int c = 0, int jump = 0) {
  Instruction i;
  i.op = op;
  i.a = a;
  i.b = b;
  i.c = c;
  i.jump = jump;
  return i;
}

static LoweredChunk lower(const Bytecode& proto) {
  return lowerToX64(Target::linuxX64(), proto, CodeGen{});
}

static bool loweringThrows(const Bytecode& proto, const char* needle) {
  try {
    lower(proto);
  } catch (const LoweringError& error) {
    return std::string(error.what()).find(needle) != std::string::npos;
  }
  return false;
}

// --- Structural tests ---

static void testImageIsSelfContained() {
  // The image carries the program, the runtime and `_start`, so it is larger
  // than the program alone and both entry points are inside it.
  LoweredChunk chunk =
      lower(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(static_cast<int64_t>(1))}, 1));
  check(!chunk.code.empty(), "the lowered image is not empty");
  check(chunk.programEntry < chunk.code.size(), "the chunk entry is inside the image");
  check(chunk.processEntry < chunk.code.size(), "the _start entry is inside the image");
  check(chunk.programEntry != chunk.processEntry, "the chunk and _start entries differ");
}

// --- Error tests: the deferred opcodes must be rejected, not miscompiled ---

static void testDeferredOpcodesThrow() {
  check(loweringThrows(makeProto({instr(OpCode::CALL, 0, 1), instr(OpCode::HALT)}, {}, 2),
                       "CALL/RETURN"),
        "CALL raises a LoweringError");
  check(loweringThrows(makeProto({instr(OpCode::NEWTABLE, 0, 0), instr(OpCode::HALT)}, {}, 2),
                       "NEWTABLE"),
        "NEWTABLE raises a LoweringError");
  check(loweringThrows(
            makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(std::string("hi"))}, 1),
            "LOADK"),
        "LOADK of a string raises a LoweringError");
}

static void testBadJumpTargetThrows() {
  auto proto = makeProto({instr(OpCode::JMP, 0, 0, 0, 99), instr(OpCode::HALT)}, {}, 1);
  check(loweringThrows(proto, "outside the code"), "an out-of-range jump target throws");
}

static void testInvalidUnopSelectorThrows() {
  auto proto = makeProto(
      {instr(OpCode::UNOP, 0, 0, 99), instr(OpCode::HALT)}, {}, 1);
  check(loweringThrows(proto, "UNOP selector"), "an invalid UNOP selector throws");
}

// --- Execution tests (Linux x86-64 only) ---

#ifdef LOWER_EXEC_TESTS

// Maps the image executable and returns a pointer to `offset` inside it.
static void* emitExecutable(const std::vector<std::uint8_t>& code,
                            std::uint64_t offset) {
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    std::exit(1);
  }
  std::memcpy(mem, code.data(), code.size());
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    std::exit(1);
  }
  return static_cast<std::uint8_t*>(mem) + offset;
}

// Runs the chunk and returns the `AotValue` it leaves in rax:rdx (R0). A chunk
// that hits a runtime error exits the process, so the error path is exercised
// end-to-end in `freestanding_test.cpp`, not here.
static AotValue run(const Bytecode& proto) {
  LoweredChunk chunk = lower(proto);
  auto fn = reinterpret_cast<AotValue (*)()>(
      emitExecutable(chunk.code, chunk.programEntry));
  return fn();
}

static void checkInt(const Bytecode& proto, std::int64_t expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == expected;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%lld, expected Int %lld\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<long long>(static_cast<std::int64_t>(value.payload)),
                static_cast<long long>(expected));
  }
}

static void checkBool(const Bytecode& proto, bool expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Bool) &&
            (value.payload != 0) == expected;
  check(ok, what);
}

static void testExecLoads() {
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(42))}, 1),
           42, "exec: LOADK int 42");
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(-7))}, 1),
           -7, "exec: LOADK int -7");
  checkBool(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(true)}, 1),
            true, "exec: LOADK true");
  checkBool(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(false)}, 1),
            false, "exec: LOADK false");

  AotValue nil = run(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                               {Value()}, 1));
  check(nil.tag == static_cast<std::uint64_t>(AotTag::Nil), "exec: LOADK nil");
}

static void testExecMove() {
  // R1 = R0; R0 = R1 -> the value moves through the register file.
  checkInt(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::MOVE, 1, 0),
                      instr(OpCode::MOVE, 0, 1), instr(OpCode::HALT)},
                     {Value(static_cast<int64_t>(9))}, 2),
           9, "exec: MOVE preserves tag and payload");
}

static void testExecArithmetic() {
  auto add = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))}, 2);
  checkInt(add, 5, "exec: 2 + 3 == 5");

  auto sub = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(10)), Value(static_cast<int64_t>(3))}, 2);
  checkInt(sub, 7, "exec: 10 - 3 == 7");

  auto mul = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::MUL, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(6)), Value(static_cast<int64_t>(7))}, 2);
  checkInt(mul, 42, "exec: 6 * 7 == 42");

  // Distinct destination: R2 = R0 + R1, then R0 = R2.
  auto distinct = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                             instr(OpCode::ADD, 2, 0, 1), instr(OpCode::MOVE, 0, 2),
                             instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(20)), Value(static_cast<int64_t>(22))}, 3);
  checkInt(distinct, 42, "exec: R2 = R0 + R1 (distinct destination)");
}

static void testExecBitwise() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::BAND, 12, 10), 8, "exec: 12 & 10 == 8");
  checkInt(mk(OpCode::BOR, 12, 10), 14, "exec: 12 | 10 == 14");
  checkInt(mk(OpCode::BXOR, 12, 10), 6, "exec: 12 ^ 10 == 6");
}

static void testExecComparisonsAsValues() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 3);
  };
  checkBool(mk(OpCode::EQ, 5, 5), true, "exec: (5 == 5) is true");
  checkBool(mk(OpCode::EQ, 5, 6), false, "exec: (5 == 6) is false");
  checkBool(mk(OpCode::NEQ, 5, 6), true, "exec: (5 != 6) is true");
  checkBool(mk(OpCode::LT, 5, 9), true, "exec: (5 < 9) is true");
  checkBool(mk(OpCode::LT, 9, 5), false, "exec: (9 < 5) is false");
  checkBool(mk(OpCode::LTE, 5, 5), true, "exec: (5 <= 5) is true");
  checkBool(mk(OpCode::GT, 9, 5), true, "exec: (9 > 5) is true");
  checkBool(mk(OpCode::GTE, 5, 9), false, "exec: (5 >= 9) is false");

  // Equality is type-aware: an int never equals a bool with the same payload.
  auto mixed = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                          instr(OpCode::EQ, 2, 0, 1), instr(OpCode::MOVE, 0, 2),
                          instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(1)), Value(true)}, 3);
  checkBool(mixed, false, "exec: 1 == true is false (tags differ)");
}

static void testExecUnop() {
  auto neg = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Negate)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(7))}, 1);
  checkInt(neg, -7, "exec: -7");

  auto bitNot = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::BitNot)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  checkInt(bitNot, -1, "exec: ~0 == -1");

  // !0 -> false: the integer 0 is truthy, so its negation is false.
  auto notZero = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  checkBool(notZero, false, "exec: !0 is false (0 is truthy)");

  auto notNil = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value()}, 1);
  checkBool(notNil, true, "exec: !nil is true");
}

static void testExecJumps() {
  // R0=1; JMP +3; R0=99 (skipped); HALT -> 1
  auto jmp = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::JMP, 0, 0, 0, 3),
                        instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(99))}, 1);
  checkInt(jmp, 1, "exec: JMP skips an instruction");

  // The integer 0 is truthy, so JMP_IF_FALSE does not jump and R0 becomes 99.
  auto truthyZero = makeProto({instr(OpCode::LOADK, 0, 0),
                               instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3),
                               instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                              {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(99))}, 1);
  checkInt(truthyZero, 99, "exec: JMP_IF_FALSE does not jump on integer 0 (truthy)");

  // nil is falsy, so JMP_IF_FALSE jumps over the assignment and R0 stays nil.
  auto nilFalse = makeProto({instr(OpCode::LOADK, 0, 0),
                             instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3),
                             instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                            {Value(), Value(static_cast<int64_t>(99))}, 1);
  AotValue nilResult = run(nilFalse);
  check(nilResult.tag == static_cast<std::uint64_t>(AotTag::Nil),
        "exec: JMP_IF_FALSE jumps on nil (falsy)");

  // true is truthy, so JMP_IF_TRUE jumps over the assignment.
  auto trueJump = makeProto({instr(OpCode::LOADK, 0, 0),
                             instr(OpCode::JMP_IF_TRUE, 0, 0, 0, 3),
                             instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                            {Value(true), Value(static_cast<int64_t>(99))}, 1);
  checkBool(trueJump, true, "exec: JMP_IF_TRUE jumps on true");
}

static void testExecBackwardJumpLoop() {
  // A countdown: R0 = 3; R1 = 1; R2 = 0;
  //   top(3): R0 = R0 - R1; R3 = (R0 != R2); if R3 goto top; HALT.
  // 3 -> 2 -> 1 -> 0, then NEQ yields false, JMP_IF_TRUE does not jump and the
  // loop exits with 0 in R0. Exercises a backward jump and a comparison value.
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0),             // 0: R0 = 3
       instr(OpCode::LOADK, 1, 1),             // 1: R1 = 1
       instr(OpCode::LOADK, 2, 2),             // 2: R2 = 0
       instr(OpCode::SUB, 0, 0, 1),            // 3: top: R0 = R0 - R1
       instr(OpCode::NEQ, 3, 0, 2),            // 4: R3 = (R0 != 0)
       instr(OpCode::JMP_IF_TRUE, 3, 0, 0, 3), // 5: if R3 goto 3
       instr(OpCode::HALT)},                   // 6
      {Value(static_cast<int64_t>(3)), Value(static_cast<int64_t>(1)),
       Value(static_cast<int64_t>(0))},
      4);
  checkInt(proto, 0, "exec: countdown loop terminates at 0");
}

static void testExecManyRegisters() {
  // The register file is on the stack, so the old 12-register ceiling is gone:
  // 40 registers must work. R39 = 5 + 6.
  auto proto = makeProto({instr(OpCode::LOADK, 38, 0), instr(OpCode::LOADK, 39, 1),
                          instr(OpCode::ADD, 39, 38, 39), instr(OpCode::MOVE, 0, 39),
                          instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(6))}, 40);
  checkInt(proto, 11, "exec: 40 registers are usable (no physical-register ceiling)");
}

static void checkDouble(const Bytecode& proto, double expected, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Double) &&
            value.payload == aotDouble(expected).payload;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%016llx, expected Double %016llx\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<unsigned long long>(value.payload),
                static_cast<unsigned long long>(aotDouble(expected).payload));
  }
}

// A `float` result stores the binary32 value widened to binary64, so the
// expected payload is `aotFloat`, not `aotDouble`.
static void checkFloat(const Bytecode& proto, double widened, const char* what) {
  AotValue value = run(proto);
  bool ok = value.tag == static_cast<std::uint64_t>(AotTag::Float) &&
            value.payload == aotFloat(widened).payload;
  check(ok, what);
  if (!ok) {
    std::printf("      got tag=%llu payload=%016llx, expected Float %016llx\n",
                static_cast<unsigned long long>(value.tag),
                static_cast<unsigned long long>(value.payload),
                static_cast<unsigned long long>(aotFloat(widened).payload));
  }
}

static void testExecFloatLoads() {
  checkDouble(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)}, {Value(1.5)}, 1),
              1.5, "exec: LOADK double 1.5");
  checkFloat(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                       {Value::fromFloat(0.1f)}, 1),
             static_cast<double>(0.1f), "exec: LOADK float 0.1f");
}

static void testExecFloatArithmetic() {
  auto mk = [](OpCode op, Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {x, y}, 2);
  };
  checkDouble(mk(OpCode::ADD, Value(1.5), Value(2.5)), 4.0, "exec: 1.5 + 2.5 == 4.0 (double)");
  checkDouble(mk(OpCode::SUB, Value(5.5), Value(2.0)), 3.5, "exec: 5.5 - 2.0 == 3.5 (double)");
  checkDouble(mk(OpCode::MUL, Value(1.5), Value(2.0)), 3.0, "exec: 1.5 * 2.0 == 3.0 (double)");
  checkDouble(mk(OpCode::DIV, Value(7.0), Value(2.0)), 3.5, "exec: 7.0 / 2.0 == 3.5 (double)");

  // float + float stays binary32: 0.1f + 0.2f rounds to 0.3f.
  checkFloat(mk(OpCode::ADD, Value::fromFloat(0.1f), Value::fromFloat(0.2f)),
             static_cast<double>(0.3f), "exec: 0.1f + 0.2f == 0.3f (binary32 rounding)");

  // int widens to float: 1 + 1.5f is a float.
  checkFloat(mk(OpCode::ADD, Value(static_cast<int64_t>(1)), Value::fromFloat(1.5f)), 2.5,
             "exec: 1 + 1.5f == 2.5 (float)");

  // A float with a double promotes to double.
  checkDouble(mk(OpCode::ADD, Value::fromFloat(1.5f), Value(1.5)), 3.0,
              "exec: 1.5f + 1.5 == 3.0 (double)");
}

static void testExecIntDivModPow() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::DIV, 7, 2), 3, "exec: 7 / 2 == 3 (int)");
  checkInt(mk(OpCode::MOD, 7, 3), 1, "exec: 7 % 3 == 1 (int)");
  checkInt(mk(OpCode::POW, 2, 10), 1024, "exec: 2 ** 10 == 1024 (int)");
  checkInt(mk(OpCode::POW, 3, 0), 1, "exec: 3 ** 0 == 1 (int)");
  // INT64_MIN / -1 is the `idiv` overflow; the hosted engine returns a itself.
  checkInt(mk(OpCode::DIV, INT64_MIN, -1), INT64_MIN, "exec: INT64_MIN / -1 wraps (int)");
  checkInt(mk(OpCode::MOD, INT64_MIN, -1), 0, "exec: INT64_MIN % -1 == 0 (int)");
}

static void testExecShifts() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 0, 0, 1), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 2);
  };
  checkInt(mk(OpCode::SHL, 1, 4), 16, "exec: 1 << 4 == 16");
  checkInt(mk(OpCode::SHR, -8, 1), -4, "exec: -8 >> 1 == -4 (arithmetic)");
  checkInt(mk(OpCode::SHL, 1, 63), INT64_MIN, "exec: 1 << 63 == INT64_MIN");
}

static void testExecFloatComparisons() {
  auto mk = [](OpCode op, Value x, Value y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {x, y}, 3);
  };
  checkBool(mk(OpCode::EQ, Value(1.5), Value(1.5)), true, "exec: 1.5 == 1.5 is true");
  checkBool(mk(OpCode::EQ, Value(1.5), Value(1.6)), false, "exec: 1.5 == 1.6 is false");
  checkBool(mk(OpCode::LT, Value(1.5), Value(1.6)), true, "exec: 1.5 < 1.6 is true");
  checkBool(mk(OpCode::GT, Value(1.5), Value(1.6)), false, "exec: 1.5 > 1.6 is false");
  // Mixed numeric equality is numeric: an int and a double compare by value.
  checkBool(mk(OpCode::EQ, Value(static_cast<int64_t>(1)), Value(1.0)), true,
            "exec: 1 == 1.0 is true (numeric equality)");
  checkBool(mk(OpCode::LT, Value(static_cast<int64_t>(1)), Value::fromFloat(1.5f)), true,
            "exec: 1 < 1.5f is true (mixed ordering)");
}

static void testExecFloatNegate() {
  auto neg = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Negate)), instr(OpCode::HALT)},
      {Value(1.5)}, 1);
  checkDouble(neg, -1.5, "exec: -(1.5) == -1.5 (double)");
}

#endif  // LOWER_EXEC_TESTS

int main() {
  testImageIsSelfContained();
  testDeferredOpcodesThrow();
  testBadJumpTargetThrows();
  testInvalidUnopSelectorThrows();

#ifdef LOWER_EXEC_TESTS
  testExecLoads();
  testExecMove();
  testExecArithmetic();
  testExecBitwise();
  testExecComparisonsAsValues();
  testExecUnop();
  testExecJumps();
  testExecBackwardJumpLoop();
  testExecManyRegisters();
  testExecFloatLoads();
  testExecFloatArithmetic();
  testExecIntDivModPow();
  testExecShifts();
  testExecFloatComparisons();
  testExecFloatNegate();
#else
  std::printf("skipped: execution tests require Linux x86-64\n");
#endif

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all lower tests passed\n");
  return 0;
}
