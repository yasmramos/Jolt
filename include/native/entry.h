#pragma once

#include <cstdint>

#include "native/codegen.h"

// Freestanding entry point and runtime prelude (PHASE 7c).
//
// A generated binary has no libc and no `_start`: this module appends both to
// the same machine-code buffer the lowering produced, so every `call` resolves
// through the emitter's own labels -- no relocations, no linker.
//
// `_start` is the ELF entry: it reads argc/argv from the initial stack, calls
// the program's entry, and exits with the returned value. The prelude provides
// the syscall wrappers the runtime calls: a `write` that retries on short
// writes, and `exit`.

// Labels the caller creates up front, so a program emitted *before* the
// prelude can already reference them; `emitFreestandingRuntime` binds them.
struct RuntimeHandles {
  CodeGen::Label write;  // long jolt_write(long fd, const char* buf, long len)
  CodeGen::Label exit;   // void jolt_exit(long code)   (never returns)
};

inline RuntimeHandles newRuntimeHandles(CodeGen& code) {
  return RuntimeHandles{code.newLabel(), code.newLabel()};
}

// Appends `jolt_write`, `jolt_exit` and `_start` to `code`, binding the
// handles. `programEntry` must be bound at the program's entry; `_start` calls
// it with (argc, argv) and hands its return value to `jolt_exit`. Returns the
// offset of `_start`, which the caller passes to `buildStaticElf` as the entry
// point.
std::uint64_t emitFreestandingRuntime(CodeGen& code, const RuntimeHandles& handles,
                                      CodeGen::Label programEntry);
