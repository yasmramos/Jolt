#include "native/elf.h"

#include <cstring>
#include <stdexcept>

namespace {

// Fixed virtual addresses for a non-PIE binary.
constexpr std::uint64_t kBaseAddress = 0x400000;
constexpr std::uint64_t kTextAddress = kBaseAddress + 0x1000;
constexpr std::uint64_t kRodataAddress = 0x600000;

// Little-endian helpers.
void write8(std::vector<std::uint8_t>& out, std::uint8_t v) {
  out.push_back(v);
}

void write16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void write32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void write64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
  }
}

void padTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
  while (out.size() % alignment != 0) out.push_back(0);
}

// Writes a program header into `out` starting at `off`.
void writeProgramHeader(std::vector<std::uint8_t>& out, std::size_t off,
                        std::uint32_t type, std::uint32_t flags,
                        std::uint64_t offset, std::uint64_t vaddr,
                        std::uint64_t filesz, std::uint64_t memsz) {
  std::vector<std::uint8_t> ph;
  ph.reserve(56);
  write32(ph, type);        // p_type
  write32(ph, flags);       // p_flags
  write64(ph, offset);      // p_offset
  write64(ph, vaddr);       // p_vaddr
  write64(ph, vaddr);       // p_paddr (same as vaddr for non-PIE)
  write64(ph, filesz);      // p_filesz
  write64(ph, memsz);       // p_memsz
  write64(ph, 0x1000);      // p_align (4KB page)
  std::memcpy(out.data() + off, ph.data(), ph.size());
}

}  // namespace

std::vector<std::uint8_t> buildStaticElf(const Target& target,
                                         const std::vector<std::uint8_t>& text,
                                         const std::vector<std::uint8_t>& rodata,
                                         std::uint64_t entryOffset) {
  requireSupportedTarget(target);

  if (text.empty()) {
    throw std::runtime_error("native/elf: text section is empty");
  }
  if (entryOffset >= text.size()) {
    throw std::runtime_error("native/elf: entry offset out of range");
  }

  std::vector<std::uint8_t> out;
  out.reserve(0x2000 + text.size() + rodata.size());

  // --- Reserve space for the ELF header (64 bytes) ---
  out.resize(64, 0);

  // --- Reserve space for 2 program headers (2 * 56 = 112 bytes) ---
  const std::size_t phoff = out.size();
  out.resize(out.size() + 112, 0);

  // --- .text (page-aligned) ---
  padTo(out, 0x1000);
  const std::size_t textFileOffset = out.size();
  out.insert(out.end(), text.begin(), text.end());

  // --- .rodata (page-aligned) ---
  std::size_t rodataFileOffset = 0;
  if (!rodata.empty()) {
    padTo(out, 0x1000);
    rodataFileOffset = out.size();
    out.insert(out.end(), rodata.begin(), rodata.end());
  }

  // --- Write ELF header ---
  {
    std::vector<std::uint8_t> ehdr;
    ehdr.reserve(64);
    // e_ident[16]
    write8(ehdr, 0x7F);  // ELFMAG0
    write8(ehdr, 'E');   // ELFMAG1
    write8(ehdr, 'L');   // ELFMAG2
    write8(ehdr, 'F');   // ELFMAG3
    write8(ehdr, 2);     // ELFCLASS64
    write8(ehdr, 1);     // ELFDATA2LSB
    write8(ehdr, 1);     // EV_CURRENT
    write8(ehdr, 0);     // ELFOSABI_SYSV
    for (int i = 0; i < 8; ++i) write8(ehdr, 0);  // padding

    write16(ehdr, 2);                     // e_type = ET_EXEC
    write16(ehdr, 0x3E);                  // e_machine = EM_X86_64
    write32(ehdr, 1);                     // e_version = EV_CURRENT
    write64(ehdr, kTextAddress + entryOffset);  // e_entry
    write64(ehdr, phoff);                 // e_phoff
    write64(ehdr, 0);                     // e_shoff (no section table)
    write32(ehdr, 0);                     // e_flags
    write16(ehdr, 64);                    // e_ehsize
    write16(ehdr, 56);                    // e_phentsize
    write16(ehdr, 2);                     // e_phnum = 2
    write16(ehdr, 64);                    // e_shentsize
    write16(ehdr, 0);                     // e_shnum = 0
    write16(ehdr, 0);                     // e_shstrndx = 0

    if (ehdr.size() != 64) {
      throw std::logic_error("native/elf: internal error, ELF header size mismatch");
    }
    std::memcpy(out.data(), ehdr.data(), ehdr.size());
  }

  // --- Program header 1: .text (PT_LOAD, PF_R | PF_X) ---
  writeProgramHeader(out, phoff, /*PT_LOAD=*/1, /*PF_R|PF_X=*/5,
                     textFileOffset, kTextAddress,
                     text.size(), text.size());

  // --- Program header 2: .rodata (PT_LOAD, PF_R) ---
  if (!rodata.empty()) {
    writeProgramHeader(out, phoff + 56, /*PT_LOAD=*/1, /*PF_R=*/4,
                       rodataFileOffset, kRodataAddress,
                       rodata.size(), rodata.size());
  }

  return out;
}