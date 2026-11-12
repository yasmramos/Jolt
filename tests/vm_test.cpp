// Tests for the register-based virtual machine (PHASE 3-6).
// Compiles Jolt snippets and runs the resulting bytecode.

// Release builds define NDEBUG, which would compile every assert() out and
// leave the suite green without testing anything.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "builtins.h"
#include "compiler.h"
#include "lexer.h"
#include "ops.h"
#include "parser.h"
#include "vm.h"

namespace {

Bytecode compileSource(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<vm-test>");
  auto program = parser.parseProgram();
  // Compile-time errors (e.g. reassigning a `let` binding) are reported as
  // failures, not crashes: abort with a clear message if one occurs here.
  try {
    return compileProgram(program);
  } catch (const CompileError& error) {
    std::fprintf(stderr, "unexpected compile error: %s\n", error.message.c_str());
    std::exit(1);
  }
}

// Compiles `source` expecting a `CompileError`, returning its message. The
// caller asserts on the text; a successful compile aborts the test.
std::string compileErrorMessage(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<vm-test>");
  auto program = parser.parseProgram();
  try {
    compileProgram(program);
  } catch (const CompileError& error) {
    return error.message;
  }
  std::fprintf(stderr, "expected a compile error but the source compiled\n");
  std::exit(1);
}

// Redirects std::cout for as long as the object lives.
struct CoutCapture {
  std::ostringstream stream;
  std::streambuf* previous;
  CoutCapture() : previous(std::cout.rdbuf(stream.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(previous); }
};

// Host double used to prove that the engine writes through `Platform` instead
// of touching std::cout/std::cerr directly.
struct StringPlatform final : Platform {
  std::string out;
  std::string err;

  void write(int fd, const char* data, std::size_t length) override {
    if (fd == kStdout) out.append(data, length);
  }
  void writeErr(const char* data, std::size_t length) override { err.append(data, length); }
  void flush(int) override {}
  std::size_t read(int, char*, std::size_t) override { return 0; }
  int open(const char*, OpenMode) override { return kInvalidFd; }
  void close(int) override {}
  bool seek(int, std::int64_t, SeekOrigin) override { return false; }
  [[noreturn]] void exit(int) override { std::abort(); }
  void* alloc(std::size_t, std::size_t) override { return nullptr; }
  void dealloc(void*) override {}
  void* lookupSymbol(const char*) override { return nullptr; }
};

std::string runAndCapture(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  registerBuiltins(vm);
  CoutCapture capture;
  vm.run(bytecode);
  return capture.stream.str();
}

// Runs a program through the VM and aborts with the error message if the
// runtime raises. Keeps the test harness from terminating on an uncaught
// RuntimeError (Phase 6b closure tests rely on this).
// Name of the test currently executing; printed by runOrFail so a runtime
// failure identifies the failing scenario instead of aborting anonymously.
const char* g_currentTest = "<unknown>";

// RAII helper that records the active test name for diagnostics below.
struct TestScope {
  explicit TestScope(const char* name) { g_currentTest = name; }
};

void runOrFail(VM& vm, const Bytecode& bytecode) {
  try {
    vm.run(bytecode);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "runtime failure in %s: %s\n", g_currentTest, error.what());
    std::exit(1);
  }
}

// Records the active test name for the diagnostics above.
#define TEST_SCOPE() TestScope _testScope(__func__)

const Value& requireGlobal(const VM& vm, const std::string& name) {
  const Value* value = vm.findGlobal(name);
  if (!value) {
    std::fprintf(stderr, "missing global: %s\n", name.c_str());
    std::exit(1);
  }
  return *value;
}

// Runs and returns the RuntimeError message, or "" if there was no error.
std::string runtimeError(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  // The VM raises compile-time style errors for some runtime failures, so
  // both exception flavors are treated as "the program failed to run".
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    return error.message;
  } catch (const CompileError& error) {
    return error.message;
  }
  return "";
}

void testIntegerArithmetic() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let x = 2 + 3 * 4;\nlet y = 10 / 4;\n");
  VM vm;
  vm.run(bytecode);

  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 14);

  // Integer division is integral, same as in the interpreter.
  const Value& y = requireGlobal(vm, "y");
  assert(y.isInt() && y.asInt() == 2);
}

void testFloatArithmetic() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let x = 10.0 / 4;\nlet y = 1.5 + 2;\n");
  VM vm;
  vm.run(bytecode);

  const Value& x = requireGlobal(vm, "x");
  assert(x.isDouble());
  assert(x.asDouble() == 2.5);

  const Value& y = requireGlobal(vm, "y");
  assert(y.isDouble());
  assert(y.asDouble() == 3.5);
}

void testFloatDoublePromotion() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let a = 1.5f + 1.5f;\n"
      "let b = 1.5f + 1.5;\n"
      "let c = 1 + 1.5f;\n"
      "let d = 1f;\n");
  VM vm;
  vm.run(bytecode);

  // float + float -> float (binary32).
  const Value& a = requireGlobal(vm, "a");
  assert(a.isFloat());
  assert(a.asFloat() == 3.0);

  // float + double -> double.
  const Value& b = requireGlobal(vm, "b");
  assert(b.isDouble());
  assert(b.asDouble() == 3.0);

  // int + float -> float.
  const Value& c = requireGlobal(vm, "c");
  assert(c.isFloat());
  assert(c.asFloat() == 2.5);

  // An integer-looking literal with an `f` suffix is a float.
  const Value& d = requireGlobal(vm, "d");
  assert(d.isFloat());
  assert(d.asFloat() == 1.0);
}

void testSubtractionAndMultiplication() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let a = 9 - 4;\nlet b = 6 * 7;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 5);
  assert(requireGlobal(vm, "b").asInt() == 42);
}

void testModulo() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let a = 5 % 2;\nlet b = 5.5 % 2;\nlet c = -5 % 2;\n");
  VM vm;
  vm.run(bytecode);

  const Value& a = requireGlobal(vm, "a");
  assert(a.isInt() && a.asInt() == 1);

  const Value& b = requireGlobal(vm, "b");
  assert(b.isDouble() && b.asDouble() == 1.5);

  const Value& c = requireGlobal(vm, "c");
  assert(c.isInt() && c.asInt() == -1);
}

void testPower() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let a = 2 ** 8;\n"
      "let b = 2.0 ** 3;\n"
      "let e = -1;\n"
      "let c = 2 ** e;\n");
  VM vm;
  vm.run(bytecode);

  const Value& a = requireGlobal(vm, "a");
  assert(a.isInt() && a.asInt() == 256);

  const Value& b = requireGlobal(vm, "b");
  assert(b.isDouble() && b.asDouble() == 8.0);

  // Negative exponent: falls into the double path, same as the interpreter.
  const Value& c = requireGlobal(vm, "c");
  assert(c.isDouble() && c.asDouble() == 0.5);
}

void testStringConcatenation() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let s = \"hola\" + \"!\";\n");
  VM vm;
  vm.run(bytecode);

  const Value& s = requireGlobal(vm, "s");
  assert(s.isStr() && s.asStr() == "hola!");
}

void testGlobalAssignmentAndRead() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("var x = 7;\nx = x - 2;\n");
  VM vm;
  vm.run(bytecode);

  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 5);
}

void testCompoundAssignment() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var x = 1;\n"
      "x += 2;\n"
      "x *= 3;\n"
      "x -= 1;\n"
      "x /= 2;\n"
      "var y = 10;\n"
      "y %= 3;\n"
      "var z = 2;\n"
      "z **= 10;\n");
  VM vm;
  vm.run(bytecode);

  // ((1 + 2) * 3 - 1) / 2 = 4
  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 4);

  const Value& y = requireGlobal(vm, "y");
  assert(y.isInt() && y.asInt() == 1);

  const Value& z = requireGlobal(vm, "z");
  assert(z.isInt() && z.asInt() == 1024);
}

void testNilAndBoolLiterals() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let a = nil;\nlet b = true;\nlet c = false;\nvar d;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").isNil());
  assert(requireGlobal(vm, "b").isBool() && requireGlobal(vm, "b").asBool());
  assert(requireGlobal(vm, "c").isBool() && !requireGlobal(vm, "c").asBool());
  assert(requireGlobal(vm, "d").isNil());
}

