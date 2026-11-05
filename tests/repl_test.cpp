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

int main() {
  testLoadPreservesGlobals();
  testClearForgetsGlobals();
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all repl tests passed\n");
  return 0;
}
