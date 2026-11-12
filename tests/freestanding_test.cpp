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

#include "bytecode.h"
#include "compiler.h"
#include "lexer.h"
#include "native/arena.h"
#include "native/entry.h"
#include "native/elf.h"
#include "native/lower.h"
#include "native/platform_freestanding.h"
#include "native/runtime.h"
#include "native/value.h"
#include "parser.h"

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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef FREESTANDING_EXEC_TESTS

static void test_platform_roundtrip() {
  PlatformFreestanding platform;
  const char* path = "/tmp/oskel_freestanding_roundtrip.txt";
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
  check(platform.open("/tmp/oskel_does_not_exist_12345", Platform::OpenMode::Read) ==
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

// `main(argc, argv)`: oskel_write(1, messageAddress, messageLength); return N.
static BuiltProgram buildHello(std::int64_t messageAddress, std::int64_t messageLength,
                               std::int64_t exitCode) {
  CodeGen code;
  RuntimeHandles handles = newRuntimeHandles(code);

  CodeGen::Label mainLabel = code.newLabel();
  code.bind(mainLabel);

  // oskel_write(1, message, length)
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
  std::string command = path + " > /tmp/oskel_freestanding_out.txt 2>&1";
  int status = std::system(command.c_str());
  RunResult result;
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::FILE* file = std::fopen("/tmp/oskel_freestanding_out.txt", "rb");
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
  const char* message = "hello from a Oskel AOT binary\n";
  std::vector<std::uint8_t> rodata(message, message + std::strlen(message));
  const std::int64_t messageAddress = 0x600000;

  BuiltProgram built = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 0);
  built.rodata = rodata;

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), built.text, built.rodata, built.entry);
  const std::string path = "/tmp/oskel_freestanding_hello";
  writeFile(path, elf);

  RunResult result = runProgram(path);
  check(result.output == message, "the generated binary prints the expected bytes");
  check(result.exitCode == 0, "the generated binary exits with the program's code");

  // A non-zero return value must become the process exit code.
  BuiltProgram failing = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 7);
  failing.rodata = rodata;
  std::vector<std::uint8_t> elfFail =
      buildStaticElf(Target::linuxX64(), failing.text, failing.rodata, failing.entry);
  writeFile("/tmp/oskel_freestanding_exit7", elfFail);
  RunResult failed = runProgram("/tmp/oskel_freestanding_exit7");
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
                          const std::vector<Value>& constants, int numRegisters,
                          int numLocals = 0) {
  Bytecode proto;
  proto.name = "<freestanding-test>";
  proto.code = code;
  proto.constants = constants;
  proto.numRegisters = numRegisters;
  proto.numLocals = numLocals;
  return proto;
}

// Compiles a Oskel source string to bytecode, the way the dev engine does.
static Bytecode compileSource(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<freestanding-test>");
  return compileProgram(parser.parseProgram());
}

// The `print` builtin, end to end: compile a real program that calls `print`,
// lower it, build a static ELF and run it. Before 7c.2d the backend had no
// builtins, so `print` was a lowering error and a compiled binary could not
// print anything (PHASE 7c.2d).
static void test_builtin_print() {
  Bytecode proto = compileSource(
      "print(\"hello\", 42, 3.5, true);"
      "write(\"no\", \"newline\");"
      "print();");
  RunResult result = buildAndRun("/tmp/oskel_builtin_print", proto);
  check(result.output == "hello 42 3.5 true\nno newline\n",
        "a compiled program prints through the print/write builtins");
  check(result.exitCode == 0, "a program using print exits 0 through _start");
}

