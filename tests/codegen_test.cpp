// Unit tests for the minimal x86-64 machine-code emitter (PHASE 7a).
//
// The emitter is verified at two levels:
//   1. Byte-exact encodings checked against hand-computed reference bytes
//      following the Intel SDM Vol. 2 tables.
//   2. Behavioral checks on Linux x86-64: emitted code is mapped as
//      executable memory via mmap and called through a function pointer.
// On any other platform the behavioral section is skipped and only the byte
// checks run.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "native/codegen.h"

#if defined(__linux__) && defined(__x86_64__)
#define CODEGEN_EXEC_TESTS 1
#include <csignal>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what.c_str());
  }
}

static std::string toHex(const std::vector<std::uint8_t>& bytes) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (std::uint8_t b : bytes) {
    out += digits[b >> 4];
    out += digits[b & 0x0F];
  }
  return out;
}

static void checkBytes(const char* label, CodeGen& gen, const std::string& expected) {
  gen.finalize();
  std::string actual = toHex(gen.code());
  check(actual == expected,
        std::string(label) + " encoding [" + actual + "] expected [" + expected + "]");
}

static void test_mov_imm() {
  // Small immediates take the sign-extended `mov r/m64, imm32` form
  // (REX.W C7 /0 id), which is correct for 64-bit registers.
  CodeGen g;
  g.movImm(X64Reg::Rcx, 0);
  checkBytes("mov rcx, 0", g, "48C7C100000000");

  // Wide immediates take `movabs`: 48 B8 <imm64 little endian>.
  CodeGen g2;
  g2.movImm(X64Reg::Rax, 0x1122334455667788LL);
  checkBytes("movabs rax, imm64", g2, "48B88877665544332211");

  // Extended registers add REX.B (0x41 for r9 with W=1 -> 0x49).
  CodeGen g3;
  g3.movImm(X64Reg::R9, 5);
  checkBytes("mov r9, 5", g3, "49C7C105000000");

  // Negative small immediates are sign-extended by the CPU.
  CodeGen g4;
  g4.movImm(X64Reg::Rax, -1);
  checkBytes("mov rax, -1", g4, "48C7C0FFFFFFFF");
}

static void test_mov_reg_and_stack() {
  // mov rdi, rsi -> 48 89 F7
  CodeGen g;
  g.movReg(X64Reg::Rdi, X64Reg::Rsi);
  checkBytes("mov rdi, rsi", g, "4889F7");

  // push rbx / pop rbx -> 53 / 5B
  CodeGen g2;
  g2.push(X64Reg::Rbx);
  g2.pop(X64Reg::Rbx);
  checkBytes("push/pop rbx", g2, "535B");

  // push r12 -> 41 54 ; pop r12 -> 41 5C
  CodeGen g3;
  g3.push(X64Reg::R12);
  g3.pop(X64Reg::R12);
  checkBytes("push/pop r12", g3, "4154415C");
}

static void test_memory_operands() {
  // mov [rbp+0x10], rax -> 48 89 45 10 (mod=01, disp8: the emitter picks the
  // smallest legal displacement form).
  CodeGen g;
  g.movToMem(X64Reg::Rbp, 0x10, X64Reg::Rax);
  checkBytes("mov [rbp+0x10], rax", g, "48894510");

  // Large displacements fall back to mod=10 with a disp32.
  CodeGen gDisp;
  gDisp.movToMem(X64Reg::Rbp, 0x1234, X64Reg::Rax);
  checkBytes("mov [rbp+0x1234], rax", gDisp, "48898534120000");

  // mov rsp as base forces a SIB byte (scale=00, index=100 no index,
  // base=100 rsp) with mod=00 for a zero displacement: 48 8B 04 24.
  CodeGen g2;
  g2.movFromMem(X64Reg::Rax, X64Reg::Rsp, 0);
  checkBytes("mov rax, [rsp] (SIB)", g2, "488B0424");

  // rbp as base with disp 0 still needs an explicit disp8 (mod=01): without
  // it mod=00/rm=101 would decode as RIP-relative addressing.
  CodeGen g3;
  g3.movFromMem(X64Reg::Rax, X64Reg::Rbp, 0);
  checkBytes("mov rax, [rbp] (forced disp8)", g3, "488B4500");

  // movsd [rbp+0x18], xmm1: legacy SSE has no REX.W, so the bytes are
  // F2 0F 11 4D 18 (reg field = src xmm1, mod=01 disp8).
  CodeGen gStore;
  gStore.movsdFromXmm(X64Reg::Rbp, 0x18, kXmmBase + 1);
  checkBytes("movsd [rbp+0x18], xmm1", gStore, "F20F114D18");

  // movsd [rbp], xmm9: extended source sets REX.R (not REX.B), reg field
  // keeps the low bits (001), and the rbp base forces mod=01 disp8 =>
  // 44 F2 0F 11 4D 00.
  CodeGen gStore9;
  gStore9.movsdFromXmm(X64Reg::Rbp, 0, kXmmBase + 9);
  checkBytes("movsd [rbp], xmm9 (REX.R)", gStore9, "44F20F114D00");

  // RIP-relative lea: 48 8D 35 <disp32>, mod=00 rm=101 without SIB.
  CodeGen g4;
  g4.leaRip(X64Reg::Rsi, 0x77);
  checkBytes("lea rsi, [rip+0x77]", g4, "488D3577000000");
}

