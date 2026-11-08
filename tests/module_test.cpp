// Tests for the module resolver and the import compiler (AGENT.md §18).
// Uses no external framework: only a failure counter and printf.

// Release builds define NDEBUG, which would compile every assert() out and
// leave the suite green without testing anything.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cstdio>
#include <string>

#include "compiler.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "util.h"
#include "vm.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

StmtPtr parseSource(const std::string& source, const std::string& name = "<test>") {
  return Parser(lex(source), name).parseProgram();
}

// A loader backed by `resolver`, identical in behaviour to the CLI's loader.
Compiler::ImportLoader loaderFor(const ModuleResolver& resolver) {
  return [&resolver](const std::string& path, int line, int col) -> StmtPtr {
    std::string file;
    std::string message;
    if (!resolver.resolve(path, file, message)) throw CompileError{message, line, col};
    std::string text;
    if (!readSourceFile(file, text)) {
      throw CompileError{"cannot read module '" + path + "'", line, col};
    }
    return parseSource(text, file);
  };
}

// Compiles `source` with `resolver` and runs it on `vm`. Tests assign the
// expression under test to a global and read it back with `findGlobal`.
void compileAndRun(const std::string& source, const ModuleResolver& resolver, VM& vm) {
  Compiler compiler;
  compiler.setImportLoader(loaderFor(resolver));
  Bytecode bytecode = compiler.compile(parseSource(source));
  vm.run(bytecode);
}

// Reads an integer global, or a sentinel when it is missing or not an int.
long globalInt(VM& vm, const std::string& name) {
  const Value* value = vm.findGlobal(name);
  if (value == nullptr || !value->isInt()) return -1;
  return value->asInt();
}

ModuleResolver fixtureResolver() { return ModuleResolver("tests", "std", ""); }

void testPathToFile() {
  check(modulePathToFile("math") == "math.jolt", "single segment -> math.jolt");
  check(modulePathToFile("std.math") == "std/math.jolt", "std.math -> std/math.jolt");
  check(modulePathToFile("myapp.utils.helpers") == "myapp/utils/helpers.jolt",
        "dotted path -> nested directories");
}

void testReservedPrefix() {
  check(ModuleResolver::isReservedStd("std"), "'std' is reserved");
  check(ModuleResolver::isReservedStd("std.math"), "'std.math' is reserved");
  check(!ModuleResolver::isReservedStd("stdlib"), "'stdlib' is not reserved");
  check(!ModuleResolver::isReservedStd("myapp.std"), "'myapp.std' is not reserved");
}

void testNotFoundMessage() {
  ModuleResolver resolver("does-not-exist", "does-not-exist", "");
  std::string file;
  std::string message;
  check(!resolver.resolve("nope.missing", file, message), "unresolved path returns false");
  check(message == "module 'nope.missing' not found", "exact not-found message");
}

void testStdResolution() {
  ModuleResolver resolver(".", "std", "");
  std::string file;
  std::string message;
  // The repository ships std/math.jolt.
  bool found = resolver.resolve("std.math", file, message);
  check(found, "std.math resolves against the stdlib directory");
  check(file == "std/math.jolt", "std.math resolves to std/math.jolt");
}

void testPackageResolution() {
  // A module that exists only under the packages root still resolves: the
  // project directory is searched first, then packages (AGENT.md §18).
  ModuleResolver resolver("tests", "std", "tests/packages");
  std::string file;
  std::string message;
  check(resolver.resolve("pkglib.tool", file, message), "a package-only module resolves");
  check(file == "tests/packages/pkglib/tool.jolt",
        "a package-only module resolves to the packages root");

  // When the same path exists in both roots, the project directory wins.
  ModuleResolver shadowed("tests", "std", "tests/packages");
  std::string shadowFile;
  std::string shadowMessage;
  check(shadowed.resolve("myapp.utils", shadowFile, shadowMessage),
        "the shadowed module still resolves");
  check(shadowFile == "tests/myapp/utils.jolt",
        "the project directory shadows the packages root");
}

void testPackageModuleCompilesAndRuns() {
  ModuleResolver resolver("tests", "std", "tests/packages");
  VM vm;
  compileAndRun(
      "import pkglib.tool.*;\n"
      "let got = tool();\n",
      resolver, vm);
  const Value* got = vm.findGlobal("got");
  check(got != nullptr && got->isStr() && got->asStr() == "package only",
        "a module loaded from the packages root runs");
}

void testQualifiedAccess() {
  VM vm;
  compileAndRun("import modules.util;\nlet got = modules.util.square(5);\n", fixtureResolver(), vm);
  check(globalInt(vm, "got") == 25, "qualified import: util.square(5) == 25");
}