// Collections, end to end (PHASE 7c.3): arrays, maps, indexing, `.length`,
// `in`, `for-in` and container printing all lower and run in a compiled binary.
static void test_collections() {
  Bytecode proto = compileSource(
      "let a = [1, 2, 3];\n"
      "a[1] = 20;\n"
      "a[3] = 4;\n"
      "print(a);\n"
      "print(a.length);\n"
      "print(a[0], a[1], a[-1]);\n"
      "let m = {\"x\": 1, \"y\": 2};\n"
      "m[\"z\"] = 3;\n"
      "m.x = 10;\n"
      "print(m);\n"
      "print(m[\"x\"], m.y, m[\"z\"]);\n"
      "print(2 in a, 99 in a, \"x\" in m, \"q\" in m);\n"
      "a[-1] = 9;\n"
      "a[1] = 20;\n"
      "m[\"x\"] = 5;\n"
      "m[\"new\"] = 7;\n"
      "print(a, m);\n"
      "let s = \"hello\";\n"
      "print(s.length, s[1], \"ell\" in s);\n"
      "print(\"\" in s, \"z\" in s, \"hello\" in s, \"helloo\" in s);\n"
      "var total = 0;\n"
      "for v in a { total = total + v; }\n"
      "print(total);\n"
      "var keys = \"\";\n"
      "for k in m { keys = keys + k; }\n"
      "print(keys);\n"
      "var pairKeys = \"\";\n"
      "var pairTotal = 0;\n"
      "for k, v in m { pairKeys = pairKeys + k; pairTotal = pairTotal + v; }\n"
      "print(pairKeys, pairTotal);\n");
  RunResult result = buildAndRun("/tmp/oskel_collections", proto);
  std::string expected =
      "[1, 20, 3, 4]\n"
      "4\n"
      "1 20 4\n"
      "{\"x\": 10, \"y\": 2, \"z\": 3}\n"
      "10 2 3\n"
      "false false true false\n"
      "[1, 20, 3, 9] {\"x\": 5, \"y\": 2, \"z\": 3, \"new\": 7}\n"
      "5 e true\n"
      "true false true false\n"
      "33\n"
      "xyznew\n"
      "xyznew 17\n";
  check(result.output == expected, "a compiled program runs collections and prints them");
  check(result.exitCode == 0, "a program using collections exits 0 through _start");
}

// The hidden helpers for PHASE 6.5 features that lower to a call (PHASE 7c.4a):
// `$nonNull` (`a!!`) and `$isError` (`try` / `x catch y`). Before this slice the
// backend rejected both with `global '$nonNull'/'$isError' is read but never
// assigned`; now they are seeded builtins and a compiled program runs them.
static void test_hidden_helpers() {
  Bytecode proto = compileSource(
      "var a = 7;\n"
      "var b = a!!;\n"
      "print(b);\n"
      "var c = b catch 0;\n"
      "print(c);\n"
      "fn f() { var x = 9; return try x; }\n"
      "print(f());\n");
  RunResult result = buildAndRun("/tmp/oskel_hidden_helpers", proto);
  check(result.output == "7\n7\n9\n",
        "a compiled program runs the non-null and catch helpers");
  check(result.exitCode == 0, "a program using the hidden helpers exits 0");
}

// `$nonNull` on nil must fail with the hosted helper's message and exit 70,
// proving the AOT helper takes the same error path as the VM.
static void test_hidden_helpers_non_null_failure() {
  Bytecode proto = compileSource(
      "var a = nil;\n"
      "var b = a!!;\n"
      "print(b);\n");
  RunResult result = buildAndRun("/tmp/oskel_hidden_helpers_nil", proto);
  check(result.exitCode == 70, "a failed '!!' exits 70");
  check(result.output.find("non-null assertion") != std::string::npos,
        "the failed '!!' names the non-null assertion");
}

// The `$str` helper (PHASE 7c.4b) renders the scalars a compiled program
// interpolates: int, negative int, float, double, bool, nil and string must
// match the hosted `Value::toString` byte for byte.
static void test_str_helper() {
  Bytecode proto = compileSource(
      "var n = 3;\n"
      "print(\"n=${n}\");\n"
      "var f = 1.0 / 3.0;\n"
      "print(\"f=${f}\");\n"
      "var d = 0.1 + 0.2;\n"
      "print(\"d=${d}\");\n"
      "var b = true;\n"
      "print(\"b=${b}\");\n"
      "var s = \"hi\";\n"
      "print(\"s=${s}\");\n"
      "var z = nil;\n"
      "print(\"z=${z}\");\n"
      "var neg = -42;\n"
      "print(\"neg=${neg}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_str_helper", proto);
  std::string expected =
      "n=3\n"
      "f=0.3333333333333333\n"
      "d=0.30000000000000004\n"
      "b=true\n"
      "s=hi\n"
      "z=nil\n"
      "neg=-42\n";
  check(result.output == expected,
        "a compiled program interpolates scalars like the VM");
  check(result.exitCode == 0, "a program using interpolation exits 0");
}

