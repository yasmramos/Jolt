// Tests for the freestanding AOT runtime (PHASE 7c.0).
//
// Three levels:
//   1. Arena: alignment, exhaustion, reset, null block -- pure, portable.
//   2. PlatformFreestanding: a real write/read file roundtrip through direct
//      syscalls, plus alloc/dealloc.
//   3. End to end: build a real ELF from emitted machine code (a `main` that
//      writes a string and returns 0), write it to a temp file, run it, and
//      check its stdout, exit code and that it has no dynamic dependencies.
//
// Levels 2 and 3 need Linux x86-64; elsewhere they are skipped and only the
// arena runs.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "native/arena.h"
#include "native/entry.h"
#include "native/elf.h"
#include "native/lower.h"
#include "native/platform_freestanding.h"
#include "native/runtime.h"
#include "native/value.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// --------------------------------------------------------------------- arena

static void test_arena() {
  alignas(64) unsigned char block[256];
  Arena arena(block, sizeof block);

  check(arena.capacity() == sizeof block, "arena reports its capacity");
  check(arena.used() == 0, "arena starts empty");

  void* a = arena.allocate(1, 1);
  check(a == block, "first allocation is at the block start");
  check(reinterpret_cast<std::uintptr_t>(a) % 1 == 0, "1-byte alignment");

  void* b = arena.allocate(8, 8);
  check(reinterpret_cast<std::uintptr_t>(b) % 8 == 0, "8-byte alignment honoured");
  check(b != a, "distinct allocations do not overlap");

  void* c = arena.allocate(16, 16);
  check(reinterpret_cast<std::uintptr_t>(c) % 16 == 0, "16-byte alignment honoured");

  // Zero-size allocations still get distinct addresses.
  void* z1 = arena.allocate(0, 8);
  void* z2 = arena.allocate(0, 8);
  check(z1 != nullptr && z2 != nullptr && z1 != z2, "zero-size allocations are distinct");

  // Exhaustion returns nullptr instead of throwing.
  void* huge = arena.allocate(1u << 20, 8);
  check(huge == nullptr, "exhausted arena returns nullptr");

  arena.reset();
  check(arena.used() == 0, "reset reclaims the region");
  void* again = arena.allocate(4, 4);
  check(again == block, "allocation after reset reuses the block start");

  // A bad alignment is rejected.
  Arena empty(nullptr, 0);
  check(empty.allocate(8, 8) == nullptr, "null-block arena fails every allocation");
  check(empty.allocate(8, 3) == nullptr, "non-power-of-two alignment rejected");
}

// --------------------------------------------------------------- platform

#if defined(__linux__) && defined(__x86_64__)
#define FREESTANDING_EXEC_TESTS 1
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef FREESTANDING_EXEC_TESTS

static void test_platform_roundtrip() {
  PlatformFreestanding platform;
  const char* path = "/tmp/jolt_freestanding_roundtrip.txt";
  const char* payload = "hello freestanding\n";

  int fd = platform.open(path, Platform::OpenMode::Write);
  check(fd != Platform::kInvalidFd, "open(Write) returns a descriptor");
  platform.write(fd, payload, std::strlen(payload));
  platform.flush(fd);
  platform.close(fd);

  fd = platform.open(path, Platform::OpenMode::Read);
  check(fd != Platform::kInvalidFd, "open(Read) returns a descriptor");
  char buffer[64] = {0};
  std::size_t n = platform.read(fd, buffer, sizeof buffer);
  platform.close(fd);
  check(n == std::strlen(payload), "read returns the written length");
  check(std::strcmp(buffer, payload) == 0, "read returns the written bytes");

  // seek back to the start and re-read.
  fd = platform.open(path, Platform::OpenMode::Read);
  check(platform.seek(fd, 0, Platform::SeekOrigin::Start), "seek(Start) succeeds");
  char first = 0;
  check(platform.read(fd, &first, 1) == 1 && first == 'h', "seek repositions the cursor");
  platform.close(fd);

  // Missing file: Read does not create.
  check(platform.open("/tmp/jolt_does_not_exist_12345", Platform::OpenMode::Read) ==
            Platform::kInvalidFd,
        "open(Read) on a missing file fails");

  // Allocator hands out aligned, distinct memory and frees are no-ops.
  void* p1 = platform.alloc(32, 16);
  void* p2 = platform.alloc(32, 16);
  check(p1 != nullptr && p2 != nullptr && p1 != p2, "platform alloc returns distinct blocks");
  check(reinterpret_cast<std::uintptr_t>(p1) % 16 == 0, "platform alloc honours alignment");
  platform.dealloc(p1);  // no-op, must not crash
}

// Builds a tiny program in `code` that writes `message` (a rodata offset known
// to the caller) and returns `exitCode`, then appends the freestanding runtime.
// Returns the entry offset and the text/rodata images.
struct BuiltProgram {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::uint64_t entry = 0;
};