void testUnaryOperators() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let a = -5;\n"
      "let b = +5;\n"
      "let c = !true;\n"
      "let d = !nil;\n"
      "let e = !0;\n"
      "let f = -2.5;\n");
  VM vm;
  vm.run(bytecode);

  const Value& a = requireGlobal(vm, "a");
  assert(a.isInt() && a.asInt() == -5);

  const Value& b = requireGlobal(vm, "b");
  assert(b.isInt() && b.asInt() == 5);

  assert(requireGlobal(vm, "c").isBool() && !requireGlobal(vm, "c").asBool());
  assert(requireGlobal(vm, "d").isBool() && requireGlobal(vm, "d").asBool());
  // `0` is truthy in Jolt (only nil and false are falsy).
  assert(requireGlobal(vm, "e").isBool() && !requireGlobal(vm, "e").asBool());

  const Value& f = requireGlobal(vm, "f");
  assert(f.isDouble() && f.asDouble() == -2.5);
}

void testComparisons() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let eq = 1 == 1.0;\n"
      "let neq = 1 != 2;\n"
      "let lt = 1 < 2;\n"
      "let lte = 1 <= 1;\n"
      "let gt = 2 > 1;\n"
      "let gte = 2 >= 3;\n"
      "let mixed = 1 < 1.5;\n"
      "let str = \"a\" < \"b\";\n"
      "let bool = false < true;\n"
      "let nilEq = nil == nil;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "eq").asBool());
  assert(requireGlobal(vm, "neq").asBool());
  assert(requireGlobal(vm, "lt").asBool());
  assert(requireGlobal(vm, "lte").asBool());
  assert(requireGlobal(vm, "gt").asBool());
  assert(!requireGlobal(vm, "gte").asBool());
  assert(requireGlobal(vm, "mixed").asBool());
  assert(requireGlobal(vm, "str").asBool());
  assert(requireGlobal(vm, "bool").asBool());
  assert(requireGlobal(vm, "nilEq").asBool());
}

void testPrint() {
  TEST_SCOPE();
  std::string output = runAndCapture("print(1 + 2);\nprint(\"hola\");\nprint(true);\n");
  assert(output == "3\nhola\ntrue\n");
}

void testPrintUsesInjectedPlatform() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("print(1 + 2);\n");
  StringPlatform platform;
  VM vm(platform);
  registerBuiltins(vm);
  vm.run(bytecode);

  assert(platform.out == "3\n");
  assert(platform.err.empty());
}

void testBuiltinIoUsesInjectedPlatform() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("print(1, 2);\nwrite(\"a\");\neprint(\"e\");\n");
  StringPlatform platform;
  VM vm(platform);
  registerBuiltins(vm);
  vm.run(bytecode);

  assert(platform.out == "1 2\na");
  assert(platform.err == "e\n");
}

void testDivisionByZero() {
  TEST_SCOPE();
  assert(runtimeError("let x = 1 / 0;\n") == "division by zero");
}

void testModuloByZero() {
  TEST_SCOPE();
  assert(runtimeError("let x = 1 % 0;\n") == "division by zero");
}

void testUndefinedGlobal() {
  TEST_SCOPE();
  assert(runtimeError("let y = missing + 1;\n").find("undefined variable") !=
         std::string::npos);
}

// Runs `source` against a VM whose `std` global is the map
// `{math: {sqrt: 0, hidden: 0}}`, mimicking `import std.math;` (which binds the
// namespace `std.math`, not a bare `math`). Two more tables (`aaa`, `zzz`) share
// a `dup` member to exercise the deterministic tie-break. Returns the
// RuntimeError message.
std::string runtimeErrorWithNamespace(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  MapPtr math = makeMap();
  math->set("sqrt", Value(int64_t{0}));
  math->set("hidden", Value(int64_t{0}));
  MapPtr std = makeMap();
  std->set("math", Value(math));
  vm.defineGlobal("std", Value(std), false);
  MapPtr aaa = makeMap();
  aaa->set("dup", Value(int64_t{0}));
  vm.defineGlobal("aaa", Value(aaa), false);
  MapPtr zzz = makeMap();
  zzz->set("dup", Value(int64_t{0}));
  vm.defineGlobal("zzz", Value(zzz), false);
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    return error.message;
  } catch (const CompileError& error) {
    return error.message;
  }
  return "";
}

void testUndefinedGlobalSuggestsNamespace() {
  TEST_SCOPE();
  // A forgotten namespace root is the common mistake: the hint names the full
  // path so the fix is obvious.
  assert(runtimeErrorWithNamespace("let x = math.sqrt(4);\n") ==
         "undefined variable 'math' (did you mean 'std.math'?)");
  // The hint walks nested maps too, reporting the dotted path. `hidden` exists
  // only inside `std.math`, so it is not shadowed by a builtin of the same name.
  assert(runtimeErrorWithNamespace("let x = hidden;\n") ==
         "undefined variable 'hidden' (did you mean 'std.math.hidden'?)");
  // A name that is nowhere in the tables gets no misleading hint.
  assert(runtimeErrorWithNamespace("let x = nope;\n") == "undefined variable 'nope'");
  // Two tables holding the same member must not make the message depend on the
  // unordered_map's iteration order: the alphabetically first root wins.
  assert(runtimeErrorWithNamespace("let x = dup;\n") ==
         "undefined variable 'dup' (did you mean 'aaa.dup'?)");
}

void testInvalidComparison() {
  TEST_SCOPE();
  assert(runtimeError("let x = 1 < \"a\";\n").find("cannot compare") != std::string::npos);
}

void testGlobalsPersistBetweenRuns() {
  TEST_SCOPE();
  VM vm;
  vm.run(compileSource("var x = 5;\n"));
  vm.run(compileSource("x = x + 1;\n"));

  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 6);
}

void testRegisterCount() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let x = 1 + 2 * 3;\n");
  std::printf("DEBUG: bytecode.numRegisters = %d\n", bytecode.numRegisters);
  VM vm;
  vm.run(bytecode);
  std::printf("DEBUG: vm.registerCount = %d\n", vm.registerCount());
  assert(vm.registerCount() == 3);
}

// --------------------------------------------------------- PHASE 5: control

void testIfElseExecution() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var a = 0;\n"
      "if (true) { a = 1; } else { a = 2; }\n"
      "if (false) { a = 3; } else { a = 4; }\n"
      "if (false) { a = 5; }\n");
  VM vm;
  vm.run(bytecode);

  const Value& a = requireGlobal(vm, "a");
  assert(a.isInt() && a.asInt() == 4);
}

void testElseIfChain() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let x = 2;\n"
      "var out = \"\";\n"
      "if (x == 1) { out = \"one\"; }\n"
      "else if (x == 2) { out = \"two\"; }\n"
      "else { out = \"other\"; }\n");
  VM vm;
  vm.run(bytecode);

  const Value& out = requireGlobal(vm, "out");
  assert(out.isStr() && out.asStr() == "two");
}

void testIfBranchIsSkipped() {
  TEST_SCOPE();
  std::string output = runAndCapture(
      "if (false) { print(\"then\"); } else { print(\"else\"); }\n"
      "if (true) { print(\"yes\"); }\n");
  assert(output == "else\nyes\n");
}

void testTruthinessInConditions() {
  TEST_SCOPE();
  // Only nil and false are falsy; 0 and "" are truthy.
  std::string output = runAndCapture(
      "if (0) { print(\"zero\"); }\n"
      "if (\"\") { print(\"empty\"); }\n"
      "if (nil) { print(\"nil\"); }\n"
      "if (false) { print(\"false\"); }\n");
  assert(output == "zero\nempty\n");
}

void testWhileLoop() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var sum = 0;\n"
      "var i = 0;\n"
      "while (i < 5) { sum += i; i += 1; }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "sum").asInt() == 10);
  assert(requireGlobal(vm, "i").asInt() == 5);
}

