// Tests for the x86-64 bytecode lowering (PHASE 7b).
//
// Byte-exact tests verify the encoding of instruction sequences against
// hand-computed reference bytes. Execution tests (Linux x86-64 only) run the
// generated code and check the value it leaves in rax (R0).
//
// PHASE 7b lowers only the runtime-free, integer-only subset. Every other
// opcode must raise a LoweringError instead of emitting wrong code, so the
// deferred cases are tested too.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bytecode.h"
#include "native/codegen.h"
#include "native/lower.h"
#include "native/target.h"

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

static std::string toHex(const std::vector<std::uint8_t>& bytes) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (std::uint8_t b : bytes) {
    out += digits[b >> 4];
    out += digits[b & 0x0F];
  }
  return out;
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

static std::vector<std::uint8_t> lower(const Bytecode& proto) {
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

// --- Byte-exact tests ---

static void testLoadKInt() {
  // R0 = 42; HALT -> mov rax, 42; ret
  auto code = lower(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                              {Value(static_cast<int64_t>(42))}, 1));
  check(toHex(code) == "48C7C02A000000C3", "LOADK int: mov rax, 42; ret");
}

static void testLoadKBool() {
  // true -> 1, false -> 0.
  auto t = lower(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                           {Value(true)}, 1));
  check(toHex(t) == "48C7C001000000C3", "LOADK true: mov rax, 1; ret");
  auto f = lower(makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                           {Value(false)}, 1));
  check(toHex(f) == "48C7C000000000C3", "LOADK false: mov rax, 0; ret");
}

static void testMove() {
  // R1 = R0; HALT -> mov rcx, rax; ret
  auto code = lower(makeProto({instr(OpCode::MOVE, 1, 0), instr(OpCode::HALT)}, {}, 2));
  check(toHex(code) == "4889C1C3", "MOVE: mov rcx, rax; ret");
}

static void testAddSameReg() {
  // R0 = R0 + R1 (a == b, the shape the compiler emits) -> add rax, rcx; ret
  auto code = lower(makeProto({instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)}, {}, 2));
  check(toHex(code) == "4801C8C3", "ADD a==b: add rax, rcx; ret");
}

static void testAddDistinctDest() {
  // R2 = R0 + R1 -> mov rdx, rax; add rdx, rcx; ret
  auto code = lower(
      makeProto({instr(OpCode::ADD, 2, 0, 1), instr(OpCode::HALT)}, {}, 3));
  check(toHex(code) == "4889C24801CAC3", "ADD a!=b: mov rdx, rax; add rdx, rcx; ret");
}

static void testCompareAsValue() {
  // R0 = R0 == R1 -> cmp rax, rcx; sete al; movzx rax, al; ret
  auto code = lower(makeProto({instr(OpCode::EQ, 0, 0, 1), instr(OpCode::HALT)}, {}, 2));
  check(toHex(code) == "4839C80F94C0480FB6C0C3",
        "EQ: cmp; sete; movzx; ret (materialized boolean)");
}

static void testUnopNotBytes() {
  // R0 = !R0 -> test rax, rax; sete al; movzx rax, al; ret
  auto code = lower(makeProto(
      {instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {}, 1));
  check(toHex(code) == "4885C00F94C0480FB6C0C3", "UNOP Not: test; sete; movzx; ret");
}

// --- Error tests: the deferred opcodes must be rejected, not miscompiled ---

static void testDeferredOpcodesThrow() {
  check(loweringThrows(makeProto({instr(OpCode::DIV, 0, 0, 1), instr(OpCode::HALT)}, {}, 2),
                       "DIV/MOD/POW"),
        "DIV raises a LoweringError");
  check(loweringThrows(makeProto({instr(OpCode::SHL, 0, 0, 1), instr(OpCode::HALT)}, {}, 2),
                       "SHL/SHR"),
        "SHL raises a LoweringError");
  check(loweringThrows(makeProto({instr(OpCode::CALL, 0, 1), instr(OpCode::HALT)}, {}, 2),
                       "CALL/RETURN"),
        "CALL raises a LoweringError");
  check(loweringThrows(
            makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(std::string("hi"))}, 1),
            "LOADK"),
        "LOADK of a string raises a LoweringError");
  check(loweringThrows(
            makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::HALT)},
                      {Value(1.5)}, 1),
            "LOADK"),
        "LOADK of a float raises a LoweringError (deferred to 7c)");
}

static void testRegisterLimitThrows() {
  Bytecode proto = makeProto({instr(OpCode::HALT)}, {}, 13);
  check(loweringThrows(proto, "up to 12"), "more than 12 registers raises a LoweringError");
}

static void testBadJumpTargetThrows() {
  // A jump past the end of the code must be rejected, not encoded blindly.
  auto proto = makeProto({instr(OpCode::JMP, 0, 0, 0, 99), instr(OpCode::HALT)}, {}, 1);
  check(loweringThrows(proto, "outside the code"), "an out-of-range jump target throws");
}

// --- Execution tests (Linux x86-64 only) ---

#ifdef LOWER_EXEC_TESTS

static void* emitExecutable(const std::vector<std::uint8_t>& code) {
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
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
  return mem;
}

static std::int64_t run(const Bytecode& proto) {
  auto code = lower(proto);
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  return fn();
}

static void testExecBinaryDistinctDest() {
  // R0=2; R1=3; R2=R0+R1; R0=R2; HALT -> 5. Exercises the mov+op path.
  auto proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                          instr(OpCode::ADD, 2, 0, 1), instr(OpCode::MOVE, 0, 2),
                          instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))}, 3);
  check(run(proto) == 5, "exec: R2 = R0 + R1 (distinct destination)");
}

