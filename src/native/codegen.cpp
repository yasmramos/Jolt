#include "native/codegen.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

// Two-pass encoding: every control-flow instruction reserves a 4-byte rel32
// displacement and records a fixup; `bind()` patches all pending references
// when the label is reached. Encoding helpers follow the Intel SDM Vol. 2
// tables for the AMD64 subset used by the Jolt backend.

namespace {

constexpr std::size_t kUnbound = static_cast<std::size_t>(-1);

bool highReg(std::uint8_t reg) { return reg >= 8; }

// Raw append helper. Every emitter member routes its bytes through the
// `emitByte`/`emit32`/`emit64` methods below; this free function is the
// low-level push used by that single path (plus the placeholder bytes of
// `recordFixup()`, which belong to an already-reserved slot).
void put8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

}  // namespace

CodeGen::CodeGen(Target target) : target_(target) {
  requireSupportedTarget(target_);
}

CodeGen::Label CodeGen::newLabel() {
  Label label;
  label.id = static_cast<int>(labelOffset_.size());
  labelOffset_.push_back(kUnbound);
  labelBound_.push_back(false);
  return label;
}

void CodeGen::bind(Label label) {
  if (label.id < 0 || static_cast<std::size_t>(label.id) >= labelOffset_.size()) {
    throw std::logic_error("codegen: binding an unknown label");
  }
  if (labelBound_[static_cast<std::size_t>(label.id)]) {
    throw std::logic_error("codegen: label bound twice");
  }
  // Bindings are append-only: the offset of a label can never change after
  // it is bound, so fixups already resolved for backward references stay
  // valid and only forward references need to be retried here.
  labelOffset_[static_cast<std::size_t>(label.id)] = code_.size();
  labelBound_[static_cast<std::size_t>(label.id)] = true;
  resolveFixups();
}

// Resolves every reference whose label is already bound. Patching is
// idempotent by construction: a fixup leaves `fixups_` the moment it is
// patched (it is simply not re-added to `remaining`), so no reference can
// ever be applied twice -- a double patch was the source of the earlier
// backward-jump off-by-one. Forward references stay pending until their
// label is bound; `finalize()` patches the last trailing jump because the
// displacement slot is reserved eagerly by `recordFixup()`.
void CodeGen::resolveFixups() {
  std::vector<PendingFixup> remaining;
  for (const PendingFixup& fixup : fixups_) {
    if (!labelBound_[static_cast<std::size_t>(fixup.labelId)]) {
      // Label not bound yet: keep it pending for the next bind (or for
      // `finalize()`, which throws if any reference is still unresolved).
      remaining.push_back(fixup);
      continue;
    }
    patchRel32(fixup);
  }
  fixups_ = std::move(remaining);
}

// rel32 displacements are measured from the end of the instruction that
// carries them. That end is exactly `patchAt + 4`: `recordFixup()` reserves
// the four-byte displacement slot immediately after the opcode, so the
// reference point is pinned at record time and later emissions -- e.g. the
// epilogue after a loop's backward `jmp top` -- can never shift it. Deriving
// the origin from the buffer size at patch time instead would mis-target
// every jump whose instruction is not the last one emitted.
void CodeGen::patchRel32(const PendingFixup& fixup) {
  std::size_t target = labelOffset_[static_cast<std::size_t>(fixup.labelId)];
  std::size_t nextRip = fixup.patchAt + 4;
  std::int32_t distance = static_cast<std::int32_t>(static_cast<std::int64_t>(target) -
                                                    static_cast<std::int64_t>(nextRip));
  code_[fixup.patchAt + 0] = static_cast<std::uint8_t>(distance & 0xFF);
  code_[fixup.patchAt + 1] = static_cast<std::uint8_t>((distance >> 8) & 0xFF);
  code_[fixup.patchAt + 2] = static_cast<std::uint8_t>((distance >> 16) & 0xFF);
  code_[fixup.patchAt + 3] = static_cast<std::uint8_t>((distance >> 24) & 0xFF);
}

