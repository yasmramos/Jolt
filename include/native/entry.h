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
  CodeGen::Label write;      // long jolt_write(long fd, const char* buf, long len)
  CodeGen::Label exit;       // void jolt_exit(long code)   (never returns)
  CodeGen::Label initArena;  // void jolt_init_arena(void)  (maps the bump region)
};

inline RuntimeHandles newRuntimeHandles(CodeGen& code) {
  return RuntimeHandles{code.newLabel(), code.newLabel(), code.newLabel()};
}

// The generated binary's bump arena (PHASE 7c.1c). `_start` maps a region and
// keeps the allocator state in two registers the runtime owns:
//
//   kAotArenaCursor -- the next free address (grows upward)
//   kAotArenaEnd    -- one past the last usable byte
//
// They are callee-saved registers no helper touches, so a value survives from
// `_start` down through `main` and the chunk to the allocator without a global
// memory word (the generated image has no writable data segment). A helper
// that allocates updates the cursor in place: `r13`/`r14` are the allocator's
// state, not a preserved register, and only `jolt_alloc` and its callers read
// them. The runtime helpers use none of r12-r15, so the arena and the register
// file on the stack never collide.
constexpr X64Reg kAotArenaCursor = X64Reg::R13;
constexpr X64Reg kAotArenaEnd = X64Reg::R14;

// Base of the global table (PHASE 7c.2a): a contiguous array of `AotValue`
// slots the chunk allocates from the arena once at entry. Like the arena
// cursor, it is a callee-saved register no helper touches, so the chunk and
// every frame reach the globals without a writable data segment. The runtime
// helpers use none of r12-r15, so the arena, the global table and the register
// file never collide.
constexpr X64Reg kAotGlobalBase = X64Reg::R15;

// Region `_start` maps for the arena, and the offset of the first allocation
// (so a live pointer is never null, which is the allocator's failure value).
constexpr std::int64_t kAotArenaSize = 1 << 20;  // 1 MiB
constexpr std::int64_t kAotArenaFirstOffset = 16;

// Appends `jolt_write`, `jolt_exit` and `_start` to `code`, binding the
// handles. `programEntry` must be bound at the program's entry; `_start` calls
// it with (argc, argv) and hands its return value to `jolt_exit`. Returns the
// offset of `_start`, which the caller passes to `buildStaticElf` as the entry
// point.
std::uint64_t emitFreestandingRuntime(CodeGen& code, const RuntimeHandles& handles,
                                      CodeGen::Label programEntry);