// The `$range` helper (PHASE 7c.4c) builds a range value from two integer
// bounds. A compiled program must print, iterate (ascending and descending,
// inclusive and exclusive), take `.length`, test membership with `in` and
// interpolate a range exactly like the VM.
static void test_range_helper() {
  Bytecode proto = compileSource(
      "print(0..5);\n"
      "print(0..=5);\n"
      "print(5..0);\n"
      "print(5..=0);\n"
      "print((0..=5).length);\n"
      "var r = 0..3;\n"
      "print(r.length);\n"
      "for i in 0..3 { print(i); }\n"
      "for i in 3..0 { print(i); }\n"
      "for i in 0..=2 { print(i); }\n"
      "print(2 in 0..5);\n"
      "print(5 in 0..5);\n"
      "print(5 in 0..=5);\n"
      "print(1 in 5..0);\n"
      "print(5 in 5..0);\n"
      "print(\"${0..=3}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_range_helper", proto);
  std::string expected =
      "0..5\n"
      "0..=5\n"
      "5..0\n"
      "5..=0\n"
      "6\n"
      "3\n"
      "0\n"
      "1\n"
      "2\n"
      "3\n"
      "2\n"
      "1\n"
      "0\n"
      "1\n"
      "2\n"
      "true\n"
      "false\n"
      "true\n"
      "true\n"
      "false\n"
      "0..=3\n";
  check(result.output == expected,
        "a compiled program handles ranges like the VM");
  check(result.exitCode == 0, "a program using ranges exits 0");
}

// A range with a non-integer bound must fail with the hosted helper's message
// and exit 70.
static void test_range_helper_bad_bounds() {
  Bytecode proto = compileSource(
      "var s = \"x\";\n"
      "var r = s..3;\n"
      "print(r);\n");
  RunResult result = buildAndRun("/tmp/oskel_range_bad", proto);
  check(result.exitCode == 70, "a non-integer range bound exits 70");
  check(result.output.find("range bounds must be integers") != std::string::npos,
        "the failed range names the integer-bounds rule");
}

// A nested container prints with the recursive repr, and a self-referential one
// stops at the depth cap instead of recursing forever.
static void test_collection_repr() {
  Bytecode proto = compileSource(
      "print([[1, 2], {\"a\": [3]}]);\n"
      "print([\"a\", \"b\\\"c\", \"d\\ne\", \"tab\\tx\", \"\\x01\"]);\n"
      "let m = {};\n"
      "m[\"self\"] = m;\n"
      "print(m);\n");
  RunResult result = buildAndRun("/tmp/oskel_collection_repr", proto);
  check(result.output.find("[[1, 2], {\"a\": [3]}]\n") == 0,
        "a nested array/map prints with the recursive repr");
  check(result.output.find("[\"a\", \"b\\\"c\", \"d\\ne\", \"tab\\tx\", \"\\x01\"]\n") !=
            std::string::npos,
        "nested strings print with the interpreter's escapes");
  check(result.output.find("...") != std::string::npos,
        "a self-referential map stops at the depth cap");
  check(result.exitCode == 0, "a self-referential program still exits 0");
}

