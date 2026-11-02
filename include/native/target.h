#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Target descriptor for the PHASE 7 native backend.
//
// The backend is written against a Target from the start, even though only
// Linux x86-64 (System V) has an implementation today. Adding Windows, macOS
// or WebAssembly must be a new backend behind this descriptor, not a rewrite
// of every call site that currently assumes "the target".
//
// `CodeGen` rejects any target it cannot emit for; the enumerators below name
// the targets the backend is expected to grow into, not a promise that they
// exist.

enum class Arch : std::uint8_t {
  X86_64,
  Aarch64,
  Wasm32,
};

enum class Os : std::uint8_t {
  Linux,
  Windows,
  MacOs,
  Freestanding,
};

enum class Abi : std::uint8_t {
  SysV,
  Win64,
  Aapcs64,
  Wasm,
};

struct Target {
  Arch arch = Arch::X86_64;
  Os os = Os::Linux;
  Abi abi = Abi::SysV;

  // The only target with a backend today.
  static constexpr Target linuxX64() { return Target{}; }
};

inline const char* archName(Arch arch) {
  switch (arch) {
    case Arch::X86_64:
      return "x86_64";
    case Arch::Aarch64:
      return "aarch64";
    case Arch::Wasm32:
      return "wasm32";
  }
  return "unknown";
}

inline const char* osName(Os os) {
  switch (os) {
    case Os::Linux:
      return "linux";
    case Os::Windows:
      return "windows";
    case Os::MacOs:
      return "macos";
    case Os::Freestanding:
      return "freestanding";
  }
  return "unknown";
}

inline const char* abiName(Abi abi) {
  switch (abi) {
    case Abi::SysV:
      return "sysv";
    case Abi::Win64:
      return "win64";
    case Abi::Aapcs64:
      return "aapcs64";
    case Abi::Wasm:
      return "wasm";
  }
  return "unknown";
}

// Canonical backend name, e.g. "x86_64-linux-sysv". This is deliberately not
// a GNU triple: the backend cares about the three components it can emit for.
inline std::string targetName(const Target& target) {
  return std::string(archName(target.arch)) + "-" + osName(target.os) + "-" +
         abiName(target.abi);
}

inline bool isSupportedTarget(const Target& target) {
  return target.arch == Arch::X86_64 && target.os == Os::Linux &&
         target.abi == Abi::SysV;
}

// Fails loudly instead of emitting bytes for an ABI the encoder does not know.
// Callers construct their backend with a Target; a missing target is a
// configuration error, not a reason to silently fall back to Linux.
inline void requireSupportedTarget(const Target& target) {
  if (!isSupportedTarget(target)) {
    throw std::invalid_argument("native: no backend for target " + targetName(target) +
                                " (supported: " + targetName(Target::linuxX64()) + ")");
  }
}
