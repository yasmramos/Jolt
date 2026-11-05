// Tests for the x86-64 lowering (PHASE 7b).
//
// Byte-exact tests verify the encoding of simple instruction sequences.
// Execution tests (Linux x86-64 only) verify the generated code actually
// runs and produces the expected result.

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Builds a proto with the given instructions and constants.
static Bytecode makeProto(const std::vector<Instruction>& code,
                          const std::vector<Value>& constants,
                          int numRegisters) {
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

// --- Byte-exact tests ---

static void testLoadKInt() {
  // R0 = 42; HALT
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0), instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(42))},
      1);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  // mov rax, 42; ret
  // 48 C7 C0 2A 00 00 00 C3
  check(code.size() == 8, "LOADK int: size is 8 bytes");
  check(code[0] == 0x48 && code[1] == 0xC7 && code[2] == 0xC0,
        "LOADK int: mov rax, imm32");
  check(code[3] == 42, "LOADK int: immediate is 42");
  check(code[7] == 0xC3, "HALT: ret");
}

static void testMove() {
  // R1 = R0; HALT
  auto proto = makeProto(
      {instr(OpCode::MOVE, 1, 0, 0), instr(OpCode::HALT)},
      {},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  // mov rcx, rax; ret
  // 48 89 C1 C3
  check(code.size() == 4, "MOVE: size is 4 bytes");
  check(code[0] == 0x48 && code[1] == 0x89 && code[2] == 0xC1,
        "MOVE: mov rcx, rax");
}

static void testAdd() {
  // R0 = R0 + R1; HALT
  auto proto = makeProto(
      {instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
      {},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  // add rax, rcx; ret
  // 48 01 C8 C3
  check(code.size() == 4, "ADD: size is 4 bytes");
  check(code[0] == 0x48 && code[1] == 0x01 && code[2] == 0xC8,
        "ADD: add rax, rcx");
}

// --- Execution tests (Linux x86-64 only) ---

#ifdef LOWER_EXEC_TESTS

static void* emitExecutable(const std::vector<std::uint8_t>& code) {
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

static void testExecAdd() {
  // R0 = 2; R1 = 3; R0 = R0 + R1; HALT
  // The function returns the value in rax (R0).
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),
       instr(OpCode::LOADK, 1, 1, 0),
       instr(OpCode::ADD, 0, 0, 1),
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(2)), Value(static_cast<int64_t>(3))},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 5, "exec: 2 + 3 == 5");
}

static void testExecSub() {
  // R0 = 10; R1 = 3; R0 = R0 - R1; HALT
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),
       instr(OpCode::LOADK, 1, 1, 0),
       instr(OpCode::SUB, 0, 0, 1),
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(10)), Value(static_cast<int64_t>(3))},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 7, "exec: 10 - 3 == 7");
}

static void testExecMul() {
  // R0 = 6; R1 = 7; R0 = R0 * R1; HALT
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),
       instr(OpCode::LOADK, 1, 1, 0),
       instr(OpCode::MUL, 0, 0, 1),
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(6)), Value(static_cast<int64_t>(7))},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 42, "exec: 6 * 7 == 42");
}

static void testExecBitwise() {
  // R0 = 0b1100; R1 = 0b1010; R0 = R0 & R1; HALT
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),
       instr(OpCode::LOADK, 1, 1, 0),
       instr(OpCode::BAND, 0, 0, 1),
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(12)), Value(static_cast<int64_t>(10))},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 8, "exec: 12 & 10 == 8");
}

static void testExecJmp() {
  // R0 = 1; JMP +2; R0 = 99; R0 = R0 + 1; HALT
  // The JMP should skip the "R0 = 99" instruction.
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),
       instr(OpCode::JMP, 0, 0, 0, 3),  // jump to instruction 3
       instr(OpCode::LOADK, 0, 1, 0),   // skipped
       instr(OpCode::LOADK, 0, 2, 0),   // R0 = 1
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0)),
       Value(static_cast<int64_t>(99)),
       Value(static_cast<int64_t>(1))},
      1);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 1, "exec: JMP skips instruction");
}

static void testExecJmpIfFalse() {
  // R0 = 0; R1 = 1; R0 = R0 + R1; JMP_IF_FALSE R0, +2; R0 = 99; HALT
  // R0 is 1 (truthy), so JMP_IF_FALSE does NOT jump, and R0 = 99 executes.
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),           // R0 = 0
       instr(OpCode::LOADK, 1, 1, 0),           // R1 = 1
       instr(OpCode::ADD, 0, 0, 1),             // R0 = 1
       instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 5), // if !R0 jump to 5
       instr(OpCode::LOADK, 0, 2, 0),           // R0 = 99
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0)),
       Value(static_cast<int64_t>(1)),
       Value(static_cast<int64_t>(99))},
      2);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 99, "exec: JMP_IF_FALSE with truthy R0 does not jump");
}

static void testExecJmpIfFalseTaken() {
  // R0 = 0; JMP_IF_FALSE R0, +2; R0 = 99; HALT
  // R0 is 0 (falsy), so JMP_IF_FALSE jumps to HALT, and R0 = 99 is skipped.
  auto proto = makeProto(
      {instr(OpCode::LOADK, 0, 0, 0),           // R0 = 0
       instr(OpCode::JMP_IF_FALSE, 0, 0, 0, 3), // if !R0 jump to 3
       instr(OpCode::LOADK, 0, 1, 0),           // R0 = 99 (skipped)
       instr(OpCode::HALT)},
      {Value(static_cast<int64_t>(0)),
       Value(static_cast<int64_t>(99))},
      1);
  auto code = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(code));
  std::int64_t result = fn();
  check(result == 0, "exec: JMP_IF_FALSE with falsy R0 jumps");
}

#endif  // LOWER_EXEC_TESTS

int main() {
  // Byte-exact tests
  testLoadKInt();
  testMove();
  testAdd();

  // Execution tests (Linux x86-64 only)
#ifdef LOWER_EXEC_TESTS
  testExecAdd();
  testExecSub();
  testExecMul();
  testExecBitwise();
  testExecJmp();
  testExecJmpIfFalse();
  testExecJmpIfFalseTaken();
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