static void test_alu() {
  // add rax, rcx -> 48 01 C8
  CodeGen g;
  g.addReg(X64Reg::Rax, X64Reg::Rcx);
  checkBytes("add rax, rcx", g, "4801C8");

  // sub rax, rcx -> 48 29 C8
  CodeGen g2;
  g2.subReg(X64Reg::Rax, X64Reg::Rcx);
  checkBytes("sub rax, rcx", g2, "4829C8");

  // imul rax, rcx -> 48 0F AF C1
  CodeGen g3;
  g3.imulReg(X64Reg::Rax, X64Reg::Rcx);
  checkBytes("imul rax, rcx", g3, "480FAFC1");

  // xor rax, rax -> 48 31 C0 (REX.W is redundant but keeps the helper uniform).
  CodeGen g4;
  g4.xorReg(X64Reg::Rax, X64Reg::Rax);
  checkBytes("xor rax, rax", g4, "4831C0");

  // idiv rcx -> 48 F7 F9 (signed divide rdx:rax by rcx).
  CodeGen g5;
  g5.idiv(X64Reg::Rcx);
  checkBytes("idiv rcx", g5, "48F7F9");

  // cmp rax, rcx -> 48 39 C8
  CodeGen g6;
  g6.cmpReg(X64Reg::Rax, X64Reg::Rcx);
  checkBytes("cmp rax, rcx", g6, "4839C8");
}

static void test_control_flow() {
  // ud2 -> 0F 0B ; ret -> C3
  CodeGen g;
  g.ud2();
  g.retInsn();
  checkBytes("ud2; ret", g, "0F0BC3");

  // syscall -> 0F 05
  CodeGen g2;
  g2.syscallInsn();
  checkBytes("syscall", g2, "0F05");

  // jmp forward rel32: jmp L ; mov rdi, rsi (3 bytes) ; bind L
  // displacement = target(8) - patch_end(5) = 3
  CodeGen g3;
  CodeGen::Label l = g3.newLabel();
  g3.jmp(l);
  g3.movReg(X64Reg::Rdi, X64Reg::Rsi);
  g3.bind(l);
  checkBytes("jmp rel32 patched", g3, "E9030000004889F7");

  // jcc Eq with the same layout: 0F 84 <disp32>
  CodeGen g4;
  CodeGen::Label l4 = g4.newLabel();
  g4.jcc(Jcc::Eq, l4);
  g4.movReg(X64Reg::Rdi, X64Reg::Rsi);
  g4.bind(l4);
  checkBytes("je rel32 patched", g4, "0F84030000004889F7");

  // Backward jump: bind first, then jump back to it.
  //   bind(top): top=0 ; mov rax,-1 (7 bytes) ; jmp top at offset 7 with a
  //   4-byte displacement slot ending at 12 -> disp = 0 - 12 = -12 (0xF4).
  //   The origin is the end of the jump instruction itself, pinned when the
  //   slot is reserved -- not the buffer size at patch time.
  CodeGen g5;
  CodeGen::Label top = g5.newLabel();
  g5.bind(top);
  g5.movImm(X64Reg::Rax, -1);
  g5.jmp(top);
  checkBytes("jmp rel32 backward", g5, "48C7C0FFFFFFFFE9F4FFFFFF");

  // Binding an unknown or twice-bound label must be rejected: `bind()`
  // throws std::logic_error in both cases.
  CodeGen g6;
  CodeGen::Label once = g6.newLabel();
  bool threwUnknown = false;
  try {
    CodeGen::Label bogus;
    bogus.id = 12345;
    g6.bind(bogus);
  } catch (const std::logic_error&) {
    threwUnknown = true;
  }
  check(threwUnknown, "bind() of an unknown label throws std::logic_error");
  g6.bind(once);
  check(g6.offset() == 0, "bind() does not emit bytes (position unchanged)");
  bool threwTwice = false;
  try {
    g6.bind(once);
  } catch (const std::logic_error&) {
    threwTwice = true;
  }
  check(threwTwice, "binding the same label twice throws std::logic_error");
  g6.jmp(once);
  g6.retInsn();
  // `once` was bound at offset 0 (bind emits no bytes), so the jmp starts at
  // offset 0 and its rel32 slot ends at 5. disp = target - next_rip =
  // 0 - 5 = -5 -> FBFFFFFF, an infinite self-loop terminated only by the
  // trailing ret never being reached in this byte-check (it is not executed).
  checkBytes("jmp to already bound label", g6, "E9FBFFFFFFC3");
}

