#include "native/pe.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint16_t kDosMagic = 0x5A4D;
constexpr std::uint32_t kPeSignature = 0x00004550;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kOptionalMagicPe32Plus = 0x020B;
constexpr std::uint64_t kImageBase = 0x140000000;
constexpr std::uint32_t kSectionAlignment = 0x1000;
constexpr std::uint32_t kFileAlignment = 0x200;

constexpr std::uint32_t kTextRva = 0x1000;
constexpr std::uint32_t kIdataRva = 0x2000;

void write16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void write64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void padTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
  while (out.size() % alignment != 0) out.push_back(0);
}

}  // namespace

std::vector<std::uint8_t> buildStaticPe(const Target& target,
                                         const std::vector<std::uint8_t>& text,
                                         const std::vector<std::uint8_t>& rdata,
                                         std::uint64_t entryOffset) {
  (void)rdata;

  if (target.arch != Arch::X86_64 || target.os != Os::Windows) {
    throw std::runtime_error("native/pe: only x86_64-windows is supported");
  }
  if (text.empty()) {
    throw std::runtime_error("native/pe: text section is empty");
  }
  if (entryOffset >= text.size()) {
    throw std::runtime_error("native/pe: entry offset out of range");
  }

  // --- Build .idata (RVA 0x2000) ---
  std::vector<std::uint8_t> idata;

  // 1. Import Directory Table (40 bytes: 1 descriptor + terminator)
  write32(idata, 0x2028);  // OriginalFirstThunk (ILT RVA)
  write32(idata, 0);       // TimeDateStamp
  write32(idata, 0);       // ForwarderChain
  write32(idata, 0x2056);  // Name (DLL name RVA)
  write32(idata, 0x2038);  // FirstThunk (IAT RVA)
  for (int i = 0; i < 5; ++i) write32(idata, 0);  // terminator (20 bytes)

  // 2. Import Lookup Table (16 bytes: 1 entry + terminator)
  write64(idata, 0x2048);  // RVA of Hint/Name entry
  write64(idata, 0);       // terminator

  // 3. Import Address Table (16 bytes: 1 entry + terminator)
  write64(idata, 0x2048);
  write64(idata, 0);

  // 4. Hint/Name Table (14 bytes: 2 hint + 12 "ExitProcess\0")
  write16(idata, 0);  // hint
  for (const char* c = "ExitProcess"; *c; ++c) idata.push_back(static_cast<std::uint8_t>(*c));
  idata.push_back(0);

  // 5. DLL name (13 bytes: "kernel32.dll\0")
  for (const char* c = "kernel32.dll"; *c; ++c) idata.push_back(static_cast<std::uint8_t>(*c));
  idata.push_back(0);

  // Sanity check: .idata should be 99 bytes
  if (idata.size() != 99) {
    throw std::logic_error("native/pe: internal error, .idata size is " +
                           std::to_string(idata.size()) + " (expected 99)");
  }

  // Pad .idata to file alignment
  padTo(idata, kFileAlignment);

  // --- Build the PE file ---
  std::vector<std::uint8_t> out;

  // --- DOS Header (64 bytes) ---
  out.resize(64, 0);
  out[0] = 'M'; out[1] = 'Z';
  out[0x3C] = 0x40;

  // --- PE Signature + COFF File Header ---
  out.resize(0x40 + 4 + 20, 0);
  std::memcpy(out.data() + 0x40, "PE\0\0", 4);

  const std::size_t fh = 0x44;
  out[fh + 0] = 0x64; out[fh + 1] = 0x86;  // Machine = AMD64
  out[fh + 2] = 2;                         // NumberOfSections = 2
  out[fh + 16] = 0xF0;                     // SizeOfOptionalHeader = 240
  out[fh + 18] = 0x22;                     // Characteristics

  // --- Optional Header (PE32+, 240 bytes) ---
  const std::size_t oh = fh + 20;
  out.resize(oh + 240, 0);

  out[oh + 0] = 0x0B; out[oh + 1] = 0x02;  // Magic = PE32+
  out[oh + 2] = 1;                         // MajorLinkerVersion
  std::uint32_t textSize = static_cast<std::uint32_t>(text.size());
  out[oh + 4] = static_cast<std::uint8_t>(textSize & 0xFF);
  out[oh + 5] = static_cast<std::uint8_t>((textSize >> 8) & 0xFF);
  std::uint32_t idataSize = static_cast<std::uint32_t>(idata.size());
  out[oh + 8] = static_cast<std::uint8_t>(idataSize & 0xFF);
  out[oh + 9] = static_cast<std::uint8_t>((idataSize >> 8) & 0xFF);
  std::uint32_t entryRva = kTextRva + static_cast<std::uint32_t>(entryOffset);
  out[oh + 16] = static_cast<std::uint8_t>(entryRva & 0xFF);
  out[oh + 17] = static_cast<std::uint8_t>((entryRva >> 8) & 0xFF);
  out[oh + 20] = 0x00; out[oh + 21] = 0x10;  // BaseOfCode = 0x1000
  for (int i = 0; i < 8; ++i)
    out[oh + 24 + i] = static_cast<std::uint8_t>((kImageBase >> (8 * i)) & 0xFF);
  out[oh + 32] = 0x00; out[oh + 33] = 0x10;  // SectionAlignment = 0x1000
  out[oh + 36] = 0x00; out[oh + 37] = 0x02;  // FileAlignment = 0x200
  out[oh + 40] = 6;                          // MajorOSVersion
  out[oh + 48] = 6;                          // MajorSubsystemVersion
  out[oh + 56] = 0x00; out[oh + 57] = 0x40;  // SizeOfImage = 0x4000
  out[oh + 60] = 0x00; out[oh + 61] = 0x04;  // SizeOfHeaders = 0x400
  out[oh + 68] = 3;                          // Subsystem = console
  out[oh + 72] = 0x00; out[oh + 73] = 0x00; out[oh + 74] = 0x10;  // StackReserve
  out[oh + 80] = 0x00; out[oh + 81] = 0x10;  // StackCommit
  out[oh + 88] = 0x00; out[oh + 89] = 0x00; out[oh + 90] = 0x10;  // HeapReserve
  out[oh + 96] = 0x00; out[oh + 97] = 0x10;  // HeapCommit
  out[oh + 108] = 16;                        // NumberOfRvaAndSizes

  // DataDirectory[1] = Import Table
  out[oh + 120] = 0x00; out[oh + 121] = 0x20;  // RVA = 0x2000
  out[oh + 124] = 40;                          // Size = 40

  // --- Section Headers (2 * 40 bytes) ---
  const std::size_t sh = oh + 240;
  out.resize(sh + 80, 0);

  // .text section
  std::memcpy(out.data() + sh, ".text\0\0\0", 8);
  out[sh + 8] = static_cast<std::uint8_t>(textSize & 0xFF);
  out[sh + 9] = static_cast<std::uint8_t>((textSize >> 8) & 0xFF);
  out[sh + 12] = 0x00; out[sh + 13] = 0x10;  // VirtualAddress = 0x1000
  std::uint32_t textRaw = (textSize + 0x1FF) & ~0x1FFu;
  out[sh + 16] = static_cast<std::uint8_t>(textRaw & 0xFF);
  out[sh + 17] = static_cast<std::uint8_t>((textRaw >> 8) & 0xFF);
  out[sh + 20] = 0x00; out[sh + 21] = 0x02;  // PointerToRawData = 0x200
  out[sh + 36] = 0x20; out[sh + 39] = 0x60;  // CODE | EXECUTE | READ

  // .idata section
  std::memcpy(out.data() + sh + 40, ".idata\0\0", 8);
  out[sh + 40 + 8] = static_cast<std::uint8_t>(idataSize & 0xFF);
  out[sh + 40 + 9] = static_cast<std::uint8_t>((idataSize >> 8) & 0xFF);
  out[sh + 40 + 12] = 0x00; out[sh + 40 + 13] = 0x20;  // VirtualAddress = 0x2000
  out[sh + 40 + 16] = static_cast<std::uint8_t>(idataSize & 0xFF);
  out[sh + 40 + 17] = static_cast<std::uint8_t>((idataSize >> 8) & 0xFF);
  out[sh + 40 + 20] = 0x00; out[sh + 40 + 21] = 0x04;  // PointerToRawData = 0x400
  out[sh + 40 + 36] = 0x40; out[sh + 40 + 39] = 0xC0;  // INITIALIZED_DATA | READ | WRITE

  // --- Write sections ---
  padTo(out, 0x200);
  out.insert(out.end(), text.begin(), text.end());
  padTo(out, 0x200);
  out.insert(out.end(), idata.begin(), idata.end());

  return out;
}