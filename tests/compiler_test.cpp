// Tests for the AST -> bytecode compiler (PHASE 2-5).
// Uses no external framework: only assert and a failure counter.

// Release builds define NDEBUG, which would compile every assert() out and
// leave the suite green without testing anything.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "compiler.h"
#include "lexer.h"
#include "parser.h"

namespace {

Bytecode compileSource(const std::string& source) {
  auto tokens = lex(source);
  Parser parser(std::move(tokens), "<compiler-test>");
  auto program = parser.parseProgram();
  return compileProgram(program);
}

void expectCompileError(const std::string& source, const char* what) {
  try {
    compileSource(source);
  } catch (const CompileError&) {
    return;
  }
  std::fprintf(stderr, "expected CompileError for %s\n", what);
  std::exit(1);
}

void expectCompiles(const std::string& source) {
  try {
    compileSource(source);
  } catch (const CompileError& error) {
    std::fprintf(stderr, "unexpected CompileError for \"%s\": %s\n", source.c_str(),
                 error.message.c_str());
    std::exit(1);
  }
}

void testArithmeticAndPrint() {
  Bytecode bc = compileSource("let x = 1 + 2;\nprint(x);\n");

  // 0 LOADK 0, 1 LOADK 1, 2 ADD 0, 3 SETGLOBAL 0,
  // 4 GETGLOBAL "print", 5 GETGLOBAL "x", 6 CALL 0, 7 HALT
  assert(bc.code.size() == 8);

  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[0].a == 0 && bc.code[0].b == 0);
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[1].a == 1 && bc.code[1].b == 1);
  assert(bc.code[2].op == OpCode::ADD);
  assert(bc.code[2].a == 0 && bc.code[2].b == 0 && bc.code[2].c == 1);
  assert(bc.code[3].op == OpCode::SETGLOBAL);
  assert(bc.code[3].a == 0 && bc.code[3].b == 2);
  assert(bc.code[4].op == OpCode::GETGLOBAL);
  assert(bc.code[4].a == 0 && bc.code[4].b == 3);  // "print"
  assert(bc.code[5].op == OpCode::GETGLOBAL);
  assert(bc.code[5].a == 1 && bc.code[5].b == 2);  // "x"
  assert(bc.code[6].op == OpCode::CALL);
  assert(bc.code[6].a == 0 && bc.code[6].b == 1);
  assert(bc.code[7].op == OpCode::HALT);

  assert(bc.constants.size() == 4);
  assert(bc.constants[0].isInt() && bc.constants[0].asInt() == 1);
  assert(bc.constants[1].isInt() && bc.constants[1].asInt() == 2);
  assert(bc.constants[2].isStr() && bc.constants[2].asStr() == "x");
  assert(bc.constants[3].isStr() && bc.constants[3].asStr() == "print");
  assert(bc.numRegisters == 2);
}

void testNumericConstantTypes() {
  Bytecode bc = compileSource("let a = 1;\nlet b = 1.0;\n");

  // int and double must not be merged in the constant pool. Global names share
  // the pool, so the layout is 1, "a", 1.0, "b".
  assert(bc.constants.size() == 4);
  assert(bc.constants[0].isInt() && bc.constants[0].asInt() == 1);
  assert(bc.constants[2].isDouble() && bc.constants[2].asDouble() == 1.0);
}

void testLeftAssociativePrecedence() {
  Bytecode bc = compileSource("print(1 + 2 * 3);\n");
for (size_t i = 0; i < bc.code.size(); ++i) {
  const Instruction& ins = bc.code[i];
  std::printf("%zu: op=%d a=%d b=%d c=%d jump=%d\n",
              i, static_cast<int>(ins.op), ins.a, ins.b, ins.c, ins.jump);
}
  // GETGLOBAL "print", LOADK 1, LOADK 2, LOADK 3, MUL, ADD, CALL 0, 1, HALT
  assert(bc.code.size() == 8);
  assert(bc.code[0].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::MUL);
  assert(bc.code[5].op == OpCode::ADD);
  assert(bc.code[6].op == OpCode::CALL);
  assert(bc.code[6].a == 0 && bc.code[6].b == 1);
  assert(bc.code[7].op == OpCode::HALT);
  assert(bc.numRegisters == 4);
}

void testStringConstant() {
  Bytecode bc = compileSource("let greeting = \"hi\";\n");

  assert(bc.code.size() == 3);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[0].a == 0 && bc.code[0].b == 0);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[1].a == 0 && bc.code[1].b == 1);
  assert(bc.code[2].op == OpCode::HALT);

  // Strings and global names share the same tagged pool.
  assert(bc.constants.size() == 2);
  assert(bc.constants[0].isStr() && bc.constants[0].asStr() == "hi");
  assert(bc.constants[1].isStr() && bc.constants[1].asStr() == "greeting");
  assert(bc.numRegisters == 1);
}

void testNewlineNormalization() {
  // CRLF and lone CR are folded to LF before lexing, so string literals and
  // line numbers are identical on every platform.
  auto crlf = lex("\"a\r\nb\"");
  assert(crlf.size() == 2);
  assert(crlf[0].parts.size() == 1 && crlf[0].parts[0].text == "a\nb");

  auto cr = lex("\"a\rb\"");
  assert(cr[0].parts.size() == 1 && cr[0].parts[0].text == "a\nb");

  auto lines = lex("a\r\nb");
  assert(lines.size() == 3);
  assert(lines[0].line == 1 && lines[1].line == 2);
}

void testBoolAndNilLiterals() {
  Bytecode bc = compileSource("let a = true;\nlet b = false;\nlet c = nil;\n");

  assert(bc.code.size() == 7);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::LOADK);
  assert(bc.code[6].op == OpCode::HALT);

  // All literals share the tagged pool with global names, in source order:
  // true, "a", false, "b", nil, "c".
  assert(bc.constants.size() == 6);
  assert(bc.constants[0].isBool() && bc.constants[0].asBool());
  assert(bc.constants[2].isBool() && !bc.constants[2].asBool());
  assert(bc.constants[4].isNil());
}

void testVarWithoutInitializer() {
  Bytecode bc = compileSource("var x;\n");

  // `var x;` must load an explicit nil constant, not leave the register
  // undefined.
  assert(bc.code.size() == 3);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[0].a == 0);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[2].op == OpCode::HALT);
  assert(bc.constants.size() == 2 && bc.constants[0].isNil());
}

void testModuloAndPower() {
  Bytecode bc = compileSource("let a = 5 % 2;\nlet b = 2 ** 8;\n");

  // Each `let` uses LOADK, LOADK, operator and SETGLOBAL.
  assert(bc.code.size() == 9);
  assert(bc.code[2].op == OpCode::MOD);
  assert(bc.code[6].op == OpCode::POW);
}