// `$str` renders a collection or function with the recursive repr (PHASE
// 7c.4b.2), so `"${[1, 2]}"` reads exactly like `print([1, 2])`: nested strings
// quoted, maps as `{k: v}`, functions as `<fn>`, and a self-referential
// structure stops at the depth cap instead of recursing forever.
static void test_str_helper_collections() {
  Bytecode proto = compileSource(
      "print(\"${[1, 2, 3]}\");\n"
      "print(\"${[\"a\", \"b\"]}\");\n"
      "print(\"${[[1, 2], {\"k\": 3}]}\");\n"
      "var m = {\"x\": 1, \"y\": [1, 2]};\n"
      "print(\"${m}\");\n"
      "fn f() {}\n"
      "print(\"${f}\");\n"
      "print(\"${0..=3}\");\n"
      "let self = {};\n"
      "self[\"me\"] = self;\n"
      "print(\"${self}\");\n"
      "print(\"sum=${[1, 2, 3].length}\");\n");
  RunResult result = buildAndRun("/tmp/oskel_str_collections", proto);
  check(result.output.find(
            "[1, 2, 3]\n"
            "[\"a\", \"b\"]\n"
            "[[1, 2], {\"k\": 3}]\n"
            "{\"x\": 1, \"y\": [1, 2]}\n"
            "<fn>\n"
            "0..=3\n") == 0,
        "a compiled program interpolates collections and functions like print");
  check(result.output.find("{\"me\":") != std::string::npos &&
            result.output.find("...") != std::string::npos,
        "interpolating a self-referential map stops at the depth cap");
  check(result.output.find("sum=3\n") != std::string::npos,
        "a collection expression interpolates with the recursive repr");
  check(result.exitCode == 0, "a program interpolating a collection exits 0");
}

// The `$slice` helper (PHASE 7c.4d) slices an array by element and a string by
// UTF-8 code point, following the hosted rule: an omitted bound is a step-aware
// default, a negative bound counts from the end, and both clamp so a reverse
// slice never wraps. A compiled program must agree with the VM.
static void test_slice_helper() {
  Bytecode proto = compileSource(
      "let a = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];\n"
      "print(a[1:4]);\n"
      "print(a[:3]);\n"
      "print(a[7:]);\n"
      "print(a[:]);\n"
      "print(a[::2]);\n"
      "print(a[::-1]);\n"
      "print(a[::-2]);\n"
      "print(a[8:2:-1]);\n"
      "print(a[-3:]);\n"
      "print(a[:-3]);\n"
      "print(a[-100:100]);\n"
      "print(a[100:]);\n"
      "print(a[5:5]);\n"
      "print(a[1:8:3]);\n"
      "let s = \"abcdef\";\n"
      "print(s[1:4]);\n"
      "print(s[::-1]);\n"
      "print(s[1:4:2]);\n"
      "print(s[-2:]);\n"
      "print(s[:-2]);\n"
      "print(s[10:]);\n"
      "let u = \"caf\\u00e9\\u00e8\";\n"
      "print(u.length);\n"
      "print(u[1:3]);\n"
      "print(u[::-1]);\n"
      "print(u[::2]);\n");
  RunResult result = buildAndRun("/tmp/oskel_slice_helper", proto);
  std::string expected =
      "[1, 2, 3]\n"
      "[0, 1, 2]\n"
      "[7, 8, 9]\n"
      "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n"
      "[0, 2, 4, 6, 8]\n"
      "[9, 8, 7, 6, 5, 4, 3, 2, 1, 0]\n"
      "[9, 7, 5, 3, 1]\n"
      "[8, 7, 6, 5, 4, 3]\n"
      "[7, 8, 9]\n"
      "[0, 1, 2, 3, 4, 5, 6]\n"
      "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n"
      "[]\n"
      "[]\n"
      "[1, 4, 7]\n"
      "bcd\n"
      "fedcba\n"
      "bd\n"
      "ef\n"
      "abcd\n"
      "\n"
      "5\n"
      "af\n"
      "\u00e8\u00e9fac\n"
      "cf\u00e8\n";
  check(result.output == expected,
        "a compiled program slices arrays and strings like the VM");
  check(result.exitCode == 0, "a program using slices exits 0");
}

// A bad slice bound, a zero step and a non-sliceable operand must fail with the
// hosted helper's message and exit 70.
static void test_slice_helper_errors() {
  Bytecode badStart = compileSource(
      "let a = [1, 2, 3];\n"
      "print(a[1.5:]);\n");
  RunResult start = buildAndRun("/tmp/oskel_slice_bad_start", badStart);
  check(start.exitCode == 70, "a non-integer slice start exits 70");
  check(start.output.find("slice start must be an integer") != std::string::npos,
        "the failed slice names the integer-start rule");

  Bytecode zeroStep = compileSource(
      "let a = [1, 2, 3];\n"
      "print(a[::0]);\n");
  RunResult step = buildAndRun("/tmp/oskel_slice_zero_step", zeroStep);
  check(step.exitCode == 70, "a zero slice step exits 70");
  check(step.output.find("slice step must not be zero") != std::string::npos,
        "the failed slice names the non-zero-step rule");

  Bytecode notSliceable = compileSource(
      "print(5[1:2]);\n");
  RunResult value = buildAndRun("/tmp/oskel_slice_not_sliceable", notSliceable);
  check(value.exitCode == 70, "slicing a non-collection exits 70");
  check(value.output.find("not sliceable") != std::string::npos,
        "the failed slice names the non-sliceable operand");
}

