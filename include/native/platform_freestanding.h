#pragma once

#include <cstddef>
#include <cstdint>

#include "platform.h"

// Freestanding `Platform` for the AOT runtime (PHASE 7c).
//
// This is the second implementation of the `Platform` contract, next to
// `PlatformPosix`. It reaches the kernel directly (Linux x86-64 `syscall`)
// and never calls libc, so the same code can be linked into a generated
// binary that has no runtime to install. The dev engine keeps using
// `PlatformPosix`; nothing here is on the hosted hot path.
//
// Only Linux x86-64 has a backend today. On any other host the class still
// compiles (the dev library must build on Windows and on every CI compiler)
// but every operation throws, so a caller cannot silently get the hosted
// behaviour under a freestanding name.
class PlatformFreestanding final : public Platform {
 public:
  // The allocator's backing region is mapped lazily, `regionSize` bytes at a
  // time. The default is generous for the small programs the AOT backend emits
  // today; the parameter exists so tests can force exhaustion.
  explicit PlatformFreestanding(std::size_t regionSize = 1u << 20) noexcept
      : regionSize_(regionSize) {}

  void write(int fd, const char* data, std::size_t length) override;
  void writeErr(const char* data, std::size_t length) override;
  void flush(int fd) override;
  std::size_t read(int fd, char* buffer, std::size_t length) override;

  int open(const char* path, OpenMode mode) override;
  void close(int fd) override;
  bool seek(int fd, std::int64_t offset, SeekOrigin origin) override;

  [[noreturn]] void exit(int code) override;

  // Bump allocation from one `mmap`'d region, mapped on first use. `dealloc`
  // is a no-op: the freestanding runtime is a region, not a heap.
  void* alloc(std::size_t size, std::size_t alignment) override;
  void dealloc(void* pointer) override;

 private:
  // Lazily maps the backing region and bumps within it.
  std::uint8_t* cursor_ = nullptr;
  std::uint8_t* end_ = nullptr;
  std::size_t regionSize_ = 0;
};