// `main(argc, argv)`: jolt_write(1, messageAddress, messageLength); return N.
static BuiltProgram buildHello(std::int64_t messageAddress, std::int64_t messageLength,
                               std::int64_t exitCode) {
  CodeGen code;
  RuntimeHandles handles = newRuntimeHandles(code);

  CodeGen::Label mainLabel = code.newLabel();
  code.bind(mainLabel);

  // jolt_write(1, message, length)
  code.movImm(X64Reg::Rdi, 1);
  code.movImm(X64Reg::Rsi, messageAddress);
  code.movImm(X64Reg::Rdx, messageLength);
  code.callLabel(handles.write);

  code.movImm(X64Reg::Rax, exitCode);
  code.retInsn();

  std::uint64_t entry = emitFreestandingRuntime(code, handles, mainLabel);
  code.finalize();

  BuiltProgram built;
  built.text = code.code();
  built.entry = entry;
  return built;
}

// Runs `path` and captures its stdout and exit code.
struct RunResult {
  std::string output;
  int exitCode = -1;
};

static RunResult runProgram(const std::string& path) {
  std::string command = path + " > /tmp/jolt_freestanding_out.txt 2>&1";
  int status = std::system(command.c_str());
  RunResult result;
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::FILE* file = std::fopen("/tmp/jolt_freestanding_out.txt", "rb");
  if (file) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
      result.output.append(buffer, n);
    }
    std::fclose(file);
  }
  return result;
}

static void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  chmod(path.c_str(), 0755);
}

static void test_end_to_end() {
  // The .rodata section is loaded at 0x600000 (see elf.cpp); the message sits
  // at its very start.
  const char* message = "hello from a Jolt AOT binary\n";
  std::vector<std::uint8_t> rodata(message, message + std::strlen(message));
  const std::int64_t messageAddress = 0x600000;

  BuiltProgram built = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 0);
  built.rodata = rodata;

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), built.text, built.rodata, built.entry);
  const std::string path = "/tmp/jolt_freestanding_hello";
  writeFile(path, elf);

  RunResult result = runProgram(path);
  check(result.output == message, "the generated binary prints the expected bytes");
  check(result.exitCode == 0, "the generated binary exits with the program's code");

  // A non-zero return value must become the process exit code.
  BuiltProgram failing = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 7);
  failing.rodata = rodata;
  std::vector<std::uint8_t> elfFail =
      buildStaticElf(Target::linuxX64(), failing.text, failing.rodata, failing.entry);
  writeFile("/tmp/jolt_freestanding_exit7", elfFail);
  RunResult failed = runProgram("/tmp/jolt_freestanding_exit7");
  check(failed.exitCode == 7, "the program's return value becomes the exit code");

  // No dynamic dependencies: this is a static, freestanding binary.
  std::string ldd = "ldd " + path + " 2>&1";
  std::FILE* pipe = popen(ldd.c_str(), "r");
  std::string lddOut;
  if (pipe) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
      lddOut.append(buffer, n);
    }
    pclose(pipe);
  }
  check(lddOut.find("not a dynamic executable") != std::string::npos ||
            lddOut.find("statically linked") != std::string::npos,
        "the generated binary has no dynamic dependencies");
}

// Builds a static ELF from a lowered chunk and runs it, returning its stdout
// and exit code. The chunk already contains `_start` and the runtime.
static RunResult buildAndRun(const std::string& path, const Bytecode& proto) {
  LoweredChunk chunk = lowerToX64(Target::linuxX64(), proto, CodeGen{});
  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), chunk.code, {}, chunk.processEntry);
  writeFile(path, elf);
  return runProgram(path);
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

static Bytecode makeProto(const std::vector<Instruction>& code,
                          const std::vector<Value>& constants, int numRegisters) {
  Bytecode proto;
  proto.name = "<freestanding-test>";
  proto.code = code;
  proto.constants = constants;
  proto.numRegisters = numRegisters;
  return proto;
}

// Drives the boxed printer directly: the runtime's `jolt_print_value` is what
// the `print` builtin will call once `CALL` lands (7c.2), so this checks the
// integer-to-text routine (including the negative case) and the boolean/nil
// rendering today.
static void test_boxed_runtime_print() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  auto printValue = [&](AotTag tag, std::int64_t payload) {
    cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(tag));
    cg.movImm(X64Reg::Rsi, payload);
    cg.callLabel(rt.printValue);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  printValue(AotTag::Int, 42);
  printValue(AotTag::Int, -7);
  printValue(AotTag::Bool, 1);
  printValue(AotTag::Bool, 0);
  printValue(AotTag::Nil, 0);
  // Float/Double render through the embedded dtoa with the hosted engine's
  // shortest round-trip text: a double uses binary64, a float binary32.
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(0.1 + 0.2).payload));
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(1.0 / 3.0).payload));
  printValue(AotTag::Double, static_cast<std::int64_t>(aotDouble(-2.5).payload));
  printValue(AotTag::Float, static_cast<std::int64_t>(aotFloat(1.0f / 3.0f).payload));
  printValue(AotTag::Float, static_cast<std::int64_t>(aotFloat(0.1f).payload));
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/jolt_boxed_print";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "42\n-7\ntrue\nfalse\nnil\n"
                         "0.30000000000000004\n0.3333333333333333\n-2.5\n"
                         "0.33333334\n0.1\n",
        "the boxed runtime prints int, bool, nil, double and float");
  check(result.exitCode == 0, "the printing program exits 0");
}

