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

// Simulates a REPL session: one compiler and one VM, several entries. Each
// entry is parsed and compiled with the same compiler, so the session state
// (immutability, types, structs, imports) carries from one entry to the next,
// exactly as in the real REPL. Returns the compiler so a test can inspect or
// clear the session.
struct ReplSession {
  Compiler compiler;
  VM vm;

  // Parses and compiles one entry, then runs it. A compile error propagates.
  Value entry(const std::string& source) {
    Parser parser(lex(source), "<repl>");
    // The REPL hands the session's struct names to each entry's parser.
    parser.seedStructNames(compiler.structNames());
    Bytecode bytecode = compiler.compileRepl(parser.parseProgram());
    return vm.run(bytecode);
  }

  long globalInt(const std::string& name) const {
    const Value* value = vm.findGlobal(name);
    return value != nullptr && value->isInt() ? value->asInt() : -1;
  }

  // Reads an integer field of a map global (`Point { x: 3 }` lowers to a map).
  long fieldInt(const std::string& object, const std::string& field) const {
    const Value* value = vm.findGlobal(object);
    if (value == nullptr || !value->isMap()) return -1;
    for (const auto& entry : value->map().entries) {
      if (entry.first == field && entry.second.isInt()) return entry.second.asInt();
    }
    return -1;
  }
};

static void testSessionImmutabilityPersists() {
  // `let x` in one entry makes `x` immutable for every later entry: the REPL
  // behaves like one accumulating program, not a sequence of unrelated ones.
  ReplSession session;
  session.entry("let x = 1;");
  bool threw = false;
  try {
    session.entry("x = 2;");
  } catch (const CompileError&) {
    threw = true;
  }
  check(threw, "a `let` from an earlier entry is immutable in a later entry");
  check(session.globalInt("x") == 1, "the rejected assignment left `x` at 1");
}

static void testSessionTypePersists() {
  // A `var x: int` from an earlier entry keeps its type: assigning a string in
  // a later entry is a compile error, as it would be in a single file.
  ReplSession session;
  session.entry("var x: int = 1;");
  bool threw = false;
  try {
    session.entry("x = \"hola\";");
  } catch (const CompileError&) {
    threw = true;
  }
  check(threw, "a type annotation from an earlier entry is enforced later");
}

static void testSessionRedefinitionStillWorks() {
  // Redefining is how a REPL is used, so it stays allowed: the new binding
  // replaces the old one.
  ReplSession session;
  session.entry("let x = 1;");
  session.entry("let x = 2;");
  check(session.globalInt("x") == 2, "redefining a `let` in a later entry wins");
}

static void testSessionStructPersists() {
  // A `struct` declared in one entry must still be a usable literal and
  // annotation in a later entry, like in a single file.
  ReplSession session;
  session.entry("struct Point { x: int; y: int; }");
  session.entry("let p = Point { x: 3, y: 4 };");
  check(session.fieldInt("p", "x") == 3,
        "a struct literal from an earlier entry resolves later");
  session.entry("let q: Point = Point { x: 7, y: 8 };");
  check(session.fieldInt("q", "y") == 8,
        "a struct annotation from an earlier entry resolves later");
}

static void testFailedEntryDoesNotPoisonSession() {
  // A definition is committed only when its entry compiles. An entry that
  // imports a module and then fails must not leave that import marked as bound,
  // or the next entry would reference a module whose body never ran.
  ReplSession session;
  session.compiler.setImportLoader([](const std::string& path, int line, int col) -> StmtPtr {
    if (path == "sess.mod") {
      return Parser(lex("fn answer() { return 42; }\n"), "<mod>").parseProgram();
    }
    throw CompileError{"module '" + path + "' not found", line, col};
  });
  bool threw = false;
  try {
    session.entry("import sess.mod; let bad: int = \"hola\";");
  } catch (const CompileError&) {
    threw = true;
  }
  check(threw, "the failing entry is reported");
  // The failed entry left nothing behind: importing again works and the module
  // is reachable.
  session.entry("import sess.mod;");
  session.entry("let got = sess.mod.answer();");
  check(session.globalInt("got") == 42,
        "an import from a failed entry can be imported again cleanly");
}

static void testSessionIsPerCompiler() {
  // The session state belongs to one compiler. A fresh one starts clean, which
  // is what makes a file (and `:load`) independent of the REPL.
  ReplSession session;
  session.entry("let x = 1;");
  Compiler fresh;
  bool threw = false;
  try {
    Parser parser(lex("x = 2;"), "<fresh>");
    fresh.compileRepl(parser.parseProgram());
  } catch (const CompileError&) {
    threw = true;
  }
  check(!threw, "a fresh compiler does not inherit another session's bindings");
}

static void testClearSessionForgets() {
  // `:clear` drops the session state with the globals: the name is unknown
  // again, so it can be rebound freely.
  ReplSession session;
  session.entry("let x = 1;");
  session.compiler.clearSession();
  session.vm.clearGlobals();
  bool threw = false;
  try {
    session.entry("let x = 2;");
  } catch (const CompileError&) {
    threw = true;
  }
  check(!threw, "after clearSession a name can be redeclared as a mutable binding");
  check(session.globalInt("x") == 2, "the new binding after clearSession takes effect");
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
  testSessionImmutabilityPersists();
  testSessionTypePersists();
  testSessionRedefinitionStillWorks();
  testSessionStructPersists();
  testFailedEntryDoesNotPoisonSession();
  testSessionIsPerCompiler();
  testClearSessionForgets();
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all repl tests passed\n");
  return 0;
}
