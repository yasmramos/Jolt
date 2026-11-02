#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Minimal x86-64 machine-code emitter (PHASE 7a).
//
// Emits native instructions directly as bytes, with labels resolved in two
// passes: instructions that reference a label are patched once the label is
// bound to an offset. No external assembler or JIT library is used, keeping
// the zero-dependency rule of AGENT.md §5.1/§8.
//
// Register naming follows System V AMD64 conventions. The backend reserves
// rbx for the register window and r12-r15 for temporary operands.

enum class X64Reg {
  Rax,
  Rcx,
  Rdx,
  Rbx,
  Rsp,
  Rbp,
  Rsi,
  Rdi,
  R8,
  R9,
  R10,
  R11,
  R12,
  R13,
  R14,
  R15,
};

// General-purpose registers live in the range [0, kXmmBase); SSE registers
// use indices kXmmBase + N (xmm0..xmm15).
constexpr int kXmmBase = 16;

// Condition codes for `jcc`, matching the Intel opcode low nibble.
enum class Jcc : std::uint8_t {
  Eq = 0x84,
  Ne = 0x85,
  Lt = 0x8C,
  Gt = 0x8F,
  Le = 0x8E,
  Ge = 0x8D,
};

class CodeGen {
 public:
  // Opaque handle for a jump target. Bind it with `bind()` at the current
  // offset; every pending reference is patched with the relative distance.
  struct Label {
    int id = -1;
  };

  CodeGen() = default;

  Label newLabel();
  void bind(Label label);

  // Data movement.
  void movImm(X64Reg dst, std::int64_t value);            // mov reg, imm (sign-extended) or movabs
  void movReg(X64Reg dst, X64Reg src);                    // mov reg, reg
  void movToMem(X64Reg base, std::int64_t disp, X64Reg src);
  void movFromMem(X64Reg dst, X64Reg base, std::int64_t disp);
  void movImmToMem(X64Reg base, std::int64_t disp, std::int64_t value);
  void leaRip(X64Reg dst, std::int32_t ripDelta);         // lea dst, [rip + delta]

  // Arithmetic/logical on registers (64-bit unless noted).
  void addReg(X64Reg dst, X64Reg src);
  void subReg(X64Reg dst, X64Reg src);
  void imulReg(X64Reg dst, X64Reg src);
  void idiv(X64Reg divisor);  // signed divide rdx:rax by divisor -> rax, remainder rdx
  void cdq();                 // sign-extend rax into rdx:rax (needed before idiv)
  void negReg(X64Reg dst);
  void notReg(X64Reg dst);
  void andReg(X64Reg dst, X64Reg src);
  void orReg(X64Reg dst, X64Reg src);
  void xorReg(X64Reg dst, X64Reg src);
  void shlCl(X64Reg dst);   // dst <<= cl
  void sarCl(X64Reg dst);   // dst >>= cl (arithmetic)

  // SSE scalar double helpers (used by the float fast paths).
  void movsdToXmm(int xmm, X64Reg base, std::int64_t disp);    // movsd xmm, [base+disp]
  void movsdFromXmm(X64Reg base, std::int64_t disp, int xmm);  // movsd [base+disp], xmm
  void addsd(int dstXmm, int srcXmm);
  void subsd(int dstXmm, int srcXmm);
  void mulsd(int dstXmm, int srcXmm);
  void divsd(int dstXmm, int srcXmm);
  void ucomisd(int a, int b);
  void cvttsd2si(X64Reg dst, int srcXmm);  // truncate toward zero
  void cvtsi2sd(int dstXmm, X64Reg src);
  void xorps(int dstXmm, int srcXmm);  // also zeroes the upper bits of xmm

  // Comparisons and control flow.
  void cmpReg(X64Reg lhs, X64Reg rhs);
  void testReg(X64Reg reg);
  void jmp(Label label);
  void jcc(Jcc cc, Label label);
  void callRel(std::int32_t offsetBytes);  // call rel32 (caller resolves the target)

  // Stack.
  void push(X64Reg reg);
  void pop(X64Reg reg);

  // Linux x86-64 syscall gate: arguments must already be in rax, rdi, rsi,
  // rdx, r10, r8, r9 (System V order with r10 in place of rcx).
  void syscallInsn();
  void retInsn();
  void ud2();

  // Raw bytes, used by the runtime blob and constant payloads.
  void emitBytes(const std::uint8_t* data, std::size_t length);

  std::size_t offset() const { return code_.size(); }
  const std::vector<std::uint8_t>& code() const { return code_; }

 private:
  struct PendingFixup {
    std::size_t patchAt = 0;  // offset of the 4-byte displacement
    int labelId = -1;
  };

  std::vector<std::uint8_t> code_;
  std::vector<std::size_t> labelOffset_;  // label id -> offset, max until bound
  std::vector<PendingFixup> fixups_;

  static std::uint8_t enc(X64Reg reg) { return static_cast<std::uint8_t>(reg); }

  void rex(bool w, std::uint8_t regField, bool hasIndex, std::uint8_t index, std::uint8_t rm);
  static bool ripForm(bool hasIndex, std::uint8_t rm);
  void modRmDisp(std::uint8_t modBits, std::uint8_t regField, bool hasIndex, std::uint8_t index,
                 std::uint8_t rm, std::int64_t disp);
  void aluRegOp(std::uint8_t opcode, X64Reg dst, X64Reg src);
  void unaryAlu(std::uint8_t ext, X64Reg dst);
  void shiftCl(std::uint8_t ext, X64Reg dst);
  void sseRegOp(std::uint8_t prefix, std::uint8_t opcode, int dstXmm, int srcXmm);
  void sseMemOp(std::uint8_t prefix, std::uint8_t opcode, int xmm, X64Reg base,
                std::int64_t disp);
  void resolveFixups();
};
