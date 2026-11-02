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

void put8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void put32(std::vector<std::uint8_t>& out, std::int32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

void put64(std::vector<std::uint8_t>& out, std::int64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
}

}  // namespace

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

void CodeGen::resolveFixups() {
  std::vector<PendingFixup> remaining;
  for (const PendingFixup& fixup : fixups_) {
    if (fixup.backward || !labelBound_[static_cast<std::size_t>(fixup.labelId)]) {
      // Backward fixups were patched at emission time; keep them around so
      // `finalize()` can double-check that every reference was resolved.
      // Forward references to unbound labels stay pending for the next bind.
      remaining.push_back(fixup);
      continue;
    }
    patchRel32(fixup);
  }
  fixups_ = std::move(remaining);
}

// rel32 displacements are measured from the end of the instruction that
// carries them. The displacement slot always ends at the instruction
// boundary, whether the reference was resolved forward or backward:
// next_rip = patchAt + 4 in both cases.
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
// this right after writing the four placeholder bytes, so patchAt points at
// the start of the displacement slot inside the instruction. If the label
// is already bound (a backward reference), the displacement is patched
// immediately instead of staying pending.
void CodeGen::recordFixup(Label label, bool /*backward*/) {
  PendingFixup fixup;
  fixup.patchAt = code_.size() - 4;
  fixup.labelId = label.id;
  if (labelBound_[static_cast<std::size_t>(label.id)]) {
    fixup.backward = true;
    patchRel32(fixup);
  }
  fixups_.push_back(fixup);
}

void CodeGen::finalize() {
  resolveFixups();
  for (const PendingFixup& fixup : fixups_) {
    if (!fixup.backward) {
      throw std::logic_error("codegen: finalize with unbound label references");
    }
  }
}

void CodeGen::rex(bool w, std::uint8_t regField, bool hasIndex, std::uint8_t index,
                  std::uint8_t rm) {
  std::uint8_t byte = 0x40;
  if (w) byte |= 0x08;
  if (regField >= 8) byte |= 0x04;
  if (hasIndex && index >= 8) byte |= 0x02;
  if (rm >= 8) byte |= 0x01;
  if (byte != 0x40) put8(code_, byte);
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
    mod = 0x10;  // disp32
  }

  if (needsSib) {
    // ModRM selects the SIB form (rm=100); the SIB byte carries scale=00,
    // index=100 ("no index") and the base register.
    put8(code_, static_cast<std::uint8_t>((mod << 6) | ((regField & 7) << 3) | 0x04));
    put8(code_, static_cast<std::uint8_t>((0x00 << 6) | (0x04 << 3) | lowRm));
  } else {
    put8(code_, static_cast<std::uint8_t>((mod << 6) | ((regField & 7) << 3) | lowRm));
  }
  if (mod == 0x01) {
    put8(code_, static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
  } else if (mod == 0x10) {
    put32(code_, static_cast<std::int32_t>(disp));
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
    put8(code_, 0xC7);
    put8(code_, static_cast<std::uint8_t>(0xC0 | (d & 0x07)));
    put32(code_, static_cast<std::int32_t>(value));
    return;
  }
  // movabs: opcode B8+r encodes the full register number in the low nibble,
  // so extended registers need REX.B but never a ModRM byte.
  std::uint8_t byte = 0x40 | 0x08;  // REX.W
  if (d >= 8) byte |= 0x01;         // REX.B
  put8(code_, byte);
  put8(code_, static_cast<std::uint8_t>(0xB8 + (d & 0x07)));
  put64(code_, value);
}

void CodeGen::movReg(X64Reg dst, X64Reg src) { aluRegOp(0x89, dst, src); }

void CodeGen::movToMem(X64Reg base, std::int64_t disp, X64Reg src) {
  storeToMem(base, disp, enc(src), false);
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
  if (byte != 0x40) put8(code_, byte);
  if (isSse) {
    put8(code_, 0xF2);
    put8(code_, 0x0F);
    put8(code_, 0x11);
  } else {
    put8(code_, 0x89);
  }
  // Real-register stores never need the SIB form: rsp/r12 as a base is a
  // backend invariant (the frame pointer plays that role), so forcing the
  // direct ModRM path keeps `[r12+disp]` from silently decoding as RIP.
  const std::uint8_t lowRm = static_cast<std::uint8_t>(b & 7);
  const std::uint8_t mod = (disp == 0 && lowRm != 5)   ? 0x00
                            : (disp >= -128 && disp <= 127) ? 0x01
                                                            : 0x10;
  put8(code_, static_cast<std::uint8_t>((mod << 6) | ((src & 7) << 3) | lowRm));
  if (mod == 0x01) {
    put8(code_, static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
  } else if (mod == 0x10) {
    put32(code_, static_cast<std::int32_t>(disp));
  }
}

void CodeGen::movFromMem(X64Reg dst, X64Reg base, std::int64_t disp) {
  std::uint8_t b = enc(base);
  std::uint8_t d = enc(dst);
  rex(true, d, false, 0, b);
  put8(code_, 0x8B);
  modRmDisp(d, b, disp);
}

void CodeGen::movImmToMem(X64Reg base, std::int64_t disp, std::int64_t value) {
  std::uint8_t b = enc(base);
  rex(true, 0, false, 0, b);
  put8(code_, 0xC7);
  modRmDisp(0, b, disp);
  put32(code_, static_cast<std::int32_t>(value));
}

void CodeGen::leaRip(X64Reg dst, std::int32_t ripDelta) {
  std::uint8_t d = enc(dst);
  rex(true, d, false, 0, 5);  // rm = 101 without SIB => RIP-relative
  put8(code_, 0x8D);
  put8(code_, static_cast<std::uint8_t>((0x00 << 6) | (d << 3) | 0x05));
  put32(code_, ripDelta);
}

void CodeGen::aluRegOp(std::uint8_t opcode, X64Reg dst, X64Reg src) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = enc(src);
  rex(true, s, false, 0, d);
  put8(code_, opcode);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (s << 3) | d));
}

