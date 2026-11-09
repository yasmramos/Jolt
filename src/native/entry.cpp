#include "native/entry.h"

namespace {

// Linux x86-64 syscall numbers used by the prelude.
constexpr long kSysWrite = 1;
constexpr long kSysMmap = 9;
constexpr long kSysExitGroup = 231;

// mmap flags: a private, anonymous, read/write mapping (no file).
constexpr long kProtReadWrite = 3;
constexpr long kMapPrivateAnonymous = 0x22;

}  // namespace

std::uint64_t emitFreestandingRuntime(CodeGen& code, const RuntimeHandles& handles,
                                      CodeGen::Label programEntry) {
  // --- long jolt_write(long fd, const char* buf, long len) ---
  // Retries until `len` bytes are out (the Platform contract forbids short
  // writes) or gives up on the first error / zero progress. Returns the number
  // of bytes written.
  code.bind(handles.write);
  {
    CodeGen::Label loop = code.newLabel();
    CodeGen::Label done = code.newLabel();
    // r8 keeps the running total; rsi/rdx advance as bytes are written.
    code.xorReg(X64Reg::R8, X64Reg::R8);
    code.bind(loop);
    code.testReg(X64Reg::Rdx);
    code.jcc(Jcc::Le, done);  // nothing left to write
    code.movImm(X64Reg::Rax, kSysWrite);
    code.syscallInsn();
    code.testReg(X64Reg::Rax);
    code.jcc(Jcc::Lt, done);  // error: rax < 0
    code.jcc(Jcc::Eq, done);  // no progress
    code.addReg(X64Reg::R8, X64Reg::Rax);
    code.addReg(X64Reg::Rsi, X64Reg::Rax);
    code.subReg(X64Reg::Rdx, X64Reg::Rax);
    code.jmp(loop);
    code.bind(done);
    code.movReg(X64Reg::Rax, X64Reg::R8);
    code.retInsn();
  }

  // --- void jolt_exit(long code) ---
  code.bind(handles.exit);
  code.movImm(X64Reg::Rax, kSysExitGroup);
  code.syscallInsn();
  code.ud2();  // unreachable: exit_group never returns

  // --- void jolt_init_arena(void) ---
  // mmap(NULL, kAotArenaSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
  //      -1, 0) and seed the allocator state. The kernel returns the region
  // base in rax (or a negative error, which the allocator later turns into a
  // null allocation). The cursor starts `kAotArenaFirstOffset` in, so no live
  // pointer is ever zero. The chunk entry calls this before running the
  // program, so a program lowered in isolation (a test that calls the chunk
  // directly) still has an arena.
  code.bind(handles.initArena);
  code.xorReg(X64Reg::Rdi, X64Reg::Rdi);            // addr = NULL
  code.movImm(X64Reg::Rsi, kAotArenaSize);          // length
  code.movImm(X64Reg::Rdx, kProtReadWrite);         // prot
  code.movImm(X64Reg::R10, kMapPrivateAnonymous);   // flags (syscall arg 4)
  code.movImm(X64Reg::R8, -1);                      // fd = -1
  code.xorReg(X64Reg::R9, X64Reg::R9);              // offset = 0
  code.movImm(X64Reg::Rax, kSysMmap);
  code.syscallInsn();
  code.movReg(kAotArenaCursor, X64Reg::Rax);
  code.addImm32(kAotArenaCursor, static_cast<std::int32_t>(kAotArenaFirstOffset));
  code.movReg(kAotArenaEnd, X64Reg::Rax);
  code.addImm32(kAotArenaEnd, static_cast<std::int32_t>(kAotArenaSize));
  code.retInsn();

  // --- _start: the ELF entry point ---
  // The kernel leaves argc at [rsp] and argv[] starting at [rsp+8]. Pass them
  // to the program entry as (argc, argv) and exit with its result. The chunk
  // entry maps the arena itself, so `_start` does not.
  std::uint64_t startOffset = code.offset();
  code.movFromMem(X64Reg::Rdi, X64Reg::Rsp, 0);   // argc
  code.movReg(X64Reg::Rsi, X64Reg::Rsp);
  code.addImm32(X64Reg::Rsi, 8);                  // argv
  code.callLabel(programEntry);
  code.movReg(X64Reg::Rdi, X64Reg::Rax);          // exit code = main's result
  code.callLabel(handles.exit);
  code.ud2();  // jolt_exit never returns

  return startOffset;
}