void testComparisons() {
  Bytecode bc = compileSource(
      "let a = 1 == 2;\n"
      "let b = 1 != 2;\n"
      "let c = 1 < 2;\n"
      "let d = 1 <= 2;\n"
      "let e = 1 > 2;\n"
      "let f = 1 >= 2;\n");

  // Each comparison takes 4 instructions: LOADK, LOADK, op, SETGLOBAL.
  assert(bc.code[2].op == OpCode::EQ);
  assert(bc.code[6].op == OpCode::NEQ);
  assert(bc.code[10].op == OpCode::LT);
  assert(bc.code[14].op == OpCode::LTE);
  assert(bc.code[18].op == OpCode::GT);
  assert(bc.code[22].op == OpCode::GTE);
}

void testUnaryOperators() {
  Bytecode neg = compileSource("let y = -5;\n");
  assert(neg.code.size() == 4);
  assert(neg.code[0].op == OpCode::LOADK);
  assert(neg.code[1].op == OpCode::UNOP);
  assert(neg.code[1].a == 0 && neg.code[1].b == 0);
  assert(neg.code[1].c == static_cast<int>(UnaryOp::Negate));

  Bytecode notBc = compileSource("let y = !1;\n");
  assert(notBc.code.size() == 4);
  assert(notBc.code[1].op == OpCode::UNOP);
  assert(notBc.code[1].c == static_cast<int>(UnaryOp::Not));

  // `+x` emits no instruction.
  Bytecode plus = compileSource("let y = +5;\n");
  assert(plus.code.size() == 3);
  assert(plus.code[0].op == OpCode::LOADK);
  assert(plus.code[1].op == OpCode::SETGLOBAL);
}

void testCompoundAssignment() {
  Bytecode bc = compileSource("var x = 1;\nx += 2;\n");

  // LOADK, SETGLOBAL, GETGLOBAL, LOADK, ADD, SETGLOBAL, HALT
  assert(bc.code.size() == 7);
  assert(bc.code[2].op == OpCode::GETGLOBAL);
  assert(bc.code[2].a == 0 && bc.code[2].b == 1);
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::ADD);
  assert(bc.code[4].a == 0 && bc.code[4].b == 0 && bc.code[4].c == 1);
  assert(bc.code[5].op == OpCode::SETGLOBAL);
  assert(bc.code[5].a == 0 && bc.code[5].b == 1);
  assert(bc.code[6].op == OpCode::HALT);
}

void testLetReassignment() {
  expectCompileError("let x = 10;\nx = 20;\n", "immutable reassignment");
  expectCompileError("let x = 10;\nx += 1;\n", "immutable compound assignment");

  // Redeclaring with `var` lifts the restriction, exactly like `Env::define`.
  Bytecode bc = compileSource("let x = 10;\nvar x = 20;\nx = 30;\n");
  assert(!bc.code.empty());
}

void testAssignment() {
  Bytecode bc = compileSource("x = 5;\n");

  assert(bc.code.size() == 3);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[1].a == 0 && bc.code[1].b == 1);
  assert(bc.code[2].op == OpCode::HALT);
  assert(bc.numRegisters == 1);
}

void testConstantDeduplication() {
  Bytecode bc = compileSource("print(1 + 1);\n");

  // The compiler resolves the callee (`"print"`) before the argument (1), so
  // the pool order is ["print", 1]. The two `1` literals deduplicate to one
  // entry.
  assert(bc.constants.size() == 2);
  assert(bc.constants[0].isStr() && bc.constants[0].asStr() == "print");
  assert(bc.constants[1].isInt() && bc.constants[1].asInt() == 1);
}

// --------------------------------------------------------- PHASE 5: control

void testIfElseJumps() {
  Bytecode bc = compileSource("if (true) { print(1); } else { print(2); }\n");

  assert(bc.code.size() == 10);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].a == 0 && bc.code[1].jump == 6);  // <-- 6, no 7
  assert(bc.code[2].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::CALL);
  assert(bc.code[4].a == 0 && bc.code[4].b == 1);
  assert(bc.code[5].op == OpCode::JMP);
  assert(bc.code[5].jump == 9);  // <-- 9, no 11
  assert(bc.code[6].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[7].op == OpCode::LOADK);
  assert(bc.code[8].op == OpCode::CALL);
  assert(bc.code[8].a == 0 && bc.code[8].b == 1);
  assert(bc.code[9].op == OpCode::HALT);
  assert(bc.numLocals == 0);
}

void testIfWithoutElse() {
  Bytecode bc = compileSource("if (false) { print(1); }\n");

  // LOADK false, JMP_IF_FALSE, GETGLOBAL "print", LOADK 1, CALL 0, 1, HALT
  assert(bc.code.size() == 6);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 5);  // to HALT
  assert(bc.code[5].op == OpCode::HALT);
}

void testWhileBackpatch() {
  Bytecode bc = compileSource("while (false) { print(1); }\n");

  // LOADK false, JMP_IF_FALSE, GETGLOBAL "print", LOADK 1, CALL 0, 1, JMP, HALT
  assert(bc.code.size() == 7);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 6);  // leaves the loop
  assert(bc.code[2].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::CALL);
  assert(bc.code[5].op == OpCode::JMP);
  assert(bc.code[5].jump == 0);  // goes back to re-evaluate the condition
  assert(bc.code[6].op == OpCode::HALT);
}

void testForBackpatch() {
  Bytecode bc = compileSource("for (var i = 0; i < 3; i += 1) { print(i); }\n");

  // LOADK 0, DECLLOCAL, GETLOCAL, LOADK 3, LT, JMP_IF_FALSE,
  // GETGLOBAL "print", GETLOCAL, CALL 0, 1,
  // GETLOCAL, LOADK 1, ADD, SETLOCAL, JMP, HALT
  assert(bc.code.size() == 15);
  assert(bc.code[1].op == OpCode::DECLLOCAL);
  assert(bc.code[1].a == 0 && bc.code[1].b == 0);
  assert(bc.code[2].op == OpCode::GETLOCAL);
  assert(bc.code[4].op == OpCode::LT);
  assert(bc.code[5].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[5].jump == 14);  // leaves to HALT
  assert(bc.code[6].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[7].op == OpCode::GETLOCAL);
  assert(bc.code[8].op == OpCode::CALL);
  assert(bc.code[9].op == OpCode::GETLOCAL);
  assert(bc.code[12].op == OpCode::SETLOCAL);
  assert(bc.code[12].b == 0);
  assert(bc.code[13].op == OpCode::JMP);
  assert(bc.code[13].jump == 2);  // goes back to the condition

  // The `for` initializer is local, not global.
  assert(bc.numLocals == 1);
  assert(bc.numRegisters == 2);
}

void testBreakAndContinueBackpatch() {
  Bytecode brk = compileSource("while (true) { break; }\n");
  // LOADK, JMP_IF_FALSE, JMP (break), JMP (back), HALT
  assert(brk.code.size() == 5);
  assert(brk.code[2].op == OpCode::JMP && brk.code[2].jump == 4);
  assert(brk.code[3].op == OpCode::JMP && brk.code[3].jump == 0);

  Bytecode cont = compileSource("while (true) { continue; }\n");
  assert(cont.code.size() == 5);
  assert(cont.code[2].op == OpCode::JMP && cont.code[2].jump == 0);

  // In a `for`, `continue` jumps to the step, not to the condition.
  Bytecode forCont = compileSource("for (;;) { continue; }\n");
  // JMP (continue -> step), JMP (back), HALT
  assert(forCont.code.size() == 3);
  assert(forCont.code[0].op == OpCode::JMP && forCont.code[0].jump == 1);
  assert(forCont.code[1].op == OpCode::JMP && forCont.code[1].jump == 0);

  expectCompileError("break;\n", "break outside loop");
  expectCompileError("continue;\n", "continue outside loop");
}

