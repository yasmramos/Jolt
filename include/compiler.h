#pragma once

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "errors.h"

// AST to bytecode compiler (PHASE 2-5).
//
// Translates the language into the register-based instructions of
// `include/bytecode.h`. When it meets a node that does not belong to the
// current phase yet, it throws `CompileError` with line/column, so the
// pipeline can report it in the same format as the parser.
//
// Current scope:
//   - nil, boolean, numeric and string literals
//   - global variables and block-local variables
//   - simple and compound assignment
//   - arithmetic (+ - * / % **), comparisons, unary operators and bitwise ops
//   - if/else, while, classic for, break and continue
//   - short-circuit operators && and ||, and the ternary operator
//   - print(expr) with a single argument
//   - top-level `fn` declarations, parameters, `return` and general calls
//   - lambdas, nested/local `fn` declarations and closures over captured
//     locals (upvalues resolved at compile time)
//   - arrays, maps, ranges, interpolation, indexing, members and `in`
//   - `for-in` over arrays, strings, maps and ranges
class Compiler {
 public:
  Compiler() = default;

  // Compiles a whole program. The resulting bytecode ends with HALT.
  Bytecode compile(const StmtPtr& program);

  // Compiles a single expression into a chunk that ends with RETURN, so the
  // VM can hand the value back to the REPL. Globals persist across chunks.
  Bytecode compileExpression(const ExprPtr& expr);

  // Compiles a REPL entry. When the entry ends with an expression statement,
  // the chunk ends with RETURN so the VM hands its value back; otherwise it
  // ends with HALT like a regular program. Top-level declarations are hoisted
  // exactly as in `compile`.
  Bytecode compileRepl(const StmtPtr& program);

 private:
  struct LocalVar {
    std::string name;
    int slot = 0;
    bool isImmutable = false;
  };

  struct Scope {
    std::unordered_map<std::string, LocalVar> vars;
    int baseSlot = 0;
  };

  struct Loop {
    std::vector<int> breaks;
    std::vector<int> continues;
  };

  // One entry of the proto stack. `compileFunctionProto` saves the current
  // state, compiles the child body into a fresh chunk and restores the parent
  // afterwards, so upvalue resolution can look at every enclosing function at
  // once (a lambda inside a function body sees that body's locals as capture
  // candidates). The stack holds every frame except the one being compiled,
  // which lives in the plain members below.
  struct FuncState {
    Bytecode bc;
    std::vector<Scope> scopes;
    std::vector<Loop> loops;
    int nextReg = 0;
    int maxReg = 0;
    int nextLocal = 0;
    int maxLocal = 0;
    int line = 1;
    int col = 1;
    bool inFunction = false;
    // Capture list of this frame, keyed by variable name and parallel to
    // `bc.upvalues`, so a name that is used twice is captured once.
    std::unordered_map<std::string, int> upvalueNames;
    // The frame one level out, or null when this is the outermost function.
    // Valid pointers into `funcStack_` (deque nodes are stable); rebuilt on
    // every push/pop of the proto stack.
    FuncState* enclosing = nullptr;
  };

  // Upvalue resolution helpers (PHASE 6b). A function captures a variable it
  // uses but does not declare. The capture is recorded in the capture list of
  // the function that uses the variable (`proto`, with the name-keyed index
  // `names`), and its descriptor says where `CLOSURE` finds the cell when it
  // instantiates that function:
  //   - the variable is a local of the enclosing function (`parent`): the
  //     cell is that function's local slot (`inParentLocal`);
  //   - otherwise the enclosing function captures it too, and the cell is one
  //     of its upvalues, so nested closures share one cell per variable.
  // Returns -1 when no enclosing function declares the name, i.e. it is a
  // global.
  static bool resolveLocalIn(const std::vector<Scope>& scopes, const std::string& name,
                             LocalVar& out);
  static int resolveFromFrame(Bytecode& proto, std::unordered_map<std::string, int>& names,
                              FuncState* parent, const std::string& name);

  Bytecode bc_;
  int nextReg_ = 0;  // next free temporary register
  int maxReg_ = 0;   // maximum number of registers in use at once
  int nextLocal_ = 0;
  int maxLocal_ = 0;

  std::vector<Scope> scopes_;
  std::vector<Loop> loops_;