// Records a reference to `label` for a rel32 displacement. jmp/jcc call
// this right after emitting the opcode byte(s); the four placeholder bytes
// are appended here so the displacement slot exists in the buffer before
// any subsequent instruction can grow it. Backward references (the label is
// already bound) are patched immediately; forward ones stay pending until
// `bind()` or `finalize()` resolves them.
void CodeGen::recordFixup(Label label) {
  PendingFixup fixup;
  fixup.patchAt = code_.size();
  fixup.labelId = label.id;
  for (int i = 0; i < 4; ++i) put8(code_, 0);
  fixups_.push_back(fixup);
}

void CodeGen::finalize() {
  // All displacement slots were reserved eagerly, so every fixup whose
  // label got bound is patchable here regardless of where its instruction
  // sits in the buffer (including a trailing `jmp`). References to labels
  // that were never bound remain and throw below.
  resolveFixups();
  if (!fixups_.empty()) {
    throw std::logic_error("codegen: finalize with unbound label references");
  }
}

// Appends one raw byte to the code buffer. Every emitter helper funnels its
// bytes through these methods so all offsets stay consistent with the
// eagerly reserved rel32 slots of `recordFixup()`.
void CodeGen::emitByte(std::uint8_t value) { put8(code_, value); }

// Multi-byte immediates and displacements: little-endian fields written
// straight into the buffer.
void CodeGen::emit32(std::int32_t value) {
  for (int i = 0; i < 4; ++i) put8(code_, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

void CodeGen::emit64(std::int64_t value) {
  for (int i = 0; i < 8; ++i) put8(code_, static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

void CodeGen::rex(bool w, std::uint8_t regField, bool hasIndex, std::uint8_t index,
                  std::uint8_t rm) {
  std::uint8_t byte = 0x40;
  if (w) byte |= 0x08;
  if (regField >= 8) byte |= 0x04;
  if (hasIndex && index >= 8) byte |= 0x02;
  if (rm >= 8) byte |= 0x01;
  if (byte != 0x40) emitByte(byte);
}

// Encodes ModRM (+ SIB and displacement) for memory operands with a base
// register and a signed displacement. The encoding is size-optimized:
// disp8 when it fits (mod=01), disp32 otherwise (mod=10), no displacement
// for [reg] (mod=00, except rbp/r13 which always need disp8, and rsp/r12 as
// base which require a SIB byte).
//
// `rm` is the full register number; only its low three bits go into the
// ModRM/SIB bytes (the caller contributes REX.B from the full value). A
// base whose low bits are 101 is rbp/r13 -- never RIP, because this helper
// is only called with real GPR bases (`leaRip()` emits the RIP form
// explicitly) -- so it must not trigger the SIB path: rm=101 with mod!=00
// already encodes [rbp+disp]/[r13+disp] directly.
void CodeGen::modRmDisp(std::uint8_t regField, std::uint8_t rm, std::int64_t disp) {
  const std::uint8_t lowRm = static_cast<std::uint8_t>(rm & 7);
  const bool baseNeedsDisp = lowRm == 5;   // rbp/r13: mod=00 would mean disp32
  const bool needsSib = lowRm == 4;        // rsp/r12: SIB byte required as base

  std::uint8_t mod;
  if (disp == 0 && !baseNeedsDisp) {
    mod = 0x00;  // [reg] without displacement
  } else if (disp >= -128 && disp <= 127) {
    mod = 0x01;  // disp8
  } else {
    mod = 0x02;  // disp32: the mod field is bits 7-6, so the value is 2
                 // (0b10), not 0x10 -- `mod << 6` of 0x10 overflows a byte
                 // and emits mod=00, decoding as RIP-relative addressing.
  }

  if (needsSib) {
    // ModRM selects the SIB form (rm=100); the SIB byte carries scale=00,
    // index=100 ("no index") and the base register.
    emitByte(static_cast<std::uint8_t>((mod << 6) | ((regField & 7) << 3) | 0x04));
    emitByte(static_cast<std::uint8_t>((0x00 << 6) | (0x04 << 3) | lowRm));
  } else {
    emitByte(static_cast<std::uint8_t>((mod << 6) | ((regField & 7) << 3) | lowRm));
  }
  if (mod == 0x01) {
    emitByte(static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
  } else if (mod == 0x02) {
    // Match the two-bit encoding chosen above (was compared against 0x10,
    // which no longer equals any emitted mod, silently dropping the disp32
    // payload and leaving the instruction short by four bytes).
    emit32(static_cast<std::int32_t>(disp));
  }
}

void CodeGen::movImm(X64Reg dst, std::int64_t value) {
  std::uint8_t d = enc(dst);
  if (value >= -2147483648LL && value <= 2147483647LL) {
    // mov r/m64, imm32: opcode C7 /0. The destination goes in the ModRM rm
    // field and the reg field is the /0 extension, so the byte is 0xC0|rm.
    // REX.B must select r8-r15 for the rm field as well; `rex` only emits
    // that bit when the full register number is passed as `rm`, so the
    // ModRM byte keeps just the low three bits.
    rex(true, 0, false, 0, d);
    emitByte(0xC7);
    emitByte(static_cast<std::uint8_t>(0xC0 | (d & 0x07)));
    emit32(static_cast<std::int32_t>(value));
    return;
  }
  // movabs r64, imm64: opcode B8+r encodes the register number in the low
  // nibble of the OPCODE byte itself, so there is no ModRM/SIB stage where
  // rsp's rm=100 could introduce a SIB byte. Extended registers (r8-r15)
  // need REX.B but still never a ModRM byte. The REX prefix is ONE byte
  // (0x48 = W|0001000); it used to be emitted as two bytes (0x40 and 0x08
  // separately), which decoded as `test al, imm8` followed by garbage --
  // silently corrupting every 64-bit immediate above the imm32 range
  // (pointer loads included).
  emitByte(static_cast<std::uint8_t>(0x48 | (d >= 8 ? 0x01 : 0x00)));
  emitByte(static_cast<std::uint8_t>(0xB8 + (d & 0x07)));
  emit64(value);
}

void CodeGen::movReg(X64Reg dst, X64Reg src) { aluRegOp(0x89, dst, src); }

void CodeGen::movToMem(X64Reg base, std::int64_t disp, X64Reg src) {
  // `enc()` returns the full register number (0-15); it must be widened to
  // int64_t before being passed as the `src` byte, otherwise a value above
  // 127 is sign-truncated to a negative int and the low-byte conversion at
  // the call site corrupts REX.R detection and the ModRM reg field.
  storeToMem(base, disp, static_cast<std::uint8_t>(enc(src)), false);
}

// Shared body of the two store forms. The GPR form is `mov [base+disp], r`
// (opcode 0x89, reg field = source). The SSE form is `movsd [base+disp],
// xmm` (F2 0F 11): legacy SSE never uses REX.W, and an extended source is
// xmm8-15 (REX.R) instead of r8-r15 (which would set REX.B on the reg
// field). The base register contributes REX.B in either case.
void CodeGen::storeToMem(X64Reg base, std::int64_t disp, std::uint8_t src, bool isSse) {
  std::uint8_t b = enc(base);
  std::uint8_t byte = 0x40 | (isSse ? 0x00 : 0x08);
  if (isSse ? (src >= kXmmBase + 8) : (src >= 8)) byte |= 0x04;
  if (b >= 8) byte |= 0x01;
  if (isSse) {
    emitByte(0xF2);                            // F2 PRIMERO (canónico GNU as)
    if (byte != 0x40) emitByte(byte);          // REX después
    emitByte(0x0F);
    emitByte(0x11);
  } else {
    if (byte != 0x40) emitByte(byte);
    emitByte(0x89);
  }
  // The displacement encoding lives in `modRmDisp()` only: hand-rolling a
  // second ModRM writer here previously emitted mod=00/rm=101 for rbp/r13
  // bases with disp32, which decodes as RIP-relative addressing instead of
  // [rbp+disp32]. Routing through the shared helper keeps every memory
  // operand on one single, correct encoder.
  modRmDisp(static_cast<std::uint8_t>(src - (isSse ? kXmmBase : 0)), b, disp);
}

void CodeGen::movFromMem(X64Reg dst, X64Reg base, std::int64_t disp) {
  std::uint8_t b = enc(base);
  std::uint8_t d = enc(dst);
  rex(true, d, false, 0, b);
  emitByte(0x8B);
  modRmDisp(d, b, disp);
}

void CodeGen::movImmToMem(X64Reg base, std::int64_t disp, std::int64_t value) {
  std::uint8_t b = enc(base);
  rex(true, 0, false, 0, b);
  emitByte(0xC7);
  modRmDisp(0, b, disp);
  emit32(static_cast<std::int32_t>(value));
}

void CodeGen::leaRip(X64Reg dst, std::int32_t ripDelta) {
  std::uint8_t d = enc(dst);
  rex(true, d, false, 0, 5);  // rm = 101 without SIB => RIP-relative
  emitByte(0x8D);
  emitByte(static_cast<std::uint8_t>((0x00 << 6) | ((d & 0x07) << 3) | 0x05));
  emit32(ripDelta);
}

void CodeGen::aluRegOp(std::uint8_t opcode, X64Reg dst, X64Reg src) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = enc(src);
  rex(true, s, false, 0, d);
  emitByte(opcode);
  // ModRM carries only the low three bits of each register; the high bit is
  // REX.R (reg field) or REX.B (rm field). Without the masks, r8-r15 leak
  // their bit 3 into the neighbouring field and corrupt the instruction.
  emitByte(static_cast<std::uint8_t>(0xC0 | ((s & 0x07) << 3) | (d & 0x07)));
}

void CodeGen::addReg(X64Reg dst, X64Reg src) { aluRegOp(0x01, dst, src); }
void CodeGen::subReg(X64Reg dst, X64Reg src) { aluRegOp(0x29, dst, src); }

// add r/m64, imm32: opcode 0x81 with /0 in the reg field of ModRM. The
// immediate is sign-extended to 64 bits by the CPU.
void CodeGen::addImm32(X64Reg dst, std::int32_t value) {
  std::uint8_t d = enc(dst);
  rex(true, 0, false, 0, d);
  emitByte(0x81);
  emitByte(static_cast<std::uint8_t>(0xC0 | (0 << 3) | (d & 0x07)));
  emit32(value);
}

// sub r/m64, imm32: opcode 0x81 with /5 in the reg field of ModRM. The
// immediate is sign-extended to 64 bits by the CPU.
void CodeGen::subRegImm32(X64Reg dst, std::int32_t value) {
  std::uint8_t d = enc(dst);
  rex(true, 5, false, 0, d);
  emitByte(0x81);
  emitByte(static_cast<std::uint8_t>(0xC0 | (5 << 3) | (d & 0x07)));
  emit32(value);
}
void CodeGen::andReg(X64Reg dst, X64Reg src) { aluRegOp(0x21, dst, src); }
void CodeGen::orReg(X64Reg dst, X64Reg src) { aluRegOp(0x09, dst, src); }
void CodeGen::xorReg(X64Reg dst, X64Reg src) { aluRegOp(0x31, dst, src); }
void CodeGen::cmpReg(X64Reg lhs, X64Reg rhs) { aluRegOp(0x39, lhs, rhs); }

void CodeGen::imulReg(X64Reg dst, X64Reg src) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = enc(src);
  rex(true, d, false, 0, s);
  emitByte(0x0F);
  emitByte(0xAF);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((d & 0x07) << 3) | (s & 0x07)));
}

void CodeGen::idiv(X64Reg divisor) {
  std::uint8_t e = enc(divisor);
  rex(true, 7, false, 0, e);
  emitByte(0xF7);
  emitByte(static_cast<std::uint8_t>(0xC0 | (7 << 3) | (e & 0x07)));
}

void CodeGen::cdq() { emitByte(0x99); }

void CodeGen::unaryAlu(std::uint8_t ext, X64Reg dst) {
  std::uint8_t d = enc(dst);
  rex(true, ext, false, 0, d);
  emitByte(0xF7);
  emitByte(static_cast<std::uint8_t>(0xC0 | (ext << 3) | (d & 0x07)));
}

void CodeGen::negReg(X64Reg dst) { unaryAlu(3, dst); }
void CodeGen::notReg(X64Reg dst) { unaryAlu(2, dst); }

void CodeGen::shiftCl(std::uint8_t ext, X64Reg dst) {
  std::uint8_t d = enc(dst);
  rex(true, ext, false, 0, d);
  emitByte(0xD3);
  emitByte(static_cast<std::uint8_t>(0xC0 | (ext << 3) | (d & 0x07)));
}

void CodeGen::shlCl(X64Reg dst) { shiftCl(4, dst); }
void CodeGen::sarCl(X64Reg dst) { shiftCl(7, dst); }

void CodeGen::testReg(X64Reg reg) {
  std::uint8_t r = enc(reg);
  rex(true, r, false, 0, r);
  emitByte(0x85);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((r & 0x07) << 3) | (r & 0x07)));
}