static void test_sse() {
  // xorps xmm0, xmm0 -> 0F 57 C0. Legacy SSE with xmm0-7 needs no REX byte.
  CodeGen g;
  g.xorps(kXmmBase + 0, kXmmBase + 0);
  checkBytes("xorps xmm0, xmm0", g, "0F57C0");

  // addsd xmm1, xmm2 -> F2 0F 58 CA (destination in the reg field).
  CodeGen g2;
  g2.addsd(kXmmBase + 1, kXmmBase + 2);
  checkBytes("addsd xmm1, xmm2", g2, "F20F58CA");

  // High-numbered xmm registers legitimately need REX.R/B bits:
  // addsd xmm9, xmm2 -> dst=9 in reg (REX.R) -> 44 F2 0F 58 CA.
  CodeGen g3;
  g3.addsd(kXmmBase + 9, kXmmBase + 2);
  checkBytes("addsd xmm9, xmm2", g3, "44F20F58CA");

  // movsd xmm0, [rsi] -> F2 0F 10 06 (load form 0x10, mod=00 rm=110).
  CodeGen g4;
  g4.movsdToXmm(kXmmBase + 0, X64Reg::Rsi, 0);
  checkBytes("movsd xmm0, [rsi]", g4, "F20F1006");

  // cvtsi2sd xmm0, rax -> F2 48 0F 2A C0 (REX.W for the 64-bit source).
  CodeGen g5;
  g5.cvtsi2sd(kXmmBase + 0, X64Reg::Rax);
  checkBytes("cvtsi2sd xmm0, rax", g5, "F2480F2AC0");
}

#ifdef CODEGEN_EXEC_TESTS
// Maps the emitted bytes as executable memory and returns an entry point.
static void* emitExecutable(const std::vector<std::uint8_t>& code) {
  long pageSize = sysconf(_SC_PAGESIZE);
  std::size_t span = static_cast<std::size_t>(pageSize);
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    std::exit(1);
  }
  std::memcpy(mem, code.data(), code.size());
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    std::exit(1);
  }
  return mem;
}

static void test_exec_arithmetic() {
  // int64_t f(void) { return 2 + 3 * 4; }
  CodeGen g;
  g.movImm(X64Reg::Rax, 2);
  g.movImm(X64Reg::Rcx, 3);
  g.movImm(X64Reg::Rdx, 4);
  g.imulReg(X64Reg::Rcx, X64Reg::Rdx);
  g.addReg(X64Reg::Rax, X64Reg::Rcx);
  g.retInsn();
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable((g.finalize(), g.code())));
  std::int64_t result = fn();
  check(result == 14, "exec: 2 + 3 * 4 == 14 (got " + std::to_string(result) + ")");
}