// A type mismatch in a lowered operation must report an error and exit 70,
// matching the development engine's runtime-error exit code.
static void test_boxed_runtime_error() {
  // R0 = true; R1 = false; R0 = R0 - R1 -> subtraction is int-only, so this
  // must hit the runtime's "unsupported operand types" path and exit 70.
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                             {Value(true), Value(false)}, 2);
  RunResult result = buildAndRun("/tmp/jolt_boxed_error", proto);
  check(result.exitCode == 70, "a runtime type error exits 70");
  check(result.output.find("unsupported operand types") != std::string::npos,
        "the runtime error names the unsupported operand types");
}

// A division by zero in a lowered operation must report an error and exit 70,
// exercising the new error path in a real static ELF.
static void test_boxed_runtime_div_zero() {
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::DIV, 0, 0, 1), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(1)), Value(static_cast<int64_t>(0))}, 2);
  RunResult result = buildAndRun("/tmp/jolt_boxed_divzero", proto);
  check(result.exitCode == 70, "a division by zero exits 70");
  check(result.output.find("division by zero") != std::string::npos,
        "the runtime error names the division by zero");
}

// Strings over the arena, end to end: lay out two `AotString`s in the image,
// concatenate them with the runtime's `jolt_concat` (which bumps the mmap'd
// arena) and print the result. Exercises the arena mapping in `_start`, the
// allocator, the byte copy and the string print path in one static binary.
static void test_boxed_runtime_string() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  // Data islands: each is an 8-byte length followed by the bytes.
  const std::string a = "hello, ";
  const std::string b = "world!";
  CodeGen::Label aLabel = cg.newLabel();
  CodeGen::Label bLabel = cg.newLabel();

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.callLabel(entry.initArena);  // map the arena the concatenation allocates from
  // rdi:rsi = a, rdx:rcx = b
  cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(X64Reg::Rsi, aLabel);
  cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Str));
  cg.leaRipLabel(X64Reg::Rcx, bLabel);
  cg.callLabel(rt.concat);          // rax:rdx = "hello, world!"
  cg.movReg(X64Reg::Rdi, X64Reg::Rax);
  cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
  cg.callLabel(rt.printValue);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);

  // Bind the string data islands after all code, then finalize.
  auto emitString = [&](CodeGen::Label label, const std::string& text) {
    cg.bind(label);
    const std::uint64_t length = text.size();
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(&length), sizeof length);
    cg.emitBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  };
  emitString(aLabel, a);
  emitString(bLabel, b);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/jolt_boxed_string";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "hello, world!\n",
        "the boxed runtime concatenates and prints strings from the arena");
  check(result.exitCode == 0, "the string program exits 0");
}

// Float `%` end to end: the x87 `fprem` path runs inside a real static ELF.
// The remainder is compared against the expected value and the boolean is
// printed, since float-to-text (`print` of a double) arrives with the dtoa
// slice. A correct remainder prints "true".
static void test_boxed_runtime_float_mod() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  // Emits `print((a % b) == expected)` for three double pairs.
  auto checkMod = [&](double a, double b, double expected) {
    cg.movImm(X64Reg::Rdi, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(aotDouble(a).payload));
    cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rcx, static_cast<std::int64_t>(aotDouble(b).payload));
    cg.movImm(X64Reg::R8, static_cast<std::int64_t>(AotArithOp::Mod));
    cg.callLabel(rt.arithValues);  // rax:rdx = a % b
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.movImm(X64Reg::Rdx, static_cast<std::int64_t>(AotTag::Double));
    cg.movImm(X64Reg::Rcx, static_cast<std::int64_t>(aotDouble(expected).payload));
    cg.movImm(X64Reg::R8, static_cast<std::int64_t>(AotCompareOp::Eq));
    cg.callLabel(rt.compareValues);  // rax:rdx = bool
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.callLabel(rt.printValue);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  checkMod(7.0, 2.5, 2.0);
  checkMod(-7.0, 3.0, -1.0);
  checkMod(7.0, -3.0, 1.0);
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/jolt_boxed_float_mod";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "true\ntrue\ntrue\n",
        "the boxed runtime computes float remainders with fprem");
  check(result.exitCode == 0, "the float mod program exits 0");
}

#endif  // FREESTANDING_EXEC_TESTS

int main() {
  test_arena();
#ifdef FREESTANDING_EXEC_TESTS
  test_platform_roundtrip();
  test_end_to_end();
  test_boxed_runtime_print();
  test_boxed_runtime_error();
  test_boxed_runtime_div_zero();
  test_boxed_runtime_string();
  test_boxed_runtime_float_mod();
#else
  std::printf("skipped: platform and execution tests require Linux x86-64\n");
#endif
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all freestanding tests passed\n");
  return 0;
}
