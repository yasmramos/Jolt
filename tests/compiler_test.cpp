// Tests for the AST -> bytecode compiler (PHASE 2-5).
// Uses no external framework: only assert and a failure counter.

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

void testArithmeticAndPrint() {
  Bytecode bc = compileSource("let x = 1 + 2;\nprint(x);\n");

  assert(bc.code.size() == 7);

  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[0].a == 0 && bc.code[0].b == 0);
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[1].a == 1 && bc.code[1].b == 1);
  assert(bc.code[2].op == OpCode::ADD);
  assert(bc.code[2].a == 0 && bc.code[2].b == 0 && bc.code[2].c == 1);
  assert(bc.code[3].op == OpCode::SETGLOBAL);
  assert(bc.code[3].a == 0 && bc.code[3].b == 0);
  assert(bc.code[4].op == OpCode::GETGLOBAL);
  assert(bc.code[4].a == 0 && bc.code[4].b == 0);
  assert(bc.code[5].op == OpCode::PRINT);
  assert(bc.code[5].a == 0);
  assert(bc.code[6].op == OpCode::HALT);

  assert(bc.numbers.size() == 2);
  assert(bc.numbers[0].isInt() && bc.numbers[0].asInt() == 1);
  assert(bc.numbers[1].isInt() && bc.numbers[1].asInt() == 2);
  assert(bc.strings.size() == 1 && bc.strings[0] == "x");
  assert(bc.num_registers == 2);
}

void testNumericConstantTypes() {
  Bytecode bc = compileSource("let a = 1;\nlet b = 1.0;\n");

  // int and float must not be merged in the constant pool.
  assert(bc.numbers.size() == 2);
  assert(bc.numbers[0].isInt() && bc.numbers[0].asInt() == 1);
  assert(bc.numbers[1].isFloat() && bc.numbers[1].asFloat() == 1.0);
}

void testLeftAssociativePrecedence() {
  Bytecode bc = compileSource("print(1 + 2 * 3);\n");

  assert(bc.code.size() == 7);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::MUL);
  assert(bc.code[4].op == OpCode::ADD);
  assert(bc.code[5].op == OpCode::PRINT);
  assert(bc.code[6].op == OpCode::HALT);
  assert(bc.num_registers == 3);
}

void testStringConstant() {
  Bytecode bc = compileSource("let greeting = \"hi\";\n");

  assert(bc.code.size() == 3);
  assert(bc.code[0].op == OpCode::LOADKS);
  assert(bc.code[0].a == 0 && bc.code[0].b == 0);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[1].a == 0 && bc.code[1].b == 1);
  assert(bc.code[2].op == OpCode::HALT);

  assert(bc.strings.size() == 2);
  assert(bc.strings[0] == "hi");
  assert(bc.strings[1] == "greeting");
  assert(bc.num_registers == 1);
}

void testBoolAndNilLiterals() {
  Bytecode bc = compileSource("let a = true;\nlet b = false;\nlet c = nil;\n");

  assert(bc.code.size() == 7);
  assert(bc.code[0].op == OpCode::LOADBOOL);
  assert(bc.code[0].a == 0 && bc.code[0].b == 1);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[2].op == OpCode::LOADBOOL);
  assert(bc.code[2].a == 0 && bc.code[2].b == 0);
  assert(bc.code[3].op == OpCode::SETGLOBAL);
  assert(bc.code[4].op == OpCode::LOADNIL);
  assert(bc.code[4].a == 0);
  assert(bc.code[5].op == OpCode::SETGLOBAL);
  assert(bc.code[6].op == OpCode::HALT);
}

void testVarWithoutInitializer() {
  Bytecode bc = compileSource("var x;\n");

  // `var x;` must emit LOADNIL explicitly, not leave the register undefined.
  assert(bc.code.size() == 3);
  assert(bc.code[0].op == OpCode::LOADNIL);
  assert(bc.code[0].a == 0);
  assert(bc.code[1].op == OpCode::SETGLOBAL);
  assert(bc.code[2].op == OpCode::HALT);
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
  assert(neg.code[1].op == OpCode::NEG);
  assert(neg.code[1].a == 0 && neg.code[1].b == 0);

  Bytecode notBc = compileSource("let y = !1;\n");
  assert(notBc.code.size() == 4);
  assert(notBc.code[1].op == OpCode::NOT);

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
  assert(bc.code[2].a == 0 && bc.code[2].b == 0);
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::ADD);
  assert(bc.code[4].a == 0 && bc.code[4].b == 0 && bc.code[4].c == 1);
  assert(bc.code[5].op == OpCode::SETGLOBAL);
  assert(bc.code[5].a == 0 && bc.code[5].b == 0);
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
  assert(bc.code[1].a == 0 && bc.code[1].b == 0);
  assert(bc.code[2].op == OpCode::HALT);
  assert(bc.num_registers == 1);
}