static void test_exec_division() {
  // int64_t f(void) { rax = 100; cdq; idiv rcx(=7); return rax; } -> 14
  CodeGen g;
  g.movImm(X64Reg::Rax, 100);
  g.cdq();
  g.movImm(X64Reg::Rcx, 7);
  g.idiv(X64Reg::Rcx);
  g.retInsn();
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable((g.finalize(), g.code())));
  std::int64_t quotient = fn();
  check(quotient == 14, "exec: 100 / 7 == 14 (got " + std::to_string(quotient) + ")");
}

static void test_exec_labels_loop() {
  // Countdown loop exercising labels, flags and a conditional backward jump:
  //   mov rax, 5 ; top: mov rcx, 1 ; sub rax, rcx ; jne top ; ret
  CodeGen g;
  CodeGen::Label top = g.newLabel();
  g.movImm(X64Reg::Rax, 5);
  g.bind(top);
  g.movImm(X64Reg::Rcx, 1);
  g.subReg(X64Reg::Rax, X64Reg::Rcx);
  g.jcc(Jcc::Ne, top);
  g.retInsn();
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable((g.finalize(), g.code())));
  std::int64_t result = fn();
  check(result == 0, "exec: countdown loop returns 0 (got " + std::to_string(result) + ")");
}

static void test_exec_stack_and_mem() {
  // Exercises push/pop plus base-relative stores:
  //   push rbx ; mov rbx, 0x123 ; mov [rbx+8], rbx... simplified:
  //   push rbx ; mov rax, 7 ; mov rcx, 11 ; add rax, rcx ; pop rbx ; ret
  CodeGen g;
  g.push(X64Reg::Rbx);
  g.movImm(X64Reg::Rax, 7);
  g.movImm(X64Reg::Rcx, 11);
  g.addReg(X64Reg::Rax, X64Reg::Rcx);
  g.pop(X64Reg::Rbx);
  g.retInsn();
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable((g.finalize(), g.code())));
  std::int64_t result = fn();
  check(result == 18, "exec: push/add/pop sequence returns 18 (got " + std::to_string(result) + ")");
}

// Runs the emitted function in a forked child with a 2-second alarm so a
// mis-patched jump (infinite loop or crash) fails the test instead of
// hanging the suite. Returns the child exit code, or -1 on signal/timeout.
// The result slot is a pointer to an int64 inside the shared arena: the
// child writes the computed value there BEFORE exiting (the parent's copy
// of the private mapping is snapshotted at fork time, so plain _exit codes
// cannot carry 64-bit results). A hang is killed after the alarm fires --
// whatever the child wrote up to that point remains visible to the parent.
// rsp captured at the most recent runGuarded call site (see below).
static std::uintptr_t gGuardedCallRsp = 0;

static int runGuarded(void* fn, void* arg, std::int64_t* resultSlot) {
  fflush(nullptr);
  // Record rsp at the guarded call site so tests that switch to a private
  // stack can restore the caller frame exactly (see the roundtrip test).
  std::uintptr_t rspHere = 0;
  asm volatile("mov %%rsp, %0" : "=r"(rspHere));
  gGuardedCallRsp = rspHere;
  pid_t child = fork();
  if (child == 0) {
    alarm(2);
    auto callable = reinterpret_cast<std::int64_t (*)(void*)>(fn);
    *resultSlot = callable(arg);
    _exit(0);
  }
  int status = 0;
  for (int waited = 0;; ++waited) {
    pid_t done = waitpid(child, &status, WNOHANG);
    if (done == child) break;
    usleep(20000);  // 20 ms poll
    if (waited >= 150) {  // ~3 s wall clock: treat as hang
      kill(child, SIGKILL);
      waitpid(child, &status, 0);
      return -1;
    }
  }
  if (WIFSIGNALED(status)) return -1;  // SIGALRM/SIGSEGV -> hang or crash
  return WEXITSTATUS(status);
}