void testSwitchBytecode() {
  // `switch (2) { case 1: {...} case 2: {...} }` compiles the subject once,
  // compares each label with EQ, jumps to the matching body and skips the rest.
  Bytecode bc = compileSource(
      "switch (2) {\n"
      "  case 1: { print(\"one\"); }\n"
      "  case 2: { print(\"two\"); }\n"
      "}\n");

  // LOADK (subject) is the first instruction; each case compares with EQ.
  assert(bc.code[0].op == OpCode::LOADK && bc.code[0].b == 0);
  size_t eqCount = 0;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::EQ) ++eqCount;
  }
  assert(eqCount == 2);  // one comparison per literal label
  assert(bc.code.back().op == OpCode::HALT);
}

void testSwitchRangeUsesIn() {
  // A range label compiles to `IN` (membership), not `EQ`.
  Bytecode bc = compileSource(
      "switch (5) {\n"
      "  case 1..=10: { print(\"in\"); }\n"
      "}\n");
  size_t inCount = 0;
  size_t eqCount = 0;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::IN) ++inCount;
    if (instruction.op == OpCode::EQ) ++eqCount;
  }
  assert(inCount == 1);
  assert(eqCount == 0);
}

void testSwitchErrors() {
  expectCompileError("case 1: {}\n", "case outside switch");
  expectCompileError("default: {}\n", "default outside switch");
  expectCompileError("switch (1) { case x: {} }\n", "case label must be a literal or a range");
  expectCompileError("switch (1) { case f(): {} }\n", "case label must be a literal or a range");
  expectCompileError("switch (1) { default: {} default: {} }\n", "duplicate default");
  expectCompileError("switch (1) { case 1 }\n", "missing ':' after case label");
  expectCompileError("switch (1) { case 1: {} }\nswitch (2) { case 2: { break; } }\n",
                     "break outside loop inside a switch");
  expectCompileError("switch (1) { case 1: {} }\nswitch (2) { case 2: { continue; } }\n",
                     "continue outside loop inside a switch");
}

void testSwitchSignedLabels() {
  // A leading sign on a numeric label is accepted.
  expectCompiles("switch (1) { case -1: {} case +2: {} }\n");
}

void testDeferCompiles() {
  // `defer { ... }` compiles: the block is inlined at the scope exit, so on
  // the `return` path the deferred call is emitted just before the RETURN.
  Bytecode bc = compileSource(
      "fn f() {\n"
      "  defer { print(\"cleanup\"); }\n"
      "  return 1;\n"
      "}\n");

  assert(bc.children.size() == 1);
  const Bytecode& proto = *bc.children[0];
  // Find the first RETURN; the deferred CALL must be the instruction before it.
  size_t firstReturn = proto.code.size();
  for (size_t i = 0; i < proto.code.size(); ++i) {
    if (proto.code[i].op == OpCode::RETURN) {
      firstReturn = i;
      break;
    }
  }
  assert(firstReturn > 0);
  assert(proto.code[firstReturn - 1].op == OpCode::CALL);
}

void testDeferRunsOnFallThrough() {
  // Falling off the end of the function runs the deferred block before the
  // implicit nil return, so the deferred call precedes the epilogue.
  Bytecode bc = compileSource(
      "fn f() {\n"
      "  defer { print(\"cleanup\"); }\n"
      "  var x = 1;\n"
      "}\n");
  assert(bc.children.size() == 1);
  const Bytecode& proto = *bc.children[0];
  // The epilogue is `LOADK nil; RETURN`; the deferred CALL runs just before it.
  assert(proto.code.back().op == OpCode::RETURN);
  assert(proto.code[proto.code.size() - 2].op == OpCode::LOADK);
  assert(proto.code[proto.code.size() - 3].op == OpCode::CALL);
}

void testDeferErrors() {
  expectCompileError("defer { print(\"x\"); }\n", "defer outside a function");
  expectCompileError("fn f() { defer print(\"x\"); }\n", "defer requires a block");
  expectCompileError("fn f() { defer { return 1; } return 2; }\n",
                     "return inside a defer block");
  expectCompileError("fn f() { for i in 0..3 { defer { break; } } }\n",
                     "break inside a defer block");
  expectCompileError("fn f() { for i in 0..3 { defer { continue; } } }\n",
                     "continue inside a defer block");
}

void testDeferNestedConstructsCompile() {
  // A loop inside the deferred block may break: it is nested past the boundary.
  expectCompiles(
      "fn f() {\n"
      "  defer { for i in 0..3 { if (i == 1) { break; } } }\n"
      "}\n");
  // A nested function inside the deferred block may return.
  expectCompiles(
      "fn f() {\n"
      "  defer { var g = () => { return 1; }; }\n"
      "}\n");
}

void testBlockScopingBytecode() {
  Bytecode bc = compileSource(
      "{\n"
      "  var x = 1;\n"
      "  x = 2;\n"
      "}\n");

  // LOADK, DECLLOCAL, LOADK, SETLOCAL, HALT
  assert(bc.code.size() == 5);
  assert(bc.code[1].op == OpCode::DECLLOCAL);
  assert(bc.code[1].b == 0);
  assert(bc.code[3].op == OpCode::SETLOCAL);
  assert(bc.code[3].b == 0);
  assert(bc.numLocals == 1);
}

void testShadowingSlots() {
  Bytecode bc = compileSource(
      "{\n"
      "  let x = 1;\n"
      "  {\n"
      "    let x = 2;\n"
      "    print(x);\n"
      "  }\n"
      "  print(x);\n"
      "}\n");

  // The inner block uses a different slot for its `x`.
  assert(bc.code[3].op == OpCode::DECLLOCAL && bc.code[3].b == 1);
  assert(bc.code[4].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.numLocals == 2);
}

void testLocalImmutableAssignment() {
  expectCompileError("{ let x = 1; x = 2; }\n", "local immutable reassignment");
  expectCompileError("{ let x = 1; x += 2; }\n", "local immutable compound assignment");
}

