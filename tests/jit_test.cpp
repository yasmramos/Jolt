// Tests for the development-engine JIT (slice 1: integer leaf protos).
//
// The JIT must be *observationally identical* to the interpreter for every
// proto it accepts, so every test here is differential: the same function is
// run through the bytecode VM and through the compiled native code, and the two
// results must match. The bytecode contract is untouched by the JIT (no new
// opcode, no superinstruction), so a mismatch is a JIT bug, never a language
// change.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "bytecode.h"
#include "compiler.h"
#include "jit/jit.h"
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

static Bytecode compileSource(const std::string& source) {
  Parser parser(lex(source), "<jit-test>");
  return Compiler{}.compile(parser.parseProgram());
}

// Runs `source` on the VM and returns the value of the global `r`.
static Value runOnVm(const std::string& source) {
  Bytecode bytecode = compileSource(source);
  VM vm;
  vm.run(bytecode);
  const Value* result = vm.findGlobal("r");
  return result ? *result : Value();
}

// Finds the child proto named `name` in `bc` (the functions a program defines).
static std::shared_ptr<const Bytecode> findProto(const Bytecode& bc, const std::string& name) {
  for (const std::shared_ptr<Bytecode>& child : bc.children) {
    if (child && child->name == name) return child;
  }
  return nullptr;
}

// Compiles the function `name` defined by `source` and runs it on the JIT with
// `args`. Returns false when the JIT did not accept the proto.
static bool runOnJit(const std::string& source, const std::string& name,
                     const std::vector<Value>& args, Value& out) {
  Bytecode bc = compileSource(source);
  std::shared_ptr<const Bytecode> proto = findProto(bc, name);
  if (!proto) return false;
  std::unique_ptr<JitFunction> fn = jitCompile(*proto);
  if (!fn) return false;
  out = fn->call(args);
  return true;
}

// The heart of the suite: run `fnName(args)` on the VM and on the JIT and
// require the same value.
static void differential(const std::string& source, const std::string& fnName,
                         const std::vector<Value>& args, const char* what) {
  const std::string call = fnName + "(";
  std::string argList;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i) argList += ", ";
    argList += std::to_string(args[i].asInt());
  }
  argList += ")";

  Value expected = runOnVm("var r = " + call + argList + ";\n" + source);
  Value actual;
  const bool compiled = runOnJit(source, fnName, args, actual);
  const bool ok = compiled && actual.equals(expected);
  check(ok, what);
  if (!ok) {
    std::printf("      compiled=%d expected=%s actual=%s\n", compiled ? 1 : 0,
                expected.toString().c_str(), actual.toString().c_str());
  }
}

// A proto the JIT must reject: it returns nullptr so the VM keeps interpreting.
static void expectRejected(const std::string& source, const std::string& fnName, const char* what) {
  Bytecode bc = compileSource(source);
  std::shared_ptr<const Bytecode> proto = findProto(bc, fnName);
  if (!proto) {
    check(false, std::string(what) + " (proto not found)");
    return;
  }
  check(jitCompile(*proto) == nullptr, what);
}

