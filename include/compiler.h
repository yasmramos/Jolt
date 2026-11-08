#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.h"
#include "bytecode.h"
#include "errors.h"

// One resolved import: a module's compiled body plus the names it exports.
//
// `code` is a proto that, when run, evaluates the module's top-level bindings
// as locals of its own frame and returns a table mapping each public name to
// its value. Keeping the bindings local to the module frame is what stops a
// module's internals from leaking into the importing program's globals.
struct ImportedModule {
  std::string path;                    // dotted module path, e.g. "std.math"
  std::shared_ptr<Bytecode> code;      // module body, run once per program
  std::vector<std::string> symbols;    // public names, in declaration order
};

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
//   - `switch` on literals and integer ranges (no fallthrough)
//   - Java-style `import` (qualified, wildcard, alias and selective forms)
//     with compile-time conflict detection
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

  // Forgets the definitions carried from one compilation to the next: global
  // immutability and types, `struct` shapes, native signatures, and the
  // imported modules and namespaces they brought in. `compile` resets the
  // session automatically, so a file always starts clean. The REPL keeps it
  // across entries and clears it on `:clear`, so its behaviour matches one
  // accumulating program instead of a sequence of unrelated ones.
  void clearSession();

  // The `struct` names declared so far in the session. The REPL hands them to
  // the next entry's parser (see Parser::seedStructNames) so a literal or an
  // annotation written after the declaration is still recognized.
  std::unordered_set<std::string> structNames() const {
    std::unordered_set<std::string> names;
    for (const auto& entry : structDefs_) names.insert(entry.first);
    return names;
  }

  // Provides the module loader used to resolve `import` declarations. The
  // callback receives a dotted module path and the position of the import
  // declaration, and returns the module's parsed program, or throws
  // `CompileError` ("module 'X' not found") when it cannot be resolved. The
  // compiler stays free of any file-system or resolver dependency, which keeps
  // the toolchain boundary (AGENT.md §2) intact; the CLI injects the loader,
  // and tests inject their own. Without a loader, `import` reports that the
  // module cannot be resolved.
  using ImportLoader = std::function<StmtPtr(const std::string& path, int line, int col)>;
  void setImportLoader(ImportLoader loader) { importLoader_ = std::move(loader); }

  // Compiles a program that runs *in the current session* instead of starting
  // a clean one. `:load` uses this so a loaded file sees the session's structs,
  // types and imports, and merges its own definitions into the session, unlike
  // `compile` which clears it. `loader` resolves the file's own imports
  // relative to the file; it is used only while this call compiles. The
  // session is snapshotted first, so a file that fails to compile leaves it
  // untouched. `name` labels the chunk (the file path) for diagnostics.
  Bytecode compileLoaded(const StmtPtr& program, ImportLoader loader, const std::string& name);

  // Compiles a module body into a standalone proto: its top-level bindings are
  // locals of the proto's frame (not globals) and the proto returns a table of
  // its public names. `path` is the dotted module path. Used by the module
  // loader when it compiles an imported file.
  ImportedModule importModule(const StmtPtr& program, const std::string& path);

 private:
  struct LocalVar {
    std::string name;
    int slot = 0;
    bool isImmutable = false;
    // Declared or inferred type (PHASE 6.5). Empty means "unknown"/any. It is
    // compile-time metadata only: it never reaches the bytecode.
    TypeRef type;
    // True when this binding is a `native fn` declaration. A call to the name
    // is checked against the native's signature; a plain local that shadows
    // the name is a different function, so the check is skipped for it.
    bool isNative = false;
  };

  struct Scope {
    std::unordered_map<std::string, LocalVar> vars;
    int baseSlot = 0;
    // Deferred blocks (`defer { ... }`) declared directly in this scope, in
    // source order. They are not emitted where they appear: the compiler runs
    // them (in reverse order) at every point where the scope exits -- normal
    // fall-through, `return`, or `break`/`continue` that leaves the scope.
    std::vector<StmtPtr> defers;
  };

  struct Loop {
    std::vector<int> breaks;
    std::vector<int> continues;
    // Deepest scope whose deferred blocks `break`/`continue` must run before
    // jumping out: a 1-based scope count, so `emitDeferredBlocks(deferMinDepth)`
    // runs every scope from the innermost down to and including it.
    size_t deferMinDepth = 0;
  };

  // Marks that the deferred body currently being emitted is a `defer`
  // boundary. A `break`/`continue` that would leave that boundary is rejected,
  // because the deferred block runs at a scope exit and letting it jump out
  // would make the execution order ambiguous. `loopDepth` is `loops_.size()`
  // when the body started, so a loop declared *inside* the deferred body (which
  // raises the count) may still be broken out of. `return` needs no field: the
  // whole stack is cleared while compiling a nested function, so a `return`
  // there always belongs to that function.
  struct DeferCtx {
    size_t loopDepth = 0;
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
    std::vector<DeferCtx> deferStack;
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
  // One entry per `defer` body currently being emitted (innermost last). A
  // `break`/`continue` that would leave the innermost defer boundary is
  // rejected; the whole stack is cleared while compiling a nested function.
  std::vector<DeferCtx> deferStack_;
  // Set by `compileTry` when it emits the conditional `RETURN` of a `try`. It
  // lets a trailing `return try f();` recover the normal return: the last
  // instruction is a RETURN, but it only runs on the error path, so the
  // "already returns" shortcut in `compileReturn` must not apply.
  bool lastWasConditionalReturn_ = false;

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

  // Names the importing module declares itself (`let`/`var`/`fn`) in its global
  // scope. A wildcard or selective import must not silently overwrite one of
  // them: the local binding wins, whatever the order of the declarations. Kept
  // separate from `globalConsts_`, which is about immutability (const folding),
  // not about shadowing. Reset per chunk.
  std::unordered_set<std::string> userGlobals_;

  // Type of every global binding, for annotation checks (PHASE 6.5). Kept
  // parallel to `globalConsts_`; both are reset per chunk.
  std::unordered_map<std::string, TypeRef> globalTypes_;

  // Field shapes of the `struct` types declared so far (PHASE 6.5), keyed by
  // type name. A struct is compile-time metadata: the declaration emits no
  // bytecode, and a `Point { ... }` literal is checked against this table
  // before it is lowered to a plain map. Reset per chunk.
  std::unordered_map<std::string, std::vector<StructField>> structDefs_;

  // Signature of every `native fn` declared so far (PHASE 6.5), keyed by name.
  // A native declaration emits no call to the compiler: it only records the
  // signature so a call can be checked (arity and argument types) and so the
  // return type can be inferred. Reset per chunk.
  struct NativeSig {
    std::vector<Param> params;
    TypeRef returnType;
  };
  std::unordered_map<std::string, NativeSig> nativeSigs_;

  // Declared return type of the function currently being compiled, checked
  // against every `return` value. Empty means "unannotated".
  TypeRef currentReturnType_;

  // Infers the static type of an expression when it is evident from its shape
  // (a literal, or a variable whose type is known). Returns an empty TypeRef
  // when the type cannot be determined, which means "no check".
  TypeRef inferExprType(const Expr& expr) const;

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

  // Scopes and variables. A module body is compiled at scope depth 0 but its
  // bindings must stay local to the module frame, so it is not a global scope.
  bool inGlobalScope() const {
    return !inFunction_ && !moduleBody_ && scopes_.size() <= 1;
  }
  void pushScope();
  void popScope();
  int allocLocalSlot();
  int declareLocal(const std::string& name, bool isImmutable, const TypeRef& type = TypeRef{});
  bool resolveLocal(const std::string& name, LocalVar& out) const;
  void declareGlobal(const std::string& name, bool isImmutable);

  // Type annotations (PHASE 6.5). Types are compile-time metadata: they are
  // inferred from literals, checked against annotations, and then erased, so
  // the bytecode is identical with or without them.
  void declareGlobalType(const std::string& name, const TypeRef& type);
  bool lookupGlobalType(const std::string& name, TypeRef& out) const;
  // Checks that a value of type `from` may be stored in a slot of type `to`,
  // reporting a compile error at `line`/`col` when it may not. `context`
  // names the target for the message.
  void checkAssignable(const TypeRef& from, const TypeRef& to, const std::string& context,
                       int line, int col);
  // The non-union part of the assignment rule, as a message ("" means allowed).
  // The union path in `checkAssignable` probes members with it without failing.
  std::string assignError(const TypeRef& from, const TypeRef& to,
                          const std::string& context) const;

  // Checks a call to a declared `native fn` against its recorded signature
  // (PHASE 6.5): the arity must match and every argument must be assignable to
  // its parameter type. An unannotated parameter (or `any`) skips the type
  // check for that argument.
  void checkNativeCall(const std::string& name, const NativeSig& sig, const CallExpr& call,
                       int line, int col);

  // Returns the signature of the `native fn` that `name` refers to, or null
  // when it is not a native binding. The name is resolved like a variable
  // read: a local or an upvalue that shadows a native name is a different
  // function and yields null. A name that resolves to nothing is a global, so
  // it matches when it is a declared native. Types are metadata only.
  const NativeSig* nativeSigFor(const std::string& name) const;

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
  int compileTry(const TryExpr& node, int line, int col);
  int compileCatch(const CatchExpr& node, int line, int col);
  int compileCall(const CallExpr& call, int line, int col);
  int compileArray(const ArrayExpr& array);
  int compileMap(const MapExpr& map);
  int compileStructLiteral(const StructLitExpr& literal, int line, int col);
  int compileRange(const RangeExpr& range, int line, int col);
  int compileSlice(const SliceExpr& slice, int line, int col);
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
  // Registers every `struct` declared directly in `block` before anything else
  // in it is compiled, so a hoisted function that builds a struct literal sees
  // the shape even when the declaration appears later in the file.
  void registerStructDecls(const BlockStmt& block);
  void compileStructDecl(const StructDeclStmt& decl, int line, int col);
  // Registers every `native fn` declared directly in `block` before anything
  // else in it is compiled, so a hoisted function that calls one is checked
  // against its signature. It also descends into `extern "C"` blocks: their
  // declarations are natives too.
  void registerNativeDecls(const BlockStmt& block);
  // Compiles an `extern "C" { ... }` block (PHASE 6.6): each declaration
  // follows the `native fn` path, so the block emits no bytecode of its own.
  void compileExternBlock(const ExternBlockStmt& block, int line, int col);
  // Resolves an `extern "C"` type name to a scalar or struct spec (PHASE 6.6).
  ffi::TypeSpec resolveTypeSpec(const TypeRef& type);
  // Rejects a union where a type must cross the FFI boundary (PHASE 6.7.4).
  void rejectUnionForFfi(const TypeRef& type, const std::string& position, int line, int col);
  void compileReturn(const ReturnStmt& stmt);
  void compileIf(const IfStmt& stmt);
  void compileWhile(const WhileStmt& stmt);
  void compileFor(const ForStmt& stmt);
  void compileForIn(const ForInStmt& stmt);
  void compileSwitch(const SwitchStmt& stmt);
  void compileBreak(const Stmt& stmt);
  void compileContinue(const Stmt& stmt);
  void compileDefer(const DeferStmt& stmt);
  // Emits the deferred blocks of every scope from the innermost down to (and
  // including) `minDepth`, in LIFO order, so the code at that exit point runs
  // them. Called at normal scope exit, before `return`, and before the jump
  // emitted by `break`/`continue`. `minDepth` is a 1-based scope count.
  void emitDeferredBlocks(size_t minDepth);

  void resetChunk(const std::string& name = "<main>");

  // Everything that outlives one chunk: the definitions a REPL session
  // carries from one entry to the next. `snapshotSession`/`restoreSession`
  // make a compile transactional (a definition is committed only when its
  // entry compiles) and let `compileLoaded` merge a loaded file's state into
  // the session.
  struct SessionSnapshot {
    std::unordered_set<std::string> globalConsts;
    std::unordered_set<std::string> userGlobals;
    std::unordered_map<std::string, TypeRef> globalTypes;
    std::unordered_map<std::string, std::vector<StructField>> structDefs;
    std::unordered_map<std::string, NativeSig> nativeSigs;
    std::map<std::string, ImportedModule> loadedModules;
    std::set<std::string> materializedModules;
    std::set<std::string> namespaces;
    std::set<std::string> boundImports;
    std::unordered_map<std::string, std::string> wildcardBindings;
  };

  SessionSnapshot snapshotSession() const;
  void restoreSession(SessionSnapshot snapshot);

  // The shared body of `compile` and `compileLoaded`: compiles a program in the
  // global scope with whatever session state the caller has set up. It does not
  // touch the session or the import loader, so `compile` (clean session) and
  // `compileLoaded` (current session, file loader) both delegate here.
  Bytecode compileTopLevelBody(const StmtPtr& program);

  // The body of `compileRepl`, split out so the public wrapper can restore the
  // session state when the entry throws (a definition is committed only when
  // its entry compiles).
  Bytecode compileReplEntry(const StmtPtr& program);

  // Compiles `body` as a child proto of the chunk being built and returns its
  // index in `bc_.children`. The parent compiler state is saved and restored.
  // `returnType` is the declared return annotation, checked against every
  // `return` (PHASE 6.5); an empty TypeRef means "unannotated".
  int compileFunctionProto(const std::string& name, const std::vector<Param>& params,
                           const StmtPtr& body, int line, int col,
                           const TypeRef& returnType);

  // Compiles a module's top-level statements into `bc_` (bindings stay
  // frame-local) and returns its public surface as a table. Called by
  // `importModule` while `moduleBody_` is set.
  void compileModuleBody(const StmtPtr& program);

  // Resolves an import path once per compiler and caches the result, so two
  // imports of the same module share one proto and one symbols list.
  const ImportedModule& loadModule(const std::string& path, int line, int col);

  // Compiles an `import` declaration: loads the module, then binds its names
  // according to the import mode, detecting wildcard conflicts.
  void compileImport(const ImportStmt& import, int line, int col);

  // Emits (once) the code that runs a module body and stores its exported
  // table in the hidden global `$module$<path>`. Later imports of the same
  // module reuse that global instead of running the body again.
  void materializeModule(const ImportedModule& module, int line, int col);

  // Compiles `import <path>;` / `import <path> as <alias>;`. The module table
  // becomes a real runtime value reachable through the dotted path (`std.math`
  // or the alias `m`), so qualified access uses the ordinary member chain.
  void compileQualifiedImport(const ImportedModule& module, const std::string& boundName,
                              int line, int col);

  // Compiles a wildcard or selective import, binding each imported name to a
  // global. Reports an ambiguity when two `.*` imports export the same name.
  void compileNameImport(const ImportedModule& module, const std::vector<std::string>& names,
                         int line, int col);

  // Creates the namespace prefix `prefix` (e.g. "std" or "std.math") as a
  // global map if it was not created by an earlier import, and returns the
  // register holding the deepest existing map on the path.
  int ensureNamespace(const std::string& prefix, int line, int col);

  // The global name a visible import binding maps to while a module body is
  // being compiled. At the top level this is the visible name itself; inside a
  // module it is a hidden per-module name (`$imp$<module>$<name>`), so a
  // module's own imports never reach the importing program's globals (AGENT.md
  // §18.2). Every global read/write and every import binding goes through this
  // so the two agree.
  std::string importBindingName(const std::string& visible) const;

  // Walks a module body's top-level imports and records the hidden global each
  // introduced name maps to (see `importBindingName`). Runs before the body is
  // compiled, so a hoisted function that references an imported name already
  // resolves it to the module-local binding.
  void predeclareModuleImports(const BlockStmt& block);

  ImportLoader importLoader_;
  std::map<std::string, ImportedModule> loadedModules_;
  // Module paths currently being loaded, outermost first. A path that is
  // already on the stack when it is requested again closes a cycle
  // (`a -> b -> a`), which is reported as a compile error instead of
  // recursing until the compiler overflows the stack.
  std::vector<std::string> loadingStack_;
  // Set while compiling a module body: its top-level declarations become
  // frame locals and are exported instead of becoming globals.
  bool moduleBody_ = false;
  // Module paths whose exported table has already been stored in its hidden
  // global, so the module body runs at most once per program.
  std::set<std::string> materializedModules_;
  // Namespace prefixes already created as runtime maps ("std", "std.math"),
  // so repeated imports reuse the same table instead of replacing it.
  std::set<std::string> namespaces_;
  // Bound qualified names ("std.math", or the alias "m"), so a repeated
  // import of the same module does not bind twice.
  std::set<std::string> boundImports_;
  // Unqualified names already introduced by an `import ... .*`, mapped to the
  // module that brought them in, so a second wildcard exporting the same name
  // is reported as ambiguous.
  std::unordered_map<std::string, std::string> wildcardBindings_;

  // Path of the module body currently being compiled. Empty at the top level.
  // Used to build the hidden global names a module's own imports bind to.
  std::string modulePath_;
  // Visible names that the module being compiled introduced through `import`.
  // A read/write of one of these names inside the module goes to its hidden
  // per-module global instead of the program's global table, so a module's
  // imports never leak into the importing program (AGENT.md §18.2).
  std::unordered_set<std::string> moduleImportNames_;
};

// Shortcut equivalent to `Compiler{}.compile(program)`.
Bytecode compileProgram(const StmtPtr& program);
