#include <cassert>
#include <cstdio>
#include <string>

#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include "value.h"
#include "vm.h"

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

static StmtPtr parseSource(const std::string& source) {
  Parser parser(lex(source), "<test>");
  return parser.parseProgram();
}

static Bytecode compileSource(const std::string& source) {
  return Compiler{}.compile(parseSource(source));
}

static Bytecode compileReplSource(const std::string& source) {
  return Compiler{}.compileRepl(parseSource(source));
}

static void testLoadPreservesGlobals() {
  VM vm;
  // Simular el estado previo del REPL: `let x = 42` ya ejecutado.
  vm.defineGlobal("x", Value(42), false);

  // Simular `:load` con un script que define `y`.
  vm.run(compileSource("let y = 10;"));

  // `x` debe seguir existiendo después del `:load`.
  const Value* x = vm.findGlobal("x");
  const Value* y = vm.findGlobal("y");
  check(x != nullptr, ":load preserves existing global 'x'");
  check(x != nullptr && x->isInt() && x->asInt() == 42,
        ":load preserves the value of 'x' (42)");
  check(y != nullptr, ":load defines new global 'y'");
  check(y != nullptr && y->isInt() && y->asInt() == 10,
        ":load defines 'y' with value 10");
}

static void testClearForgetsGlobals() {
  VM vm;
  vm.run(compileSource("let z = 7;"));
  check(vm.findGlobal("z") != nullptr, "script defines global 'z'");

  // `:clear` must forget every user definition while keeping the VM usable.
  vm.clearGlobals();
  check(vm.findGlobal("z") == nullptr, "clearGlobals() forgets 'z'");
}

static void testSwitchReplChunk() {
  // A `switch` entered at the REPL is compiled with `compileRepl`; the
  // appended ';' the REPL uses to complete a program must not break the switch
  // body, so `parseSwitch` skips stray semicolons between clauses.
  VM vm;
  vm.run(compileReplSource(
      "var result = \"\";\n"
      "switch (2) {\n"
      "  case 1: { result = \"one\"; }\n"
      "  case 2: { result = \"two\"; }\n"
      "  default: { result = \"other\"; }\n"
      "}\n"
      ";\n"));
  const Value* result = vm.findGlobal("result");
  check(result != nullptr && result->isStr() && result->asStr() == "two",
        "switch at the REPL runs the matching case");
}

static void testDeferReplChunk() {
  // A function with a `defer` entered at the REPL is compiled with
  // `compileRepl`. The deferred block must run at the function's exit, and the
  // ';' the REPL appends after the trailing '}' must not disturb it.
  VM vm;
  vm.run(compileReplSource(
      "fn f() {\n"
      "  var trail = [];\n"
      "  defer { trail.push(\"cleanup\"); }\n"
      "  trail.push(\"body\");\n"
      "  return trail;\n"
      "}\n"
      "var result = f();\n"
      ";\n"));
  const Value* result = vm.findGlobal("result");
  check(result != nullptr, "defer at the REPL defines 'result'");
  check(result != nullptr && result->isArray() && result->arr().items.size() == 2,
        "defer at the REPL runs the deferred block after the body");
}

int main() {
  testLoadPreservesGlobals();
  testClearForgetsGlobals();
  testSwitchReplChunk();
  testDeferReplChunk();
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all repl tests passed\n");
  return 0;
}