// setcc r/m8: 0F 90+cc /0. The condition code is the low nibble of the Jcc
// value (0x84 -> 4, `sete`). Only the low byte of `dst` is written, so the
// caller zero-extends with `movzx` to get a clean 0/1 in the full register.
//
// The low-byte registers spl/bpl/sil/dil (numbers 4-7) are only reachable
// through a REX prefix: without one, ModRM rm=100..111 still means
// ah/ch/dh/bh. Registers 8-15 additionally need REX.B.
void CodeGen::setcc(Jcc cc, X64Reg dst) {
  std::uint8_t d = enc(dst);
  if (d >= 4) {
    std::uint8_t byte = 0x40;
    if (d >= 8) byte |= 0x01;
    emitByte(byte);
  }
  emitByte(0x0F);
  emitByte(static_cast<std::uint8_t>(0x90 | (static_cast<std::uint8_t>(cc) & 0x0F)));
  emitByte(static_cast<std::uint8_t>(0xC0 | (d & 0x07)));
}

// movzx r64, r/m8: REX.W 0F B6 /r. Zero-extends the low byte of `src` into
// the full 64-bit `dst`, which turns the byte written by `setcc` into a
// canonical 0/1 integer.
void CodeGen::movzx(X64Reg dst, X64Reg src) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = enc(src);
  rex(true, d, false, 0, s);
  emitByte(0x0F);
  emitByte(0xB6);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((d & 0x07) << 3) | (s & 0x07)));
}

