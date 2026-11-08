#include "vm.h"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#include "ops.h"
#include "ffi.h"

// ---------------------------------------------------------------- diagnostics

void VM::raise(const std::string& message, int line, int col) const {
  RuntimeError error;
  error.message = message;
  error.line = line > 0 ? line : currentLine;
  error.col = col > 0 ? col : currentCol;
  error.stack = stack;
  throw error;
}

const Value& VM::constantAt(const Bytecode& bytecode, int index) const {
  if (index < 0 || static_cast<size_t>(index) >= bytecode.constants.size()) {
    fail("constant index out of range: " + std::to_string(index));
  }
  return bytecode.constants[static_cast<size_t>(index)];
}

const std::string& VM::nameAt(const Bytecode& bytecode, int index) const {
  const Value& value = constantAt(bytecode, index);
  if (!value.isStr()) fail("global name is not a string constant");
  return value.asStr();
}

// ------------------------------------------------------------------ registers

Value& VM::getReg(int index) {
  if (frames_.empty()) fail("register access outside of a call frame");
  if (index < 0) fail("register index out of range: " + std::to_string(index));
  const std::size_t absolute = frames_.back().regBase + static_cast<std::size_t>(index);
  if (absolute >= registers_.size()) {
    fail("register index out of range: " + std::to_string(index));
  }
  return registers_[absolute];
}

void VM::setReg(int index, Value value) { getReg(index) = std::move(value); }

CellPtr& VM::localAt(int index) {
  if (frames_.empty()) fail("local access outside of a call frame");
  if (index < 0) fail("local index out of range: " + std::to_string(index));
  const std::size_t absolute = frames_.back().localBase + static_cast<std::size_t>(index);
  if (absolute >= locals_.size()) {
    fail("local index out of range: " + std::to_string(index));
  }
  return locals_[absolute];
}

// -------------------------------------------------------------------- runtime

void VM::defineGlobal(const std::string& name, Value value, bool /*isImmutable*/) {
  // Immutability of globals is enforced by the compiler, which rejects the
  // assignment before the bytecode exists; the VM has no binding metadata.
  globals_[name] = std::move(value);
}

Value VM::callNative(Function& fn, std::vector<Value> args, int line, int col) {
  int savedLine = currentLine;
  int savedCol = currentCol;
  // A native that receives Jolt closures (map/filter/forEach/...) may invoke
  // them through this very `callNative` entry point. Capturing the raw vector
  // slot in a `std::function` and re-entering here would invalidate it on
  // reallocation (PHASE 6b: nested callback support). Copy the arguments so
  // every level owns its own storage.
  std::vector<Value> ownedArgs(std::move(args));
  currentLine = line;
  currentCol = col;
  try {
    Value result = fn.native(*this, ownedArgs);
    currentLine = savedLine;
    currentCol = savedCol;
    return result;
  } catch (RuntimeError& error) {
    if (error.line == 0) {
      error.line = line;
      error.col = col;
    }
    currentLine = savedLine;
    currentCol = savedCol;
    throw;
  }
}