void testAlias() {
  VM vm;
  compileAndRun("import modules.util as u;\nlet got = u.cube(3);\n", fixtureResolver(), vm);
  check(globalInt(vm, "got") == 27, "alias import: u.cube(3) == 27");
}

void testWildcard() {
  VM vm;
  compileAndRun("import modules.util.*;\nlet got = square(6);\n", fixtureResolver(), vm);
  check(globalInt(vm, "got") == 36, "wildcard import: square(6) == 36");
}

void testSelective() {
  VM vm;
  compileAndRun("import { square, cube } from modules.util;\nlet got = cube(2) + square(3);\n",
                fixtureResolver(), vm);
  check(globalInt(vm, "got") == 17, "selective import binds only the listed names");
}

void testPrivateNameIsNotExported() {
  Compiler compiler;
  ModuleResolver resolver = fixtureResolver();
  compiler.setImportLoader(loaderFor(resolver));
  bool threw = false;
  try {
    // `_hidden` starts with `_`, so it is private and absent from the exports.
    compiler.compile(parseSource("import { _hidden } from modules.util;\n"));
  } catch (const CompileError&) {
    threw = true;
  }
  check(threw, "a private name (leading underscore) is not exported");
}

void testAmbiguityIsCompileError() {
  ModuleResolver resolver("tests/errors", "std", "");
  Compiler compiler;
  compiler.setImportLoader(loaderFor(resolver));
  bool threw = false;
  std::string message;
  try {
    compiler.compile(parseSource("import mods.a.*;\nimport mods.b.*;\n"));
  } catch (const CompileError& error) {
    threw = true;
    message = error.message;
  }
  check(threw, "two wildcards exporting the same name is a compile error");
  check(message.find("is ambiguous between") != std::string::npos,
        "ambiguity error uses the specified wording");
  check(message.find("mods.a") != std::string::npos &&
            message.find("mods.b") != std::string::npos,
        "ambiguity error names both modules");
}

void testModuleRunsOnce() {
  // `modules/counter.jolt` increments the caller's `runs` global at load time.
  // Three imports must run it once, so `runs` ends at 1.
  VM vm;
  compileAndRun(
      "let runs = 0;\n"
      "import modules.counter;\n"
      "import modules.counter as c;\n"
      "import modules.counter.*;\n",
      fixtureResolver(), vm);
  check(globalInt(vm, "runs") == 1,
        "a module body runs exactly once however many times it is imported");
}

void testNestedImport() {
  VM vm;
  compileAndRun("import modules.nested;\nlet got = modules.nested.sumOfSquares(3, 4);\n",
                fixtureResolver(), vm);
  check(globalInt(vm, "got") == 25, "a module can import another module (nested resolution)");
}

void testRepeatedImportBindsOnce() {
  // Importing the same module twice must not fail or rebind conflicting names.
  bool threw = false;
  try {
    VM vm;
    compileAndRun("import modules.util;\nimport modules.util;\nlet got = modules.util.answer;\n",
                  fixtureResolver(), vm);
  } catch (const CompileError&) {
    threw = true;
  }
  check(!threw, "importing the same module twice is not an error");
}

void testNestedNamespaceQualifiedAccess() {
  // A three-segment dotted import builds a real namespace tree: `ns.deep.leaf`
  // must reach the module through `ns` -> `deep`, not through a `ns.deep`
  // global (which does not exist). Regression for the namespace walk that
  // dropped the intermediate table.
  VM vm;
  compileAndRun("import ns.deep.leaf;\nlet got = ns.deep.leaf.leaf();\n",
                fixtureResolver(), vm);
  check(globalInt(vm, "got") == 123,
        "a 3-segment qualified import resolves through the nested namespace");

  // A second module under the same nested namespace reuses the tables already
  // created instead of replacing them.
  VM vm2;
  compileAndRun(
      "import ns.deep.leaf;\nimport ns.deep.other;\n"
      "let got = ns.deep.leaf.leaf() + ns.deep.other.other();\n",
      fixtureResolver(), vm2);
  check(globalInt(vm2, "got") == 130,
        "two modules under the same nested namespace share the namespace tables");
}