void testLogicalShortCircuit() {
  Bytecode andBc = compileSource("let a = 1 && 2;\n");
  // LOADK, JMP_IF_FALSE, LOADK, MOVE, SETGLOBAL, HALT
  assert(andBc.code.size() == 6);
  assert(andBc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(andBc.code[1].jump == 4);  // if the left side is falsy, the right side is not evaluated
  assert(andBc.code[2].op == OpCode::LOADK && andBc.code[2].b == 1);
  assert(andBc.code[3].op == OpCode::MOVE);
  assert(andBc.code[3].a == 0 && andBc.code[3].b == 1);

  Bytecode orBc = compileSource("let a = 1 || 2;\n");
  assert(orBc.code.size() == 6);
  assert(orBc.code[1].op == OpCode::JMP_IF_TRUE);
  assert(orBc.code[1].jump == 4);
}

void testTernary() {
  Bytecode bc = compileSource("let a = true ? 1 : 2;\n");

  // LOADK, JMP_IF_FALSE, LOADK, MOVE, JMP, LOADK, MOVE, SETGLOBAL, HALT
  assert(bc.code.size() == 9);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 5);
  assert(bc.code[3].op == OpCode::MOVE);
  assert(bc.code[4].op == OpCode::JMP);
  assert(bc.code[4].jump == 7);
  assert(bc.code[6].op == OpCode::MOVE);
  assert(bc.code[7].op == OpCode::SETGLOBAL);
}

void testBitwiseOps() {
  Bytecode bc = compileSource(
      "let a = 1 & 2;\n"
      "let b = 1 | 2;\n"
      "let c = 1 ^ 2;\n"
      "let d = 1 << 2;\n"
      "let e = 1 >> 2;\n"
      "let f = ~1;\n");

  assert(bc.code[2].op == OpCode::BAND);
  assert(bc.code[6].op == OpCode::BOR);
  assert(bc.code[10].op == OpCode::BXOR);
  assert(bc.code[14].op == OpCode::SHL);
  assert(bc.code[18].op == OpCode::SHR);
  assert(bc.code[21].op == OpCode::UNOP);
  assert(bc.code[21].a == 0 && bc.code[21].b == 0);
  assert(bc.code[21].c == static_cast<int>(UnaryOp::BitNot));
}

void testFunctionProtosAndCalls() {
  Bytecode bc = compileSource(
      "fn add(a, b) { return a + b; }\n"
      "let r = add(2, 3);\n");

  // The declaration becomes a child proto, not a constant.
  assert(bc.children.size() == 1);
  const Bytecode& add = *bc.children[0];
  assert(add.name == "add");
  assert(add.numParams == 2);
  assert(add.numLocals == 2);
  assert(add.numRegisters == 2);

  // Parameters arrive in R0/R1 and are boxed into cells by the prologue.
  assert(add.code.size() == 8);
  assert(add.code[0].op == OpCode::DECLLOCAL);
  assert(add.code[0].a == 0 && add.code[0].b == 0);
  assert(add.code[1].op == OpCode::DECLLOCAL);
  assert(add.code[1].a == 1 && add.code[1].b == 1);
  assert(add.code[2].op == OpCode::GETLOCAL && add.code[2].b == 0);
  assert(add.code[3].op == OpCode::GETLOCAL && add.code[3].b == 1);
  assert(add.code[4].op == OpCode::ADD);
  assert(add.code[4].a == 0 && add.code[4].b == 0 && add.code[4].c == 1);
  assert(add.code[5].op == OpCode::RETURN && add.code[5].a == 0);

  // Falling off the end returns nil.
  assert(add.code[6].op == OpCode::LOADK);
  assert(add.code[7].op == OpCode::RETURN);

  // The root instantiates the proto and stores it as a global.
  assert(bc.code[0].op == OpCode::CLOSURE);
  assert(bc.code[0].a == 0 && bc.code[0].b == 0);
  assert(bc.code[1].op == OpCode::SETGLOBAL);

  // Callee and arguments occupy a contiguous window; `b` is the arity.
  bool sawCall = false;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::CALL) {
      sawCall = true;
      assert(instruction.a == 0);
      assert(instruction.b == 2);
    }
  }
  assert(sawCall);
}

void testFunctionHoisting() {
  // A call may precede the declaration: the compiler emits every top-level
  // `fn` before the statements that use it.
  Bytecode bc = compileSource(
      "let r = add(1, 2);\n"
      "fn add(a, b) { return a + b; }\n");
  assert(bc.children.size() == 1);
  assert(bc.code[0].op == OpCode::CLOSURE);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
}

void testMultiArgumentPrint() {
  // Only the single-argument form used PRINT; now every arity is a CALL.
  Bytecode bc = compileSource("print(1, 2);\n");
  assert(bc.code.size() == 5);
  assert(bc.code[0].op == OpCode::GETGLOBAL);
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::CALL);
  assert(bc.code[3].a == 0 && bc.code[3].b == 2);
  assert(bc.code[4].op == OpCode::HALT);
}

bool hasOp(const Bytecode& bc, OpCode op, int b) {
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == op && instruction.b == b) return true;
  }
  return false;
}

// ------------------------------------------------- PHASE 6b: where captures live

void testCaptureLivesInTheFunctionThatUsesTheVariable() {
  // `n` is a parameter of makeAdder, and the lambda is the function that uses
  // it. The capture belongs to the lambda and points at makeAdder's local
  // slot; makeAdder itself captures nothing.
  Bytecode bc = compileSource("fn makeAdder(n) { return x => x + n; }\n");
  assert(bc.upvalues.empty());
  assert(bc.children.size() == 1);

  const Bytecode& makeAdder = *bc.children[0];
  assert(makeAdder.upvalues.empty());
  assert(makeAdder.children.size() == 1);

  const Bytecode& lambda = *makeAdder.children[0];
  assert(lambda.upvalues.size() == 1);
  assert(lambda.upvalues[0].inParentLocal);
  assert(lambda.upvalues[0].index == 0);
  assert(hasOp(lambda, OpCode::GETUPVAL, 0));
  // The captured variable is read through the upvalue, not as a local or global.
  assert(!hasOp(makeAdder, OpCode::GETUPVAL, 0));
}

void testNestedCaptureIsReexportedByTheMiddleFunction() {
  // The lambda uses `n` from two levels up. The middle function captures it
  // from its parent's local slot, and the lambda reaches it through the middle
  // function's upvalue, so both share one cell.
  Bytecode bc = compileSource(
      "fn outer(n) {\n"
      "  fn middle() { return () => n; }\n"
      "  return middle;\n"
      "}\n");
  const Bytecode& outer = *bc.children[0];
  assert(outer.upvalues.empty());

  const Bytecode& middle = *outer.children[0];
  assert(middle.upvalues.size() == 1);
  assert(middle.upvalues[0].inParentLocal && middle.upvalues[0].index == 0);

  const Bytecode& lambda = *middle.children[0];
  assert(lambda.upvalues.size() == 1);
  assert(!lambda.upvalues[0].inParentLocal);
  assert(lambda.upvalues[0].index == 0);
}

void testCapturesFollowOrderOfFirstUse() {
  // A parameter and a local of the enclosing function are captured separately,
  // each from its own slot, in the order the lambda first uses them.
  Bytecode bc = compileSource("fn f(a) { let y = 2; return () => y + a; }\n");
  const Bytecode& lambda = *bc.children[0]->children[0];
  assert(lambda.upvalues.size() == 2);
  assert(lambda.upvalues[0].inParentLocal && lambda.upvalues[0].index == 1);  // y
  assert(lambda.upvalues[1].inParentLocal && lambda.upvalues[1].index == 0);  // a
}