// The public standard-library builtins (PHASE 7c.5a) through the real process
// entry: `len`, `type`, `str`, `repr`, `bool`, `ord`, `chr`, `assert`,
// `panic` and `isError`. The program prints one line per result and the output
// is asserted byte for byte against the hosted engine's semantics.
static void test_public_builtins() {
  Bytecode proto = compileSource(
      "print(len(\"h\\u00e9llo\"));\n"      // 5 code points, not 6 bytes
      "print(len([1, 2, 3]));\n"
      "print(len({\"a\": 1, \"b\": 2}));\n"
      "print(len(0..=5));\n"
      "print(type(nil));\n"
      "print(type(true));\n"
      "print(type(1));\n"
      "print(type(1.5));\n"
      "print(type(\"x\"));\n"
      "print(type([1]));\n"
      "print(type({\"k\": 1}));\n"
      "print(type(0..1));\n"
      "print(str(42), str(3.5), str(true), str(nil));\n"
      "print(repr(\"hi\"), repr([1, \"two\"]), repr(7));\n"
      "print(bool(0), bool(1), bool(nil), bool(\"\"), bool([]));\n"
      "print(ord(\"A\"), ord(\"\\u00e9\"), ord(\"\U0001F600\"));\n"
      "print(chr(65), chr(233), chr(128512));\n"
      "assert(true);\n"
      "assert(1 == 1, \"should pass\");\n"
      "print(\"after asserts\");\n"
      "print(isError(5), isError(\"x\"));\n");
  RunResult result = buildAndRun("/tmp/oskel_public_builtins", proto);
  std::string expected =
      "5\n"
      "3\n"
      "2\n"
      "6\n"
      "nil\n"
      "bool\n"
      "int\n"
      "double\n"
      "string\n"
      "array\n"
      "map\n"
      "range\n"
      "42 3.5 true nil\n"
      "\"hi\" [1, \"two\"] 7\n"
      "true true false true true\n"
      "65 233 128512\n"
      "A \u00e9 \U0001F600\n"
      "after asserts\n"
      "false false\n";
  check(result.output == expected,
        "the public builtins compute like the hosted engine");
  check(result.exitCode == 0, "a program using the public builtins exits 0");
}