void testForLoop() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var sum = 0;\n"
      "for (var i = 1; i <= 4; i += 1) { sum += i; }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "sum").asInt() == 10);

  // The `for` variable is local: it is not registered as a global.
  assert(vm.findGlobal("i") == nullptr);
}

void testForWithoutCondition() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var n = 0;\n"
      "for (;;) { n += 1; if (n >= 3) { break; } }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "n").asInt() == 3);
}

void testBreakOutOfWhile() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var n = 0;\n"
      "while (true) { n += 1; if (n == 3) { break; } }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "n").asInt() == 3);
}

void testContinueInWhile() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var sum = 0;\n"
      "var i = 0;\n"
      "while (i < 5) {\n"
      "  i += 1;\n"
      "  if (i % 2 == 0) { continue; }\n"
      "  sum += i;\n"
      "}\n");
  VM vm;
  vm.run(bytecode);

  // Sum of the odd numbers 1, 3, 5.
  assert(requireGlobal(vm, "sum").asInt() == 9);
  assert(requireGlobal(vm, "i").asInt() == 5);
}

void testContinueInForRunsStep() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var sum = 0;\n"
      "for (var i = 1; i <= 4; i += 1) {\n"
      "  if (i % 2 == 0) { continue; }\n"
      "  sum += i;\n"
      "}\n");
  VM vm;
  vm.run(bytecode);

  // If `continue` did not run the step, the loop would never end.
  assert(requireGlobal(vm, "sum").asInt() == 4);
}

void testNestedLoops() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "var total = 0;\n"
      "for (var i = 0; i < 3; i += 1) {\n"
      "  for (var j = 0; j < 3; j += 1) {\n"
      "    if (j == 1) { continue; }\n"
      "    if (i == 2) { break; }\n"
      "    total += 1;\n"
      "  }\n"
      "}\n");
  VM vm;
  vm.run(bytecode);

  // i = 0: j = 0 and j = 2 add up; i = 1: same; i = 2: break at j = 0.
  assert(requireGlobal(vm, "total").asInt() == 4);
}

void testBlockScopingAndShadowing() {
  TEST_SCOPE();
  std::string output = runAndCapture(
      "let x = 1;\n"
      "{\n"
      "  let x = 2;\n"
      "  print(x);\n"
      "}\n"
      "print(x);\n");
  assert(output == "2\n1\n");
}

void testBlockLocalIsNotGlobal() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("{ let inner = 42; print(inner); }\n");
  VM vm;
  CoutCapture capture;
  vm.run(bytecode);

  assert(capture.stream.str() == "42\n");
  assert(vm.findGlobal("inner") == nullptr);
}

void testLocalCompoundAssignment() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "{\n"
      "  var y = 10;\n"
      "  y += 5;\n"
      "  y *= 2;\n"
      "  print(y);\n"
      "}\n");
  VM vm;
  CoutCapture capture;
  vm.run(bytecode);

  assert(capture.stream.str() == "30\n");
  assert(vm.findGlobal("y") == nullptr);
}

void testLocalCount() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "{\n"
      "  let a = 1;\n"
      "  { let b = 2; print(b); }\n"
      "  print(a);\n"
      "}\n");
  VM vm;
  CoutCapture capture;
  vm.run(bytecode);

  assert(capture.stream.str() == "2\n1\n");
  // Slots are reused when each scope closes.
  assert(vm.localCount() == 2);
}

void testBitwiseExecution() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let a = 6 & 3;\n"
      "let b = 4 | 1;\n"
      "let c = 5 ^ 1;\n"
      "let d = 1 << 4;\n"
      "let e = 32 >> 2;\n"
      "let f = ~0;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 2);
  assert(requireGlobal(vm, "b").asInt() == 5);
  assert(requireGlobal(vm, "c").asInt() == 4);
  assert(requireGlobal(vm, "d").asInt() == 16);
  assert(requireGlobal(vm, "e").asInt() == 8);
  assert(requireGlobal(vm, "f").asInt() == -1);
}

void testShiftErrors() {
  TEST_SCOPE();
  assert(runtimeError("let x = 1 << 64;\n") == "shift count must be between 0 and 63");
  assert(runtimeError("let x = 1 >> -1;\n") == "shift count must be between 0 and 63");
  assert(runtimeError("let x = 1.5 & 1;\n").find("unsupported operand types") !=
         std::string::npos);
  assert(runtimeError("let x = ~1.5;\n").find("unsupported unary operator") !=
         std::string::npos);
}

void testLogicalShortCircuit() {
  TEST_SCOPE();
  // The right-hand side must not be evaluated: 1 / 0 would throw a RuntimeError.
  Bytecode bytecode = compileSource(
      "let a = false && (1 / 0);\n"
      "let b = true || (1 / 0);\n"
      "let c = true ? 1 : (1 / 0);\n"
      "let d = false ? (1 / 0) : 2;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").isBool() && !requireGlobal(vm, "a").asBool());
  assert(requireGlobal(vm, "b").isBool() && requireGlobal(vm, "b").asBool());
  assert(requireGlobal(vm, "c").asInt() == 1);
  assert(requireGlobal(vm, "d").asInt() == 2);
}

void testLogicalOperandValues() {
  TEST_SCOPE();
  // `&&` and `||` return an operand, not a forced boolean.
  Bytecode bytecode = compileSource(
      "let a = nil || 7;\n"
      "let b = false || \"x\";\n"
      "let c = 0 && 5;\n"   // 0 is truthy: the right operand is returned
      "let d = false && 9;\n"
      "let e = \"\" || \"fallback\";\n"  // "" is also truthy: the left operand is returned
      "let f = nil || \"fallback\";\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 7);
  assert(requireGlobal(vm, "b").isStr() && requireGlobal(vm, "b").asStr() == "x");
  assert(requireGlobal(vm, "c").asInt() == 5);
  assert(requireGlobal(vm, "d").isBool() && !requireGlobal(vm, "d").asBool());
  assert(requireGlobal(vm, "e").isStr() && requireGlobal(vm, "e").asStr().empty());
  assert(requireGlobal(vm, "f").isStr() && requireGlobal(vm, "f").asStr() == "fallback");
}

void testLogicalEvaluatesRightWhenNeeded() {
  TEST_SCOPE();
  assert(runtimeError("let x = true && (1 / 0);\n") == "division by zero");
  assert(runtimeError("let x = false || (1 / 0);\n") == "division by zero");
  assert(runtimeError("let x = true ? (1 / 0) : 2;\n") == "division by zero");
}

// ------------------------------------------------- PHASE 6: shared Runtime

void testVmCallsNatives() {
  TEST_SCOPE();
  VM vm;
  registerBuiltins(vm);

  const Value* toStr = vm.findGlobal("str");
  assert(toStr != nullptr && toStr->isFunc());
  Value result = vm.call(*toStr, {Value(42)}, 1, 1);
  assert(result.isStr() && result.asStr() == "42");

  // A failing native reports the caller's position, exactly like the
  // interpreter does, so both engines print the same diagnostic.
  const Value* assertEq = vm.findGlobal("assertEq");
  assert(assertEq != nullptr);
  bool threw = false;
  try {
    vm.call(*assertEq, {Value(1), Value(2)}, 7, 9);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message.find("assertion failed") != std::string::npos);
    assert(error.line == 7 && error.col == 9);
  }
  assert(threw);
}

void testVmCallsBoundMethods() {
  TEST_SCOPE();
  VM vm;
  registerBuiltins(vm);

  ArrayPtr array = makeArray({Value(1)});
  Value push = ops::memberGet(vm, Value(array), "push", 1, 1);
  assert(push.isFunc());
  vm.call(push, {Value(2)}, 1, 1);

  assert(array->items.size() == 2);
  assert(array->items[1].isInt() && array->items[1].asInt() == 2);
}

void testRuntimeErrorCarriesSourcePosition() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("let a = 1;\nlet b = 1 / 0;\n");
  VM vm;
  bool threw = false;
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message == "division by zero");
    // The instruction points at the operator, not at the start of the line.
    assert(error.line == 2);
    assert(error.col == 11);
  }
  assert(threw);
}

