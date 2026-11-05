#pragma once

#include <cstdint>
#include <vector>

#include "native/target.h"

// Generates a static ELF64 executable (Linux x86-64) with no external
// dependencies. Non-PIE: fixed virtual addresses (0x400000).
//
// The generated binary contains:
//   - ELF header (64 bytes)
//   - 2 PT_LOAD program headers (text R|X, rodata R)
//   - .text (machine code)
//   - .rodata (constant data: strings, floats, etc.)
//
// It does not include a section table (not needed for execution).
// Linking with musl static is delegated to musl-gcc externally (or done in
// a later step when the own linker is ready).
std::vector<std::uint8_t> buildStaticElf(const Target& target,
                                         const std::vector<std::uint8_t>& text,
                                         const std::vector<std::uint8_t>& rodata,
                                         std::uint64_t entryOffset);