void testVariableUsedTwiceIsCapturedOnce() {
  Bytecode bc = compileSource("fn f(n) { return () => n + n; }\n");
  const Bytecode& lambda = *bc.children[0]->children[0];
  assert(lambda.upvalues.size() == 1);
}

void testFunctionCapturesLocalOfTheMainChunk() {
  // A block-local variable of the main chunk is capturable. Top-level `let`s
  // are globals and are never captured.
  Bytecode bc = compileSource("{ let k = 1; fn g() { return k; } }\n");
  assert(bc.upvalues.empty());
  const Bytecode& g = *bc.children[0];
  assert(g.upvalues.size() == 1);
  assert(g.upvalues[0].inParentLocal && g.upvalues[0].index == 0);

  Bytecode globals = compileSource("let k = 1;\nfn g() { return k; }\n");
  assert(globals.children[0]->upvalues.empty());
}

void testNothingIsCapturedFromTheOutermostFunction() {
  // A function directly under the main chunk with no enclosing local captures
  // nothing: free names are globals.
  Bytecode bc = compileSource("fn f() { return missing; }\n");
  assert(bc.children[0]->upvalues.empty());
}

void testUnsupportedConstructs() {
  // Every expression and statement form of the language now has a bytecode
  // encoding; the only remaining compile-time rejection is `return` outside a
  // function body.
  expectCompileError("return 1;\n", "return statement");
}

void testPhase6ConstructsCompile() {
  // Collections, iteration, interpolation, ranges, `in`, indexing and members
  // are all part of the bytecode pipeline since PHASE 6.
  expectCompiles("let x = [1, 2];\n");
  expectCompiles("let m = {a: 1};\n");
  expectCompiles("let s = \"a${b}\";\n");
  expectCompiles("let r = 1..2;\n");
  // No parentheses: `for (x in ...)` would be rejected by the parser instead.
  expectCompiles("for x in 1..2 { print(x); }\n");
  expectCompiles("let x = 1 in 2;\n");
  expectCompiles("let a = 1;\nlet x = a[0];\n");
  expectCompiles("let m = 1;\nlet x = m.a;\n");
  expectCompiles("switch (1) { case 1: {} default: {} }\n");
}

void testNestedFunctionDeclarationsAreSupported() {
  // Regression guard: nested and block-level `fn` declarations compile since
  // PHASE 6b; they must not be reported as unsupported constructs anymore.
  expectCompiles("fn outer() { fn inner() {} }\n");
  expectCompiles("{ fn f() {} }\n");
  expectCompiles("let f = x => x;\n");
}

void testTypeAnnotationsAreErased() {
  // PHASE 6.5: type annotations are compile-time metadata. A declaration with
  // an annotation must produce exactly the same bytecode as the same
  // declaration without it, so the register VM and the AOT backend never see
  // types (AGENT.md §3).
  Bytecode annotated = compileSource("let x: int = 5;\n");
  Bytecode plain = compileSource("let x = 5;\n");
  assert(annotated.code.size() == plain.code.size());
  for (size_t i = 0; i < plain.code.size(); ++i) {
    assert(annotated.code[i].op == plain.code[i].op);
    assert(annotated.code[i].a == plain.code[i].a);
    assert(annotated.code[i].b == plain.code[i].b);
    assert(annotated.code[i].c == plain.code[i].c);
  }
  assert(annotated.numRegisters == plain.numRegisters);
  assert(annotated.numLocals == plain.numLocals);

  // The same holds for a typed parameter and return annotation.
  Bytecode typedFn = compileSource("fn f(a: int): int { return a; }\n");
  Bytecode plainFn = compileSource("fn f(a) { return a; }\n");
  const Bytecode& typedChild = *typedFn.children[0];
  const Bytecode& plainChild = *plainFn.children[0];
  assert(typedChild.code.size() == plainChild.code.size());
  assert(typedChild.numParams == plainChild.numParams);
}

void testTypeAnnotationsAccepted() {
  expectCompiles("let x: int = 5;\n");
  expectCompiles("let x: float = 5;\n");       // int widens to float
  expectCompiles("let x: string? = nil;\n");   // nil fits a nullable target
  expectCompiles("var x: any = 1;\nvar y: any = \"s\";\n");
  expectCompiles("fn f(a: int, b: string): bool { return true; }\n");
  expectCompiles("let f = (x: int) => x;\n");
  expectCompiles("var x: int?;\nx = 3;\n");
}

void testTypeMismatchErrors() {
  expectCompileError("let x: int = \"s\";\n", "string into int");
  expectCompileError("let x: int = 1.5;\n", "float into int");
  expectCompileError("let x: int = nil;\n", "nil into non-nullable");
  expectCompileError("var x = 0;\nx = \"s\";\n", "inferred type mismatch");
  expectCompileError("fn f(): int { return \"s\"; }\n", "return type mismatch");
  expectCompileError("fn f(): int { return; }\n", "bare return in typed function");
  expectCompileError("let x: integer = 1;\n", "unknown type name");
}

void testNullCoalesceBytecode() {
  // PHASE 6.5: `a ?? b` lowers to existing opcodes only (no new opcode).
  // LOADK a; LOADK nil; EQ isNil, a, nil; JMP_IF_FALSE isNil -> end;
  // <b>; MOVE a, b; end: ...  The right-hand side is skipped when `a` != nil.
  Bytecode bc = compileSource("let x = a ?? b;\n");

  bool sawEq = false;
  bool sawJmpIfFalse = false;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::EQ) sawEq = true;
    if (instruction.op == OpCode::JMP_IF_FALSE) sawJmpIfFalse = true;
  }
  assert(sawEq && "?? must compare against nil with EQ");
  assert(sawJmpIfFalse && "?? must short-circuit with JMP_IF_FALSE");

  expectCompiles("let x = nil ?? 1 ?? 2;\n");
}

void testSafeMemberBytecode() {
  // `a?.b` lowers to EQ nil + JMP_IF_TRUE + GETTABLE, all existing opcodes.
  Bytecode bc = compileSource("let x = a?.b;\n");
  bool sawEq = false;
  bool sawJmpIfTrue = false;
  bool sawGetTable = false;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::EQ) sawEq = true;
    if (instruction.op == OpCode::JMP_IF_TRUE) sawJmpIfTrue = true;
    if (instruction.op == OpCode::GETTABLE) sawGetTable = true;
  }
  assert(sawEq && "?. must compare the receiver against nil");
  assert(sawJmpIfTrue && "?. must skip the lookup when the receiver is nil");
  assert(sawGetTable && "?. must still perform the member lookup otherwise");

  // A plain member access must not add the nil guard.
  Bytecode plain = compileSource("let x = a.b;\n");
  bool plainHasEq = false;
  for (const Instruction& instruction : plain.code) {
    if (instruction.op == OpCode::EQ) plainHasEq = true;
  }
  assert(!plainHasEq && "plain `.` must not emit a nil guard");

  // A safe member access is not an assignment target.
  expectCompileError("a?.b = 1;\n", "safe member assignment");
}