static void testExecBinaryDestIsRightOperand() {
  // R0=2; R1=3; R0 = R1 + R0; HALT -> 5. a==c, so the operand is stashed.
  auto proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                          instr(OpCode::ADD, 0, 1, 0), instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))}, 2);
  check(run(proto) == 5, "exec: R0 = R1 + R0 (destination aliases right operand)");
}

static void testExecArithmetic() {
  auto sub = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(10)), Value(static_cast<int64_t>(3))}, 2);
  check(run(sub) == 7, "exec: 10 - 3 == 7");

  auto mul = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                        instr(OpCode::MUL, 0, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(6)), Value(static_cast<int64_t>(7))}, 2);
  check(run(mul) == 42, "exec: 6 * 7 == 42");

  auto band = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                         instr(OpCode::BAND, 0, 0, 1), instr(OpCode::HALT)},
                        {Value(static_cast<int64_t>(12)), Value(static_cast<int64_t>(10))}, 2);
  check(run(band) == 8, "exec: 12 & 10 == 8");
}

static void testExecComparisonsAsValues() {
  auto mk = [](OpCode op, std::int64_t x, std::int64_t y) {
    return makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                      instr(op, 2, 0, 1), instr(OpCode::MOVE, 0, 2), instr(OpCode::HALT)},
                     {Value(x), Value(y)}, 3);
  };
  check(run(mk(OpCode::EQ, 5, 5)) == 1, "exec: (5 == 5) == 1");
  check(run(mk(OpCode::EQ, 5, 6)) == 0, "exec: (5 == 6) == 0");
  check(run(mk(OpCode::NEQ, 5, 6)) == 1, "exec: (5 != 6) == 1");
  check(run(mk(OpCode::LT, 5, 9)) == 1, "exec: (5 < 9) == 1");
  check(run(mk(OpCode::LT, 9, 5)) == 0, "exec: (9 < 5) == 0");
  check(run(mk(OpCode::LTE, 5, 5)) == 1, "exec: (5 <= 5) == 1");
  check(run(mk(OpCode::GT, 9, 5)) == 1, "exec: (9 > 5) == 1");
  check(run(mk(OpCode::GTE, 5, 9)) == 0, "exec: (5 >= 9) == 0");
}

static void testExecUnop() {
  auto neg = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Negate)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(7))}, 1);
  check(run(neg) == -7, "exec: -7");

  auto bitNot = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::BitNot)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  check(run(bitNot) == -1, "exec: ~0 == -1");

  // !0 -> 1 (zero is truthy in Jolt, but here it is the integer 0 -> not 0 == 1).
  auto notZero = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0))}, 1);
  check(run(notZero) == 1, "exec: !0 == 1");

  auto notFive = makeProto(
      {instr(OpCode::LOADK, 0, 0),
       instr(OpCode::UNOP, 0, 0, static_cast<int>(UnaryOp::Not)), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(5))}, 1);
  check(run(notFive) == 0, "exec: !5 == 0");
}

static void testExecJumps() {
  // R0=1; JMP +2; R0=99 (skipped); HALT -> 1
  auto jmp = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::JMP, 0, 0, 0, 3),
                        instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                       {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(99))}, 1);
  check(run(jmp) == 1, "exec: JMP skips an instruction");

  // R0=0; R1=1; R0=R0+R1 (=1); JMP_IF_FALSE R0,+2; R0=99; HALT -> 99
  auto jif = makeProto(
      {instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1), instr(OpCode::ADD, 0, 0, 1),
       instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 5), instr(OpCode::LOADK, 0, 2),
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(1)),
       Value(static_cast<int64_t>(99))},
      2);
  check(run(jif) == 99, "exec: JMP_IF_FALSE does not jump when truthy");

  // R0=0; JMP_IF_FALSE R0,+2; R0=99; HALT -> 0
  auto jift = makeProto({instr(OpCode::LOADK, 0, 0),
                         instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3),
                         instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                        {Value(static_cast<int64_t>(0)), Value(static_cast<int64_t>(99))}, 1);
  check(run(jift) == 0, "exec: JMP_IF_FALSE jumps when falsy");

  // R0=1; JMP_IF_TRUE R0,+2; R0=99; HALT -> 1
  auto jtrue = makeProto({instr(OpCode::LOADK, 0, 0),
                          instr(OpCode::JMP_IF_TRUE, 0, 0, 0, 3),
                          instr(OpCode::LOADK, 0, 1), instr(OpCode::HALT)},
                         {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(99))}, 1);
  check(run(jtrue) == 1, "exec: JMP_IF_TRUE jumps when truthy");
}

static void testExecBackwardJumpLoop() {
  // A countdown: R0 = 3; R1 = 1; R2 = 0;
  //   top(3): R0 = R0 - R1; R3 = (R0 != R2); if R3 goto top; HALT.
  // 3 -> 2 -> 1 -> 0, then NEQ yields 0, JMP_IF_TRUE does not jump and the
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
  check(run(proto) == 0, "exec: countdown loop terminates at 0");
}

#endif  // LOWER_EXEC_TESTS

int main() {
  testLoadKInt();
  testLoadKBool();
  testMove();
  testAddSameReg();
  testAddDistinctDest();
  testCompareAsValue();
  testUnopNotBytes();
  testDeferredOpcodesThrow();
  testRegisterLimitThrows();
  testBadJumpTargetThrows();

#ifdef LOWER_EXEC_TESTS
  testExecBinaryDistinctDest();
  testExecBinaryDestIsRightOperand();
  testExecArithmetic();
  testExecComparisonsAsValues();
  testExecUnop();
  testExecJumps();
  testExecBackwardJumpLoop();
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