// ------------------------------------------------- PHASE 6a: functions
// Note: slice 6a supports top-level `fn` declarations with parameters,
// `return`, recursion and general calls. Lambdas, upvalues, collections and
// for-in arrive in 6b-6d.

void testFunctionCallAndReturn() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn add(a, b) { return a + b; }\n"
      "let sum = add(2, 3);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "sum").isInt() && requireGlobal(vm, "sum").asInt() == 5);

  const Value& add = requireGlobal(vm, "add");
  assert(add.isFunc() && add.func().isVm());
  assert(add.func().name == "add");
}

void testFunctionWithoutReturnYieldsNil() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn noop() {}\n"
      "let r = noop();\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "r").isNil());
}

void testEarlyReturn() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn sign(n) {\n"
      "  if (n < 0) { return -1; }\n"
      "  if (n == 0) { return 0; }\n"
      "  return 1;\n"
      "}\n"
      "let a = sign(-5);\n"
      "let b = sign(0);\n"
      "let c = sign(7);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == -1);
  assert(requireGlobal(vm, "b").asInt() == 0);
  assert(requireGlobal(vm, "c").asInt() == 1);
}

void testRecursion() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn fact(n) {\n"
      "  if (n <= 1) { return 1; }\n"
      "  return n * fact(n - 1);\n"
      "}\n"
      "fn fib(n) {\n"
      "  if (n < 2) { return n; }\n"
      "  return fib(n - 1) + fib(n - 2);\n"
      "}\n"
      "let a = fact(0);\n"
      "let b = fact(5);\n"
      "let c = fact(10);\n"
      "let d = fib(10);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 1);
  assert(requireGlobal(vm, "b").asInt() == 120);
  assert(requireGlobal(vm, "c").asInt() == 3628800);
  assert(requireGlobal(vm, "d").asInt() == 55);
}

void testMutualRecursion() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn isEven(n) { if (n == 0) { return true; } return isOdd(n - 1); }\n"
      "fn isOdd(n) { if (n == 0) { return false; } return isEven(n - 1); }\n"
      "let a = isEven(10);\n"
      "let b = isOdd(7);\n"
      "let c = isEven(7);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").isBool() && requireGlobal(vm, "a").asBool());
  assert(requireGlobal(vm, "b").isBool() && requireGlobal(vm, "b").asBool());
  assert(requireGlobal(vm, "c").isBool() && !requireGlobal(vm, "c").asBool());
}

void testFunctionParametersAreLocals() {
  TEST_SCOPE();
  // A parameter shadows a global of the same name, and can be called.
  Bytecode bytecode = compileSource(
      "let x = 100;\n"
      "fn double(x) { return x * 2; }\n"
      "fn apply(f, a) { return f(a); }\n"
      "let a = double(21);\n"
      "let b = apply(double, 21);\n"
      "let c = x;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 42);
  assert(requireGlobal(vm, "b").asInt() == 42);
  // The global was not mutated by the parameter with the same name.
  assert(requireGlobal(vm, "c").asInt() == 100);
}

void testFunctionLocalsAndLoops() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn sum(n) {\n"
      "  var total = 0;\n"
      "  for (var i = 1; i <= n; i += 1) { total += i; }\n"
      "  return total;\n"
      "}\n"
      "let a = sum(5);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 15);
  // `total` and `i` lived in the call frame, never in the global table.
  assert(vm.findGlobal("total") == nullptr);
  assert(vm.findGlobal("i") == nullptr);
}

void testFunctionValueInVariable() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn double(x) { return x * 2; }\n"
      "let d = double;\n"
      "let r = d(21);\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "r").asInt() == 42);
}

void testPrintInsideFunction() {
  TEST_SCOPE();
  std::string output = runAndCapture(
      "fn show(x) { print(x); }\n"
      "show(5);\n");
  assert(output == "5\n");
}

void testFunctionSurvivesRunBoundary() {
  TEST_SCOPE();
  // The closure owns its proto, so a global function stays callable after the
  // Bytecode that produced it has been destroyed.
  VM vm;
  vm.run(compileSource("fn inc(x) { return x + 1; }\n"));
  vm.run(compileSource("let y = inc(41);\n"));
  assert(requireGlobal(vm, "y").asInt() == 42);
}

void testVmCallIntoBytecodeFunction() {
  TEST_SCOPE();
  VM vm;
  vm.run(compileSource("fn add(a, b) { return a + b; }\n"));

  const Value* add = vm.findGlobal("add");
  assert(add != nullptr && add->isFunc());
  Value result = vm.call(*add, {Value(20), Value(22)}, 1, 1);
  assert(result.isInt() && result.asInt() == 42);
}

void testFunctionArityErrors() {
  TEST_SCOPE();
  assert(runtimeError("fn f(a) { return a; }\nf();\n") ==
         "f() expects 1 argument, got 0");
  assert(runtimeError("fn f(a) { return a; }\nf(1, 2);\n") ==
         "f() expects 1 argument, got 2");
  assert(runtimeError("fn f() { return nil; }\nf(1);\n") ==
         "f() expects 0 arguments, got 1");
}

void testFunctionDepthLimit() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn loop(n) { return loop(n + 1); }\n"
      "let x = loop(0);\n");
  VM vm;
  vm.maxDepth = 20;
  bool threw = false;
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message.find("maximum call depth exceeded") != std::string::npos);
  }
  assert(threw);
}

void testFunctionStackTrace() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "fn inner() { return 1 / 0; }\n"
      "fn outer() { return inner(); }\n"
      "let x = outer();\n");
  VM vm;
  bool threw = false;
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message == "division by zero");
    bool sawOuter = false;
    bool sawInner = false;
    for (const Frame& frame : error.stack) {
      if (frame.function == "outer") sawOuter = true;
      if (frame.function == "inner") sawInner = true;
    }
    assert(sawOuter && sawInner);
  }
  assert(threw);
}

void testMultiArgumentPrintThroughNative() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource("print(1, 2, 3);\n");
  VM vm;
  registerBuiltins(vm);
  CoutCapture capture;
  vm.run(bytecode);

  assert(capture.stream.str() == "1 2 3\n");
}

// ------------------------------------------- PHASE 6b: lambdas, upvalues, closures

void testLambdaCompilesToClosureAndGetUpval() {
  TEST_SCOPE();
  // A lambda that captures an enclosing local reads it through GETUPVAL;
  // the closure value survives in a global slot.
  Bytecode bytecode = compileSource(
      "fn makeAdder(n) { return x => x + n; }\n"
      "let add5 = makeAdder(5);\n"
      "let r = add5(40);\n");
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "r").asInt() == 45);
  const Value& adder = requireGlobal(vm, "add5");
  assert(adder.isFunc() && adder.func().isVm());
  assert(!adder.func().upvalues.empty());  // `n` captured as one cell
}

void testClosureCapturesParameterAndLocalTogether() {
  // A parameter and a local of the same enclosing function are captured from
  // different slots; both must read their own value, not nil.
  Bytecode bytecode = compileSource(
      "fn f(a) { let y = 2; return () => a + y; }\n"
      "let r = f(40)();\n");
  VM vm;
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "r").asInt() == 42);
}

void testNestedClosuresShareTheOuterParameter() {
  // Three levels: the innermost lambda reaches `x` and `y` through the
  // upvalues of the lambdas around it.
  Bytecode bytecode = compileSource(
      "fn outer(x) { return y => z => x + y + z; }\n"
      "let r = outer(1)(2)(3);\n");
  VM vm;
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "r").asInt() == 6);
}

void testClosureOverTheParameterSeesLaterWrites() {
  // The closure captures the variable, not its value at creation time.
  Bytecode bytecode = compileSource(
      "fn f(v) { let read = () => v; v = 9; return read(); }\n"
      "let r = f(1);\n");
  VM vm;
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "r").asInt() == 9);
}