void CodeGen::sseRegOp(std::uint8_t prefix, std::uint8_t opcode, int dstXmm, int srcXmm) {
  // dstXmm/srcXmm are register indices in [kXmmBase, kXmmBase+16). SSE reg
  // ops use the reg<-rm direction: the destination is the ModRM reg field
  // (REX.R) and the source is the rm field (REX.B).
  std::uint8_t d = static_cast<std::uint8_t>(dstXmm & 0x0F);
  std::uint8_t s = static_cast<std::uint8_t>(srcXmm & 0x0F);
  // Legacy SSE never needs REX.W; xmm8-15 are selected through REX.R (the
  // ModRM reg field) and REX.B (the ModRM rm field). No REX byte at all for
  // xmm0-7 keeps the encoding canonical.
  std::uint8_t byte = 0x40;
  if (d >= 8) byte |= 0x04;
  if (s >= 8) byte |= 0x01;
  if (prefix != 0) emitByte(prefix);
  if (byte != 0x40) emitByte(byte);
  emitByte(0x0F);
  emitByte(opcode);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((d & 0x07) << 3) | (s & 0x07)));
}

void CodeGen::addsd(int dstXmm, int srcXmm) { sseRegOp(0xF2, 0x58, dstXmm, srcXmm); }
void CodeGen::subsd(int dstXmm, int srcXmm) { sseRegOp(0xF2, 0x5C, dstXmm, srcXmm); }
void CodeGen::mulsd(int dstXmm, int srcXmm) { sseRegOp(0xF2, 0x59, dstXmm, srcXmm); }
void CodeGen::divsd(int dstXmm, int srcXmm) { sseRegOp(0xF2, 0x5E, dstXmm, srcXmm); }
void CodeGen::ucomisd(int a, int b) { sseRegOp(0x66, 0x2E, a, b); }
void CodeGen::xorps(int dstXmm, int srcXmm) { sseRegOp(0x00, 0x57, dstXmm, srcXmm); }