void testLocalBindingWinsOverWildcard() {
  // A name the importing module declares itself must win over a wildcard
  // import, whatever the order. The import must not silently shadow a local
  // `fn` or `var` (AGENT.md §18: no silent shadowing).
  {
    VM vm;
    compileAndRun(
        "import modules.util.*;\n"
        "fn square(x) { return 3; }\n"
        "let got = square(4);\n",
        fixtureResolver(), vm);
    check(globalInt(vm, "got") == 3, "a local fn declared after a wildcard wins");
  }
  {
    VM vm;
    compileAndRun(
        "fn square(x) { return 3; }\n"
        "import modules.util.*;\n"
        "let got = square(4);\n",
        fixtureResolver(), vm);
    check(globalInt(vm, "got") == 3, "a local fn declared before a wildcard wins");
  }
  {
    VM vm;
    compileAndRun(
        "import modules.util.*;\n"
        "var answer = 99;\n"
        "let got = answer;\n",
        fixtureResolver(), vm);
    check(globalInt(vm, "got") == 99, "a local var declared after a wildcard wins");
  }
  {
    // An alias must respect a local binding too, not rebind it to the module.
    VM vm;
    compileAndRun(
        "fn util(x) { return 3; }\n"
        "import modules.util as util;\n"
        "let got = util(4);\n",
        fixtureResolver(), vm);
    check(globalInt(vm, "got") == 3, "a local fn wins over an alias of the same name");
  }
}

// A module's own imports must stay inside the module: the importing program
// must not see the namespace, alias or wildcard binding a nested module created
// (AGENT.md §18.2: "importing a module never injects its internals into the
// importing program's globals").
void testModuleImportDoesNotLeakNamespace() {
  // `qualuser` imports `modules.helper`; the importer only imports `qualuser`,
  // so `modules.helper` must not be reachable from the importer.
  bool threw = false;
  try {
    VM vm;
    compileAndRun("import modules.qualuser;\nlet got = modules.helper.helper();\n",
                  fixtureResolver(), vm);
  } catch (const RuntimeError&) {
    threw = true;
  }
  check(threw, "a module's qualified import does not leak its namespace to the importer");
}

void testModuleImportDoesNotLeakWildcard() {
  // `wilduser` wildcard-imports `modules.helper`; `helper` must not become a
  // global of the importing program.
  bool threw = false;
  try {
    VM vm;
    compileAndRun("import modules.wilduser;\nlet got = helper();\n", fixtureResolver(), vm);
  } catch (const RuntimeError&) {
    threw = true;
  }
  check(threw, "a module's wildcard import does not leak its names to the importer");
}

void testModuleSeesItsOwnImports() {
  // The module's own functions still resolve the imports it made: a wildcard
  // name and a qualified namespace are visible inside the module.
  {
    VM vm;
    compileAndRun("import modules.wilduser;\nlet got = modules.wilduser.viaWild();\n",
                  fixtureResolver(), vm);
    check(globalInt(vm, "got") == 100, "a module function sees its own wildcard import");
  }
  {
    VM vm;
    compileAndRun("import modules.qualuser;\nlet got = modules.qualuser.viaQual();\n",
                  fixtureResolver(), vm);
    check(globalInt(vm, "got") == 100, "a module function sees its own qualified import");
  }
}

void testNestedWildcardDoesNotCreatePhantomAmbiguity() {
  // `wilduser` exports `helper` via a wildcard of `modules.helper`. Importing
  // `modules.helper2.*` in the program introduces the same visible name, but
  // only one of them is a real wildcard binding of the program, so there is no
  // ambiguity: the module's internal binding is not the importer's business.
  bool threw = false;
  try {
    VM vm;
    compileAndRun(
        "import modules.wilduser;\n"
        "import modules.helper2.*;\n"
        "let got = helper();\n",
        fixtureResolver(), vm);
    check(globalInt(vm, "got") == 200,
          "a module's wildcard import does not create a phantom ambiguity in the importer");
  } catch (const CompileError&) {
    threw = true;
  }
  check(!threw, "no phantom ambiguity from a module's internal wildcard import");
}

}  // namespace

int main() {
  testPathToFile();
  testReservedPrefix();
  testNotFoundMessage();
  testStdResolution();
  testPackageResolution();
  testPackageModuleCompilesAndRuns();
  testQualifiedAccess();
  testAlias();
  testWildcard();
  testSelective();
  testPrivateNameIsNotExported();
  testAmbiguityIsCompileError();
  testModuleRunsOnce();
  testNestedImport();
  testRepeatedImportBindsOnce();
  testNestedNamespaceQualifiedAccess();
  testLocalBindingWinsOverWildcard();
  testModuleImportDoesNotLeakNamespace();
  testModuleImportDoesNotLeakWildcard();
  testModuleSeesItsOwnImports();
  testNestedWildcardDoesNotCreatePhantomAmbiguity();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all module checks passed\n");
  return 0;
}