void testSliceBytecode() {
  // `a[start:end]` lowers to a call to the hidden `$slice` helper, so slicing
  // adds no opcode to the budget (§3.3).
  Bytecode bc = compileSource("let x = [1, 2, 3][0:2];\n");
  bool sawGetGlobalSlice = false;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::GETGLOBAL) {
      const Value& name = bc.constants[static_cast<size_t>(instruction.b)];
      if (name.isStr() && name.asStr() == "$slice") sawGetGlobalSlice = true;
    }
  }
  assert(sawGetGlobalSlice && "slicing must call the $slice helper");

  // Every form parses and compiles; an omitted bound passes nil.
  expectCompiles("let a = [1, 2, 3];\nlet b = a[:];\n");
  expectCompiles("let a = [1, 2, 3];\nlet b = a[1:];\n");
  expectCompiles("let a = [1, 2, 3];\nlet b = a[:2];\n");
  expectCompiles("let a = [1, 2, 3];\nlet b = a[0:2];\n");
  expectCompiles("let s = \"hello\";\nlet t = s[1:3];\n");

  // A slice is read-only and there is no step.
  expectCompileError("let a = [1, 2, 3];\na[0:1] = [9];\n", "assignment to a slice");
  expectCompileError("let a = [1, 2, 3];\nlet b = a[0:2:1];\n", "slice step");
}

void testNonNullBytecode() {
  // `a!!` lowers to a call to the hidden `$nonNull` helper (no new opcode).
  Bytecode bc = compileSource("let x = a!!;\n");
  bool sawGetGlobalNonNull = false;
  for (const Instruction& instruction : bc.code) {
    if (instruction.op == OpCode::GETGLOBAL) {
      const Value& name = bc.constants[static_cast<size_t>(instruction.b)];
      if (name.isStr() && name.asStr() == "$nonNull") sawGetGlobalNonNull = true;
    }
  }
  assert(sawGetGlobalNonNull && "!! must call the $nonNull helper");
  expectCompiles("let s: string? = \"x\";\nlet t = s!!;\n");
}

void testStructDeclEmitsNoBytecode() {
  // PHASE 6.5: a `struct` declaration is pure compile-time metadata. It must
  // emit no bytecode, so a program with the declaration has the same code as
  // the same program without it (only the literal lowers, to a map).
  Bytecode withDecl =
      compileSource("struct Point { x: int; y: int; }\nlet p = Point { x: 1, y: 2 };\n");
  Bytecode withoutDecl = compileSource("let p = {x: 1, y: 2};\n");
  assert(withDecl.code.size() == withoutDecl.code.size());
  for (size_t i = 0; i < withoutDecl.code.size(); ++i) {
    assert(withDecl.code[i].op == withoutDecl.code[i].op);
    assert(withDecl.code[i].a == withoutDecl.code[i].a);
    assert(withDecl.code[i].b == withoutDecl.code[i].b);
    assert(withDecl.code[i].c == withoutDecl.code[i].c);
  }
  assert(withDecl.numRegisters == withoutDecl.numRegisters);
}

void testStructLiteralLowersToTable() {
  // The literal is a NEWTABLE followed by one SETTABLE per field; no new
  // opcode is introduced (AGENT.md §3.3).
  Bytecode bc = compileSource("struct Point { x: int; y: int; }\nlet p = Point { x: 1, y: 2 };\n");
  bool sawNewTable = false;
  int setTables = 0;
  for (const Instruction& ins : bc.code) {
    if (ins.op == OpCode::NEWTABLE) sawNewTable = true;
    if (ins.op == OpCode::SETTABLE) ++setTables;
  }
  assert(sawNewTable && "a struct literal must build a table");
  assert(setTables == 2 && "one SETTABLE per field");
}

void testStructErrors() {
  // A literal must match the declaration exactly.
  expectCompileError("struct P { x: int; y: int; }\nlet p = P { x: 1 };\n", "a missing field");
  expectCompileError("struct P { x: int; }\nlet p = P { x: 1, z: 2 };\n", "an unknown field");
  expectCompileError("struct P { x: int; }\nlet p = P { x: \"no\" };\n", "a wrong field type");
  // The declaration itself.
  expectCompileError("struct P { x: int; x: int; }\n", "a duplicate field");
  expectCompileError("struct P { x: int; }\nstruct P { y: int; }\n", "a duplicate struct");
  expectCompileError("fn f() { struct P { x: int; } }\n", "a nested struct declaration");
}

void testStructNamesAreUsableAsTypes() {
  // A struct name is a valid annotation, and a struct declared later in the
  // file is visible earlier, like a hoisted `fn`.
  expectCompiles("struct P { x: int; }\nlet p: P = P { x: 1 };\n");
  expectCompiles("fn make() { return P { x: 1 }; }\nstruct P { x: int; }\n");
}

void testTryCatchBytecode() {
  // `e catch d` (PHASE 6.7.3): evaluate the operand, test it with the hidden
  // `$isError` helper, and on failure evaluate the fallback and MOVE it over
  // the result. No new opcode is introduced (§3.3).
  Bytecode bc = compileSource("fn f() { return 1; }\nlet x = f() catch 0;\n");
  bool sawIsErrorCall = false;
  bool sawConditionalJump = false;
  bool sawMove = false;
  for (const Instruction& ins : bc.code) {
    if (ins.op == OpCode::CALL) sawIsErrorCall = true;
    if (ins.op == OpCode::JMP_IF_FALSE) sawConditionalJump = true;
    if (ins.op == OpCode::MOVE) sawMove = true;
  }
  assert(sawIsErrorCall && "catch must test the operand with $isError");
  assert(sawConditionalJump && "catch must branch on the failure");
  assert(sawMove && "catch must move the fallback into the result");

  // `try e`: a failed operand returns the error from the enclosing function.
  // The lowering is `CALL $isError` + `JMP_IF_FALSE` + `RETURN`.
  Bytecode tb = compileSource("fn g() { return error(\"x\"); }\nfn f() { return try g(); }\n");
  const Bytecode* fProto = nullptr;
  for (const auto& child : tb.children) {
    if (child->name == "f") fProto = child.get();
  }
  assert(fProto != nullptr);
  bool sawReturn = false;
  bool sawTryCall = false;
  for (const Instruction& ins : fProto->code) {
    if (ins.op == OpCode::CALL) sawTryCall = true;
    if (ins.op == OpCode::RETURN) sawReturn = true;
  }
  assert(sawTryCall && sawReturn);
  // The normal return must survive the conditional RETURN of `try`.
  int returns = 0;
  for (const Instruction& ins : fProto->code) {
    if (ins.op == OpCode::RETURN) ++returns;
  }
  assert(returns >= 2 && "`return try f();` must emit its own RETURN too");
}

