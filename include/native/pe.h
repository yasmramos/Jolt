#pragma once

#include <cstdint>
#include <vector>

#include "native/target.h"

// Generates a static PE32+ executable (Windows x86-64) with no external
// dependencies. Non-PIE: fixed image base (0x140000000).
//
// The generated binary contains:
//   - DOS header (64 bytes) + DOS stub (optional)
//   - PE signature + COFF file header
//   - Optional header (PE32+)
//   - Section headers (.text, .rdata, .idata)
//   - .text (machine code)
//   - .rdata (constant data)
//   - .idata (import table for ExitProcess, etc.)
//
// This is a minimal PE generator. It does NOT include relocations, resources,
// or TLS. It assumes the code uses only imports from kernel32.dll.
std::vector<std::uint8_t> buildStaticPe(const Target& target,
                                         const std::vector<std::uint8_t>& text,
                                         const std::vector<std::uint8_t>& rdata,
                                         std::uint64_t entryOffset);