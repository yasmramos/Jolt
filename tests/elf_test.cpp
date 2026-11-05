// Minimal test for the ELF emitter: generates an ELF with a `ret` and
// verifies the structure (magic, type, machine, entry point).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "native/elf.h"
#include "native/target.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

int main() {
  Target target = Target::linuxX64();

  // Machine code: `ret` (0xC3)
  std::vector<std::uint8_t> text = {0xC3};
  std::vector<std::uint8_t> rodata;
  std::uint64_t entryOffset = 0;

  std::vector<std::uint8_t> elf = buildStaticElf(target, text, rodata, entryOffset);

  // Verify magic
  check(elf.size() >= 64, "ELF has at least 64 bytes");
  check(elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F',
        "ELF magic is 7F 45 4C 46");
  check(elf[4] == 2, "ELF class is 64-bit");
  check(elf[5] == 1, "ELF data is little-endian");

  // Verify e_type (offset 16) = ET_EXEC (2)
  check(elf[16] == 2 && elf[17] == 0, "e_type is ET_EXEC");

  // Verify e_machine (offset 18) = EM_X86_64 (0x3E)
  check(elf[18] == 0x3E && elf[19] == 0, "e_machine is EM_X86_64");

  // Verify e_entry (offset 24) = kTextAddress + 0 = 0x401000
  std::uint64_t entry = 0;
  for (int i = 0; i < 8; ++i) {
    entry |= static_cast<std::uint64_t>(elf[24 + i]) << (8 * i);
  }
  check(entry == 0x401000, "e_entry is 0x401000");

  // Verify e_phnum (offset 56) = 2
  check(elf[56] == 2 && elf[57] == 0, "e_phnum is 2");

  // Verify there are bytes after the header (code)
  check(elf.size() > 0x1000, "ELF contains .text section");

  // Verify the 0xC3 byte is at offset 0x1000
  check(elf[0x1000] == 0xC3, ".text starts at file offset 0x1000");

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all elf tests passed\n");
  return 0;
}