// Byte-level companion for the backward-jmp fix: `jmp rel32` must encode
// disp = target - (instruction_end), pinned when the slot is reserved,
// regardless of epilogue bytes emitted afterwards. The expectation is
// computed from the offsets recorded by the emitter itself (label position
// and displacement-slot end), so it cannot drift out of sync with the
// instruction layout.
static void checkBackwardJmpBytes(const char* name, CodeGen& g,
                                  CodeGen::Label target) {
  const std::size_t jmpStart = g.offset();
  g.jmp(target);
  // The jump was backward (label already bound), so the emitter patched it
  // immediately. Locate the displacement slot by scanning backwards for the
  // `jmp rel32` opcode: the instruction length is variable because movImm
  // may emit either a 7-byte sign-extended form or a 10-byte movabs form.
  const std::size_t dispSlot = jmpStart + 1;  // tentative: opcode at jmpStart
  std::size_t opcode = dispSlot - 1;
  if (opcode >= 1 && g.code()[dispSlot - 1] != 0xE9) {
    // REX prefix precedes the opcode byte; step back over it.
    opcode = dispSlot - 2;
  }
  const std::size_t instrEnd = dispSlot + 4;  // next RIP after the jmp
  const int32_t disp = static_cast<int32_t>(g.labelOffset(target)) -
                       static_cast<int32_t>(instrEnd);
  std::string expected;
  for (std::size_t i = 0; i < opcode; ++i) {  // bytes before the opcode
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02X", g.code()[i]);
    expected += buf;
  }
  expected += "E9";
  for (int b = 0; b < 4; ++b) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02X", (static_cast<uint32_t>(disp) >> (8 * b)) & 0xFF);
    expected += buf;
  }
  // Trailing bytes after the jump (if any) must stay untouched.
  for (std::size_t i = instrEnd; i < g.code().size(); ++i) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02X", g.code()[i]);
    expected += buf;
  }
  checkBytes(name, g, expected);
}

static void test_backward_jmp_bytes() {
  // Backward `jmp` followed by a trailing epilogue: the displacement must
  // be target - jmp_end, NOT target - buffer-size-at-patch-time.
  CodeGen m;
  CodeGen::Label s2 = m.newLabel();
  m.movImm(X64Reg::Rax, 0);
  m.bind(s2);
  m.movImm(X64Reg::Rcx, 1);
  checkBackwardJmpBytes("backward jmp pins next_rip before trailing epilogue", m, s2);
  m.movImm(X64Reg::Rax, 0xAD);
  m.retInsn();

  // Same check for a jump at the very end of the buffer (no trailing code).
  CodeGen n;
  CodeGen::Label t2 = n.newLabel();
  n.movImm(X64Reg::Rcx, 1);
  n.bind(t2);
  n.movImm(X64Reg::Rax, 0);
  checkBackwardJmpBytes("backward jmp with no trailing code", n, t2);
}

