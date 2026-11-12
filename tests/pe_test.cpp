// Test for the PE emitter: verifies the structure (magic, machine, entry
// point, import table) and, on Windows, executes the generated binary.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "native/pe.h"
#include "native/target.h"

#if defined(_WIN32)
#include <cstdlib>
#include <fstream>
#endif

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// Reads a little-endian uint32 from `data` at `offset`.
static std::uint32_t read32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
  }
  return v;
}

// Reads a little-endian uint16 from `data` at `offset`.
static std::uint16_t read16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint16_t v = 0;
  for (int i = 0; i < 2; ++i) {
    v |= static_cast<std::uint16_t>(data[offset + i]) << (8 * i);
  }
  return v;
}

int main() {
  Target target;
  target.arch = Arch::X86_64;
  target.os = Os::Windows;
  target.abi = Abi::Win64;

  // Machine code that calls ExitProcess(0) via the IAT.
  //
  // Assembly:
  //   sub rsp, 0x28            ; shadow space (32 bytes) + alignment
  //   xor ecx, ecx             ; ExitProcess(0)
  //   call qword [rip+0x102C]  ; call IAT entry (RVA 0x2038)
  //   ; (ExitProcess never returns)
  //
  // Encoding:
  //   48 83 EC 28              ; sub rsp, 0x28
  //   31 C9                    ; xor ecx, ecx
  //   FF 15 2C 10 00 00        ; call qword [rip+0x102C]
  //
  // The RIP-relative offset is:
  //   target_rva (0x2038) - next_instruction_rva (0x100C) = 0x102C
  std::vector<std::uint8_t> text = {
    0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28
    0x31, 0xC9,                          // xor ecx, ecx
    0xFF, 0x15, 0x2C, 0x10, 0x00, 0x00,  // call qword [rip+0x102C]
    0xCC, 0xCC, 0xCC, 0xCC,              // padding (int3)
  };

  std::vector<std::uint8_t> rdata;
  std::uint64_t entryOffset = 0;

  std::vector<std::uint8_t> pe = buildStaticPe(target, text, rdata, entryOffset);

  // --- Basic structure ---
  check(pe.size() >= 0x200, "PE has at least 512 bytes");
  check(pe[0] == 'M' && pe[1] == 'Z', "DOS magic is MZ");
  check(read32(pe, 0x3C) == 0x40, "e_lfanew is 0x40");
  check(std::memcmp(pe.data() + 0x40, "PE\0\0", 4) == 0, "PE signature is PE\\0\\0");

  // --- COFF file header ---
  check(read16(pe, 0x44) == 0x8664, "machine is AMD64");
  check(read16(pe, 0x46) == 2, "number of sections is 2");
  check(read16(pe, 0x44 + 16) == 240, "SizeOfOptionalHeader is 240");

  // --- Optional header ---
  const std::size_t oh = 0x44 + 20;  // 0x58
  check(read16(pe, oh) == 0x020B, "optional magic is PE32+");
  check(read32(pe, oh + 16) == 0x1000, "AddressOfEntryPoint is 0x1000");
  check(read32(pe, oh + 32) == 0x1000, "SectionAlignment is 0x1000");
  check(read32(pe, oh + 36) == 0x200, "FileAlignment is 0x200");

  // --- Section headers ---
  const std::size_t sh = oh + 240;  // 0x148
  // Section 1: .text
  check(std::memcmp(pe.data() + sh, ".text", 5) == 0, "section 1 is .text");
  std::uint32_t textVa = read32(pe, sh + 12);
  std::uint32_t textPtr = read32(pe, sh + 20);
  check(textVa == 0x1000, ".text VirtualAddress is 0x1000");
  // Section 2: .idata
  check(std::memcmp(pe.data() + sh + 40, ".idata", 6) == 0, "section 2 is .idata");
  std::uint32_t idataVa = read32(pe, sh + 40 + 12);
  std::uint32_t idataPtr = read32(pe, sh + 40 + 20);
  check(idataVa == 0x2000, ".idata VirtualAddress is 0x2000");

  // DEBUG: print idataPtr and the first 64 bytes of .idata
  std::printf("DEBUG: idataPtr = 0x%X\n", idataPtr);
  std::printf("DEBUG: pe.size() = 0x%zX\n", pe.size());
  std::printf("DEBUG: bytes at idataPtr (first 64):\n");
  for (std::size_t i = 0; i < 64 && idataPtr + i < pe.size(); ++i) {
    std::printf("%02X ", pe[idataPtr + i]);
    if ((i + 1) % 16 == 0) std::printf("\n");
  }
  std::printf("\n");
  
  // --- Import table ---
  // The import directory RVA is 0x2000. The import descriptor is at
  // idataPtr (file offset), and its `Name` field is at +12.
  std::uint32_t nameRva = read32(pe, idataPtr + 12);
  check(nameRva == 0x2056, "import descriptor Name RVA is 0x2056");

  // The DLL name is at RVA 0x2056, which is file offset:
  //   idataPtr + (0x2056 - idataVa)
  std::size_t dllNameFileOffset = idataPtr + (nameRva - idataVa);
  check(std::memcmp(pe.data() + dllNameFileOffset, "kernel32.dll", 12) == 0,
        "DLL name is kernel32.dll");

  // Verify the import lookup table entry (ILT) points to the hint/name RVA.
  // The ILT is at idataPtr + 0x28 (after the 40-byte import directory).
  // The first entry is the RVA of the hint/name table (0x2048).
  std::uint32_t iltEntry = read32(pe, idataPtr + 0x28);
  check(iltEntry == 0x2048, "ILT entry is 0x2048");

  // Verify the hint/name table at RVA 0x2048 (file offset idataPtr + 0x48).
  // Hint is a 2-byte 0, followed by "ExitProcess\0".
  std::uint16_t hint = read16(pe, idataPtr + 0x48);
  check(hint == 0, "hint/name hint is 0");
  check(std::memcmp(pe.data() + idataPtr + 0x4A, "ExitProcess", 11) == 0,
        "hint/name is ExitProcess");

  // --- IAT ---
  // The IAT is at idataPtr + 0x38 (after the ILT). Before binding, it
  // contains the same RVA as the ILT (0x2048).
  std::uint32_t iatEntry = read32(pe, idataPtr + 0x38);
  check(iatEntry == 0x2048, "IAT entry is 0x2048");

#if defined(_WIN32)
  // Write the PE to disk and execute it.
  const char* path = "oskel_pe_test.exe";
  {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
      check(false, "cannot open oskel_pe_test.exe for writing");
      return g_failures != 0 ? 1 : 0;
    }
    f.write(reinterpret_cast<const char*>(pe.data()),
            static_cast<std::streamsize>(pe.size()));
  }

  // Execute: the binary should exit with code 0 (ExitProcess(0)).
  int rc = std::system(path);
  check(rc == 0, "generated PE runs and exits 0");

  std::remove(path);
#else
  std::printf("skipped: execution test requires Windows\n");
#endif

  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all pe tests passed\n");
  return 0;
}