void CodeGen::sseMemOp(std::uint8_t prefix, std::uint8_t opcode, int xmm, X64Reg base,
                       std::int64_t disp) {
  std::uint8_t d = static_cast<std::uint8_t>(xmm - kXmmBase);
  std::uint8_t b = enc(base);
  // REX.R selects xmm8-15 in the reg field; REX.B is only for GPR r12-r15.
  std::uint8_t byte = 0x40;
  if (d >= 8) byte |= 0x04;
  if (b >= 8) byte |= 0x01;
  if (prefix != 0) emitByte(prefix);
  if (byte != 0x40) emitByte(byte);
  emitByte(0x0F);
  emitByte(opcode);
  modRmDisp(d, b, disp);
}

void CodeGen::movsdToXmm(int xmm, X64Reg base, std::int64_t disp) {
  sseMemOp(0xF2, 0x10, xmm, base, disp);
}

void CodeGen::movsdFromXmm(X64Reg base, std::int64_t disp, int xmm) {
  // The store form (F2 0F 11) has the source in the reg field, which is
  // exactly what `storeToMem` emits; routing through it keeps one single
  // implementation of the SSE store encoding.
  storeToMem(base, disp, static_cast<std::uint8_t>(xmm), true);
}

void CodeGen::cvttsd2si(X64Reg dst, int srcXmm) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = static_cast<std::uint8_t>(srcXmm - kXmmBase);
  std::uint8_t byte = 0x40 | 0x08;  // W=1
  if (d >= 8) byte |= 0x01;
  if (s >= 8) byte |= 0x04;
  if (byte != 0x48) emitByte(byte);
  emitByte(0xF2);
  emitByte(0x0F);
  emitByte(0x2C);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((d & 0x07) << 3) | (s & 0x07)));
}