static void test_exec_backward_jmp_with_trailing_epilogue() {
  // Regression for the backward-jmp rel32 off-by-one: the original bug
  // derived next_rip from the buffer size at patch time instead of pinning
  // it at the end of the jump instruction when the slot is reserved. Any
  // code emitted AFTER a backward jmp therefore shifted the displacement
  // and made the jump land inside the trailing epilogue -- a scenario a
  // pure byte dump taken right after emission cannot catch. Executed here
  // under the watchdog: a correct patch returns cleanly, an off-by-one
  // lands mid-instruction (crash) or loops (timeout).
  //   mov rax, 3 ; mov rcx, 1 ; top: sub rax, rcx ; cmp rax, 0 ; jne top
  //   movabs rbx, 0xBEEF   <- trailing epilogue emitted AFTER the jump
  //   ret                  <- returns rax == 0 only if control flow is exact
  CodeGen g;
  CodeGen::Label top = g.newLabel();
  g.movImm(X64Reg::Rax, 3);
  g.movImm(X64Reg::Rcx, 1);
  g.bind(top);
  g.subReg(X64Reg::Rax, X64Reg::Rcx);
  g.testReg(X64Reg::Rax);            // ZF set when the counter hits zero
  g.jcc(Jcc::Ne, top);               // backward jcc fixup
  g.movImm(X64Reg::Rbx, 0xBEEF);     // extra bytes AFTER the jump: must not
  g.retInsn();                        // influence the pinned next_rip
  auto fn = reinterpret_cast<std::int64_t (*)()>(
      emitExecutable((g.finalize(), g.code())));
  std::int64_t result = fn();
  check(result == 0,
        "exec: backward jcc with trailing epilogue terminates at 0 (got " +
            std::to_string(result) + ")");

  // Unconditional backward `jmp` executed for real. The loop body cannot
  // test termination without a jcc, so the counter lives in a private
  // mmap'd arena and the watchdog child runs the emitted code until it
  // exits on its own. A correct displacement keeps spinning safely until
  // the guard kills it (-1 expected); a mis-patched one escapes into the
  // sentinel epilogue emitted AFTER the jmp and returns normally (0) or
  // crashes.
  const std::size_t kSpinArena = 4096;
  void* sarena = mmap(nullptr, kSpinArena, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (sarena == MAP_FAILED) {
    std::perror("mmap spin arena");
    std::exit(1);
  }
  CodeGen k;
  CodeGen::Label spin = k.newLabel();
  k.movReg(X64Reg::Rbp, X64Reg::Rdi);              // arena base from caller
  k.movImm(X64Reg::Rax, 0);
  k.movToMem(X64Reg::Rbp, 0, X64Reg::Rax);         // counter = 0
  k.movImm(X64Reg::Rcx, 1);                        // step
  k.movImm(X64Reg::Rdx, 1u << 30);                 // effectively never hit
  k.bind(spin);                                    // backward target
  k.movFromMem(X64Reg::Rax, X64Reg::Rbp, 0);       // load counter
  k.addReg(X64Reg::Rax, X64Reg::Rcx);              // counter += step
  k.movToMem(X64Reg::Rbp, 0, X64Reg::Rax);         // store counter
  k.cmpReg(X64Reg::Rax, X64Reg::Rdx);              // flags only
  k.jmp(spin);                                     // unconditional backward
                                                   // jmp; epilogue follows
  k.movImm(X64Reg::Rax, 0xAD);                     // reachable only if the
                                                   // jmp is mis-patched
  k.retInsn();
  std::int64_t spinResult = 0x5EED;  // poison: untouched if the child hangs
  int guarded = runGuarded(emitExecutable((k.finalize(), k.code())), sarena,
                           &spinResult);
  munmap(sarena, kSpinArena);
  check(guarded == -1 && spinResult == 0x5EED,
        "exec: unconditional backward jmp spins (watchdog -1), got " +
            std::to_string(guarded) + " result=" + std::to_string(spinResult));
}

static void test_exec_rbp_disp32_roundtrip() {
  // Regression for the ModRM fix: `mov [rbp+disp32], reg` used to encode
  // mod=00/rm=101 (RIP-relative) for rbp-based disp32 operands, so stores
  // silently wrote into the executable page neighborhood and loads read
  // back zeros. This roundtrip runs its frame on a PRIVATE mmap'd stack
  // (the requested "private stack frame" scenario) and passes only if the
  // store and the load address the SAME rbp-relative cell with the full
  // 64-bit imm64 payload.
  //   Callee body (invoked through a tiny trampoline that switches stacks):
  //     push rbp                       ; chain onto the private stack
  //     mov  rbp, rsp                  ; private frame pointer
  //     movabs rcx, 0x77AA3355DEADBEEF ; imm64 payload (forces movabs)
  //     mov [rbp-0x2000], rcx          ; forced disp32 store (mod=10)
  //     mov rdx, [rbp-0x2000]          ; forced disp32 load (same cell)
  //     sub rdx, rcx                   ; zero iff the roundtrip preserved
  //                                    ; the payload
  //     mov rax, rdx                   ; return value = diff
  //     pop  rbp
  //     ret                            ; returns to the exit stub
  //   Trampoline (called from C, then jumps into the callee):
  //     movabs rsp, <private stack top> ; switch to the private frame
  //     movabs rax, <callee>
  //     jmp  *rax                        ; no call/return through the C frame
  //   Exit stub (callee `ret` lands here):
  //     movabs rdi, &gRoundtripDiff     ; > INT32_MAX -> movabs path
  //     mov  [rdi], rax                 ; publish the full 64-bit verdict
  //     hlt                             ; alarm-driven SIGSEGV ends the child
  // The stub publishes the diff into ordinary process memory BEFORE the
  // child dies, so the value survives even if the hang is killed with
  // SIGKILL (which bypasses _exit and would otherwise truncate results to
  // the low byte of the exit code). A correctly patched roundtrip returns
  // diff == 0; a mis-encoded store/load leaves the 0xBAD poison or a
  // nonzero difference. Status -1 (hang via hlt) is therefore the EXPECTED
  // outcome here; any clean exit means the emitted control flow escaped.
  static volatile std::int64_t gRoundtripDiff = 0xBAD;
  gRoundtripDiff = 0xBAD;

  const std::int64_t kPayload = static_cast<std::int64_t>(0x77AA3355DEADBEEFULL);

  const std::size_t kPrivateStack = 64 * 1024;
  void* stack = mmap(nullptr, kPrivateStack, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    std::perror("mmap private stack");
    std::exit(1);
  }
  std::uintptr_t rawTop =
      reinterpret_cast<std::uintptr_t>(stack) + kPrivateStack - 64;
  std::uintptr_t stackTop = rawTop & ~static_cast<std::uintptr_t>(15);

  // Build the three pieces first so every absolute address is known when
  // each buffer is finalized; all live inside ONE executable page.
  CodeGen callee;
  callee.push(X64Reg::Rbp);
  callee.movReg(X64Reg::Rbp, X64Reg::Rsp);
  callee.movImm(X64Reg::Rcx, kPayload);         // imm64 forces movabs
  callee.movToMem(X64Reg::Rbp, -0x2000, X64Reg::Rcx);   // forced disp32 store
  callee.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -0x2000); // forced disp32 load
  callee.subReg(X64Reg::Rdx, X64Reg::Rcx);      // zero iff identical
  callee.movReg(X64Reg::Rax, X64Reg::Rdx);
  callee.pop(X64Reg::Rbp);
  callee.retInsn();
  callee.finalize();

  CodeGen stub;
  stub.movImm(X64Reg::Rdi, reinterpret_cast<std::int64_t>(&gRoundtripDiff));
  stub.movToMem(X64Reg::Rdi, 0, X64Reg::Rax);   // publish the verdict
  stub.halt();                                  // spin until the watchdog kills
  stub.finalize();

  CodeGen tramp;
  tramp.movImm(X64Reg::Rsp, static_cast<std::int64_t>(stackTop));
  tramp.movImm(X64Reg::Rax, 0);  // placeholder; patched once layout is known
  tramp.jumpReg(X64Reg::Rax);
  tramp.finalize();

  // Layout: [trampoline][callee][stub] inside one RWX page.
  const std::size_t tOff = 0;
  const std::size_t cOff = tramp.code().size();
  const std::size_t sOff = cOff + callee.code().size();
  long pageSize = sysconf(_SC_PAGESIZE);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(pageSize), 0);
  memcpy(bytes.data() + tOff, tramp.code().data(), tramp.code().size());
  memcpy(bytes.data() + cOff, callee.code().data(), callee.code().size());
  memcpy(bytes.data() + sOff, stub.code().data(), stub.code().size());

  // Patch the trampoline's `movabs rax, callee` operand (opcode at tOff+7,
  // 48 B8 imm64) now that the callee's entry point is fixed.
  std::uint64_t base = 0;  // resolved per-run below via the mapped address.
  void* mem = mmap(nullptr, static_cast<std::size_t>(pageSize),
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap exec page");
    std::exit(1);
  }
  base = reinterpret_cast<std::uintptr_t>(mem);
  std::uint64_t calleeAddr = base + cOff;
  memcpy(bytes.data() + tOff + 7, &calleeAddr, sizeof(calleeAddr));
  memcpy(mem, bytes.data(), bytes.size());
  if (mprotect(mem, static_cast<std::size_t>(pageSize), PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect exec page");
    std::exit(1);
  }

  // Watchdog child: the stub halts after publishing, so runGuarded returns
  // -1 by design; the verdict lives in gRoundtripDiff (fork shares COW
  // pages, but the child WRITES the value before dying, and the parent
  // reads its own mapping -- so the child must publish through a MAP_SHARED
  // channel instead). Use a plain global plus _exit-free death: writing to
  // a normal variable in the child does NOT propagate to the parent. The
  // stub therefore writes into a SHARED mmap'd slot instead.
  auto* shared = static_cast<std::int64_t*>(
      mmap(nullptr, sizeof(std::int64_t), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (shared == MAP_FAILED) {
    std::perror("mmap shared slot");
    std::exit(1);
  }
  *shared = 0xBAD;
  // Rebuild the stub to target the shared slot, then re-patch the page.
  CodeGen stub2;
  stub2.movImm(X64Reg::Rdi, reinterpret_cast<std::int64_t>(shared));
  stub2.movToMem(X64Reg::Rdi, 0, X64Reg::Rax);
  stub2.halt();
  stub2.finalize();
  // Same length as the original stub (same instruction shapes).
  if (stub2.code().size() != stub.code().size()) {
    std::fprintf(stderr, "stub size changed; abort\n");
    std::exit(1);
  }
  // Unprotect, rewrite the stub region, re-protect.
  if (mprotect(mem, static_cast<std::size_t>(pageSize),
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    std::perror("mprotect rw+x");
    std::exit(1);
  }
  memcpy(static_cast<std::uint8_t*>(mem) + sOff, stub2.code().data(),
         stub2.code().size());

  int status = runGuarded(mem, nullptr, nullptr);
  std::int64_t diff = *shared;
  check(diff == 0,
        "exec: [rbp+disp32] store->load roundtrip on private stack preserves "
        "imm64 (status=" +
            std::to_string(status) + ", diff=" + std::to_string(diff) + ")");

  // Byte-level companion: rbp-based disp32 must use mod=10/rm=101 (never
  // the RIP-relative mod=00 encoding that produced the original bug).
  CodeGen b;
  b.movToMem(X64Reg::Rbp, 0x1234, X64Reg::Rax);
  b.movFromMem(X64Reg::Rcx, X64Reg::Rbp, 0x1234);
  check(toHex((b.finalize(), b.code())) == "48898534120000488B8D34120000",
        "mov [rbp+0x1234]/[rbp+0x1234] byte-exact ModRM (mod=10, rm=101)");
  munmap(stack, kPrivateStack);
}

static void test_exec_syscall_write() {
  // Emits `write(1, "Jolt\n", 5)` followed by a zero return, proving the
  // syscall gate and rip-relative addressing work end to end.
  static const char message[] = "Jolt\n";
  CodeGen g;
  g.movImm(X64Reg::Rax, 1);  // SYS_write
  g.movImm(X64Reg::Rdi, 1);  // stdout
  std::size_t beforeLea = g.offset();
  g.leaRip(X64Reg::Rsi, 0);  // placeholder displacement, patched below
  std::size_t leaLen = g.offset() - beforeLea;
  g.movImm(X64Reg::Rdx, 5);  // length
  g.syscallInsn();
  g.xorReg(X64Reg::Rax, X64Reg::Rax);
  g.retInsn();
  // Append the payload after the code and compute the signed rip delta:
  // data starts at the end of the buffer; next_rip is right after the lea.
  std::vector<std::uint8_t> bytes = g.code();
  std::int32_t delta =
      static_cast<std::int32_t>(bytes.size()) - static_cast<std::int32_t>(beforeLea + leaLen);
  std::memcpy(bytes.data() + beforeLea + leaLen - 4, &delta, sizeof(delta));
  bytes.insert(bytes.end(), message, message + sizeof(message));
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(bytes));
  std::int64_t result = fn();
  check(result == 0, "exec: write(\"Jolt\\n\", stdout) via syscall returned 0");
}
#endif

int main() {
  test_mov_imm();
  test_mov_reg_and_stack();
  test_memory_operands();
  test_alu();
  test_control_flow();
  test_backward_jmp_bytes();
  test_sse();
#ifdef CODEGEN_EXEC_TESTS
  test_exec_arithmetic();
  test_exec_division();
  test_exec_labels_loop();
  test_exec_stack_and_mem();
  test_exec_backward_jmp_with_trailing_epilogue();
  test_exec_rbp_disp32_roundtrip();
  test_exec_syscall_write();
#else
  std::printf("skipped: execution tests require Linux x86-64\n");
#endif
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all codegen tests passed\n");
  return 0;
}