void testConstantDeduplication() {
  Bytecode bc = compileSource("print(1 + 1);\n");

  assert(bc.numbers.size() == 1);
  assert(bc.strings.empty());
}

// --------------------------------------------------------- PHASE 5: control

void testIfElseJumps() {
  Bytecode bc = compileSource("if (true) { print(1); } else { print(2); }\n");

  // 0 LOADBOOL, 1 JMP_IF_FALSE, 2 LOADK, 3 PRINT,
  // 4 JMP, 5 LOADK, 6 PRINT, 7 HALT
  assert(bc.code.size() == 8);
  assert(bc.code[0].op == OpCode::LOADBOOL);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].a == 0 && bc.code[1].jump == 5);  // jumps to the `else`
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::PRINT);
  assert(bc.code[4].op == OpCode::JMP);
  assert(bc.code[4].jump == 7);  // end of the if/else, before HALT
  assert(bc.code[5].op == OpCode::LOADK);
  assert(bc.code[6].op == OpCode::PRINT);
  assert(bc.code[7].op == OpCode::HALT);
  assert(bc.num_locals == 0);
}

void testIfWithoutElse() {
  Bytecode bc = compileSource("if (false) { print(1); }\n");

  assert(bc.code.size() == 5);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 4);  // to HALT
  assert(bc.code[4].op == OpCode::HALT);
}

void testWhileBackpatch() {
  Bytecode bc = compileSource("while (false) { print(1); }\n");

  // 0 LOADBOOL, 1 JMP_IF_FALSE, 2 LOADK, 3 PRINT, 4 JMP, 5 HALT
  assert(bc.code.size() == 6);
  assert(bc.code[0].op == OpCode::LOADBOOL);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 5);  // leaves the loop
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::PRINT);
  assert(bc.code[4].op == OpCode::JMP);
  assert(bc.code[4].jump == 0);  // goes back to re-evaluate the condition
  assert(bc.code[5].op == OpCode::HALT);
}

void testForBackpatch() {
  Bytecode bc = compileSource("for (var i = 0; i < 3; i += 1) { print(i); }\n");

  // 0 LOADK, 1 DECLLOCAL, 2 GETLOCAL, 3 LOADK, 4 LT, 5 JMP_IF_FALSE,
  // 6 GETLOCAL, 7 PRINT, 8 GETLOCAL, 9 LOADK, 10 ADD, 11 SETLOCAL,
  // 12 JMP, 13 HALT
  assert(bc.code.size() == 14);
  assert(bc.code[1].op == OpCode::DECLLOCAL);
  assert(bc.code[1].a == 0 && bc.code[1].b == 0);
  assert(bc.code[2].op == OpCode::GETLOCAL);
  assert(bc.code[4].op == OpCode::LT);
  assert(bc.code[5].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[5].jump == 13);  // leaves to HALT
  assert(bc.code[6].op == OpCode::GETLOCAL);
  assert(bc.code[7].op == OpCode::PRINT);
  assert(bc.code[8].op == OpCode::GETLOCAL);
  assert(bc.code[11].op == OpCode::SETLOCAL);
  assert(bc.code[11].b == 0);
  assert(bc.code[12].op == OpCode::JMP);
  assert(bc.code[12].jump == 2);  // goes back to the condition

  // The `for` initializer is local, not global.
  assert(bc.num_locals == 1);
  assert(bc.num_registers == 2);
}

