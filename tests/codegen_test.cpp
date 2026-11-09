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
#include <unordered_map>
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

static void test_target_descriptor() {
  CodeGen g;
  check(g.target().arch == Arch::X86_64 && g.target().os == Os::Linux &&
            g.target().abi == Abi::SysV,
        "codegen defaults to x86_64-linux-sysv");
  check(targetName(Target::linuxX64()) == "x86_64-linux-sysv",
        "target name is stable");

  bool rejected = false;
  try {
    CodeGen unsupported(Target{Arch::X86_64, Os::Windows, Abi::Win64});
    (void)unsupported;
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "codegen rejects a target with no backend");
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
  // keeps the low bits (001), and the rbp base forces mod=01 disp8. The
  // mandatory SSE prefix comes before REX, so the bytes are
  // F2 44 0F 11 4D 00.
  CodeGen gStore9;
  gStore9.movsdFromXmm(X64Reg::Rbp, 0, kXmmBase + 9);
  checkBytes("movsd [rbp], xmm9 (REX.R)", gStore9, "F2440F114D00");

  // RIP-relative lea: 48 8D 35 <disp32>, mod=00 rm=101 without SIB.
  CodeGen g4;
  g4.leaRip(X64Reg::Rsi, 0x77);
  checkBytes("lea rsi, [rip+0x77]", g4, "488D3577000000");

  // mov qword [rbp-16], 5 -> REX.W C7 /0, mod=01 disp8, imm32.
  CodeGen g5;
  g5.movImmToMem(X64Reg::Rbp, -16, 5);
  checkBytes("mov qword [rbp-16], 5", g5, "48C745F005000000");

  // mov byte [rbp], 'A' -> C6 /0, no REX for a low base.
  CodeGen g6;
  g6.movImmByteToMem(X64Reg::Rbp, 0, 'A');
  checkBytes("mov byte [rbp], 'A'", g6, "C6450041");

  // A base above r7 sets REX.B: mov byte [r8], 'A' -> 41 C6 00 41 (mod=00,
  // since r8 is not rbp/r13 and needs no forced disp8).
  CodeGen g7;
  g7.movImmByteToMem(X64Reg::R8, 0, 'A');
  checkBytes("mov byte [r8], 'A' (REX.B)", g7, "41C60041");

  // mov byte [rbp-1], dl -> REX 40 (always, to reach dl not dh), 88 /r.
  CodeGen g8;
  g8.movByteToMem(X64Reg::Rbp, -1, X64Reg::Rdx);
  checkBytes("mov [rbp-1], dl", g8, "408855FF");

  // mov byte [rbp], r8b -> REX.R selects r8b: 44 88 45 00.
  CodeGen g9;
  g9.movByteToMem(X64Reg::Rbp, 0, X64Reg::R8);
  checkBytes("mov [rbp], r8b (REX.R)", g9, "44884500");

  // cmp rax, 2 -> REX.W 81 /7 imm32.
  CodeGen g10;
  g10.cmpImm32(X64Reg::Rax, 2);
  checkBytes("cmp rax, 2", g10, "4881F802000000");

  // divUnsigned(rcx): xor rdx, rdx; div rcx -> 48 31 D2 48 F7 F1.
  CodeGen g11;
  g11.divUnsigned(X64Reg::Rcx);
  checkBytes("unsigned div rcx", g11, "4831D248F7F1");

  // divReg(rcx): cqo; idiv rcx -> 48 99 48 F7 F9.
  CodeGen g12;
  g12.divReg(X64Reg::Rcx);
  checkBytes("signed idiv rcx", g12, "489948F7F9");

  // lea rax, [rip+label] with the label bound immediately after: the
  // displacement is zero, and the four bytes are the reserved fixup slot.
  CodeGen g13;
  CodeGen::Label target = g13.newLabel();
  g13.leaRipLabel(X64Reg::Rax, target);
  g13.bind(target);
  g13.retInsn();
  checkBytes("lea rax, [rip+label] (forward fixup)", g13, "488D0500000000C3");

  // A backward reference resolves to the negative distance.
  CodeGen g14;
  CodeGen::Label back = g14.newLabel();
  g14.bind(back);
  g14.retInsn();
  g14.leaRipLabel(X64Reg::Rax, back);
  g14.retInsn();
  // lea sits at offset 1, its next instruction at offset 8, so the distance to
  // offset 0 is -8 = FFFFFFF8.
  checkBytes("lea rax, [rip+label] (backward fixup)", g14, "C3488D05F8FFFFFFC3");
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
  checkBytes("addsd xmm9, xmm2", g3, "F2440F58CA");

  // movsd xmm0, [rsi] -> F2 0F 10 06 (load form 0x10, mod=00 rm=110).
  CodeGen g4;
  g4.movsdToXmm(kXmmBase + 0, X64Reg::Rsi, 0);
  checkBytes("movsd xmm0, [rsi]", g4, "F20F1006");

  // cvtsi2sd xmm0, rax -> F2 48 0F 2A C0 (REX.W for the 64-bit source).
  CodeGen g5;
  g5.cvtsi2sd(kXmmBase + 0, X64Reg::Rax);
  checkBytes("cvtsi2sd xmm0, rax", g5, "F2480F2AC0");

  // movq xmm0, rax -> 66 48 0F 6E C0 (REX.W is mandatory: without it the
  // instruction degrades to the 32-bit `movd`).
  CodeGen g6;
  g6.movqToXmm(kXmmBase + 0, X64Reg::Rax);
  checkBytes("movq xmm0, rax", g6, "66480F6EC0");

  // movq rax, xmm0 -> 66 48 0F 7E C0.
  CodeGen g7;
  g7.movqFromXmm(X64Reg::Rax, kXmmBase + 0);
  checkBytes("movq rax, xmm0", g7, "66480F7EC0");

  // movsd xmm0, xmm1 -> F2 0F 10 C1 (register-register load form).
  CodeGen g8;
  g8.movsdReg(kXmmBase + 0, kXmmBase + 1);
  checkBytes("movsd xmm0, xmm1", g8, "F20F10C1");

  // cvtsd2ss xmm0, xmm0 -> F2 0F 5A C0; cvtss2sd xmm0, xmm0 -> F3 0F 5A C0.
  CodeGen g9;
  g9.cvtsd2ss(kXmmBase + 0, kXmmBase + 0);
  checkBytes("cvtsd2ss xmm0, xmm0", g9, "F20F5AC0");
  CodeGen g10;
  g10.cvtss2sd(kXmmBase + 0, kXmmBase + 0);
  checkBytes("cvtss2sd xmm0, xmm0", g10, "F30F5AC0");

  // shr rax, cl -> 48 D3 E8 (logical shift, extension /5).
  CodeGen g11;
  g11.shrCl(X64Reg::Rax);
  checkBytes("shr rax, cl", g11, "48D3E8");

  // test rax, 1 -> 48 F7 C0 01 00 00 00 (F7 /0 with an imm32).
  CodeGen g12;
  g12.testImm32(X64Reg::Rax, 1);
  checkBytes("test rax, 1", g12, "48F7C001000000");
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
  // int64_t f(void) { rax = 100; cqo; idiv rcx(=7); return rax; } -> 14
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
//
// The result slot lives in a MAP_SHARED page: a plain store into a stack
// variable would be invisible to the parent, because fork gives the child a
// private copy-on-write address space. Writing the computed value into shared
// memory carries it across the fork. The caller's incoming value is seeded
// into the slot first, so a test can plant a poison value and confirm it
// survives an untouched run.
static int runGuarded(void* fn, void* arg, std::int64_t* resultSlot) {
  fflush(nullptr);
  std::int64_t* sharedResult = static_cast<std::int64_t*>(
      mmap(nullptr, sizeof(std::int64_t), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (sharedResult == MAP_FAILED) {
    std::perror("mmap shared result slot");
    std::exit(1);
  }
  *sharedResult = *resultSlot;  // carry the caller's value (poison) into the child

  pid_t child = fork();
  if (child == 0) {
    alarm(2);
    auto callable = reinterpret_cast<std::int64_t (*)(void*)>(fn);
    *sharedResult = callable(arg);
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
      *resultSlot = *sharedResult;  // keep whatever the child published
      munmap(sharedResult, sizeof(std::int64_t));
      return -1;
    }
  }
  *resultSlot = *sharedResult;
  munmap(sharedResult, sizeof(std::int64_t));
  if (WIFSIGNALED(status)) return -1;  // SIGALRM/SIGSEGV -> hang or crash
  return WEXITSTATUS(status);
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
  // mod=00/rm=101 (RIP-relative) for rbp-based disp32 operands, so a store
  // silently wrote into the executable page neighborhood and the matching
  // load read back zeros. The roundtrip runs on a private mmap'd stack (a
  // standalone frame, not the C stack) and passes only if the store and the
  // load address the SAME rbp-relative cell with the full 64-bit payload:
  //
  //   push rbp                       ; keep the C frame
  //   mov  rbp, rsp
  //   mov  r11, rsp                  ; r11 = the rsp to restore after the switch
  //   movabs rax, privTop
  //   mov  rsp, rax                  ; switch to the private stack
  //   push rbp; mov rbp, rsp         ; private frame pointer
  //   movabs rcx, payload            ; imm64 forces movabs
  //   mov  [rbp-0x2000], rcx         ; forced disp32 store (mod=10)
  //   mov  rdx, [rbp-0x2000]         ; forced disp32 load (same cell)
  //   sub  rdx, rcx                  ; zero iff the payload was preserved
  //   mov  rax, rdx                  ; return value = diff
  //   mov  rsp, r11                  ; back onto the C frame
  //   pop  rbp; ret                  ; complete the call contract
  //
  // The private stack is MAP_SHARED, so after the guarded child returns the
  // parent can read the cell at its exact address and confirm the store
  // physically landed there rather than at a RIP-relative alias.
  const std::int64_t kPayload = static_cast<std::int64_t>(0x77AA3355DEADBEEFULL);
  const std::size_t kPrivateStack = 128 * 1024;
  void* stack = mmap(nullptr, kPrivateStack, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) {
    std::perror("mmap private stack");
    std::exit(1);
  }
  std::uintptr_t rawTop =
      reinterpret_cast<std::uintptr_t>(stack) + kPrivateStack - 64;
  std::uintptr_t privTop = rawTop & ~static_cast<std::uintptr_t>(15);
  // The trampoline's `push rbp` drops rsp by 8, so [rbp-0x2000] is 8 bytes
  // below privTop-0x2000.
  std::int64_t* cell = reinterpret_cast<std::int64_t*>(privTop - 0x2008);

  CodeGen g;
  g.push(X64Reg::Rbp);
  g.movReg(X64Reg::Rbp, X64Reg::Rsp);
  g.movReg(X64Reg::R11, X64Reg::Rsp);              // survives the stack switch
  g.movImm(X64Reg::Rax, static_cast<std::int64_t>(privTop));
  g.movReg(X64Reg::Rsp, X64Reg::Rax);
  g.push(X64Reg::Rbp);
  g.movReg(X64Reg::Rbp, X64Reg::Rsp);
  g.movImm(X64Reg::Rcx, kPayload);                 // imm64 forces movabs
  g.movToMem(X64Reg::Rbp, -0x2000, X64Reg::Rcx);   // forced disp32 store
  g.movFromMem(X64Reg::Rdx, X64Reg::Rbp, -0x2000); // forced disp32 load
  g.subReg(X64Reg::Rdx, X64Reg::Rcx);              // zero iff identical
  g.movReg(X64Reg::Rax, X64Reg::Rdx);              // verdict = diff
  g.movReg(X64Reg::Rsp, X64Reg::R11);              // restore the C frame
  g.pop(X64Reg::Rbp);
  g.retInsn();
  g.finalize();

  void* mem = emitExecutable(g.code());
  std::int64_t diff = 0xBAD;  // poison: overwritten by the child's return
  int status = runGuarded(mem, nullptr, &diff);

  check(status == 0 && diff == 0 && *cell == kPayload,
        "exec: [rbp+disp32] store->load roundtrip on a private stack preserves "
        "imm64 (status=" +
            std::to_string(status) + ", diff=" + std::to_string(diff) +
            ", cell=0x" + [&] {
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%llx",
                            static_cast<unsigned long long>(*cell));
              return std::string(buf);
            }() + ")");

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

static void test_setcc_movzx() {
  // setcc r/m8 -> 0F 90+cc /0. The condition code is the low nibble of Jcc.
  CodeGen g;
  g.setcc(Jcc::Eq, X64Reg::Rax);
  checkBytes("sete al", g, "0F94C0");

  CodeGen g2;
  g2.setcc(Jcc::Ne, X64Reg::Rax);
  checkBytes("setne al", g2, "0F95C0");

  CodeGen g3;
  g3.setcc(Jcc::Lt, X64Reg::Rax);
  checkBytes("setl al", g3, "0F9CC0");

  // spl/bpl/sil/dil (registers 4-7) need a REX prefix even without REX.W:
  // without it, rm=100 means ah, not spl. `sete spl` -> 40 0F 94 C4.
  CodeGen g4;
  g4.setcc(Jcc::Eq, X64Reg::Rsp);
  checkBytes("sete spl (REX needed)", g4, "400F94C4");

  // r8-r15 add REX.B: `sete r8b` -> 41 0F 94 C0.
  CodeGen g5;
  g5.setcc(Jcc::Eq, X64Reg::R8);
  checkBytes("sete r8b (REX.B)", g5, "410F94C0");

  // Extended registers (r8-r15) must not leak bit 3 of their register number
  // into the neighbouring ModRM field: the reg/rm fields are only 3 bits wide
  // and the high bit lives in REX. `test r8, r8` -> 4D 85 C0, not 4D 85 C8.
  CodeGen g8;
  g8.testReg(X64Reg::R8);
  checkBytes("test r8, r8 (ModRM mask)", g8, "4D85C0");

  CodeGen g9;
  g9.testReg(X64Reg::R15);
  checkBytes("test r15, r15 (ModRM mask)", g9, "4D85FF");

  // `mov r13, r14` -> 4D 89 F5.
  CodeGen g10;
  g10.movReg(X64Reg::R13, X64Reg::R14);
  checkBytes("mov r13, r14 (ModRM mask)", g10, "4D89F5");

  // `cmp r12, r13` -> 4D 39 EC.
  CodeGen g11;
  g11.cmpReg(X64Reg::R12, X64Reg::R13);
  checkBytes("cmp r12, r13 (ModRM mask)", g11, "4D39EC");

  // `imul r9, r10` -> 4D 0F AF CA.
  CodeGen g12;
  g12.imulReg(X64Reg::R9, X64Reg::R10);
  checkBytes("imul r9, r10 (ModRM mask)", g12, "4D0FAFCA");

  // movzx r64, r/m8 -> REX.W 0F B6 /r.
  CodeGen g6;
  g6.movzx(X64Reg::Rax, X64Reg::Rax);
  checkBytes("movzx rax, al", g6, "480FB6C0");

  CodeGen g7;
  g7.movzx(X64Reg::R8, X64Reg::Rax);
  checkBytes("movzx r8, al (REX.R)", g7, "4C0FB6C0");

  // `add rax, imm32` -> REX.W 81 /0 id. `add rax, 8` -> 48 81 C0 08 00 00 00.
  CodeGen g13;
  g13.addImm32(X64Reg::Rax, 8);
  checkBytes("add rax, 8", g13, "4881C008000000");

  // The rm field is masked to three bits, so `add r8, imm32` keeps REX.B and
  // a clean ModRM: 49 81 C0 08 00 00 00 (not ...C8).
  CodeGen g14;
  g14.addImm32(X64Reg::R8, 8);
  checkBytes("add r8, 8 (ModRM mask)", g14, "4981C008000000");

  // `sub rax, imm32` -> REX.W 81 /5 id.
  CodeGen g15;
  g15.subRegImm32(X64Reg::Rax, 16);
  checkBytes("sub rax, 16", g15, "4881E810000000");

  CodeGen g16;
  g16.subRegImm32(X64Reg::R15, 16);
  checkBytes("sub r15, 16 (ModRM mask)", g16, "4981EF10000000");

  // `call rel32` to a label patches the displacement at finalize: the target
  // sits 5 bytes after the call, so the displacement is 0.
  CodeGen g17;
  CodeGen::Label target = g17.newLabel();
  g17.callLabel(target);
  g17.bind(target);
  checkBytes("call label (displacement 0)", g17, "E800000000");
}

#ifdef CODEGEN_EXEC_TESTS
static void test_exec_setcc() {
  // cmp rax, rcx ; sete al ; movzx rax, al ; ret. With rax=5, rcx=5 the
  // result must be exactly 1 (not 0xFF.. or a stale high byte).
  CodeGen g;
  g.movImm(X64Reg::Rax, 5);
  g.movImm(X64Reg::Rcx, 5);
  g.cmpReg(X64Reg::Rax, X64Reg::Rcx);
  g.setcc(Jcc::Eq, X64Reg::Rax);
  g.movzx(X64Reg::Rax, X64Reg::Rax);
  g.retInsn();
  auto fn = reinterpret_cast<std::int64_t (*)()>(emitExecutable(g.code()));
  check(fn() == 1, "exec: (5 == 5) materializes as 1");

  // A false comparison must materialize as exactly 0.
  CodeGen g2;
  g2.movImm(X64Reg::Rax, 5);
  g2.movImm(X64Reg::Rcx, 9);
  g2.cmpReg(X64Reg::Rax, X64Reg::Rcx);
  g2.setcc(Jcc::Lt, X64Reg::Rax);
  g2.movzx(X64Reg::Rax, X64Reg::Rax);
  g2.retInsn();
  auto fn2 = reinterpret_cast<std::int64_t (*)()>(emitExecutable(g2.code()));
  check(fn2() == 1, "exec: (5 < 9) materializes as 1");

  CodeGen g3;
  g3.movImm(X64Reg::Rax, 9);
  g3.movImm(X64Reg::Rcx, 5);
  g3.cmpReg(X64Reg::Rax, X64Reg::Rcx);
  g3.setcc(Jcc::Lt, X64Reg::Rax);
  g3.movzx(X64Reg::Rax, X64Reg::Rax);
  g3.retInsn();
  auto fn3 = reinterpret_cast<std::int64_t (*)()>(emitExecutable(g3.code()));
  check(fn3() == 0, "exec: (9 < 5) materializes as 0");
}
#endif

// Byte-exact encodings for the x87 helpers float `%` uses. Verified against
// `objdump -Mintel` on the equivalent assembly (the FPU is legacy: no REX
// prefix, so the memory form is `DD`/`D9` + ModRM).
static void test_x87() {
  // fld qword [rbp-16] -> DD 45 F0 (DD /0, the 64-bit load).
  CodeGen g;
  g.fldM64(X64Reg::Rbp, -16);
  checkBytes("fld qword [rbp-16]", g, "DD45F0");

  // fld qword [rsp] -> DD 04 24 (SIB because rsp is the base).
  CodeGen g2;
  g2.fldM64(X64Reg::Rsp, 0);
  checkBytes("fld qword [rsp]", g2, "DD0424");

  // fstp qword [rsp+8] -> DD 5C 24 08 (DD /3).
  CodeGen g3;
  g3.fstpM64(X64Reg::Rsp, 8);
  checkBytes("fstp qword [rsp+8]", g3, "DD5C2408");

  // fstp st(0) -> DD D8.
  CodeGen g4;
  g4.fstpSt0();
  checkBytes("fstp st(0)", g4, "DDD8");

  // fprem -> D9 F8.
  CodeGen g5;
  g5.fprem();
  checkBytes("fprem", g5, "D9F8");

  // fnstsw ax -> DF E0.
  CodeGen g6;
  g6.fnstswAx();
  checkBytes("fnstsw ax", g6, "DFE0");

  // test ah, 4 -> F6 C4 04.
  CodeGen g7;
  g7.testAhImm8(0x04);
  checkBytes("test ah, 4", g7, "F6C404");
}

int main() {
  test_target_descriptor();
  test_mov_imm();
  test_mov_reg_and_stack();
  test_memory_operands();
  test_alu();
  test_control_flow();
  test_backward_jmp_bytes();
  test_sse();
  test_x87();
  test_setcc_movzx();
#ifdef CODEGEN_EXEC_TESTS
  test_exec_arithmetic();
  test_exec_division();
  test_exec_labels_loop();
  test_exec_stack_and_mem();
  test_exec_backward_jmp_with_trailing_epilogue();
  test_exec_rbp_disp32_roundtrip();
  test_exec_syscall_write();
  test_exec_setcc();
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