void testCounterClosuresShareMutableCell() {
  TEST_SCOPE();
  // Two closures created by the same call share the counter's cells: writes
  // from one (SETUPVAL) are visible through the other (GETUPVAL), and two
  // separate calls own independent state. Collections arrive in 6c, so the
  // pair is passed back through an out-parameter style call instead.
  Bytecode bytecode = compileSource(
      "fn makeCounter(start, setBump, setPeek) {\n"
      "  var count = start;\n"
      "  setBump(() => { count += 1; return count; });\n"
      "  setPeek(() => count);\n"
      "}\n"
      "var bump1 = nil;\n"
      "var peek1 = nil;\n"
      "var bump2 = nil;\n"
      "makeCounter(0, f => { bump1 = f; }, f => { peek1 = f; });\n"
      "makeCounter(100, f => { bump2 = f; }, f => {});\n"
      "let a = bump1();\n"
      "let b = bump1();\n"
      "let p = peek1();\n"
      "let d = bump2();\n");
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "a").asInt() == 1);
  assert(requireGlobal(vm, "b").asInt() == 2);
  // `peek` sees the write `bump` made through SETUPVAL: shared cell.
  assert(requireGlobal(vm, "p").asInt() == 2);
  // The second counter has its own cell chain.
  assert(requireGlobal(vm, "d").asInt() == 101);
}

void testCapturedLocalsSurviveFrameExit() {
  TEST_SCOPE();
  // After the defining frame returns, its captured cells stay alive through the
  // closure that captured them and keep their values.
  Bytecode bytecode = compileSource(
      "fn scopeTest() {\n"
      "  let y = 5;\n"
      "  if (true) {\n"
      "    let z = 1;\n"
      "    return () => y + z;\n"
      "  }\n"
      "}\n"
      "let f = scopeTest();\n"
      "let r = f();\n");
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "r").asInt() == 6);
}

void testCapturedCellsAreNotRetainedByTheVM() {
  TEST_SCOPE();
  VM vm;                          // <-- construir VM PRIMERO
  gcCollect();                    // <-- colectar DESPUÉS (builtins ya registrados)
  const std::size_t before = gcTrackedCount();
  Bytecode bytecode = compileSource(
      "fn make(i) {\n"
      "  let inner = () => i;\n"
      "  let outer = () => inner();\n"
      "  return outer();\n"
      "}\n"
      "var total = 0;\n"
      "var n = 0;\n"
      "while (n < 1000) { total += make(n); n += 1; }\n");
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 499500);
  gcCollect();
  assert(gcTrackedCount() <= before + 2);
}

void testClosureCyclesAreCollected() {
  TEST_SCOPE();
  VM vm;                                          // <-- VM PRIMERO
  gcCollect();                                    // <-- colectar después
  const std::size_t before = gcTrackedCount();    // <-- before incluye builtins
  Bytecode bytecode = compileSource(
      "fn make(i) {\n"
      "  var a = () => b();\n"
      "  var b = () => a();\n"
      "  return i;\n"
      "}\n"
      "var total = 0;\n"
      "var n = 0;\n"
      "while (n < 40000) { total += make(n); n += 1; }\n");
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 799980000);
  assert(gcTrackedCount() < before + 20000);
  gcCollect();
  assert(gcTrackedCount() <= before + 2);
}

void testTableCyclesAreCollectedWithoutClosures() {
  TEST_SCOPE();
  // A table that contains itself is a cycle reference counting cannot free. No
  // closure is created here, so nothing but the tables themselves may trigger
  // the collector.
  VM vm;
  gcCollect();
  const std::size_t before = gcTrackedCount();
  Bytecode bytecode = compileSource(R"OSKEL(
fn make(i) {
  let m = {};
  m.me = m;
  let a = [0];
  a[0] = a;
  return i;
}
var total = 0;
var k = 0;
while (k < 30000) { total += make(k); k += 1; }
)OSKEL");
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 449985000);
  // 60000 cyclic tables were created; the collector must have run meanwhile.
  assert(gcTrackedCount() < before + 20000);
  gcCollect();
  assert(gcTrackedCount() <= before + 2);
}

void testClosureStoredInTheTableItCapturesIsCollected() {
  TEST_SCOPE();
  // map -> closure -> captured cell -> map, and the same through an array. The
  // collector has to follow a function's captured cells to see these cycles.
  VM vm;
  gcCollect();
  const std::size_t before = gcTrackedCount();
  Bytecode bytecode = compileSource(R"OSKEL(
fn make(i) {
  let m = {};
  m.cb = () => m;
  let xs = [0];
  xs[0] = () => xs;
  return i;
}
var total = 0;
var k = 0;
while (k < 10000) { total += make(k); k += 1; }
)OSKEL");
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 49995000);
  // 60000 objects were left in cycles (table, closure and cell, twice).
  assert(gcTrackedCount() < before + 20000);
  gcCollect();
  assert(gcTrackedCount() <= before + 2);
}

void testNestedClosureThroughTwoLevels() {
  TEST_SCOPE();
  // An inner lambda resolves an outer-local via the parent's upvalue list
  // (UpvalDesc with inParentLocal = false), not via the frame's locals.
  Bytecode bc = compileSource(
      "let outer = a => (b => () => a + b);\n"
      "let g = outer(10)(3);\n"
      "let r = g();\n");
  VM vm;
  runOrFail(vm, bc);

  assert(requireGlobal(vm, "r").asInt() == 13);
}

void testUpvalueIndexOutOfRangeFailsCleanly() {
  TEST_SCOPE();
  // Hand-built chunk: GETUPVAL with no upvalues bound must raise a runtime
  // error instead of touching memory out of bounds.
  Bytecode chunk;
  chunk.numRegisters = 2;
  chunk.code.push_back(Instruction{OpCode::GETUPVAL, 0, 0, 0, 0, 1, 1});
  chunk.code.push_back(Instruction{OpCode::HALT, 0, 0, 0, 0, 1, 1});
  VM vm;
  bool threw = false;
  try {
    vm.run(chunk);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message.find("upvalue index out of range") != std::string::npos);
  }
  assert(threw);
}

void testClosureWriteBackIntoEnclosingLocal() {
  TEST_SCOPE();
  // The enclosing function keeps running after creating the closure and
  // observes writes made through the shared cell while it is still open.
  Bytecode bytecode = compileSource(
      "fn writer() {\n"
      "  var v = 1;\n"
      "  let set = x => { v = x; };\n"
      "  set(42);\n"
      "  return v;\n"
      "}\n"
      "let r = writer();\n");
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "r").asInt() == 42);
}

void testVmRejectsAstFunctions() {
  TEST_SCOPE();
  // A function with neither a native body nor a proto cannot be executed by
  // the VM; it must refuse instead of silently running the AST.
  VM vm;
  auto function = makeRef<Function>();
  function->name = "user";
  bool threw = false;
  try {
    vm.call(Value(function), {}, 3, 4);
  } catch (const RuntimeError& error) {
    threw = true;
    assert(error.message.find("cannot call") != std::string::npos);
    assert(error.line == 3 && error.col == 4);
  }
  assert(threw);
}

void testErrorValues() {
  // An error is a distinct runtime type, not a map. The reference counting of
  // the underlying ErrorObj must behave like a string: copying a Value bumps
  // the count, moving transfers it, and the last handle frees it.
  Value e = makeError("boom", 42);
  assert(e.isError());
  assert(e.type() == VType::Error);
  assert(e.typeName() == std::string("error"));
  assert(e.err().message == "boom");
  assert(e.err().code == 42);

  // `errorPtr()` returns a handle by value, so hold the raw pointer to observe
  // the count without the temporary handle skewing it.
  ErrorObj* object = e.errorPtr().get();
  assert(object->refs == 1);

  // Copy bumps the count.
  Value copy = e;
  assert(object->refs == 2);
  assert(copy.isError() && copy.err().message == "boom");
  assert(copy.equals(e));

  // Move transfers the reference without touching the count.
  Value moved = std::move(copy);
  assert(moved.isError());
  assert(object->refs == 2);
  // The moved-from value is left nil, as for every other reference type.
  assert(copy.isNil());

  // Equality is by value (message and code).
  assert(makeError("x", 1).equals(makeError("x", 1)));
  assert(!makeError("x", 1).equals(makeError("x", 2)));
  assert(!makeError("x", 0).equals(Value("x")));

  // Dropping every handle frees the object.
  moved = Value();
  e = Value();
}