void testBreakAndContinueBackpatch() {
  Bytecode brk = compileSource("while (true) { break; }\n");
  // 0 LOADBOOL, 1 JMP_IF_FALSE, 2 JMP (break), 3 JMP (back), 4 HALT
  assert(brk.code.size() == 5);
  assert(brk.code[2].op == OpCode::JMP && brk.code[2].jump == 4);
  assert(brk.code[3].op == OpCode::JMP && brk.code[3].jump == 0);

  Bytecode cont = compileSource("while (true) { continue; }\n");
  assert(cont.code.size() == 5);
  assert(cont.code[2].op == OpCode::JMP && cont.code[2].jump == 0);

  // In a `for`, `continue` jumps to the step, not to the condition.
  Bytecode forCont = compileSource("for (;;) { continue; }\n");
  // 0 JMP (continue -> step), 1 JMP (back), 2 HALT
  assert(forCont.code.size() == 3);
  assert(forCont.code[0].op == OpCode::JMP && forCont.code[0].jump == 1);
  assert(forCont.code[1].op == OpCode::JMP && forCont.code[1].jump == 0);

  expectCompileError("break;\n", "break outside loop");
  expectCompileError("continue;\n", "continue outside loop");
}

void testBlockScopingBytecode() {
  Bytecode bc = compileSource(
      "{\n"
      "  var x = 1;\n"
      "  x = 2;\n"
      "}\n");

  // 0 LOADK, 1 DECLLOCAL, 2 LOADK, 3 SETLOCAL, 4 HALT
  assert(bc.code.size() == 5);
  assert(bc.code[1].op == OpCode::DECLLOCAL);
  assert(bc.code[1].b == 0);
  assert(bc.code[3].op == OpCode::SETLOCAL);
  assert(bc.code[3].b == 0);
  assert(bc.num_locals == 1);
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
  assert(bc.code[4].op == OpCode::GETLOCAL && bc.code[4].b == 1);
  assert(bc.code[6].op == OpCode::GETLOCAL && bc.code[6].b == 0);
  assert(bc.num_locals == 2);
}

void testLocalImmutableAssignment() {
  expectCompileError("{ let x = 1; x = 2; }\n", "local immutable reassignment");
  expectCompileError("{ let x = 1; x += 2; }\n", "local immutable compound assignment");
}

void testLogicalShortCircuit() {
  Bytecode andBc = compileSource("let a = 1 && 2;\n");
  // 0 LOADK, 1 JMP_IF_FALSE, 2 LOADK, 3 MOVE, 4 SETGLOBAL, 5 HALT
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

  // 0 LOADBOOL, 1 JMP_IF_FALSE, 2 LOADK, 3 MOVE, 4 JMP,
  // 5 LOADK, 6 MOVE, 7 SETGLOBAL, 8 HALT
  assert(bc.code.size() == 9);
  assert(bc.code[0].op == OpCode::LOADBOOL);
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
  assert(bc.code[21].op == OpCode::BNOT);
  assert(bc.code[21].a == 0 && bc.code[21].b == 0);
}

void testUnsupportedConstructs() {
  // PHASE 6: functions, calls, collections and iteration.
  expectCompileError("fn f() { return 1; }\n", "function declaration");
  expectCompileError("print(1, 2);\n", "multiple print arguments");
  expectCompileError("foo(1);\n", "arbitrary call");
  expectCompileError("let x = [1, 2];\n", "array literal");
  expectCompileError("let m = {a: 1};\n", "map literal");
  expectCompileError("let s = \"a${b}\";\n", "string interpolation");
  expectCompileError("let r = 1..2;\n", "range");
  expectCompileError("let f = x => x;\n", "lambda");
  expectCompileError("for (x in 1..2) { print(x); }\n", "for-in loop");
  expectCompileError("let x = 1 in 2;\n", "in operator");
  expectCompileError("return 1;\n", "return statement");
  expectCompileError("let x = y[0];\n", "index expression");
  expectCompileError("let x = y.z;\n", "member expression");
}

}  // namespace

int main() {
  testArithmeticAndPrint();
  testNumericConstantTypes();
  testLeftAssociativePrecedence();
  testStringConstant();
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
  testBlockScopingBytecode();
  testShadowingSlots();
  testLocalImmutableAssignment();
  testLogicalShortCircuit();
  testTernary();
  testBitwiseOps();
  testUnsupportedConstructs();

  std::puts("compiler tests passed");
  return 0;
}