void testTryCatchErrors() {
  // `try` propagates by returning, so it needs a function to return from.
  expectCompileError("let x = try 5;\n", "try outside a function");
  // And it is rejected inside a `defer` block, like `return`.
  expectCompileError("fn f() { defer { let x = try 5; } return 1; }\n",
                     "try inside a defer block");
  // `catch` on its own is always fine, even outside a function.
  expectCompiles("let x = error(\"a\") catch 0;\n");
}

void testVoidReturnType() {
  // `void` is a return-only annotation. A function that falls off the end or
  // uses a bare `return;` compiles, and it still emits the implicit nil return
  // (void is metadata only, so no opcode is added).
  expectCompiles("fn f(): void { print(1); }\n");
  expectCompiles("fn f(): void { return; }\n");
  expectCompiles("fn f(x: int): void { if (x > 0) { return; } print(x); }\n");

  Bytecode bc = compileSource("fn f(): void { return; }\nf();\n");
  const Bytecode* fProto = nullptr;
  for (const auto& child : bc.children) {
    if (child->name == "f") fProto = child.get();
  }
  assert(fProto != nullptr);
  bool sawReturn = false;
  for (const Instruction& ins : fProto->code) {
    if (ins.op == OpCode::RETURN) sawReturn = true;
  }
  assert(sawReturn && "a void function still emits a RETURN");

  // A void value is not a value: `var x = voidFn();` stays untyped (like
  // `var x = nil`), so a later assignment is allowed and no bogus type sticks.
  expectCompiles("fn f(): void {}\nvar x = f();\nx = 5;\n");
}

void testVoidErrors() {
  // Returning a value from a void function is rejected.
  expectCompileError("fn f(): void { return 1; }\n", "return a value from void");
  // `void` is not valid on a variable, a parameter or a struct field.
  expectCompileError("let x: void = nil;\n", "void variable");
  expectCompileError("fn f(x: void) {}\n", "void parameter");
  expectCompileError("struct S { v: void; }\n", "void struct field");
  // `void` has no value, so a nullable `void?` is meaningless.
  expectCompileError("fn f(): void? {}\n", "nullable void");
}

void testFfiTypeVocabulary() {
  // Pointers and fixed-width/unsigned integers are accepted type names
  // (PHASE 6.6). They are erased before the bytecode, so a signature using them
  // compiles exactly like one using `int`.
  expectCompiles("native fn strlen(s: cstring): size_t;\n");
  expectCompiles("native fn free(p: ptr): void;\n");
  expectCompiles("let p: ptr = nil;\n");
  expectCompiles("let s: cstring = nil;\n");

  // `nil` is the NULL of a pointer parameter, even without a `?`.
  expectCompiles("native fn free(p: ptr): void;\nfree(nil);\n");

  // `cstring` upcasts to `ptr` (to `void*`); the reverse is not implicit.
  expectCompiles("fn f(s: cstring) { let p: ptr = s; }\n");
  expectCompileError("fn f(p: ptr) { let s: cstring = p; }\n", "ptr does not downcast to cstring");

  // The integer family is mutually assignable and widens to float/double.
  expectCompiles("let a: int32 = 1;\nlet b: int = a;\nlet c: size_t = b;\nlet d: double = c;\n");
  expectCompiles("let a: uint = 1;\nlet b: int64 = a;\nlet c: float = b;\n");
  expectCompileError("let x: size_t = \"nope\";\n", "string is not an integer");
  expectCompileError("let x: ptr = 5;\n", "int is not a pointer");
  expectCompileError("let x: cstring = \"hi\";\n", "a string is not a cstring");

  // A string is not an owned `cstring` binding, but it is a valid `const char *`
  // *argument*: it is NUL-terminated and stays alive for the whole call.
  expectCompiles("native fn strlen(s: cstring): size_t;\nlet n = strlen(\"hi\");\n");
  expectCompiles("extern \"C\" { fn strlen(s: cstring): size_t; }\nprint(strlen(\"hi\"));\n");
  // A nullable string is not a valid `cstring` argument: it may be nil.
  expectCompileError("native fn strlen(s: cstring): size_t;\nlet s: string? = nil;\nstrlen(s);\n",
                     "argument 1");

  // A `native fn` return type feeds inference: the result is a `size_t`.
  expectCompiles("native fn strlen(s: cstring): size_t;\nlet n: int = strlen(nil);\n");
}

void testExternBlock() {
  // An `extern "C"` block declares C functions. Each declaration follows the
  // `native fn` path, so a block compiles to exactly the bytecode of the
  // equivalent top-level `native fn` declarations (no dedicated opcode).
  Bytecode block = compileSource("extern \"C\" {\n  fn f(): int;\n}\n");
  Bytecode native = compileSource("native fn f(): int;\n");
  assert(block.code.size() == native.code.size());
  for (size_t i = 0; i < block.code.size(); ++i) {
    assert(block.code[i].op == native.code[i].op);
    assert(block.code[i].a == native.code[i].a);
    assert(block.code[i].b == native.code[i].b);
  }

  // The declaration is hoisted, so a function may call one declared later and
  // the call is checked against the signature.
  expectCompiles(
      "fn g(): double { return h(2.0); }\n"
      "extern \"C\" {\n  fn h(x: double): double;\n}\n");

  // `extern fn ...;` is sugar for a one-entry `extern "C"` block: both spellings
  // compile to the same bytecode, so the single-line form adds no new path.
  Bytecode single = compileSource("extern fn f(): int;\n");
  assert(single.code.size() == block.code.size());
  for (size_t i = 0; i < single.code.size(); ++i) {
    assert(single.code[i].op == block.code[i].op);
    assert(single.code[i].a == block.code[i].a);
    assert(single.code[i].b == block.code[i].b);
  }
  // The single-line form hoists too, and a call is checked against it.
  expectCompiles("fn g(): int { return h(2); }\nextern fn h(x: int): int;\n");

  // A `string` parameter is a valid `const char *`, exactly like `cstring`.
  expectCompiles("extern fn strlen(s: string): size_t;\nprint(strlen(\"hi\"));\n");
  expectCompiles("extern \"C\" { fn puts(s: string): int; }\nputs(\"hi\");\n");
  // A `string` is not a valid return type: C returns a `cstring`.
  expectCompileError("extern fn f(): string;\n", "string return");
  // The single-line form is module-level metadata and bodyless, like the block.
  expectCompileError("fn g() { extern fn f(): int; }\n", "nested single extern");
  expectCompileError("extern fn f(): int { return 1; }\n", "single extern body");
  expectCompileError("extern 42;\n", "single extern without fn");

  // The FFI vocabulary is accepted inside a block, and a `void` result too.
  expectCompiles("extern \"C\" { fn free(p: ptr): void; }\nfree(nil);\n");
  expectCompiles("extern \"C\" { fn strlen(s: cstring): size_t; }\n");

  // The signature is enforced at the call site, like `native fn`.
  expectCompileError("extern \"C\" { fn f(x: int): int; }\nf();\n",
                     "extern call with too few arguments");
  expectCompileError("extern \"C\" { fn f(x: int): int; }\nf(\"s\");\n",
                     "extern call with a wrong argument type");

  // The block is module-level metadata: nesting it is an error, the ABI must be
  // "C", only `fn` declarations are allowed, and a body is rejected.
  expectCompileError("fn g() { extern \"C\" { fn f(): int; } }\n", "nested extern block");
  expectCompileError("extern \"Rust\" { fn f(): int; }\n", "unsupported ABI");
  expectCompileError("extern \"C\" { let x = 1; }\n", "non-fn inside extern");
  expectCompileError("extern \"C\" { fn f(): int { return 1; } }\n", "extern body");
  expectCompileError("extern \"C${1}\" { fn f(): int; }\n", "interpolated ABI");
}

