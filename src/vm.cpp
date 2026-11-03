#include "vm.h"

#include <algorithm>
#include <string>
#include <utility>

#include "ops.h"

// ---------------------------------------------------------------- diagnostics

void VM::fail(const std::string& message, int line, int col) const {
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
  currentLine = line;
  currentCol = col;
  try {
    Value result = fn.native(*this, args);
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
  if (args.size() != static_cast<std::size_t>(proto.numParams)) {
    fail(function->name + "() expects " + std::to_string(proto.numParams) + " argument" +
             (proto.numParams == 1 ? "" : "s") + ", got " + std::to_string(args.size()),
         line, col);
  }
  if (depth >= maxDepth) {
    fail("maximum call depth exceeded (possible infinite recursion)", line, col);
  }

  // The new window starts where the caller's data ends. Arguments are copied
  // into R0 .. R(n-1), which is exactly the parameter layout the compiler
  // expects; the prologue then boxes them into local cells.
  const std::size_t regBase = registers_.size();
  const int registerCount = proto.numRegisters > proto.numParams ? proto.numRegisters
                                                                 : proto.numParams;
  registers_.resize(regBase + static_cast<std::size_t>(registerCount));
  for (std::size_t i = 0; i < args.size(); ++i) registers_[regBase + i] = std::move(args[i]);

  const std::size_t localBase = locals_.size();
  const int localCount = proto.numLocals > 0 ? proto.numLocals : 0;
  locals_.resize(localBase + static_cast<std::size_t>(localCount));

  // The closure's captured cells become the frame's upvalue window, so
  // GETUPVAL/SETUPVAL and nested CLOSUREs resolve through it (PHASE 6b).
  CallFrame frame{function->proto, 0, resultReg, regBase, localBase, function->upvalues};
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
  // The nested loop must stop when the frame it pushed is popped: that pop
  // publishes the call's result into `resultReg`, which lives in *this*
  // (the caller's) window. Comparing against the caller's frame count would
  // only stop after the caller itself returns, running the outer chunk past
  // its epilogue with a dangling frame reference (PHASE 6b fix).
  const std::size_t stopDepth = frames_.size();
  pushJoltFrame(std::move(function), std::move(args), kNoResult, line, col);
  try {
    Value result = runFrames(stopDepth);
    currentLine = savedLine;
    currentCol = savedCol;
    return result;
  } catch (...) {
    currentLine = savedLine;
    currentCol = savedCol;
    throw;
  }
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

void VM::run(const Bytecode& bytecode) {
  const int registerCount = bytecode.numRegisters > 0 ? bytecode.numRegisters : 0;
  const int localCount = bytecode.numLocals > 0 ? bytecode.numLocals : 0;
  registers_.assign(static_cast<size_t>(registerCount), Value{});
  locals_.assign(static_cast<size_t>(localCount), CellPtr{});
  // Closures created by a previous entry may still hold cells that this run
  // adopts through `retiredCells_`; keep them alive, drop the stale ones.
  pruneRetiredCells();
  frames_.clear();
  depth = 0;
  stack.clear();

  // The root chunk is owned by the caller, so the frame keeps a non-owning
  // handle. Child protos are owned by the Function values that instantiate
  // them and outlive both the frame and this call to `run`.
  std::shared_ptr<const Bytecode> root(&bytecode, [](const Bytecode*) {});
  frames_.push_back(CallFrame{std::move(root), 0, kNoResult, 0, 0, {}});
  runFrames(0);
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
      }
      // Retire the locals this frame still shares with escaping closures
      // before shrinking the shared cell stack (PHASE 6b).
      closeCapturedUpvalues(finished);
      registers_.resize(finished.regBase);
      locals_.resize(finished.localBase);
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
              // Lazily allocate the cell when the captured local was written
              // through SETUPVAL/assignment before this frame returned. This
              // mirrors the eager allocation done by DECLLOCAL and keeps the
              // shared `Cell` valid across closures (PHASE 6b).
              cell = makeRef<Cell>();
              locals_[absolute] = cell;
            }
          } else {
            if (desc.index < 0 || static_cast<std::size_t>(desc.index) >= frame.upvalues.size()) {
              fail("upvalue index out of range: " + std::to_string(desc.index), line, col);
            }
            cell = frame.upvalues[static_cast<std::size_t>(desc.index)];
          }
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

      // PHASE 6c-6d opcodes are declared now so the instruction budget stays
      // at 40. The compiler does not emit them until their slice lands, so
      // reaching one of these cases would be a compiler bug.
      case OpCode::NEWTABLE:
      case OpCode::GETTABLE:
      case OpCode::SETTABLE:
      case OpCode::ITER:
      case OpCode::IN:
        fail("opcode not implemented yet", line, col);

      case OpCode::PRINT: {
        const std::string text = getReg(instruction.a).toString();
        platform().write(Platform::kStdout, text.data(), text.size());
        platform().write(Platform::kStdout, "\n", 1);
        platform().flush(Platform::kStdout);
        break;
      }

      case OpCode::HALT:
        return Value();
    }
  }
  return Value();
}

// ------------------------------------------------------- upvalue cell retiring

void VM::closeCapturedUpvalues(const CallFrame& frame) {
  // A local slot is "captured" when some live closure's upvalue list points at
  // the same Cell this frame owns. When the frame dies its window in the
  // shared `locals_` stack disappears, so any captured cell must be adopted by
  // `retiredCells_` to keep the sharing intact (PHASE 6b). The reference count
  // tells us exactly that: a cell with more than one owner outlives this
  // window shrink, and the extra owners are closures somewhere in the heap.
  const std::size_t base = frame.localBase;
  const std::size_t end = locals_.size();  // window before `leaveFrame` shrank it
  for (std::size_t i = base; i < end; ++i) {
    const CellPtr& cell = locals_[i];
    if (cell && cell->refs > 1) retiredCells_.push_back(cell);
  }
}

void VM::pruneRetiredCells() {
  // Keep only cells that somebody still references. A retired cell with a
  // single owner (this vector) is dead: no live frame window or closure holds
  // it anymore, so releasing it here mirrors what shrinking `locals_` did.
  retiredCells_.erase(std::remove_if(retiredCells_.begin(), retiredCells_.end(),
                                     [](const CellPtr& cell) { return cell->refs <= 1; }),
                      retiredCells_.end());
}

const Value* VM::findGlobal(const std::string& name) const {
  auto it = globals_.find(name);
  return it == globals_.end() ? nullptr : &it->second;
}