void VM::pushJoltFrame(FuncPtr function, std::vector<Value> args, std::size_t resultReg, int line,
                       int col) {
  if (!function->proto) {
    // An AST function (or a bare `Function`) has no bytecode to run; the VM
    // never falls back to executing the AST.
    fail("cannot call '" + function->name + "' from the bytecode VM yet", line, col);
  }

  const Bytecode& proto = *function->proto;
  // A variadic function accepts `numParams - 1` or more arguments: the extras
  // are packed into an array bound to the last parameter (PHASE 6.5). A
  // non-variadic one needs the exact count.
  const std::size_t required = static_cast<std::size_t>(proto.numParams) - (proto.variadic ? 1 : 0);
  const bool arityOk = proto.variadic ? args.size() >= required : args.size() == required;
  if (!arityOk) {
    fail(function->name + "() expects " + std::to_string(required) + " argument" +
             (required == 1 ? "" : "s") + (proto.variadic ? " or more" : "") + ", got " +
             std::to_string(args.size()),
         line, col);
  }
  if (depth >= maxDepth) {
    fail("maximum call depth exceeded (possible infinite recursion)", line, col);
  }

  // The new window starts where the caller's data ends. Arguments are copied
  // into R0 .. R(n-1), which is exactly the parameter layout the compiler
  // expects; the prologue then boxes them into local cells. A variadic
  // function packs its extra arguments into an array in the last slot instead.
  const std::size_t regBase = registers_.size();
  const int registerCount = proto.numRegisters > proto.numParams ? proto.numRegisters
                                                                 : proto.numParams;
  registers_.resize(regBase + static_cast<std::size_t>(registerCount));
  if (proto.variadic) {
    const std::size_t fixed = required;  // numParams - 1
    for (std::size_t i = 0; i < fixed; ++i) registers_[regBase + i] = std::move(args[i]);
    std::vector<Value> rest;
    rest.reserve(args.size() - fixed);
    for (std::size_t i = fixed; i < args.size(); ++i) rest.push_back(std::move(args[i]));
    registers_[regBase + fixed] = Value(makeArray(std::move(rest)));
  } else {
    for (std::size_t i = 0; i < args.size(); ++i) registers_[regBase + i] = std::move(args[i]);
  }

  const std::size_t localBase = locals_.size();
  const int localCount = proto.numLocals > 0 ? proto.numLocals : 0;
  locals_.resize(localBase + static_cast<std::size_t>(localCount));

  // The closure's captured cells become the frame's upvalue window, so
  // GETUPVAL/SETUPVAL and nested CLOSUREs resolve through it (PHASE 6b).
  // `resultScratch` defaults to kNoResult; `call` overwrites it with the
  // dedicated scratch slot right after pushing the frame.
  CallFrame frame{function->proto, 0, kNoResult, resultReg, regBase, localBase, function->upvalues};
  frames_.push_back(std::move(frame));
  ++depth;
  stack.push_back(Frame{function->name, line, col});
}

Value VM::call(const Value& callee, std::vector<Value> args, int line, int col) {
  if (!callee.isFunc()) {
    fail(std::string("cannot call a value of type '") + callee.typeName() + "'", line, col);
  }

  // Keep the callee alive for the whole call: a temporary bound method owns
  // the only strong reference to its `Function` (see Interpreter::call).
  FuncPtr function = callee.funcPtr();

  if (function->isNative()) return callNative(*function, std::move(args), line, col);

  const int savedLine = currentLine;
  const int savedCol = currentCol;
  // The nested loop must stop when the frame it pushed is popped. Comparing
  // against the caller's frame count would only stop after the caller itself
  // returns, running the outer chunk past its epilogue with a dangling frame
  // reference (PHASE 6b fix). The result always travels back through
  // `runFrames`' return value, so the new frame gets `kNoResult`: writing
  // into `registers_` would be unsafe anyway, because `leaveFrame` truncates
  // the shared register stack to the callee's regBase before any outer code
  // could observe the slot.
  // The callee's frame index (0-based) inside `runFrames`. A RETURN pops the
  // callee and leaves the caller's loop running; a nested call returning from
  // its own callee must not pop this frame again.
  const std::size_t stopDepth = frames_.size();
  // The scratch cell for the kNoResult protocol lives in a dedicated vector,
  // one slot per nested call, so no callee or nested frame can ever overwrite
  // it while this call is running (PHASE 6b fix: sharing `locals_` for the
  // scratch allowed nested calls to clobber the return value). `leaveFrame`
  // writes the returned value into this cell; `call` reads it back below.
  if (scratchCells_.size() <= stopDepth) scratchCells_.resize(stopDepth + 1);
  if (!scratchCells_[stopDepth]) scratchCells_[stopDepth] = makeRef<Cell>();
  pushJoltFrame(std::move(function), std::move(args), kNoResult, line, col);
  // Tag the freshly pushed frame with its scratch destination.
  frames_.back().resultScratch = stopDepth;
  Value result;
  try {
    // `runFrames` returns the callee's value directly when its frame pops
    // past `stopDepth`; reading only the scratch cell would miss that path
    // (the cell is written exclusively by `leaveFrame`). Use whichever of
    // the two channels carries the value: the returned Value is authoritative
    // when non-empty, otherwise fall back to the scratch cell (root-chunk
    // implicit-return edge cases).
    Value returned = runFrames(stopDepth + 1);
    // `runFrames` returns a default-constructed (Nil) Value when it exits
    // without crossing `stopDepth` (e.g. the frame stack emptied through an
    // unrelated path). In that case fall back to the dedicated scratch cell,
    // which `leaveFrame` wrote before shrinking any shared windows. A real
    // Nil return from the callee is indistinguishable here, and both channels
    // agree on Nil, so the fallback is safe either way.
    if (returned.isNil()) {
      if (scratchCells_.size() > stopDepth && scratchCells_[stopDepth]) {
        result = scratchCells_[stopDepth]->value;
      }
      // Otherwise keep the Nil `returned` value as the result.
    } else {
      result = std::move(returned);
    }
  } catch (...) {
    // Unwind: drop every frame this call pushed (and any deeper ones) so the
    // VM stays consistent for a subsequent run(). `leaveFrame` normally does
    // this bookkeeping; on an exception we replicate it, shrinking the shared
    // windows. `stopDepth` is the index of
    // the callee frame, so we unwind while frames remain at or above it.
    while (frames_.size() > stopDepth) {
      CallFrame finished = std::move(frames_.back());
      frames_.pop_back();
      registers_.resize(finished.regBase);
      locals_.resize(finished.localBase);
      // Keep every scratch slot alive: outer nested-call loops still read
      // their own cell after this frame is popped (PHASE 6b fix).
      // Slots are reclaimed lazily by the owning `call` scope, never here.
      if (!stack.empty()) stack.pop_back();
      if (depth > 0) --depth;
    }
    currentLine = savedLine;
    currentCol = savedCol;
    throw;
  }
  // Note: if the root chunk happened to finish within this nested loop its
  // frame was popped by `leaveFrame`; the next `run()` rebuilds all windows
  // from scratch, so no restoration is needed here.
  currentLine = savedLine;
  currentCol = savedCol;
  return result;
}

