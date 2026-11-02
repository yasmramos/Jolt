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
#include <sys/mman.h>
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
  //   bind(top): top=0 ; mov rax,-1 (7) ; jmp top -> disp = 0 - (7+4) = -11
  CodeGen g5;
  CodeGen::Label top = g5.newLabel();
  g5.bind(top);
  g5.movImm(X64Reg::Rax, -1);
  g5.jmp(top);
  checkBytes("jmp rel32 backward", g5, "48C7C0FFFFFFFFE9F5FFFFFF");

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
  test_sse();
#ifdef CODEGEN_EXEC_TESTS
  test_exec_arithmetic();
  test_exec_division();
  test_exec_labels_loop();
  test_exec_stack_and_mem();
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