// The failure paths of the public builtins must report a message and exit 70,
// matching the hosted engine's runtime-error code.
static void test_public_builtin_errors() {
  Bytecode badLen = compileSource("print(len(42));\n");
  RunResult len = buildAndRun("/tmp/oskel_builtin_bad_len", badLen);
  check(len.exitCode == 70, "len of a non-collection exits 70");
  check(len.output.find("len() is not defined") != std::string::npos,
        "the failed len names the unsupported type");

  Bytecode badOrd = compileSource("print(ord(5));\n");
  RunResult ord = buildAndRun("/tmp/oskel_builtin_bad_ord", badOrd);
  check(ord.exitCode == 70, "ord of a non-string exits 70");
  check(ord.output.find("ord() expects a string") != std::string::npos,
        "the failed ord names the string argument");

  Bytecode longOrd = compileSource("print(ord(\"ab\"));\n");
  RunResult multi = buildAndRun("/tmp/oskel_builtin_long_ord", longOrd);
  check(multi.exitCode == 70, "ord of a multi-character string exits 70");
  check(multi.output.find("ord() expects a single character") != std::string::npos,
        "the failed ord names the single-character rule");

  Bytecode badChr = compileSource("print(chr(2000000));\n");
  RunResult chr = buildAndRun("/tmp/oskel_builtin_bad_chr", badChr);
  check(chr.exitCode == 70, "chr of an out-of-range code point exits 70");
  check(chr.output.find("chr() code point out of range") != std::string::npos,
        "the failed chr names the code-point range");

  Bytecode failedAssert = compileSource("assert(false);\n");
  RunResult assertion = buildAndRun("/tmp/oskel_builtin_assert", failedAssert);
  check(assertion.exitCode == 70, "a failed assert exits 70");
  check(assertion.output.find("oskel: assertion failed") != std::string::npos,
        "a failed assert reports the default message");

  Bytecode customAssert = compileSource("assert(false, \"custom message\");\n");
  RunResult custom = buildAndRun("/tmp/oskel_builtin_assert_msg", customAssert);
  check(custom.exitCode == 70, "a failed assert with a message exits 70");
  check(custom.output.find("oskel: custom message") != std::string::npos,
        "a failed assert reports the caller's message");

  Bytecode panicked = compileSource("panic(\"boom\");\n");
  RunResult panic = buildAndRun("/tmp/oskel_builtin_panic", panicked);
  check(panic.exitCode == 70, "panic exits 70");
  check(panic.output.find("oskel: boom") != std::string::npos,
        "panic reports the caller's message");

  Bytecode barePanic = compileSource("panic();\n");
  RunResult bare = buildAndRun("/tmp/oskel_builtin_panic_bare", barePanic);
  check(bare.exitCode == 70, "a bare panic exits 70");
  check(bare.output.find("oskel: panic") != std::string::npos,
        "a bare panic reports the default message");
}

// Drives the boxed printer directly: the runtime's `oskel_print_value` is what
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
  const std::string path = "/tmp/oskel_boxed_print";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "42\n-7\ntrue\nfalse\nnil\n"
                         "0.30000000000000004\n0.3333333333333333\n-2.5\n"
                         "0.33333334\n0.1\n",
        "the boxed runtime prints int, bool, nil, double and float");
  check(result.exitCode == 0, "the printing program exits 0");
}

// The global table (PHASE 7c.2a) inside a real static ELF: the chunk allocates
// it from the arena at entry, every slot starts nil, and set/get round-trip
// through r15. The helpers are driven directly, like the printer above.
static void test_boxed_runtime_globals() {
  CodeGen cg(Target::linuxX64());
  RuntimeHandles entry = newRuntimeHandles(cg);
  AotRuntimeHandles rt = newAotRuntimeHandles(cg);
  CodeGen::Label mainLabel = cg.newLabel();

  auto printSlot = [&](int slot) {
    cg.movImm(X64Reg::Rdi, slot);
    cg.callLabel(rt.globalGet);
    cg.movReg(X64Reg::Rdi, X64Reg::Rax);
    cg.movReg(X64Reg::Rsi, X64Reg::Rdx);
    cg.callLabel(rt.printValue);
  };
  auto setInt = [&](int slot, std::int64_t value) {
    cg.movImm(X64Reg::Rdi, slot);
    cg.movImm(X64Reg::Rsi, static_cast<std::int64_t>(AotTag::Int));
    cg.movImm(X64Reg::Rdx, value);
    cg.callLabel(rt.globalSet);
  };

  cg.bind(mainLabel);
  cg.push(X64Reg::Rbp);
  cg.movReg(X64Reg::Rbp, X64Reg::Rsp);
  cg.callLabel(entry.initArena);
  cg.movImm(X64Reg::Rdi, 3);
  cg.callLabel(rt.initGlobals);
  setInt(0, 7);
  setInt(2, -9);
  printSlot(0);  // 7
  printSlot(2);  // -9
  printSlot(1);  // nil: never assigned
  cg.xorReg(X64Reg::Rax, X64Reg::Rax);
  cg.movReg(X64Reg::Rsp, X64Reg::Rbp);
  cg.pop(X64Reg::Rbp);
  cg.retInsn();

  std::uint64_t start = emitFreestandingRuntime(cg, entry, mainLabel);
  emitAotRuntime(cg, rt, entry);
  cg.finalize();

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), cg.code(), {}, start);
  const std::string path = "/tmp/oskel_boxed_globals";
  writeFile(path, elf);
  RunResult result = runProgram(path);
  check(result.output == "7\n-9\nnil\n",
        "the boxed runtime keeps globals in distinct slots and starts them nil");
  check(result.exitCode == 0, "the global program exits 0");
}

