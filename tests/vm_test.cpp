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
};

std::string runAndCapture(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
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
  assert(x.isFloat());
  assert(x.asFloat() == 2.5);

  const Value& y = requireGlobal(vm, "y");
  assert(y.isFloat());
  assert(y.asFloat() == 3.5);
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
  assert(b.isFloat() && b.asFloat() == 1.5);

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
  assert(b.isFloat() && b.asFloat() == 8.0);

  // Negative exponent: falls into the double path, same as the interpreter.
  const Value& c = requireGlobal(vm, "c");
  assert(c.isFloat() && c.asFloat() == 0.5);
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
  assert(f.isFloat() && f.asFloat() == -2.5);
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
  VM vm;
  vm.run(bytecode);
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
  // Nothing but the closures themselves may keep a captured cell alive. `outer`
  // captures the cell that holds `inner`, so a cell kept around after its frame
  // returned would also keep a function alive. With plain reference counting
  // every one of them must be gone as soon as `make` returns, with no
  // collection involved.
  gcCollect();  // start with a fresh collection counter
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
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 499500);
  // Only the global `make` is left.
  assert(gcTrackedCount() <= before + 2);
}

void testClosureCyclesAreCollected() {
  TEST_SCOPE();
  // Two closures that capture each other's variable form a cycle through the
  // captured cells: plain reference counting never frees it. The VM must let
  // the collector reclaim these while the program is still running.
  gcCollect();
  const std::size_t before = gcTrackedCount();
  Bytecode bytecode = compileSource(
      "fn make(i) {\n"
      "  var a = () => b();\n"
      "  var b = () => a();\n"
      "  return i;\n"
      "}\n"
      "var total = 0;\n"
      "var n = 0;\n"
      "while (n < 40000) { total += make(n); n += 1; }\n");
  VM vm;
  runOrFail(vm, bytecode);

  assert(requireGlobal(vm, "total").asInt() == 799980000);
  // Each iteration leaves a cycle behind; without the collector the VM would
  // still hold every one of the 40000 of them here.
  assert(gcTrackedCount() < before + 20000);
  // An explicit collection reclaims whatever the automatic ones left.
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

}  // namespace

int main() {
  testIntegerArithmetic();
  testFloatArithmetic();
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
  testNestedClosureThroughTwoLevels();
  testUpvalueIndexOutOfRangeFailsCleanly();
  testClosureWriteBackIntoEnclosingLocal();
  testVmRejectsAstFunctions();

  std::puts("vm tests passed");
  return 0;
}