  // Slots reserved for the direct declarations of the block currently being
  // compiled, keyed by the statement node. The pre-pass in `compileBlockBody`
  // allocates them in source order and creates their cells before hoisted
  // functions are compiled, so a function declared after a block-local `let`
  // can capture it. `compileLet`/`compileFnDecl` reuse the reserved slot
  // instead of allocating another one.
  std::unordered_map<const LetStmt*, int> predeclaredLetSlots_;
  std::unordered_map<const FnDeclStmt*, int> predeclaredFnSlots_;
  // Proto stack: every enclosing function except the one being compiled.
  // A deque keeps FuncState addresses stable, so `enclosing` pointers stay
  // valid while more frames are pushed.
  std::deque<FuncState> funcStack_;
  std::unordered_set<std::string> globalConsts_;

  // Capture list of the frame currently being compiled, keyed by variable
  // name and parallel to `bc_.upvalues`.
  std::unordered_map<std::string, int> currentUpvalueNames_;

  // True while compiling a function body. It keeps block locals local even
  // when the body's own scope is the innermost one.
  bool inFunction_ = false;

  // Position of the node being compiled; stamped onto every emitted
  // instruction so the VM can report runtime errors with line/column.
  int currentLine_ = 1;
  int currentCol_ = 1;

  int allocReg();
  void freeReg(int count = 1);

  int addConstant(const Value& value);
  int addString(const std::string& value);

  int emit(OpCode op, int a = 0, int b = 0, int c = 0);
  int emitJump(OpCode op, int a = 0);
  void patchJump(int index, int target);
  int currentOffset() const;

  // Scopes and variables.
  bool inGlobalScope() const { return !inFunction_ && scopes_.size() <= 1; }
  void pushScope();
  void popScope();
  int allocLocalSlot();
  int declareLocal(const std::string& name, bool isImmutable);
  bool resolveLocal(const std::string& name, LocalVar& out) const;
  void declareGlobal(const std::string& name, bool isImmutable);

  // Upvalue resolution (PHASE 6b). Returns the index of `name` in the current
  // proto's capture list, adding the entry (and, transitively, the re-export
  // entries in every function in between) when it is not there yet. Returns -1
  // when the name is not a local or upvalue of any enclosing function, i.e. it
  // resolves to a global instead.
  int resolveUpvalue(const std::string& name);

  int compileExpr(const Expr& expr);
  int compileVariable(const std::string& name);
  int compileLambda(const LambdaExpr& lambda, int line, int col);
  int compileAssign(const AssignExpr& assign, int line, int col);
  int compileTernary(const TernaryExpr& expr);
  int compileCall(const CallExpr& call, int line, int col);
  int compileArray(const ArrayExpr& array);
  int compileMap(const MapExpr& map);
  int compileRange(const RangeExpr& range, int line, int col);
  int compileInterpolation(const InterpExpr& interpolation, int line, int col);
  // Converts `source` to a string through the compiler's private runtime
  // helper and returns the register holding the result.
  int compileToString(int source, int line, int col);
  // Emits a call to a hidden runtime helper (name not writable by user code).
  int compileHelperCall(const std::string& helper, const std::vector<int>& args, int line,
                        int col);

  void compileStmt(const Stmt& stmt);
  void compileBlockBody(const BlockStmt& block);
  void compileScopedBlock(const Stmt& stmt);
  void compileLet(const LetStmt& stmt);
  void compileFnDecl(const FnDeclStmt& fn, int line, int col);
  void compileReturn(const ReturnStmt& stmt);
  void compileIf(const IfStmt& stmt);
  void compileWhile(const WhileStmt& stmt);
  void compileFor(const ForStmt& stmt);
  void compileForIn(const ForInStmt& stmt);
  void compileBreak(const Stmt& stmt);
  void compileContinue(const Stmt& stmt);

  void resetChunk(const std::string& name = "<main>");

  // Compiles `body` as a child proto of the chunk being built and returns its
  // index in `bc_.children`. The parent compiler state is saved and restored.
  int compileFunctionProto(const std::string& name, const std::vector<std::string>& params,
                           const StmtPtr& body, int line, int col);
};

// Shortcut equivalent to `Compiler{}.compile(program)`.
Bytecode compileProgram(const StmtPtr& program);
