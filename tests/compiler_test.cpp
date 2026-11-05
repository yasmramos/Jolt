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

  // int and float must not be merged in the constant pool. Global names share
  // the pool, so the layout is 1, "a", 1.0, "b".
  assert(bc.constants.size() == 4);
  assert(bc.constants[0].isInt() && bc.constants[0].asInt() == 1);
  assert(bc.constants[2].isFloat() && bc.constants[2].asFloat() == 1.0);
}

void testLeftAssociativePrecedence() {
  Bytecode bc = compileSource("print(1 + 2 * 3);\n");

  // GETGLOBAL "print", LOADK 1, LOADK 2, LOADK 3, MUL, ADD, CALL 0, 1, HALT
  assert(bc.code.size() == 9);
  assert(bc.code[0].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[1].op == OpCode::LOADK);
  assert(bc.code[2].op == OpCode::LOADK);
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::MUL);
  assert(bc.code[5].op == OpCode::ADD);
  assert(bc.code[6].op == OpCode::CALL);
  assert(bc.code[6].a == 0 && bc.code[6].b == 1);
  assert(bc.code[7].op == OpCode::HALT);
  assert(bc.numRegisters == 3);
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

  assert(bc.constants.size() == 2);  // 1, "print"
  assert(bc.constants[0].isInt() && bc.constants[0].asInt() == 1);
  assert(bc.constants[1].isStr() && bc.constants[1].asStr() == "print");
}

// --------------------------------------------------------- PHASE 5: control

void testIfElseJumps() {
  Bytecode bc = compileSource("if (true) { print(1); } else { print(2); }\n");

  // LOADK true, JMP_IF_FALSE, GETGLOBAL "print", LOADK 1, CALL 0, 1,
  // JMP, GETGLOBAL "print", LOADK 2, CALL 0, 1, HALT
  assert(bc.code.size() == 12);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].a == 0 && bc.code[1].jump == 7);  // jumps to the `else`
  assert(bc.code[2].op == OpCode::GETGLOBAL);  // "print"
  assert(bc.code[3].op == OpCode::LOADK);
  assert(bc.code[4].op == OpCode::CALL);
  assert(bc.code[4].a == 0 && bc.code[4].b == 1);
  assert(bc.code[5].op == OpCode::JMP);
  assert(bc.code[5].jump == 11);  // end of the if/else, before HALT
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
  assert(bc.code.size() == 7);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 6);  // to HALT
  assert(bc.code[6].op == OpCode::HALT);
}

void testWhileBackpatch() {
  Bytecode bc = compileSource("while (false) { print(1); }\n");

  // LOADK false, JMP_IF_FALSE, GETGLOBAL "print", LOADK 1, CALL 0, 1, JMP, HALT
  assert(bc.code.size() == 8);
  assert(bc.code[0].op == OpCode::LOADK);
  assert(bc.code[1].op == OpCode::JMP_IF_FALSE);
  assert(bc.code[1].jump == 7);  // leaves the loop
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
  assert(bc.code.size() == 16);
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
}

void testNestedFunctionDeclarationsAreSupported() {
  // Regression guard: nested and block-level `fn` declarations compile since
  // PHASE 6b; they must not be reported as unsupported constructs anymore.
  expectCompiles("fn outer() { fn inner() {} }\n");
  expectCompiles("{ fn f() {} }\n");
  expectCompiles("let f = x => x;\n");
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

  std::puts("compiler tests passed");
  return 0;
}