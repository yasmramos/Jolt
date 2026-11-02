#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "native/target.h"

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

// General-purpose registers occupy the enum values Rax (0) through R15 (15),
// so `kGprCount` is one past the last GPR. SSE registers are encoded as
// `kGprCount + N` (xmm0..xmm15) when passed to the SSE emitters; they never
// appear in the X64Reg enum itself.
constexpr int kGprCount = 16;

// Named alias for the base of the synthetic xmm numbering: callers write
// `kXmmBase + N` for xmmN. Declared in the header so tests and the backend
// share one definition instead of relying on `kGprCount` directly.
constexpr int kXmmBase = kGprCount;

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

  // CodeGen is the x86-64 backend. It takes a Target so callers never hardcode
  // the platform; another architecture gets its own backend behind the same
  // descriptor. Construction throws for a target that has no backend.
  explicit CodeGen(Target target = Target::linuxX64());

  const Target& target() const { return target_; }

  Label newLabel();
  void bind(Label label);


  // Data movement.
  void movImm(X64Reg dst, std::int64_t value);  // mov r/m64, imm32 (sign-extended) or movabs
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

  // Offset of an already-bound label (throws if the label is unknown or
  // still unbound). Exposed for tests that need to verify jump
  // displacements against the emitter's own bookkeeping.
  std::size_t labelOffset(Label label) const {
    if (label.id < 0 || static_cast<std::size_t>(label.id) >= labelOffset_.size() ||
        !labelBound_[static_cast<std::size_t>(label.id)]) {
      throw std::logic_error("codegen: labelOffset of an unbound label");
    }
    return labelOffset_[static_cast<std::size_t>(label.id)];
  }

  // Patches every pending rel32 fixup whose label is already bound. Call
  // once after emitting the whole function; any reference to an unbound
  // label throws (the backend must bind all labels before finishing).
  void finalize();

  const std::vector<std::uint8_t>& code() const { return code_; }

 private:
  struct PendingFixup {
    std::size_t patchAt = 0;  // offset of the 4-byte displacement slot
    int labelId = -1;
  };

  // Records a reference to `label` for a rel32 displacement: reserves the
  // four-byte displacement slot at the current buffer end (the opcode byte
  // is already emitted by the caller). The reference is patched later, when
  // the label gets bound or at `finalize()`.
  void recordFixup(Label label);

  // Patches one rel32 displacement, measured from `patchAt + 4`, which is
  // the end of the instruction carrying the slot (next RIP). The slot is
  // reserved up front by `recordFixup()`, so no later emission can shift
  // this reference point -- that pinning is what keeps backward jumps with
  // trailing code (e.g. a loop epilogue after `jmp top`) off-by-one free.
  void patchRel32(const PendingFixup& fixup);

  // Appends bytes to the buffer. All emitter helpers funnel through these
  // so every byte takes part in exactly one instruction boundary.
  void emitByte(std::uint8_t value);
  void emit32(std::int32_t value);
  void emit64(std::int64_t value);

  std::vector<std::uint8_t> code_;
  std::vector<std::size_t> labelOffset_;  // label id -> offset, kUnbound until bound
  std::vector<bool> labelBound_;          // label id -> bound (offset 0 is valid)
  std::vector<PendingFixup> fixups_;
  Target target_;

  static std::uint8_t enc(X64Reg reg) { return static_cast<std::uint8_t>(reg); }

  void rex(bool w, std::uint8_t regField, bool hasIndex, std::uint8_t index, std::uint8_t rm);
  // ModRM (+ SIB + disp8/disp32) for [base+disp] memory operands. Never
  // emits the RIP form; `leaRip()` handles that addressing explicitly.
  void modRmDisp(std::uint8_t regField, std::uint8_t rm, std::int64_t disp);
  // Shared body of `movToMem`/`movsdFromXmm`: store `src` (a GPR or, when
  // `isSse` is set, an xmm register encoded as kXmmBase + N) to [base+disp].
  void storeToMem(X64Reg base, std::int64_t disp, std::uint8_t src, bool isSse);
  void aluRegOp(std::uint8_t opcode, X64Reg dst, X64Reg src);
  void unaryAlu(std::uint8_t ext, X64Reg dst);
  void shiftCl(std::uint8_t ext, X64Reg dst);
  void sseRegOp(std::uint8_t prefix, std::uint8_t opcode, int dstXmm, int srcXmm);
  void sseMemOp(std::uint8_t prefix, std::uint8_t opcode, int xmm, X64Reg base,
                std::int64_t disp);
  void resolveFixups();
};