Value VM::indexGet(const Value& object, const Value& index, int line, int col) {
  return ops::indexGet(*this, object, index, line, col);
}

void VM::indexSet(const Value& object, const Value& index, Value value, int line, int col) {
  ops::indexSet(*this, object, index, std::move(value), line, col);
}

Value VM::memberGet(const Value& object, const std::string& name, int line, int col) {
  return ops::memberGet(*this, object, name, line, col);
}

void VM::memberSet(const Value& object, const std::string& name, Value value, int line, int col) {
  ops::memberSet(*this, object, name, std::move(value), line, col);
}

Value VM::binary(Tok op, const Value& left, const Value& right, int line, int col) {
  return ops::binary(*this, op, left, right, line, col);
}

bool VM::lessThan(const Value& left, const Value& right, int line, int col) {
  return ops::lessThan(*this, left, right, line, col);
}

// ------------------------------------------------------------------ execution

void VM::installExtern(const ExternSymbol& symbol) {
  // The compiler already validated the signature, so a failure here is about
  // the symbol, not the types. `defineGlobal` overwrites, so a repeated run
  // (REPL) simply re-binds the same native.
  void* address = platform().lookupSymbol(symbol.name.c_str());
  if (address == nullptr) {
    fail("extern \"C\" symbol '" + symbol.name + "' was not found in the host");
  }
  defineGlobal(symbol.name, ffi::makeExternNative(*this, symbol.name, address, symbol.paramTypes,
                                                  symbol.returnType));
}

void VM::installExterns(const Bytecode& proto) {
  for (const ExternSymbol& symbol : proto.externs) installExtern(symbol);
  for (const std::shared_ptr<Bytecode>& child : proto.children) {
    if (child) installExterns(*child);
  }
}