void CodeGen::addReg(X64Reg dst, X64Reg src) { aluRegOp(0x01, dst, src); }
void CodeGen::subReg(X64Reg dst, X64Reg src) { aluRegOp(0x29, dst, src); }
void CodeGen::andReg(X64Reg dst, X64Reg src) { aluRegOp(0x21, dst, src); }
void CodeGen::orReg(X64Reg dst, X64Reg src) { aluRegOp(0x09, dst, src); }
void CodeGen::xorReg(X64Reg dst, X64Reg src) { aluRegOp(0x31, dst, src); }
void CodeGen::cmpReg(X64Reg lhs, X64Reg rhs) { aluRegOp(0x39, lhs, rhs); }

void CodeGen::imulReg(X64Reg dst, X64Reg src) {
  std::uint8_t d = enc(dst);
  std::uint8_t s = enc(src);
  rex(true, d, false, 0, s);
  put8(code_, 0x0F);
  put8(code_, 0xAF);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (d << 3) | s));
}

void CodeGen::idiv(X64Reg divisor) {
  std::uint8_t e = enc(divisor);
  rex(true, 7, false, 0, e);
  put8(code_, 0xF7);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (7 << 3) | (e & 0x07)));
}

void CodeGen::cdq() { put8(code_, 0x99); }

void CodeGen::unaryAlu(std::uint8_t ext, X64Reg dst) {
  std::uint8_t d = enc(dst);
  rex(true, ext, false, 0, d);
  put8(code_, 0xF7);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (ext << 3) | (d & 0x07)));
}

void CodeGen::negReg(X64Reg dst) { unaryAlu(3, dst); }
void CodeGen::notReg(X64Reg dst) { unaryAlu(2, dst); }

void CodeGen::shiftCl(std::uint8_t ext, X64Reg dst) {
  std::uint8_t d = enc(dst);
  rex(true, ext, false, 0, d);
  put8(code_, 0xD3);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (ext << 3) | (d & 0x07)));
}

void CodeGen::shlCl(X64Reg dst) { shiftCl(4, dst); }
void CodeGen::sarCl(X64Reg dst) { shiftCl(7, dst); }

void CodeGen::testReg(X64Reg reg) {
  std::uint8_t r = enc(reg);
  rex(true, r, false, 0, r);
  put8(code_, 0x85);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (r << 3) | r));
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
  if (byte != 0x40) put8(code_, byte);
  if (prefix != 0) put8(code_, prefix);
  put8(code_, 0x0F);
  put8(code_, opcode);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (d << 3) | s));
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
  if (byte != 0x40) put8(code_, byte);
  if (prefix != 0) put8(code_, prefix);
  put8(code_, 0x0F);
  put8(code_, opcode);
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
  if (byte != 0x48) put8(code_, byte);
  put8(code_, 0xF2);
  put8(code_, 0x0F);
  put8(code_, 0x2C);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (d << 3) | s));
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
  put8(code_, 0xF2);
  put8(code_, byte);
  put8(code_, 0x0F);
  put8(code_, 0x2A);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (d << 3) | (s & 7)));
}

void CodeGen::jmp(Label label) {
  put8(code_, 0xE9);
  put32(code_, 0);
  recordFixup(label, labelBound_[static_cast<std::size_t>(label.id)]);
}

void CodeGen::jcc(Jcc cc, Label label) {
  put8(code_, 0x0F);
  put8(code_, static_cast<std::uint8_t>(cc));
  put32(code_, 0);
  recordFixup(label, labelBound_[static_cast<std::size_t>(label.id)]);
}

void CodeGen::callRel(std::int32_t offsetBytes) {
  put8(code_, 0xE8);
  put32(code_, offsetBytes);
}

void CodeGen::push(X64Reg reg) {
  std::uint8_t r = enc(reg);
  if (highReg(r)) put8(code_, 0x41);
  put8(code_, static_cast<std::uint8_t>(0x50 + (r & 0x07)));
}

void CodeGen::pop(X64Reg reg) {
  std::uint8_t r = enc(reg);
  if (highReg(r)) put8(code_, 0x41);
  put8(code_, static_cast<std::uint8_t>(0x58 + (r & 0x07)));
}

void CodeGen::syscallInsn() {
  put8(code_, 0x0F);
  put8(code_, 0x05);
}

void CodeGen::retInsn() { put8(code_, 0xC3); }

void CodeGen::ud2() {
  put8(code_, 0x0F);
  put8(code_, 0x0B);
}

void CodeGen::emitBytes(const std::uint8_t* data, std::size_t length) {
  code_.insert(code_.end(), data, data + length);
}