void CodeGen::cvtsi2sd(int dstXmm, X64Reg src) {
  std::uint8_t d = static_cast<std::uint8_t>(dstXmm - kXmmBase);
  std::uint8_t s = enc(src);
  std::uint8_t byte = 0x40 | 0x08;  // W=1
  if (d >= 8) byte |= 0x04;
  if (s >= 8) byte |= 0x01;
  // GNU as emits the F2 legacy prefix before REX for this instruction
  // (`f2 48 0f 2a c0`); both orders decode identically on x86-64 CPUs, but
  // matching the assembler keeps our byte-exact tests aligned with objdump.
  emitByte(0xF2);
  emitByte(byte);
  emitByte(0x0F);
  emitByte(0x2A);
  emitByte(static_cast<std::uint8_t>(0xC0 | ((d & 0x07) << 3) | (s & 0x07)));
}

void CodeGen::jmp(Label label) {
  emitByte(0xE9);
  // `recordFixup` appends the four zero placeholder bytes itself, so no
  // extra displacement emission happens here (emitting them again would
  // duplicate the slot and drift every following offset by four bytes).
  recordFixup(label);
}

void CodeGen::jcc(Jcc cc, Label label) {
  emitByte(0x0F);
  emitByte(static_cast<std::uint8_t>(cc));
  // The displacement placeholders belong to `recordFixup` (see `jmp`).
  recordFixup(label);
}

void CodeGen::callRel(std::int32_t offsetBytes) {
  emitByte(0xE8);
  emit32(offsetBytes);
}

void CodeGen::callLabel(Label label) {
  emitByte(0xE8);
  recordFixup(label);
}

void CodeGen::push(X64Reg reg) {
  std::uint8_t r = enc(reg);
  if (highReg(r)) emitByte(0x41);
  emitByte(static_cast<std::uint8_t>(0x50 + (r & 0x07)));
}

void CodeGen::pop(X64Reg reg) {
  std::uint8_t r = enc(reg);
  if (highReg(r)) emitByte(0x41);
  emitByte(static_cast<std::uint8_t>(0x58 + (r & 0x07)));
}

void CodeGen::syscallInsn() {
  emitByte(0x0F);
  emitByte(0x05);
}

void CodeGen::retInsn() { emitByte(0xC3); }

void CodeGen::ud2() {
  emitByte(0x0F);
  emitByte(0x0B);
}

void CodeGen::emitBytes(const std::uint8_t* data, std::size_t length) {
  // Byte-at-a-time on purpose: raw payloads must advance the emission
  // cursor through `emitByte` so pending rel32 fixups keep exact
  // instruction-end offsets instead of silently skipping notification.
  for (std::size_t i = 0; i < length; ++i) emitByte(data[i]);
}