// A type mismatch in a lowered operation must report an error and exit 70,
// matching the development engine's runtime-error exit code.
static void test_boxed_runtime_error() {
  // R0 = true; R1 = false; R0 = R0 - R1 -> subtraction is int-only, so this
  // must hit the runtime's "unsupported operand types" path and exit 70.
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::SUB, 0, 0, 1), instr(OpCode::HALT)},
                             {Value(true), Value(false)}, 2);
  RunResult result = buildAndRun("/tmp/oskel_boxed_error", proto);
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
  RunResult result = buildAndRun("/tmp/oskel_boxed_divzero", proto);
  check(result.exitCode == 70, "a division by zero exits 70");
  check(result.output.find("division by zero") != std::string::npos,
        "the runtime error names the division by zero");
}

// Calls (PHASE 7c.2c.1) through the real process entry: a hand-built chunk with
// a child proto, a CLOSURE, a CALL and a RETURN must build, run under `_start`
// and return the callee's value in R0. The exact value is checked by printing
// it, so this also exercises the runtime dispatch path end to end.
static void test_boxed_runtime_call() {
  // Root: R0 = closure(add); R1 = 20; R2 = 22; R0 = R0(R1, R2); print R0; HALT.
  Bytecode root = makeProto({instr(OpCode::CLOSURE, 0, 0), instr(OpCode::LOADK, 1, 0),
                             instr(OpCode::LOADK, 2, 1), instr(OpCode::CALL, 0, 2),
                             instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(20)), Value(static_cast<int64_t>(22))}, 3);
  root.name = "<call-root>";
  auto child = std::make_shared<Bytecode>();
  child->name = "add";
  child->numParams = 2;
  child->numRegisters = 2;
  child->numLocals = 2;
  child->code = {instr(OpCode::DECLLOCAL, 0, 0), instr(OpCode::DECLLOCAL, 1, 1),
                 instr(OpCode::GETLOCAL, 0, 0), instr(OpCode::GETLOCAL, 1, 1),
                 instr(OpCode::ADD, 0, 0, 1), instr(OpCode::RETURN, 0)};
  root.children.push_back(child);

  // Run the chunk directly and read the AotValue it leaves in R0. Printing the
  // result needs the `print` builtin, which lands with the library (7c.2c.2), so
  // this test reads R0 through the program entry like `lower_test` does.
  LoweredChunk chunk = lowerToX64(Target::linuxX64(), root, CodeGen{});
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < chunk.code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    check(false, "mmap for the call test");
    return;
  }
  std::memcpy(mem, chunk.code.data(), chunk.code.size());
  mprotect(mem, span, PROT_READ | PROT_EXEC);
  auto fn = reinterpret_cast<AotValue (*)()>(
      static_cast<std::uint8_t*>(mem) + chunk.programEntry);
  AotValue value = fn();
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 42,
        "a lowered CALL returns the callee's result (20 + 22 == 42)");

  // The same chunk through `_start` must exit 0 (R0 is discarded by `main`).
  RunResult result = buildAndRun("/tmp/oskel_boxed_call", root);
  check(result.exitCode == 0, "a chunk with a call exits 0 through _start");
}

// Calling a non-function value is a fatal runtime error: build a chunk that
// calls an integer and check the error message and exit code 70.
static void test_boxed_runtime_call_non_function() {
  // R0 = 5; R1 = 7; R0 = R0(R1) -> R0 is not a function.
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::LOADK, 1, 1),
                              instr(OpCode::CALL, 0, 1), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7))}, 2);
  RunResult result = buildAndRun("/tmp/oskel_boxed_call_nonfunc", proto);
  check(result.exitCode == 70, "calling a non-function exits 70");
  check(result.output.find("non-function") != std::string::npos,
        "the runtime error names the non-function call");
}