void testErrorValuesAreNotConfusedWithMaps() {
  // The whole point of a dedicated type: `isError` is exact, so a user map that
  // happens to have an `ok` field can never be mistaken for an error.
  assert(!Value("error").isError());
  assert(!Value().isError());
  assert(!Value(int64_t{0}).isError());
  MapPtr lookalike = makeMap();
  lookalike->set("ok", Value(false));
  lookalike->set("error", Value("boom"));
  assert(!Value(lookalike).isError());
}

// ------------------------------------------------------------- FFI pointers

void testFfiCStringRoundTrip() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let s = cstring(\"hello\");\n"
      "let t = cstr(s);\n"
      "let n = byteLen(s);\n"
      "let kind = type(s);\n");
  VM vm;
  registerBuiltins(vm);
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "t").asStr() == "hello");
  assert(requireGlobal(vm, "n").asInt() == 5);
  assert(std::string(requireGlobal(vm, "kind").asStr()) == "cstring");
  assert(requireGlobal(vm, "s").isPtr());
}

void testFfiBufferLoadStore() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let b = buffer(4);\n"
      "storeByte(b, 1, 65);\n"
      "let v = loadByte(b, 1);\n"
      "let n = byteLen(b);\n");
  VM vm;
  registerBuiltins(vm);
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "v").asInt() == 65);
  assert(requireGlobal(vm, "n").asInt() == 4);
}

void testFfiVoidTokenRoundTrip() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "let p = intToPtr(4096);\n"
      "let token = ptrToInt(p);\n"
      "let same = (p == p);\n"
      "let distinct = (p == intToPtr(4096));\n");
  VM vm;
  registerBuiltins(vm);
  runOrFail(vm, bytecode);
  assert(requireGlobal(vm, "token").asInt() == 4096);
  assert(requireGlobal(vm, "same").asBool());
  // A distinct handle with the same token is not the same pointer.
  assert(!requireGlobal(vm, "distinct").asBool());
}

// C-linkage host functions the `extern "C"` tests resolve by name.
extern "C" int joltTestAdd(int a, int b) { return a + b; }
extern "C" double joltTestScale(double x) { return x * 2.0; }
extern "C" const char* joltTestGreeting() { return "hi"; }
extern "C" int joltTestBump(int* p) {
  if (!p) return -1;
  int previous = *p;
  *p = previous + 1;
  return previous;
}

// Struct-by-value host functions (PHASE 6.6). Each returns or takes a small
// struct so the marshalling is exercised across the six System V register
// shapes: I, S, II, SS, IS and SI.
extern "C" struct JoltPairInt { int a; int b; };
extern "C" struct JoltPairDouble { double a; double b; };
extern "C" struct JoltMixed { int a; double b; };
extern "C" struct JoltMixedRev { double a; int b; };
extern "C" struct JoltOneDouble { double a; };

extern "C" JoltPairInt joltTestMakePair(int a, int b) { return JoltPairInt{a, b}; }
extern "C" JoltPairDouble joltTestMakePairDouble(double a, double b) {
  return JoltPairDouble{a, b};
}
extern "C" JoltMixed joltTestMakeMixed(int a, double b) { return JoltMixed{a, b}; }
extern "C" JoltMixedRev joltTestMakeMixedRev(double a, int b) { return JoltMixedRev{a, b}; }
extern "C" JoltOneDouble joltTestMakeOneDouble(double a) { return JoltOneDouble{a}; }
// Takes a struct by value and returns an integer derived from it, so a struct
// *argument* is proven to arrive with the right bytes in the right registers.
extern "C" int joltTestSumPair(JoltPairInt p) { return p.a + p.b; }
extern "C" double joltTestScalePair(JoltPairDouble p) { return p.a * 100.0 + p.b; }
extern "C" double joltTestMixMagnitude(JoltMixed m) { return m.a + m.b; }

// A struct smaller than one eightbyte: `{char; char}` is 2 bytes, so the whole
// struct travels in a single INTEGER register with the upper bytes unspecified.
// Reading the return must touch only the two declared bytes.
extern "C" struct JoltTwoBytes { char a; char b; };
extern "C" JoltTwoBytes joltTestMakeTwoBytes(char a, char b) { return JoltTwoBytes{a, b}; }

// C functions that call a Jolt callback, one per ABI shape: a GPR argument and
// return, an SSE argument and return, and a `void` callback.
extern "C" int joltTestApply(int (*f)(int), int x) { return f(x); }
extern "C" double joltTestApplyDouble(double (*f)(double), double x) { return f(x); }
extern "C" int joltTestApplyTwice(int (*f)(int), int x) { return f(x) + f(x); }
extern "C" int joltTestApplyVoid(void (*f)(int), int x) {
  f(x);
  return x;
}

// A host that resolves the symbols above. Everything else behaves like the
// dev platform (writes go to captured strings, so a test can read them).
struct SymbolPlatform final : Platform {
  std::string out;
  std::string err;
  void write(int fd, const char* data, std::size_t length) override {
    if (fd == kStdout) out.append(data, length);
  }
  void writeErr(const char* data, std::size_t length) override { err.append(data, length); }
  void flush(int) override {}
  std::size_t read(int, char*, std::size_t) override { return 0; }
  int open(const char*, OpenMode) override { return kInvalidFd; }
  void close(int) override {}
  bool seek(int, std::int64_t, SeekOrigin) override { return false; }
  [[noreturn]] void exit(int) override { std::abort(); }
  void* alloc(std::size_t, std::size_t) override { return nullptr; }
  void dealloc(void*) override {}
  void* lookupSymbol(const char* name) override {
    const std::string symbol = name;
    if (symbol == "joltTestAdd") return reinterpret_cast<void*>(&joltTestAdd);
    if (symbol == "joltTestScale") return reinterpret_cast<void*>(&joltTestScale);
    if (symbol == "joltTestGreeting") return reinterpret_cast<void*>(&joltTestGreeting);
    if (symbol == "joltTestBump") return reinterpret_cast<void*>(&joltTestBump);
    if (symbol == "joltTestMakePair") return reinterpret_cast<void*>(&joltTestMakePair);
    if (symbol == "joltTestMakePairDouble")
      return reinterpret_cast<void*>(&joltTestMakePairDouble);
    if (symbol == "joltTestMakeMixed") return reinterpret_cast<void*>(&joltTestMakeMixed);
    if (symbol == "joltTestMakeMixedRev") return reinterpret_cast<void*>(&joltTestMakeMixedRev);
    if (symbol == "joltTestMakeOneDouble")
      return reinterpret_cast<void*>(&joltTestMakeOneDouble);
    if (symbol == "joltTestSumPair") return reinterpret_cast<void*>(&joltTestSumPair);
    if (symbol == "joltTestScalePair") return reinterpret_cast<void*>(&joltTestScalePair);
    if (symbol == "joltTestMixMagnitude") return reinterpret_cast<void*>(&joltTestMixMagnitude);
    if (symbol == "joltTestMakeTwoBytes") return reinterpret_cast<void*>(&joltTestMakeTwoBytes);
    if (symbol == "joltTestApply") return reinterpret_cast<void*>(&joltTestApply);
    if (symbol == "joltTestApplyDouble") return reinterpret_cast<void*>(&joltTestApplyDouble);
    if (symbol == "joltTestApplyTwice") return reinterpret_cast<void*>(&joltTestApplyTwice);
    if (symbol == "joltTestApplyVoid") return reinterpret_cast<void*>(&joltTestApplyVoid);
    return nullptr;
  }
};

// Compiles `source` and runs it on a VM backed by `SymbolPlatform`, returning
// the captured stdout. Runtime failures abort with their message, so a broken
// marshalling is a clear failure rather than a wrong string.
std::string runWithSymbols(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  SymbolPlatform platform;
  VM vm(platform);
  registerBuiltins(vm);
  try {
    vm.run(bytecode);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "runtime failure in %s: %s\n", g_currentTest, error.what());
    std::exit(1);
  }
  return platform.out;
}

void testExternResolvesAndMarshalsIntegers() {
  TEST_SCOPE();
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestAdd(a: int, b: int): int; }\n"
      "print(joltTestAdd(2, 40));\n");
  assert(out == "42\n");
}

