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
  // Unsigned forms, needed when a comparison is on pointers or on a
  // magnitude that must not be read as a signed value (the integer-to-text
  // routine compares buffer cursors).
  B = 0x82,   // below (jb/jc)
  Ae = 0x83,  // above or equal (jae/jnb)
  Be = 0x86,  // below or equal (jbe)
  A = 0x87,   // above (ja)
  S = 0x88,   // sign set
  Ns = 0x89,  // sign clear
  // Parity, used to reject unordered (NaN) results of `ucomisd`.
  P = 0x8A,   // parity even (PF=1, i.e. unordered)
  Np = 0x8B,  // parity odd (PF=0, i.e. ordered)
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
  // `mov r32, r32` (89 /r without REX.W). Zero-extends the source into the
  // 64-bit destination, so it is the cheapest way to mask a value to its low
  // 32 bits -- the dtoa bignum keeps 32-bit limbs in 64-bit slots and uses
  // this after every multiply to drop the high half.
  void movReg32(X64Reg dst, X64Reg src);
  void movToMem(X64Reg base, std::int64_t disp, X64Reg src);
  void movFromMem(X64Reg dst, X64Reg base, std::int64_t disp);
  // 32-bit forms (`89 /r` / `8B /r` without REX.W). The dtoa bignum keeps
  // 32-bit limbs: a 32-bit load zero-extends into the 64-bit register, so a
  // limb is a clean machine word for the 64-bit `imul`, and a 32-bit store
  // writes exactly the limb without a mask.
  void movToMem32(X64Reg base, std::int64_t disp, X64Reg src);
  void movFromMem32(X64Reg dst, X64Reg base, std::int64_t disp);
  void movImmToMem(X64Reg base, std::int64_t disp, std::int64_t value);
  void movImmByteToMem(X64Reg base, std::int64_t disp, std::uint8_t value);  // mov byte [m], imm8
  void movByteToMem(X64Reg base, std::int64_t disp, X64Reg src);             // mov [m], r8
  void leaRip(X64Reg dst, std::int32_t ripDelta);         // lea dst, [rip + delta]
  // `lea dst, [rip + label]`, the label resolved by `finalize()`. Used to
  // address constant data emitted inline (error messages): unlike `leaRip`
  // the distance is not known when the instruction is emitted.
  void leaRipLabel(X64Reg dst, Label label);

  // Arithmetic/logical on registers (64-bit unless noted).
  void addReg(X64Reg dst, X64Reg src);
  void addImm32(X64Reg dst, std::int32_t value);  // add r/m64, imm32
  void subReg(X64Reg dst, X64Reg src);
  void subRegImm32(X64Reg dst, std::int32_t value);  // sub r/m64, imm32
  void imulReg(X64Reg dst, X64Reg src);
  // `imul dst, src, imm32` (69 /r id): dst = src * imm, without needing src in
  // dst first. The dtoa power-of-ten scaling multiplies by 10 repeatedly.
  void imulImm32(X64Reg dst, X64Reg src, std::int32_t imm);
  void idiv(X64Reg divisor);  // signed divide rdx:rax by divisor -> rax, remainder rdx
  void cdq();                 // sign-extend rax into rdx:rax (CQO, 64-bit)
  void divReg(X64Reg divisor);  // cdq + idiv: rax / divisor -> rax, remainder rdx
  void divUnsigned(X64Reg divisor);  // rdx=0 + div: unsigned rax / divisor
  // `div r/m64` with the caller-supplied rdx:rax dividend (opcode F7 /6). The
  // bignum long division keeps a running remainder in rdx, so it cannot use
  // `divUnsigned`, which zeroes rdx.
  void divRaw(X64Reg divisor);
  void negReg(X64Reg dst);
  void notReg(X64Reg dst);
  void andReg(X64Reg dst, X64Reg src);
  // `and dst, imm32` (81 /4 id). The float/double unpack masks the exponent
  // and fraction fields out of the raw bit pattern.
  void andImm32(X64Reg dst, std::int32_t value);
  void orReg(X64Reg dst, X64Reg src);
  void xorReg(X64Reg dst, X64Reg src);
  void shlCl(X64Reg dst);   // dst <<= cl
  void shrCl(X64Reg dst);   // dst >>= cl (logical)
  void sarCl(X64Reg dst);   // dst >>= cl (arithmetic)
  // Shift by an immediate count (`C1 /4 ib` / `C1 /5 ib`). The dtoa bignum
  // splits a 32-bit limb with `>> 32` and shifts a limb by `rem` bits, both
  // with a count known at emit time, so no `cl` setup is needed.
  void shlImm8(X64Reg dst, std::uint8_t count);
  void shrImm8(X64Reg dst, std::uint8_t count);
  // `bsr dst, src`: index of the most significant set bit of `src` (0 for bit
  // 0). Undefined for src == 0, so the caller must test first. The dtoa
  // generator uses it to measure the mantissa's bit length for the decimal
  // exponent estimate.
  void bsrReg(X64Reg dst, X64Reg src);
  // `rep movsb`: copy rcx bytes from [rsi] to [rdi], advancing both. Assumes
  // DF=0 (the ABI guarantees it). The string concatenation uses it to join two
  // byte ranges without a per-byte loop.
  void repMovsb();

  // SSE scalar double helpers (used by the float fast paths).
  void movsdToXmm(int xmm, X64Reg base, std::int64_t disp);    // movsd xmm, [base+disp]
  void movsdFromXmm(X64Reg base, std::int64_t disp, int xmm);  // movsd [base+disp], xmm
  void movsdReg(int dstXmm, int srcXmm);                       // movsd xmm, xmm
  // Bit-exact moves between a GPR and an xmm register (`movq`). They are how
  // the runtime gets a float's payload bits into and out of an SSE register
  // without going through memory: the payload of a Float/Double value is the
  // IEEE-754 bit pattern, and `movq` moves it verbatim.
  void movqToXmm(int xmm, X64Reg src);      // movq xmm, r64
  void movqFromXmm(X64Reg dst, int xmm);    // movq r64, xmm
  void addsd(int dstXmm, int srcXmm);
  void subsd(int dstXmm, int srcXmm);
  void mulsd(int dstXmm, int srcXmm);
  void divsd(int dstXmm, int srcXmm);
  // Narrowing/widening conversions between the two binary floating-point
  // formats. `cvtsd2ss` rounds to binary32 using the current MXCSR mode
  // (round-to-nearest-even by default), which is exactly what a C++ `float`
  // cast does, so a `float` operation rounds the same way the hosted engine's
  // `Value::fromFloat` does.
  void cvtsd2ss(int dstXmm, int srcXmm);    // double -> float
  void cvtss2sd(int dstXmm, int srcXmm);    // float -> double
  void ucomisd(int a, int b);
  void cvttsd2si(X64Reg dst, int srcXmm);  // truncate toward zero
  void cvtsi2sd(int dstXmm, X64Reg src);
  void xorps(int dstXmm, int srcXmm);  // also zeroes the upper bits of xmm

  // x87 FPU helpers, used only by float `%`. SSE and x87 do not share
  // registers, so a value crosses through memory: the caller stores the
  // operand with `movsdFromXmm`, loads it with `fldM64`, and reads the result
  // back with `movsdToXmm`. Every path drains the x87 stack before returning,
  // so no x87 state survives a call and the rest of the backend stays SSE.
  void fldM64(X64Reg base, std::int64_t disp);   // DD /0: push st(0) = double [base+disp]
  void fstpM64(X64Reg base, std::int64_t disp);  // DD /3: store st(0) to [base+disp] and pop
  void fstpSt0();                                // DD D8: pop st(0)
  void fprem();                                  // D9 F8: st(0) = partial remainder st(0)/st(1)
  void fnstswAx();                               // DF E0: FPU status word -> ax
  // `test ah, imm8` (F6 /0): the x87 status word's condition flags live in
  // `ah` after `fnstsw ax`, and `fprem`'s C2 (incomplete) bit is bit 2 of `ah`.
  void testAhImm8(std::uint8_t value);

  // Comparisons and control flow.
  void cmpReg(X64Reg lhs, X64Reg rhs);
  void cmpImm32(X64Reg lhs, std::int32_t value);  // cmp r/m64, imm32
  void testReg(X64Reg reg);
  void testImm32(X64Reg reg, std::int32_t value);  // test r/m64, imm32 (F7 /0)
  void setcc(Jcc cc, X64Reg dst);          // setcc r/m8: dst low byte = 0/1
  void movzx(X64Reg dst, X64Reg src);      // movzx r64, r/m8 (zero-extend)
  // movzx r64, byte [base+disp]: loads one byte and zero-extends it. The
  // string helpers walk bytes with this; a plain `mov` would need a separate
  // mask to clear the upper bits.
  void movzxFromMem(X64Reg dst, X64Reg base, std::int64_t disp);
  void jmp(Label label);
  void jcc(Jcc cc, Label label);
  void callRel(std::int32_t offsetBytes);  // call rel32 (caller resolves the target)
  // call rel32 to a label in the same buffer. `callRel` cannot express this
  // because the target's offset is not known when the call is emitted; the
  // displacement is patched by `finalize()`, exactly like a jump.
  void callLabel(Label label);

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
  void shiftImm8(std::uint8_t ext, X64Reg dst, std::uint8_t count);
  void sseRegOp(std::uint8_t prefix, std::uint8_t opcode, int dstXmm, int srcXmm);
  void sseMemOp(std::uint8_t prefix, std::uint8_t opcode, int xmm, X64Reg base,
                std::int64_t disp);
  void resolveFixups();
};
