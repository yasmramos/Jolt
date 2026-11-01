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

std::string runAndCapture(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  CoutCapture capture;
  vm.run(bytecode);
  return capture.stream.str();
}

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
  Bytecode bytecode = compileSource("let a = 9 - 4;\nlet b = 6 * 7;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").asInt() == 5);
  assert(requireGlobal(vm, "b").asInt() == 42);
}

void testModulo() {
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
  Bytecode bytecode = compileSource("let s = \"hola\" + \"!\";\n");
  VM vm;
  vm.run(bytecode);

  const Value& s = requireGlobal(vm, "s");
  assert(s.isStr() && s.asStr() == "hola!");
}

void testGlobalAssignmentAndRead() {
  Bytecode bytecode = compileSource("var x = 7;\nx = x - 2;\n");
  VM vm;
  vm.run(bytecode);

  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 5);
}

void testCompoundAssignment() {
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
  Bytecode bytecode = compileSource("let a = nil;\nlet b = true;\nlet c = false;\nvar d;\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "a").isNil());
  assert(requireGlobal(vm, "b").isBool() && requireGlobal(vm, "b").asBool());
  assert(requireGlobal(vm, "c").isBool() && !requireGlobal(vm, "c").asBool());
  assert(requireGlobal(vm, "d").isNil());
}

void testUnaryOperators() {
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
  std::string output = runAndCapture("print(1 + 2);\nprint(\"hola\");\nprint(true);\n");
  assert(output == "3\nhola\ntrue\n");
}

void testDivisionByZero() {
  assert(runtimeError("let x = 1 / 0;\n") == "division by zero");
}

void testModuloByZero() {
  assert(runtimeError("let x = 1 % 0;\n") == "division by zero");
}

void testUndefinedGlobal() {
  assert(runtimeError("let y = missing + 1;\n").find("undefined variable") !=
         std::string::npos);
}

void testInvalidComparison() {
  assert(runtimeError("let x = 1 < \"a\";\n").find("cannot compare") != std::string::npos);
}

void testGlobalsPersistBetweenRuns() {
  VM vm;
  vm.run(compileSource("var x = 5;\n"));
  vm.run(compileSource("x = x + 1;\n"));

  const Value& x = requireGlobal(vm, "x");
  assert(x.isInt() && x.asInt() == 6);
}

void testRegisterCount() {
  Bytecode bytecode = compileSource("let x = 1 + 2 * 3;\n");
  VM vm;
  vm.run(bytecode);
  assert(vm.registerCount() == 3);
}

// --------------------------------------------------------- PHASE 5: control

void testIfElseExecution() {
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
  std::string output = runAndCapture(
      "if (false) { print(\"then\"); } else { print(\"else\"); }\n"
      "if (true) { print(\"yes\"); }\n");
  assert(output == "else\nyes\n");
}

void testTruthinessInConditions() {
  // Only nil and false are falsy; 0 and "" are truthy.
  std::string output = runAndCapture(
      "if (0) { print(\"zero\"); }\n"
      "if (\"\") { print(\"empty\"); }\n"
      "if (nil) { print(\"nil\"); }\n"
      "if (false) { print(\"false\"); }\n");
  assert(output == "zero\nempty\n");
}

void testWhileLoop() {
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
  Bytecode bytecode = compileSource(
      "var n = 0;\n"
      "for (;;) { n += 1; if (n >= 3) { break; } }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "n").asInt() == 3);
}

void testBreakOutOfWhile() {
  Bytecode bytecode = compileSource(
      "var n = 0;\n"
      "while (true) { n += 1; if (n == 3) { break; } }\n");
  VM vm;
  vm.run(bytecode);

  assert(requireGlobal(vm, "n").asInt() == 3);
}

void testContinueInWhile() {
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
  Bytecode bytecode = compileSource("{ let inner = 42; print(inner); }\n");
  VM vm;
  CoutCapture capture;
  vm.run(bytecode);

  assert(capture.stream.str() == "42\n");
  assert(vm.findGlobal("inner") == nullptr);
}

void testLocalCompoundAssignment() {
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
  Bytecode bytecode = compileSource(
      "{\n"
      "  let a = 1;\n"
      "  { let b = 2; print(b); }\n"
      "  print(a);\n"
      "}\n");
  VM vm;
  vm.run(bytecode);

  // Slots are reused when each scope closes.
  assert(vm.localCount() == 2);
}

void testBitwiseExecution() {
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
  assert(runtimeError("let x = 1 << 64;\n") == "shift count must be between 0 and 63");
  assert(runtimeError("let x = 1 >> -1;\n") == "shift count must be between 0 and 63");
  assert(runtimeError("let x = 1.5 & 1;\n").find("unsupported operand types") !=
         std::string::npos);
  assert(runtimeError("let x = ~1.5;\n").find("unsupported unary operator") !=
         std::string::npos);
}

void testLogicalShortCircuit() {
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
  assert(runtimeError("let x = true && (1 / 0);\n") == "division by zero");
  assert(runtimeError("let x = false || (1 / 0);\n") == "division by zero");
  assert(runtimeError("let x = true ? (1 / 0) : 2;\n") == "division by zero");
}

// ------------------------------------------------- PHASE 6: shared Runtime

void testVmCallsNatives() {
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

void testVmRejectsUserFunctionCallsForNow() {
  // Until CALL/RETURN land (slice 6a), the VM must refuse to enter a Jolt
  // function instead of silently running the AST.
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
  testVmRejectsUserFunctionCallsForNow();

  std::puts("vm tests passed");
  return 0;
}