Value VM::run(const Bytecode& bytecode) {
  // Resolve `extern "C"` symbols before executing anything: the module bodies
  // (children) that declare them run as part of this chunk, so their bindings
  // must exist first.
  installExterns(bytecode);
  const int registerCount = bytecode.numRegisters > 0 ? bytecode.numRegisters : 0;
  const int localCount = bytecode.numLocals > 0 ? bytecode.numLocals : 0;
  registers_.assign(static_cast<size_t>(registerCount), Value{});
  locals_.assign(static_cast<size_t>(localCount), CellPtr{});
  frames_.clear();
  depth = 0;
  stack.clear();
  scratchCells_.clear();
  std::shared_ptr<const Bytecode> root(&bytecode, [](const Bytecode*) {});
  frames_.push_back(CallFrame{std::move(root), 0, kNoResult, kNoResult, 0, 0, {}});
  Value result = runFrames(1);
  // `HALT` pops the root frame and shrinks `registers_` / `locals_` to the
  // root's bases (0), so `registerCount()` / `localCount()` would report 0
  // after `run()`. Restore the proto's counts so the public accessors keep
  // reporting what the last execution reserved.
  registers_.resize(static_cast<size_t>(registerCount));
  locals_.resize(static_cast<size_t>(localCount));
  if (!frames_.empty()) {
    frames_.clear();
    registers_.clear();
    locals_.clear();
    scratchCells_.clear();
    fail("VM::run: HALT left frames on the stack (malformed bytecode)");
  }
  return result;
}