void testExternStructByValueIsAbiGated() {
  // A struct by value crosses the C boundary only where the marshalling's ABI
  // holds: the System V AMD64 register classification. On a target whose ABI
  // passes a struct through memory (the Microsoft x64 ABI on Windows) the
  // declaration is rejected at compile time instead of crashing at the call.
  const char* source =
      "struct Pair { a: int; b: int; }\n"
      "extern \"C\" { fn f(p: Pair): int; }\n";
  if (ffi::kStructByValueSupported) {
    expectCompiles(source);
  } else {
    expectCompileError(source, "struct cannot cross the C boundary by value");
  }
}

void testExternCallbackSignatureIsRecordedOnTheProto() {
  // A callback parameter is recorded as an `isCallback` spec with its scalar
  // parameter and return names, so the runtime can build the trampoline and the
  // AOT backend (PHASE 7) can read the same shape off the proto to emit a
  // compile-time stub. `void` is the default return when none is written.
  Bytecode bc = compileSource(
      "extern \"C\" {\n"
      "  fn apply(f: fn(int, double): int, x: int): int;\n"
      "  fn visit(f: fn(int): void): void;\n"
      "}\n");
  assert(bc.externs.size() == 2);

  const ExternSymbol& apply = bc.externs[0];
  assert(apply.name == "apply");
  assert(apply.paramTypes.size() == 2);
  assert(apply.paramTypes[0].isCallback);
  assert(apply.paramTypes[0].callbackParams.size() == 2);
  assert(apply.paramTypes[0].callbackParams[0] == "int");
  assert(apply.paramTypes[0].callbackParams[1] == "double");
  assert(apply.paramTypes[0].callbackReturn == "int");
  assert(!apply.paramTypes[1].isCallback);

  const ExternSymbol& visit = bc.externs[1];
  assert(visit.paramTypes.size() == 1);
  assert(visit.paramTypes[0].isCallback);
  assert(visit.paramTypes[0].callbackParams.size() == 1);
  assert(visit.paramTypes[0].callbackReturn == "void");

  // A callback is a parameter only; as a return type it is rejected, and a
  // non-scalar inside a callback signature too.
  expectCompileError("extern \"C\" { fn get(): fn(int): int; }\n", "callback return type");
  expectCompileError("extern \"C\" { fn apply(f: fn(map): int): int; }\n",
                     "non-scalar callback parameter");
}

void testCyclicImportIsRejected() {
  // A cycle (a -> b -> a) must be reported as a compile error instead of
  // recursing until the compiler overflows the stack. The loader maps dotted
  // paths to fixed sources, so the cycle is exercised without touching disk.
  auto parseModule = [](const std::string& source) {
    return Parser(lex(source), "<module>").parseProgram();
  };

  Compiler compiler;
  compiler.setImportLoader([&](const std::string& path, int, int) -> StmtPtr {
    if (path == "cyclic.a") return parseModule("import cyclic.b;\nfn a() { return 1; }\n");
    if (path == "cyclic.b") return parseModule("import cyclic.a;\nfn b() { return 2; }\n");
    throw CompileError{"module '" + path + "' not found", 1, 1};
  });

  bool threw = false;
  try {
    compiler.compile(parseModule("import cyclic.a;\n"));
  } catch (const CompileError& error) {
    threw = true;
    assert(error.message.find("cyclic import detected") != std::string::npos &&
           "the cycle must be named in the message");
  }
  assert(threw && "a cyclic import must raise a CompileError");

  // A self-import is the shortest cycle and must be caught too.
  Compiler selfCompiler;
  selfCompiler.setImportLoader([&](const std::string& path, int, int) -> StmtPtr {
    if (path == "cyclic.self") return parseModule("import cyclic.self;\n");
    throw CompileError{"module '" + path + "' not found", 1, 1};
  });
  bool selfThrew = false;
  try {
    selfCompiler.compile(parseModule("import cyclic.self;\n"));
  } catch (const CompileError& error) {
    selfThrew = true;
    assert(error.message.find("cyclic import detected") != std::string::npos);
  }
  assert(selfThrew && "a self-import must raise a CompileError");
}

}  // namespace

int main() {
  testCaptureLivesInTheFunctionThatUsesTheVariable();
  testNestedCaptureIsReexportedByTheMiddleFunction();
  testCapturesFollowOrderOfFirstUse();
  testVariableUsedTwiceIsCapturedOnce();
  testFunctionCapturesLocalOfTheMainChunk();
  testNothingIsCapturedFromTheOutermostFunction();
  testArithmeticAndPrint();
  testNumericConstantTypes();
  testLeftAssociativePrecedence();
  testStringConstant();
  testNewlineNormalization();
  testBoolAndNilLiterals();
  testVarWithoutInitializer();
  testModuloAndPower();
  testComparisons();
  testUnaryOperators();
  testCompoundAssignment();
  testLetReassignment();
  testAssignment();
  testConstantDeduplication();
  testIfElseJumps();
  testIfWithoutElse();
  testWhileBackpatch();
  testForBackpatch();
  testBreakAndContinueBackpatch();
  testSwitchBytecode();
  testSwitchRangeUsesIn();
  testSwitchErrors();
  testSwitchSignedLabels();
  testDeferCompiles();
  testDeferRunsOnFallThrough();
  testDeferErrors();
  testDeferNestedConstructsCompile();
  testBlockScopingBytecode();
  testShadowingSlots();
  testLocalImmutableAssignment();
  testLogicalShortCircuit();
  testTernary();
  testBitwiseOps();
  testFunctionProtosAndCalls();
  testFunctionHoisting();
  testMultiArgumentPrint();
  testUnsupportedConstructs();
  testPhase6ConstructsCompile();
  testNestedFunctionDeclarationsAreSupported();
  testTypeAnnotationsAreErased();
  testTypeAnnotationsAccepted();
  testTypeMismatchErrors();
  testNullCoalesceBytecode();
  testSafeMemberBytecode();
  testNonNullBytecode();
  testSliceBytecode();
  testStructDeclEmitsNoBytecode();
  testStructLiteralLowersToTable();
  testStructErrors();
  testStructNamesAreUsableAsTypes();
  testTryCatchBytecode();
  testTryCatchErrors();
  testVoidReturnType();
  testVoidErrors();
  testFfiTypeVocabulary();
  testExternBlock();
  testExternStructByValueIsAbiGated();
  testExternCallbackSignatureIsRecordedOnTheProto();
  testCyclicImportIsRejected();

  std::puts("compiler tests passed");
  return 0;
}