void testExternTruncatesNarrowReturnToDeclaredWidth() {
  TEST_SCOPE();
  // `joltTestAdd` returns a 32-bit `int`; the upper half of `rax` is undefined
  // after the call. Reading it as a 64-bit value would surface garbage here,
  // so a correct `int` (not `int64`) result proves the truncation.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestAdd(a: int, b: int): int; }\n"
      "var total = 0;\n"
      "for (var i = 0; i < 100; i = i + 1) { total = total + joltTestAdd(i, i); }\n"
      "print(total);\n");
  assert(out == "9900\n");
}

void testExternMarshalsDoubleReturnAndArgument() {
  TEST_SCOPE();
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestScale(x: double): double; }\n"
      "print(joltTestScale(21.0));\n");
  assert(out == "42\n");
}

void testExternMarshalsStringReturnAsCString() {
  TEST_SCOPE();
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestGreeting(): cstring; }\n"
      "print(cstr(joltTestGreeting()));\n");
  assert(out == "hi\n");
}

void testExternMarshalsPointerArgument() {
  TEST_SCOPE();
  // The buffer's bytes are passed as the address the callee writes through.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestBump(p: ptr): int; }\n"
      "let b = buffer(4);\n"
      "storeByte(b, 0, 7);\n"
      "let before = joltTestBump(b);\n"
      "print(before);\n"
      "print(loadByte(b, 0));\n");
  assert(out == "7\n8\n");
}

#if OSKEL_FFI_STRUCT_BY_VALUE
void testExternMarshalsStructReturnTwoIntegers() {
  TEST_SCOPE();
  // A two-`int` struct is 8 bytes, class INTEGER: it comes back wholly in `rax`.
  const std::string out = runWithSymbols(
      "struct PairInt { a: int; b: int; }\n"
      "extern \"C\" { fn joltTestMakePair(a: int, b: int): PairInt; }\n"
      "let p = joltTestMakePair(3, 4);\n"
      "print(p.a);\n"
      "print(p.b);\n");
  assert(out == "3\n4\n");
}

void testExternMarshalsStructReturnTwoDoubles() {
  TEST_SCOPE();
  // Two `double` fields are 16 bytes, class SSE: the eightbytes come back in
  // `xmm0`/`xmm1`.
  const std::string out = runWithSymbols(
      "struct PairDouble { a: double; b: double; }\n"
      "extern \"C\" { fn joltTestMakePairDouble(a: double, b: double): PairDouble; }\n"
      "let p = joltTestMakePairDouble(1.5, 2.5);\n"
      "print(p.a);\n"
      "print(p.b);\n");
  assert(out == "1.5\n2.5\n");
}

void testExternMarshalsStructReturnMixedClasses() {
  TEST_SCOPE();
  // `{int; double}` is class INTEGER+SSE: `rax` + `xmm0`. `{double; int}` is the
  // reverse: `xmm0` + `rax`. Both must be read from the right registers.
  const std::string out = runWithSymbols(
      "struct Mixed { a: int; b: double; }\n"
      "struct MixedRev { a: double; b: int; }\n"
      "extern \"C\" {\n"
      "  fn joltTestMakeMixed(a: int, b: double): Mixed;\n"
      "  fn joltTestMakeMixedRev(a: double, b: int): MixedRev;\n"
      "}\n"
      "let m = joltTestMakeMixed(7, 8.5);\n"
      "print(m.a);\n"
      "print(m.b);\n"
      "let r = joltTestMakeMixedRev(1.25, 9);\n"
      "print(r.a);\n"
      "print(r.b);\n");
  assert(out == "7\n8.5\n1.25\n9\n");
}

void testExternMarshalsStructArgument() {
  TEST_SCOPE();
  // A struct argument is flattened to its eightbytes and passed in the right
  // registers; the callee reads them back as the struct it declared.
  const std::string out = runWithSymbols(
      "struct PairInt { a: int; b: int; }\n"
      "extern \"C\" { fn joltTestSumPair(p: PairInt): int; }\n"
      "print(joltTestSumPair(PairInt { a: 20, b: 22 }));\n");
  assert(out == "42\n");
}

void testExternMarshalsMixedStructArgument() {
  TEST_SCOPE();
  // `{int; double}` argument: the INTEGER eightbyte goes in a GPR and the SSE
  // eightbyte in an xmm register, in that order.
  const std::string out = runWithSymbols(
      "struct Mixed { a: int; b: double; }\n"
      "extern \"C\" { fn joltTestMixMagnitude(m: Mixed): double; }\n"
      "print(joltTestMixMagnitude(Mixed { a: 2, b: 0.5 }));\n");
  assert(out == "2.5\n");
}

void testExternMarshalsStructArgumentAndReturnTogether() {
  TEST_SCOPE();
  // Two `double` fields passed in `xmm0`/`xmm1` and the result read back from
  // `xmm0`: the full round trip for the SSE shape.
  const std::string out = runWithSymbols(
      "struct PairDouble { a: double; b: double; }\n"
      "extern \"C\" { fn joltTestScalePair(p: PairDouble): double; }\n"
      "print(joltTestScalePair(PairDouble { a: 1.5, b: 0.25 }));\n");
  assert(out == "150.25\n");
}

void testExternMarshalsSubEightbyteStruct() {
  TEST_SCOPE();
  // A 2-byte struct is one INTEGER eightbyte: it comes back in `rax`, and only
  // the two declared bytes are read (the rest of the register is unspecified).
  const std::string out = runWithSymbols(
      "struct TwoBytes { a: int8; b: int8; }\n"
      "extern \"C\" { fn joltTestMakeTwoBytes(a: int8, b: int8): TwoBytes; }\n"
      "let t = joltTestMakeTwoBytes(65, 66);\n"
      "print(t.a);\n"
      "print(t.b);\n");
  assert(out == "65\n66\n");
}
#endif  // OSKEL_FFI_STRUCT_BY_VALUE

#ifdef OSKEL_HAVE_LIBFFI
void testExternCallbackGprShape() {
  TEST_SCOPE();
  // The callback is a Jolt function; C calls it through a libffi trampoline that
  // re-enters the VM with the C argument boxed as a Jolt value.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestApply(f: fn(int): int, x: int): int; }\n"
      "fn inc(v: int): int { return v + 1; }\n"
      "print(joltTestApply(inc, 41));\n");
  assert(out == "42\n");
}

void testExternCallbackSseShape() {
  TEST_SCOPE();
  // A `double` callback argument and return travel through SSE registers; the
  // trampoline boxes and writes them as `double`, not as a general-purpose word.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestApplyDouble(f: fn(double): double, x: double): double; }\n"
      "fn twice(v: double): double { return v * 2.0; }\n"
      "print(joltTestApplyDouble(twice, 21.0));\n");
  assert(out == "42\n");
}

void testExternCallbackVoidReturnAndClosure() {
  TEST_SCOPE();
  // A `void` callback writes no return value, and a closure works: the
  // trampoline re-enters the VM, so the callback sees its captured variable.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestApplyVoid(f: fn(int): void, x: int): int; }\n"
      "var seen = 0;\n"
      "let record = (v) => { seen = v; };\n"
      "print(joltTestApplyVoid(record, 7));\n"
      "print(seen);\n");
  assert(out == "7\n7\n");
}

void testExternCallbackInvokedRepeatedly() {
  TEST_SCOPE();
  // C calls the same trampoline twice within one extern call: the closure must
  // stay valid for the whole call, not just the first entry.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestApplyTwice(f: fn(int): int, x: int): int; }\n"
      "fn inc(v: int): int { return v + 1; }\n"
      "print(joltTestApplyTwice(inc, 10));\n");
  assert(out == "22\n");
}