Value VM::runFrames(std::size_t stopDepth) {
  while (!frames_.empty()) {
    const std::size_t frameIndex = frames_.size() - 1;
    const Bytecode& chunk = *frames_[frameIndex].proto;
    const std::size_t size = chunk.code.size();

    // Pops the current frame, publishing its result into the caller's window.
    // Returns true when the pop crossed `stopDepth`, so the nested `call()`
    // that owns this loop must return the value to its caller.
    auto leaveFrame = [&](const Value& result) {
      CallFrame finished = std::move(frames_.back());
      frames_.pop_back();
      if (finished.resultReg != kNoResult) {
        registers_[finished.resultReg] = result;
      } else if (finished.resultScratch != kNoResult) {
        // kNoResult protocol (nested `call`): hand the value back through the
        // dedicated scratch cell owned by the caller. The cell lives outside
        // the shared local window, so it is immune to clobbering by nested
        // frames and survives the shrink below (PHASE 6b fix).
        scratchCells_[finished.resultScratch]->value = result;
      }
      // Captured cells need no special handling here: every closure owns a
      // reference to each cell it captured, so shrinking the shared cell stack
      // only drops this frame's own reference.
      registers_.resize(finished.regBase);
      locals_.resize(finished.localBase);
      // Keep every scratch slot alive: outer nested-call loops still read
      // their own cell after this frame is popped (PHASE 6b fix).
      // Slots are reclaimed lazily by the owning `call` scope, never here.
      if (!stack.empty()) stack.pop_back();
      if (depth > 0) --depth;
      return frames_.size() < stopDepth;
    };

    if (frames_[frameIndex].pc >= size) {
      // Falling off the end is an implicit `return nil`.
      Value result;
      if (leaveFrame(result)) return result;
      continue;
    }

    const Instruction instruction = chunk.code[frames_[frameIndex].pc++];
    if (instruction.line > 0) {
      currentLine = instruction.line;
      currentCol = instruction.col;
    }

    // Position of the current instruction, for the shared value semantics.
    const int line = instruction.line;
    const int col = instruction.col;

    switch (instruction.op) {
      case OpCode::LOADK:
        setReg(instruction.a, constantAt(chunk, instruction.b));
        break;

      case OpCode::MOVE:
        setReg(instruction.a, getReg(instruction.b));
        break;

      case OpCode::ADD:
        setReg(instruction.a, binary(Tok::Plus, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SUB:
        setReg(instruction.a, binary(Tok::Minus, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::MUL:
        setReg(instruction.a, binary(Tok::Star, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::DIV:
        setReg(instruction.a, binary(Tok::Slash, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::MOD:
        setReg(instruction.a, binary(Tok::Percent, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::POW:
        setReg(instruction.a,
               binary(Tok::StarStar, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::UNOP: {
        Tok op;
        switch (static_cast<UnaryOp>(instruction.c)) {
          case UnaryOp::Negate:
            op = Tok::Minus;
            break;
          case UnaryOp::Not:
            op = Tok::Bang;
            break;
          case UnaryOp::BitNot:
            op = Tok::Tilde;
            break;
          default:
            fail("invalid unary operator: " + std::to_string(instruction.c), line, col);
        }
        setReg(instruction.a, ops::unary(*this, op, getReg(instruction.b), line, col));
        break;
      }

      case OpCode::BAND:
        setReg(instruction.a, binary(Tok::Amp, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::BOR:
        setReg(instruction.a, binary(Tok::Pipe, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::BXOR:
        setReg(instruction.a, binary(Tok::Caret, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SHL:
        setReg(instruction.a, binary(Tok::Shl, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::SHR:
        setReg(instruction.a, binary(Tok::Shr, getReg(instruction.b), getReg(instruction.c), line, col));
        break;

      case OpCode::EQ:
        setReg(instruction.a, Value(getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::NEQ:
        setReg(instruction.a, Value(!getReg(instruction.b).equals(getReg(instruction.c))));
        break;

      case OpCode::LT:
        setReg(instruction.a,
               Value(lessThan(getReg(instruction.b), getReg(instruction.c), line, col)));
        break;

      case OpCode::LTE:
        setReg(instruction.a,
               Value(!lessThan(getReg(instruction.c), getReg(instruction.b), line, col)));
        break;

      case OpCode::GT:
        setReg(instruction.a,
               Value(lessThan(getReg(instruction.c), getReg(instruction.b), line, col)));
        break;

      case OpCode::GTE:
        setReg(instruction.a,
               Value(!lessThan(getReg(instruction.b), getReg(instruction.c), line, col)));
        break;

      case OpCode::JMP:
        if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
          fail("invalid jump target: " + std::to_string(instruction.jump));
        }
        frames_[frameIndex].pc = static_cast<size_t>(instruction.jump);
        break;

      case OpCode::JMP_IF_FALSE:
        if (!getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            fail("invalid jump target: " + std::to_string(instruction.jump));
          }
          frames_[frameIndex].pc = static_cast<size_t>(instruction.jump);
        }
        break;

      case OpCode::JMP_IF_TRUE:
        if (getReg(instruction.a).truthy()) {
          if (instruction.jump < 0 || static_cast<size_t>(instruction.jump) > size) {
            fail("invalid jump target: " + std::to_string(instruction.jump));
          }
          frames_[frameIndex].pc = static_cast<size_t>(instruction.jump);
        }
        break;

      case OpCode::SETGLOBAL:
        globals_[nameAt(chunk, instruction.b)] = getReg(instruction.a);
        break;

      case OpCode::GETGLOBAL: {
        const std::string& name = nameAt(chunk, instruction.b);
        auto it = globals_.find(name);
        if (it == globals_.end()) fail("undefined variable '" + name + "'", line, col);
        setReg(instruction.a, it->second);
        break;
      }

      case OpCode::DECLLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        cell = makeRef<Cell>();
        cell->value = getReg(instruction.a);
        break;
      }

      case OpCode::SETLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        if (!cell) fail("undefined local variable", line, col);
        cell->value = getReg(instruction.a);
        break;
      }

      case OpCode::GETLOCAL: {
        CellPtr& cell = localAt(instruction.b);
        if (!cell) fail("undefined local variable", line, col);
        setReg(instruction.a, cell->value);
        break;
      }

      case OpCode::CALL: {
        // The callee and its arguments occupy a contiguous window:
        // R[a] = callee, R[a+1 .. a+b] = arguments. Copy them before entering
        // the callee, since the register stack may be resized by the call.
        const Value& calleeValue = getReg(instruction.a);
        if (!calleeValue.isFunc()) {
          fail(std::string("cannot call a value of type '") + calleeValue.typeName() + "'", line,
               col);
        }
        FuncPtr function = calleeValue.funcPtr();
        const int argc = instruction.b;
        std::vector<Value> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 1; i <= argc; ++i) args.push_back(getReg(instruction.a + i));

        if (function->isNative()) {
          setReg(instruction.a, callNative(*function, std::move(args), line, col));
        } else {
          // A bytecode function gets a new frame; its RETURN publishes the
          // result into the caller's R[a].
          const std::size_t resultReg =
              frames_[frameIndex].regBase + static_cast<std::size_t>(instruction.a);
          pushJoltFrame(std::move(function), std::move(args), resultReg, line, col);
        }
        break;
      }

      case OpCode::RETURN: {
        Value result = getReg(instruction.a);
        if (leaveFrame(result)) return result;
        break;
      }

      case OpCode::CLOSURE: {
        // A safe point: everything the running program can still reach is held by
        // a register, a local cell, a frame or a global, so only unreachable
        // cycles are freed. Closures are where such cycles are born.
        gcCollectIfNeeded();

        const int child = instruction.b;
        if (child < 0 || static_cast<std::size_t>(child) >= chunk.children.size()) {
          fail("invalid function prototype index: " + std::to_string(child), line, col);
        }
        std::shared_ptr<const Bytecode> proto = chunk.children[static_cast<std::size_t>(child)];
        FuncPtr function = makeRef<Function>();
        function->name = proto->name;
        function->proto = proto;
        // Bind the capture list (PHASE 6b). Each descriptor points either at
        // an open local cell of this frame or at one of its upvalues; both
        // share the same `Cell`, so mutations stay visible across closures.
        const CallFrame& frame = frames_.back();
        function->upvalues.reserve(proto->upvalues.size());
        for (const UpvalDesc& desc : proto->upvalues) {
          CellPtr cell;
          if (desc.inParentLocal) {
            // The captured slot may sit past the frame's declared `numLocals`
            // window when a block-scoped variable left its scope before this
            // closure was instantiated. Grow the shared cell stack so the
            // descriptor stays valid (PHASE 6b).
            const std::size_t absolute = frame.localBase + static_cast<std::size_t>(desc.index);
            if (desc.index < 0) {
              fail("upvalue local index out of range: " + std::to_string(desc.index), line, col);
            }
            if (absolute >= locals_.size()) locals_.resize(absolute + 1);
            cell = locals_[absolute];
            if (!cell) {
              // A captured slot that was never declared still needs a cell so
              // the closure can read/write it; treat it as an open upvalue.
              cell = makeRef<Cell>();
              locals_[absolute] = cell;
            }
          } else {
            if (desc.index < 0 || static_cast<std::size_t>(desc.index) >= frame.upvalues.size()) {
              fail("upvalue index out of range: " + std::to_string(desc.index), line, col);
            }
            cell = frame.upvalues[static_cast<std::size_t>(desc.index)];
          }
          // The closure now shares the cell, so the cell can end up in a cycle
          // (a closure stored in a variable it captures). Let the collector see it.
          cell->track();
          function->upvalues.push_back(std::move(cell));
        }
        setReg(instruction.a, Value(std::move(function)));
        break;
      }

      case OpCode::GETUPVAL: {
        const std::vector<CellPtr>& upvalues = frames_.back().upvalues;
        if (instruction.b < 0 || static_cast<std::size_t>(instruction.b) >= upvalues.size()) {
          fail("upvalue index out of range: " + std::to_string(instruction.b), line, col);
        }
        setReg(instruction.a, upvalues[static_cast<std::size_t>(instruction.b)]->value);
        break;
      }

      case OpCode::SETUPVAL: {
        std::vector<CellPtr>& upvalues = frames_.back().upvalues;
        if (instruction.b < 0 || static_cast<std::size_t>(instruction.b) >= upvalues.size()) {
          fail("upvalue index out of range: " + std::to_string(instruction.b), line, col);
        }
        upvalues[static_cast<std::size_t>(instruction.b)]->value = getReg(instruction.a);
        break;
      }

      case OpCode::NEWTABLE: {
        // A safe point, like CLOSURE: tables are what cycles are made of, and a
        // program that only builds self-referencing tables creates no closure.
        gcCollectIfNeeded();
        if (instruction.c == 0) {
          // `b` is a capacity hint, not the number of elements. The array
          // starts empty; `SETTABLE` with an index equal to the current
          // length grows it, and `push` grows it from the end. Creating `b`
          // nils would make `len(arr)` report the hint instead of the real
          // size (PHASE 6c fix).
          setReg(instruction.a, Value(makeArray()));
        } else if (instruction.c == 1) {
          setReg(instruction.a, Value(makeMap()));
        } else {
          fail("invalid table kind: " + std::to_string(instruction.c), line, col);
        }
        break;
      }

      case OpCode::GETTABLE: {
        const Value& object = getReg(instruction.b);
        if (instruction.c >= 0) {
          const Value& index = getReg(instruction.c);
          setReg(instruction.a, ops::indexGet(*this, object, index, line, col));
        } else {
          const std::string& name = nameAt(chunk, -instruction.c - 1);
          setReg(instruction.a, ops::memberGet(*this, object, name, line, col));
        }
        break;
      }

      case OpCode::SETTABLE: {
        Value value = getReg(instruction.a);
        const Value& object = getReg(instruction.b);
        if (instruction.c >= 0) {
          const Value& index = getReg(instruction.c);
          ops::indexSet(*this, object, index, std::move(value), line, col);
        } else {
          const std::string& name = nameAt(chunk, -instruction.c - 1);
          ops::memberSet(*this, object, name, std::move(value), line, col);
        }
        break;
      }

      case OpCode::ITER: {
        // The cursor starts as nil (the compiler loads it that way), so the
        // first ITER can initialize it from the iterable's own shape.
        const Value iterable = getReg(instruction.b);
        Value cursor = getReg(instruction.c);
        Value item;
        bool done = false;

        if (iterable.isArray()) {
          if (!cursor.isNil() && !cursor.isInt()) {
            fail("invalid array iteration state", line, col);
          }
          const int64_t index = cursor.isNil() ? 0 : cursor.asInt();
          if (index < 0 || static_cast<std::size_t>(index) >= iterable.arr().items.size()) {
            done = true;
          } else {
            item = iterable.arr().items[static_cast<std::size_t>(index)];
            cursor = Value(static_cast<int64_t>(index + 1));
          }
        } else if (iterable.isStr()) {
          if (!cursor.isNil() && !cursor.isInt()) {
            fail("invalid string iteration state", line, col);
          }
          const std::vector<std::string> chars = utf8Chars(iterable.asStr());
          const int64_t index = cursor.isNil() ? 0 : cursor.asInt();
          if (index < 0 || static_cast<std::size_t>(index) >= chars.size()) {
            done = true;
          } else {
            item = Value(chars[static_cast<std::size_t>(index)]);
            cursor = Value(static_cast<int64_t>(index + 1));
          }
        } else if (iterable.isMap()) {
          if (!cursor.isNil() && !cursor.isInt()) {
            fail("invalid map iteration state", line, col);
          }
          const int64_t index = cursor.isNil() ? 0 : cursor.asInt();
          if (index < 0 || static_cast<std::size_t>(index) >= iterable.map().entries.size()) {
            done = true;
          } else {
            item = Value(iterable.map().entries[static_cast<std::size_t>(index)].first);
            cursor = Value(static_cast<int64_t>(index + 1));
          }
        } else if (iterable.isRange()) {
          const Range& range = iterable.range();
          if (cursor.isNil()) cursor = Value(range.start);
          if (!cursor.isInt()) fail("invalid range iteration state", line, col);

          const bool ascending = range.start <= range.end;
          const int64_t current = cursor.asInt();
          const bool inBounds =
              ascending ? (range.inclusive ? current <= range.end : current < range.end)
                        : (range.inclusive ? current >= range.end : current > range.end);
          if (!inBounds) {
            done = true;
          } else {
            item = Value(current);
            // Unsigned arithmetic keeps INT64_MIN/MAX bounds well-defined.
            const int64_t next =
                ascending ? static_cast<int64_t>(static_cast<uint64_t>(current) + 1ULL)
                          : static_cast<int64_t>(static_cast<uint64_t>(current) - 1ULL);
            cursor = Value(next);
          }
        } else {
          fail(std::string("cannot iterate over a value of type '") + iterable.typeName() + "'",
               line, col);
        }
        // Persist the advanced cursor; without this the loop would restart
        setReg(instruction.c, std::move(cursor));
        setReg(instruction.a, done ? Value() : std::move(item));
        setReg(instruction.jump, Value(done));
        break;
      }

      case OpCode::IN: {
        const Value& needle = getReg(instruction.b);
        const Value& haystack = getReg(instruction.c);
        setReg(instruction.a, Value(ops::contains(*this, needle, haystack, line, col)));
        break;
      }

      case OpCode::HALT:
        leaveFrame(Value());
        return Value();
    }
  }
  return Value();
}

const Value* VM::findGlobal(const std::string& name) const {
  auto it = globals_.find(name);
  return it == globals_.end() ? nullptr : &it->second;
}
