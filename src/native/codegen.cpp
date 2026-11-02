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
  return label;
}

void CodeGen::bind(Label label) {
  if (label.id < 0 || static_cast<std::size_t>(label.id) >= labelOffset_.size()) {
    throw std::logic_error("codegen: binding an unknown label");
  }
  if (labelOffset_[static_cast<std::size_t>(label.id)] != kUnbound) {
    throw std::logic_error("codegen: label bound twice");
  }
  labelOffset_[static_cast<std::size_t>(label.id)] = code_.size();
  resolveFixups();
}

void CodeGen::resolveFixups() {
  std::vector<PendingFixup> remaining;
  for (const PendingFixup& fixup : fixups_) {
    std::size_t target = labelOffset_[static_cast<std::size_t>(fixup.labelId)];
    if (target == kUnbound) {
      remaining.push_back(fixup);
      continue;
    }
    // The displacement is relative to the end of the instruction, which is
    // four bytes after the start of the patch slot.
    std::int32_t distance = static_cast<std::int32_t>(target - (fixup.patchAt + 4));
    code_[fixup.patchAt + 0] = static_cast<std::uint8_t>(distance & 0xFF);
    code_[fixup.patchAt + 1] = static_cast<std::uint8_t>((distance >> 8) & 0xFF);
    code_[fixup.patchAt + 2] = static_cast<std::uint8_t>((distance >> 16) & 0xFF);
    code_[fixup.patchAt + 3] = static_cast<std::uint8_t>((distance >> 24) & 0xFF);
  }
  fixups_ = std::move(remaining);
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
// for [reg] (mod=00, except rbp/r12 which always need disp8, and rsp/r12 as
// base which require a SIB byte).
void CodeGen::modRmDisp(std::uint8_t modBits, std::uint8_t regField, bool hasIndex,
                        std::uint8_t index, std::uint8_t rm, std::int64_t disp) {
  (void)modBits;  // caller-provided hint is recomputed below from the value
  const bool baseNeedsDisp = (rm & 7) == 5;   // rbp/r12: mod=00 means disp32-RIP
  const bool baseIsRsp = (rm & 7) == 4;        // rsp/r12: requires SIB
  bool needsSib = baseIsRsp || hasIndex;
  std::int8_t lowRm = static_cast<std::int8_t>(rm & 7);

  std::uint8_t mod = 0x00;
  if (disp == 0 && !baseNeedsDisp && !needsSib) {
    mod = 0x00;  // [reg]
  } else if (disp >= -128 && disp <= 127) {
    mod = 0x01;  // disp8
  } else {
    mod = 0x10;  // disp32
  }

  if (ripForm(hasIndex, rm)) {
    // [rip + disp]: mod=00 rm=101 without SIB; the displacement is always
    // 32 bits, so the mod hint computed above is overridden here.
    put8(code_, static_cast<std::uint8_t>((0x00 << 6) | (regField << 3) | 0x05));
    put32(code_, static_cast<std::int32_t>(disp));
    return;
  }
  if (needsSib) {
    // ModRM selects the SIB form (rm=100); the SIB byte carries scale=00,
    // the index (rsp/100 means "no index") and the base register.
    std::uint8_t idx = hasIndex ? (index & 7) : 0x04;
    put8(code_, static_cast<std::uint8_t>((mod << 6) | (regField << 3) | 0x04));
    put8(code_, static_cast<std::uint8_t>((0x00 << 6) | (idx << 3) | lowRm));
  } else {
    put8(code_, static_cast<std::uint8_t>((mod << 6) | (regField << 3) | rm));
  }
  if (mod == 0x01) {
    put8(code_, static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
  } else if (mod != 0x00) {
    put32(code_, static_cast<std::int32_t>(disp));
  }
}

bool CodeGen::ripForm(bool hasIndex, std::uint8_t rm) {
  // [rip + disp32]: rm = 101 without SIB.
  return rm == 5 && !hasIndex;
}

void CodeGen::movImm(X64Reg dst, std::int64_t value) {
  std::uint8_t d = enc(dst);
  if (value >= -2147483648LL && value <= 2147483647LL) {
    rex(true, 0, false, 0, d);
    put8(code_, 0xC7);
    put8(code_, static_cast<std::uint8_t>(0xC0 | (d & 0x07)));  // ModRM reg=/0, rm=dst
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
  std::uint8_t b = enc(base);
  std::uint8_t s = enc(src);
  rex(true, s, false, 0, b);
  put8(code_, 0x89);
  modRmDisp(disp == 0 && (b & 7) != 5 ? 0x00 : 0x10, s, false, 0, b, disp);
}

void CodeGen::movFromMem(X64Reg dst, X64Reg base, std::int64_t disp) {
  std::uint8_t b = enc(base);
  std::uint8_t d = enc(dst);
  rex(true, d, false, 0, b);
  put8(code_, 0x8B);
  modRmDisp(disp == 0 && (b & 7) != 5 ? 0x00 : 0x10, d, false, 0, b, disp);
}

void CodeGen::movImmToMem(X64Reg base, std::int64_t disp, std::int64_t value) {
  std::uint8_t b = enc(base);
  rex(true, 0, false, 0, b);
  put8(code_, 0xC7);
  modRmDisp(disp == 0 && (b & 7) != 5 ? 0x00 : 0x10, 0, false, 0, b, disp);
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
  std::uint8_t d = static_cast<std::uint8_t>(dstXmm - kXmmBase);
  std::uint8_t s = static_cast<std::uint8_t>(srcXmm - kXmmBase);
  // Legacy SSE never needs REX.W; xmm8-15 are selected through REX.R (the
  // ModRM reg field) and REX.B (the ModRM rm field).
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
  bool zeroDisp = disp == 0 && (b & 7) != 5;
  modRmDisp(zeroDisp ? 0x00 : 0x10, d, false, 0, b, disp);
}

void CodeGen::movsdToXmm(int xmm, X64Reg base, std::int64_t disp) {
  sseMemOp(0xF2, 0x10, xmm, base, disp);
}

void CodeGen::movsdFromXmm(X64Reg base, std::int64_t disp, int xmm) {
  sseMemOp(0xF2, 0x11, xmm, base, disp);
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
  if (byte != 0x48) put8(code_, byte);
  put8(code_, 0xF2);
  put8(code_, 0x0F);
  put8(code_, 0x2A);
  put8(code_, static_cast<std::uint8_t>(0xC0 | (d << 3) | s));
}

void CodeGen::jmp(Label label) {
  put8(code_, 0xE9);
  PendingFixup fixup;
  fixup.patchAt = code_.size();
  fixup.labelId = label.id;
  fixups_.push_back(fixup);
  put32(code_, 0);
}

void CodeGen::jcc(Jcc cc, Label label) {
  put8(code_, 0x0F);
  put8(code_, static_cast<std::uint8_t>(cc));
  PendingFixup fixup;
  fixup.patchAt = code_.size();
  fixup.labelId = label.id;
  fixups_.push_back(fixup);
  put32(code_, 0);
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
