#pragma once

#include <cstddef>
#include <cstdint>

// Host interface for the embeddable Jolt runtime.
//
// The runtime core (values, ops, the bytecode VM and, later, the AOT runtime)
// must not call libc directly. Everything that touches the outside world goes
// through this interface, so the same runtime can run on:
//
//   - PlatformPosix        (dev engine: libc and the C++ standard library)
//   - PlatformFreestanding (PHASE 7c: direct syscalls, no libc)
//
// The signatures are deliberately byte-oriented: no `std::string`, no
// `<iostream>`, no allocation hidden behind a container. A freestanding
// implementation can therefore satisfy them with an arena and raw fd numbers.
// The hosted side converts its strings at the call site, which is cheap and
// keeps the contract honest.
class Platform {
 public:
  enum class OpenMode {
    Read,   // open an existing file for reading; fails if it does not exist
    Write,  // create or truncate for writing
  };

  enum class SeekOrigin {
    Start,    // offset from the beginning of the file
    Current,  // offset from the current position
    End,      // offset from the end of the file (offset is usually <= 0)
  };

  // Sentinel returned by `open` on failure. Call sites compare against it
  // instead of hardcoding -1, so a future ABI with a different convention
  // (or a mock platform) stays a one-line change.
  static constexpr int kInvalidFd = -1;

  // Standard descriptors. Named so the VM, the runtime and the stdlib agree
  // on which fd carries diagnostics (kStderr) versus program output
  // (kStdout) without every call site hardcoding 1 and 2.
  static constexpr int kStdin = 0;
  static constexpr int kStdout = 1;
  static constexpr int kStderr = 2;

  virtual ~Platform() = default;

  // Writes the whole buffer. It must not return short: partial writes are the
  // implementation's problem (retry, buffer, or fail loudly).
  virtual void write(int fd, const char* data, std::size_t length) = 0;

  // Diagnostics channel. Always fd 2. Kept separate from `write` so that
  // `Runtime::fail` can never be redirected by a program that reassigns its
  // own stdout, and so the freestanding implementation can route it to
  // something other than the program's output without a policy switch.
  virtual void writeErr(const char* data, std::size_t length) = 0;

  // Pushes any buffered bytes for `fd` to the host. A platform that does not
  // buffer (PlatformPosix forwards to libc, which buffers on its own) makes
  // this a no-op.
  virtual void flush(int fd) = 0;

  // Reads up to `length` bytes and returns how many were read. A return value
  // of 0 means end of input (or a closed descriptor); the caller decides
  // whether that is an error.
  virtual std::size_t read(int fd, char* buffer, std::size_t length) = 0;

  // Opens `path` and returns a descriptor, or kInvalidFd. `Write` truncates.
  virtual int open(const char* path, OpenMode mode) = 0;
  virtual void close(int fd) = 0;

  // Moves the read/write offset. `offset` is signed so rewinding and
  // backward seeks are expressible; the freestanding implementation maps
  // SeekOrigin to the Linux whence values (0/1/2).
  virtual bool seek(int fd, std::int64_t offset, SeekOrigin origin) = 0;

  // Terminates the process. A test implementation may throw instead of
  // returning; either way it never returns normally.
  [[noreturn]] virtual void exit(int code) = 0;

  // Storage for the embedded runtime's arena. The hosted engine's GC still
  // allocates through its own `makeRef`, so these are only exercised once the
  // freestanding arena lands in PHASE 7c; they are part of the contract now so
  // adding them later is not an interface break.
  virtual void* alloc(std::size_t size, std::size_t alignment) = 0;
  virtual void dealloc(void* pointer) = 0;

  // Resolves a C symbol declared in an `extern "C"` block (PHASE 6.6). The
  // hosted engine answers with the address of the libc/host symbol; a platform
  // that cannot reach one (the freestanding AOT runtime, where extern symbols
  // are link-time relocations resolved by the static linker, not at run time)
  // returns nullptr. The caller turns nullptr into a clear runtime error, so a
  // program never silently calls a wrong address.
  virtual void* lookupSymbol(const char* name) = 0;
};

// The development engine's implementation (libc and the C++ standard library
// are allowed there). It is process-wide and stateless: no buffering of its
// own, `flush` forwards to libc (a no-op for the descriptors libc already
// buffers). Callers that need to capture output or simulate a host construct
// their own `Platform`.
Platform& platformPosix();