// Captured locals through the real process entry (PHASE 7c.2c.2): a child
// proto that captures a local of its parent. The closure escapes its defining
// frame and is called afterwards, so the captured cell must outlive the frame.
// The root reads the result back into R0 (checked by direct execution) and the
// same chunk runs cleanly under `_start`.
static void test_boxed_runtime_closure_capture() {
  // Root: R0 = closure(inner)  -- `inner` captures the root's local slot 0.
  //       R1 = 40; R0 = R0(); R0 = R0 + R1; HALT.  -> 40 + 40 = 80 in R0.
  Bytecode root = makeProto({instr(OpCode::LOADK, 1, 0), instr(OpCode::DECLLOCAL, 1, 0),
                             instr(OpCode::CLOSURE, 0, 0), instr(OpCode::CALL, 0, 0),
                             instr(OpCode::ADD, 0, 0, 1), instr(OpCode::HALT)},
                            {Value(static_cast<int64_t>(40))}, 3, 1);
  root.name = "<capture-root>";
  auto inner = std::make_shared<Bytecode>();
  inner->name = "inner";
  inner->numParams = 0;
  inner->numRegisters = 1;
  inner->numLocals = 0;
  inner->upvalues.push_back(UpvalDesc{true, 0});  // capture the parent's slot 0
  inner->code = {instr(OpCode::GETUPVAL, 0, 0), instr(OpCode::RETURN, 0)};
  root.children.push_back(inner);

  LoweredChunk chunk = lowerToX64(Target::linuxX64(), root, CodeGen{});
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  while (span < chunk.code.size()) span += static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    check(false, "mmap for the closure capture test");
    return;
  }
  std::memcpy(mem, chunk.code.data(), chunk.code.size());
  mprotect(mem, span, PROT_READ | PROT_EXEC);
  auto fn = reinterpret_cast<AotValue (*)()>(
      static_cast<std::uint8_t*>(mem) + chunk.programEntry);
  AotValue value = fn();
  check(value.tag == static_cast<std::uint64_t>(AotTag::Int) &&
            static_cast<std::int64_t>(value.payload) == 80,
        "a lowered closure reads a captured local (40 + 40 == 80)");

  RunResult result = buildAndRun("/tmp/oskel_boxed_capture", root);
  check(result.exitCode == 0, "a chunk with a captured local exits 0 through _start");
}

// Locals (PHASE 7c.2b) through the real process entry: a chunk with a local
// region must build, allocate its larger frame and run cleanly under `_start`.
// The value itself is covered by direct execution in `lower_test.cpp`; here the
// subject is the frame-size math on the freestanding path, so a clean exit and
// no output is the assertion.
static void test_boxed_runtime_locals() {
  Bytecode proto = makeProto({instr(OpCode::LOADK, 0, 0), instr(OpCode::DECLLOCAL, 0, 0),
                              instr(OpCode::LOADK, 1, 1), instr(OpCode::SETLOCAL, 1, 0),
                              instr(OpCode::GETLOCAL, 0, 0), instr(OpCode::HALT)},
                             {Value(static_cast<int64_t>(5)), Value(static_cast<int64_t>(7))},
                             2, 1);
  RunResult result = buildAndRun("/tmp/oskel_boxed_locals", proto);
  check(result.output.empty(), "a locals-only chunk prints nothing");
  check(result.exitCode == 0, "a chunk with a local region exits 0 through _start");
}

// Strings over the arena, end to end: lay out two `AotString`s in the image,
// concatenate them with the runtime's `oskel_concat` (which bumps the mmap'd
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
  const std::string path = "/tmp/oskel_boxed_string";
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
  const std::string path = "/tmp/oskel_boxed_float_mod";
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
  test_boxed_runtime_globals();
  test_boxed_runtime_error();
  test_boxed_runtime_div_zero();
  test_boxed_runtime_locals();
  test_boxed_runtime_call();
  test_boxed_runtime_call_non_function();
  test_boxed_runtime_closure_capture();
  test_boxed_runtime_string();
  test_boxed_runtime_float_mod();
  test_builtin_print();
  test_collections();
  test_hidden_helpers();
  test_hidden_helpers_non_null_failure();
  test_str_helper();
  test_range_helper();
  test_range_helper_bad_bounds();
  test_collection_repr();
  test_str_helper_collections();
  test_slice_helper();
  test_slice_helper_errors();
  test_public_builtins();
  test_public_builtin_errors();
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