void testExternCallbackErrorIsReraisedAfterCReturns() {
  TEST_SCOPE();
  // A Jolt error inside the callback cannot unwind through the C frame, so the
  // extern call captures it and re-raises it once C returns; `pcall` catches it.
  const std::string out = runWithSymbols(
      "extern \"C\" { fn joltTestApply(f: fn(int): int, x: int): int; }\n"
      "fn boom(v: int): int { panic(\"callback failed\"); }\n"
      "let r = pcall(() => joltTestApply(boom, 1));\n"
      "print(r.ok);\n"
      "print(r.error.message);\n");
  assert(out == "false\ncallback failed\n");
}
#endif  // OSKEL_HAVE_LIBFFI

void testExternRejectsLargeStructAtCompileTime() {
  TEST_SCOPE();
  // A struct larger than 16 bytes is passed in memory, a shape the register
  // marshalling does not support, so the declaration is rejected up front.
  const std::string message = compileErrorMessage(
      "struct Big { a: int; b: int; c: int; d: int; e: int; }\n"
      "extern \"C\" { fn f(x: Big): int; }\n");
  assert(message.find("memory") != std::string::npos);
}

void testExternRejectsNonScalarStructFieldAtCompileTime() {
  TEST_SCOPE();
  // A nested struct field cannot cross the boundary: it is not a scalar member.
  const std::string message = compileErrorMessage(
      "struct Inner { a: int; b: int; }\n"
      "struct Outer { p: Inner; q: int; }\n"
      "extern \"C\" { fn f(x: Outer): int; }\n");
  assert(message.find("cross the C boundary") != std::string::npos);
}

void testExternMissingSymbolIsAClearRuntimeError() {
  TEST_SCOPE();
  Bytecode bytecode = compileSource(
      "extern \"C\" { fn noSuchSymbolHere(x: int): int; }\n"
      "print(noSuchSymbolHere(1));\n");
  SymbolPlatform platform;
  VM vm(platform);
  registerBuiltins(vm);
  std::string message;
  try {
    vm.run(bytecode);
  } catch (const RuntimeError& error) {
    message = error.message;
  }
  assert(message.find("noSuchSymbolHere") != std::string::npos);
  assert(message.find("not found") != std::string::npos);
}

void testExternRejectsUnsupportedSignatureAtCompileTime() {
  TEST_SCOPE();
  // A `map` cannot cross the C boundary: the compiler must reject the
  // declaration before any bytecode is produced.
  bool threw = false;
  try {
    auto tokens = lex("extern \"C\" { fn bad(m: map): int; }");
    Parser parser(std::move(tokens), "<vm-test>");
    auto program = parser.parseProgram();
    compileProgram(program);
  } catch (const CompileError& error) {
    threw = true;
    assert(error.message.find("bad") != std::string::npos);
  }
  assert(threw);
}

void testExternRejectsVariadicDeclarationAtCompileTime() {
  TEST_SCOPE();
  bool threw = false;
  try {
    auto tokens = lex("extern \"C\" { fn bad(rest...): int; }");
    Parser parser(std::move(tokens), "<vm-test>");
    auto program = parser.parseProgram();
    compileProgram(program);
  } catch (const CompileError& error) {
    threw = true;
    assert(error.message.find("variadic") != std::string::npos);
  }
  assert(threw);
}

void testFfiErrorsAreCleanRuntimeFailures() {
  assert(runtimeError("loadByte(cstring(\"x\"), 9);").find("out of range") != std::string::npos);
  assert(runtimeError("storeByte(cstring(\"x\"), 0, 65);").find("read-only") != std::string::npos);
  assert(runtimeError("loadByte(1, 0);").find("expects a pointer") != std::string::npos);
  assert(runtimeError("buffer(-1);").find("must not be negative") != std::string::npos);
  assert(runtimeError("let p = buffer(2); free(p); loadByte(p, 0);").find("after it was freed") !=
         std::string::npos);
  assert(runtimeError("let p = buffer(2); free(p); free(p);").find("after it was freed") !=
         std::string::npos);
}

}  // namespace

int main() {
  testIntegerArithmetic();
  testFloatArithmetic();
  testFloatDoublePromotion();
  testSubtractionAndMultiplication();
  testModulo();
  testPower();
  testStringConcatenation();
  testGlobalAssignmentAndRead();
  testCompoundAssignment();
  testNilAndBoolLiterals();
  testUnaryOperators();
  testComparisons();
  testPrint();
  testPrintUsesInjectedPlatform();
  testBuiltinIoUsesInjectedPlatform();
  testDivisionByZero();
  testModuloByZero();
  testUndefinedGlobal();
  testUndefinedGlobalSuggestsNamespace();
  testInvalidComparison();
  testGlobalsPersistBetweenRuns();
  testRegisterCount();
  testIfElseExecution();
  testElseIfChain();
  testIfBranchIsSkipped();
  testTruthinessInConditions();
  testWhileLoop();
  testForLoop();
  testForWithoutCondition();
  testBreakOutOfWhile();
  testContinueInWhile();
  testContinueInForRunsStep();
  testNestedLoops();
  testBlockScopingAndShadowing();
  testBlockLocalIsNotGlobal();
  testLocalCompoundAssignment();
  testLocalCount();
  testBitwiseExecution();
  testShiftErrors();
  testLogicalShortCircuit();
  testLogicalOperandValues();
  testLogicalEvaluatesRightWhenNeeded();
  testVmCallsNatives();
  testVmCallsBoundMethods();
  testRuntimeErrorCarriesSourcePosition();
  testFunctionCallAndReturn();
  testFunctionWithoutReturnYieldsNil();
  testEarlyReturn();
  testRecursion();
  testMutualRecursion();
  testFunctionParametersAreLocals();
  testFunctionLocalsAndLoops();
  testFunctionValueInVariable();
  testPrintInsideFunction();
  testFunctionSurvivesRunBoundary();
  testVmCallIntoBytecodeFunction();
  testFunctionArityErrors();
  testFunctionDepthLimit();
  testFunctionStackTrace();
  testMultiArgumentPrintThroughNative();
  testLambdaCompilesToClosureAndGetUpval();
  testClosureCapturesParameterAndLocalTogether();
  testNestedClosuresShareTheOuterParameter();
  testClosureOverTheParameterSeesLaterWrites();
  testCounterClosuresShareMutableCell();
  testCapturedLocalsSurviveFrameExit();
  testCapturedCellsAreNotRetainedByTheVM();
  testClosureCyclesAreCollected();
  testTableCyclesAreCollectedWithoutClosures();
  testClosureStoredInTheTableItCapturesIsCollected();
  testNestedClosureThroughTwoLevels();
  testUpvalueIndexOutOfRangeFailsCleanly();
  testClosureWriteBackIntoEnclosingLocal();
  testVmRejectsAstFunctions();
  testErrorValues();
  testErrorValuesAreNotConfusedWithMaps();
  testFfiCStringRoundTrip();
  testFfiBufferLoadStore();
  testFfiVoidTokenRoundTrip();
  testFfiErrorsAreCleanRuntimeFailures();
  testExternResolvesAndMarshalsIntegers();
  testExternTruncatesNarrowReturnToDeclaredWidth();
  testExternMarshalsDoubleReturnAndArgument();
  testExternMarshalsStringReturnAsCString();
  testExternMarshalsPointerArgument();
#if OSKEL_FFI_STRUCT_BY_VALUE
  testExternMarshalsStructReturnTwoIntegers();
  testExternMarshalsStructReturnTwoDoubles();
  testExternMarshalsStructReturnMixedClasses();
  testExternMarshalsStructArgument();
  testExternMarshalsMixedStructArgument();
  testExternMarshalsStructArgumentAndReturnTogether();
  testExternMarshalsSubEightbyteStruct();
#endif
#ifdef OSKEL_HAVE_LIBFFI
  testExternCallbackGprShape();
  testExternCallbackSseShape();
  testExternCallbackVoidReturnAndClosure();
  testExternCallbackInvokedRepeatedly();
  testExternCallbackErrorIsReraisedAfterCReturns();
#endif
  testExternRejectsLargeStructAtCompileTime();
  testExternRejectsNonScalarStructFieldAtCompileTime();
  testExternMissingSymbolIsAClearRuntimeError();
  testExternRejectsUnsupportedSignatureAtCompileTime();
  testExternRejectsVariadicDeclarationAtCompileTime();

  std::puts("vm tests passed");
  return 0;
}