// A tiny deterministic PRNG so the fuzz corpus is reproducible.
struct Rng {
  std::uint64_t state = 0x9E3779B97F4A7C15ULL;
  std::uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

// Builds a random integer-only expression. Every operator here yields an int,
// so the expression never leaves the JIT's subset. `depth` bounds the size.
static std::string randomExpr(Rng& rng, int depth) {
  if (depth <= 0) {
    switch (rng.below(4)) {
      case 0: return "a";
      case 1: return "b";
      case 2: return "c";
      default: return std::to_string(rng.below(21) - 10);
    }
  }
  const int choice = rng.below(10);
  if (choice == 0) return "(-" + randomExpr(rng, depth - 1) + ")";
  if (choice == 1) return "(~" + randomExpr(rng, depth - 1) + ")";
  static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
  const char* op = ops[rng.below(6)];
  return "(" + randomExpr(rng, depth - 1) + " " + op + " " + randomExpr(rng, depth - 1) + ")";
}

// A random comparison, used only as a branch condition.
static std::string randomCond(Rng& rng, int depth) {
  static const char* cmps[] = {"<", "<=", ">", ">=", "==", "!="};
  const char* cmp = cmps[rng.below(6)];
  return randomExpr(rng, depth) + " " + cmp + " " + randomExpr(rng, depth);
}

// Generates a whole leaf function that is guaranteed to terminate and to stay
// inside the integer subset, then checks the JIT against the VM over several
// argument tuples. This is the differential fuzz: a mismatch is a JIT bug.
static void fuzzDifferential(Rng& rng, int rounds) {
  int mismatches = 0;
  int compiled = 0;
  for (int r = 0; r < rounds; ++r) {
    std::string body =
        "fn f(a, b, c) {\n"
        "  var t = " + randomExpr(rng, 3) + ";\n"
        "  if (" + randomCond(rng, 2) + ") { t = t + (" + randomExpr(rng, 2) + "); }\n"
        "  else { t = t - (" + randomExpr(rng, 2) + "); }\n"
        "  var i = 0;\n"
        "  while (i < " + std::to_string(rng.below(6)) + ") { t = t + (i * " +
        std::to_string(rng.below(5) - 2) + "); i = i + 1; }\n"
        "  if (" + randomCond(rng, 1) + ") { return -t; }\n"
        "  return t;\n"
        "}\n";
    const std::vector<Value> args = {
        Value(static_cast<int64_t>(rng.below(41) - 20)),
        Value(static_cast<int64_t>(rng.below(41) - 20)),
        Value(static_cast<int64_t>(rng.below(41) - 20))};
    Value expected = runOnVm("var r = f(" + std::to_string(args[0].asInt()) + ", " +
                             std::to_string(args[1].asInt()) + ", " +
                             std::to_string(args[2].asInt()) + ");\n" + body);
    Value actual;
    const bool ok = runOnJit(body, "f", args, actual);
    if (ok) ++compiled;
    if (!ok || !actual.equals(expected)) {
      ++mismatches;
      if (mismatches <= 3) {
        std::printf("      fuzz mismatch\n%s      expected=%s actual=%s\n", body.c_str(),
                    expected.toString().c_str(), actual.toString().c_str());
      }
    }
  }
  check(mismatches == 0, "random integer leaf functions match the VM");
  check(compiled == rounds, "every random integer leaf function was compiled");
}

int main() {
  // --- Arithmetic and locals -------------------------------------------------
  const char* arith =
      "fn add(a, b) { return a + b; }\n"
      "fn sub(a, b) { return a - b; }\n"
      "fn mul(a, b) { return a * b; }\n"
      "fn mix(a, b, c) { return a * b - c; }\n"
      "fn negate(a) { return -a; }\n"
      "fn bits(a, b) { return (a & b) | (a ^ b); }\n";

  differential(arith, "add", {Value(2), Value(3)}, "add(2,3) matches the VM");
  differential(arith, "sub", {Value(10), Value(40)}, "sub(10,40) matches the VM");
  differential(arith, "mul", {Value(7), Value(6)}, "mul(7,6) matches the VM");
  differential(arith, "mix", {Value(4), Value(5), Value(3)}, "mix(4,5,3) matches the VM");
  differential(arith, "negate", {Value(9)}, "negate(9) matches the VM");
  differential(arith, "bits", {Value(12), Value(10)}, "bits(12,10) matches the VM");

  // --- Comparisons, booleans and truthiness ---------------------------------
  const char* cmp =
      "fn lt(a, b) { return a < b; }\n"
      "fn le(a, b) { return a <= b; }\n"
      "fn gt(a, b) { return a > b; }\n"
      "fn ge(a, b) { return a >= b; }\n"
      "fn eq(a, b) { return a == b; }\n"
      "fn ne(a, b) { return a != b; }\n"
      "fn notx(a) { return !a; }\n"
      "fn notBool(a) { if (a) { return !true; } return !false; }\n";

  differential(cmp, "lt", {Value(3), Value(5)}, "a < b matches the VM");
  differential(cmp, "lt", {Value(5), Value(3)}, "a < b (false) matches the VM");
  differential(cmp, "le", {Value(5), Value(5)}, "a <= b matches the VM");
  differential(cmp, "gt", {Value(5), Value(3)}, "a > b matches the VM");
  differential(cmp, "ge", {Value(3), Value(5)}, "a >= b (false) matches the VM");
  differential(cmp, "eq", {Value(4), Value(4)}, "a == b matches the VM");
  differential(cmp, "ne", {Value(4), Value(5)}, "a != b matches the VM");
  differential(cmp, "notx", {Value(0)}, "!0 is false (0 is truthy) like the VM");
  differential(cmp, "notx", {Value(7)}, "!7 is false like the VM");
  differential(cmp, "notBool", {Value(1)}, "!true through a branch matches the VM");

  // --- Control flow and loops ------------------------------------------------
  const char* loops =
      "fn sumTo(n) { var s = 0; var i = 1; while (i <= n) { s = s + i; i = i + 1; } return s; }\n"
      "fn fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n";

  differential(loops, "sumTo", {Value(10)}, "sumTo(10) loop matches the VM");
  differential(loops, "sumTo", {Value(0)}, "sumTo(0) loop matches the VM");
  differential(loops, "sumTo", {Value(1000)}, "sumTo(1000) loop matches the VM");
  // `fib` recurses, so its proto has a CALL and must be rejected; the VM runs it.
  expectRejected(loops, "fib", "a proto with a recursive CALL is rejected");

  // --- Nested branches and mixed nil/bool/int --------------------------------
  const char* branches =
      "fn classify(a) { if (a < 0) { return 0 - 1; } if (a == 0) { return 0; } return 1; }\n"
      "fn abs(a) { if (a < 0) { return -a; } return a; }\n";

  differential(branches, "classify", {Value(-5)}, "classify(-5) matches the VM");
  differential(branches, "classify", {Value(0)}, "classify(0) matches the VM");
  differential(branches, "classify", {Value(9)}, "classify(9) matches the VM");
  differential(branches, "abs", {Value(-42)}, "abs(-42) matches the VM");
  differential(branches, "abs", {Value(42)}, "abs(42) matches the VM");

  // --- Wide/edge integer payloads -------------------------------------------
  differential(arith, "add",
               {Value(static_cast<int64_t>(900000000000LL)), Value(static_cast<int64_t>(700000000000LL))},
               "large ints add like the VM");
  differential(arith, "negate", {Value(-1)}, "negate(-1) matches the VM");

  // --- Rejections: everything outside the integer-leaf subset ----------------
  expectRejected("fn greet(n) { return \"hi\"; }\n", "greet", "a string return is rejected");
  expectRejected("fn useGlobal(n) { return n + 1.5; }\n", "useGlobal",
                 "a float literal is rejected");
  expectRejected("fn callIt(n) { return n; }\nfn g(n) { return callIt(n); }\n", "g",
                 "a proto that CALLs is rejected");
  expectRejected("fn mk(n) { return (x) => x; }\n", "mk",
                 "a proto with a child closure is rejected");

  // --- The cache compiles each proto once and stays correct ------------------
  {
    Bytecode bc = compileSource(arith);
    std::shared_ptr<const Bytecode> proto = findProto(bc, "add");
    Jit jit;
    JitFunction* first = jit.get(proto);
    JitFunction* second = jit.get(proto);
    check(first != nullptr && first == second, "Jit::get caches the compiled proto");
    check(jit.compiledCount() == 1, "the cache holds one entry after two gets");
    if (first) {
      Value out = first->call({Value(20), Value(22)});
      check(out.isInt() && out.asInt() == 42, "the cached function still computes");
    }
  }

  // --- The VM with the JIT enabled must agree with the VM without it ---------
  {
    const std::string program =
        "fn add(a, b) { return a + b; }\n"
        "fn work(n) { var s = 0; var i = 0; while (i < n) { s = s + (i * 2); i = i + 1; } return s; }\n"
        "var r = work(100) + add(2, 3);\n";

    auto runWith = [&](bool jit) {
      Bytecode bc = compileSource(program);
      VM vm;
      vm.setJitEnabled(jit);
      vm.run(bc);
      const Value* result = vm.findGlobal("r");
      return std::pair<Value, std::size_t>(result ? *result : Value(), vm.jitCompiledCount());
    };

    auto plain = runWith(false);
    auto jitted = runWith(true);
    check(plain.first.equals(jitted.first), "VM result is identical with the JIT on");
    check(plain.second == 0, "the JIT compiles nothing while disabled");
    check(jitted.second >= 1, "the JIT compiles an accepted proto while enabled");
  }

  // --- Differential fuzz over random integer leaf functions ------------------
  {
    Rng rng;
    fuzzDifferential(rng, 200);
  }

  if (g_failures != 0) {
    std::printf("\n%d JIT test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("\nall JIT tests passed\n");
  return 0;
}
