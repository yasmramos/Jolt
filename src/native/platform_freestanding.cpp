#include "native/platform_freestanding.h"

#include <cstdint>

#if defined(__linux__) && defined(__x86_64__)

namespace {

// Raw Linux x86-64 syscall gate. The System V register order is
// rax (number), rdi, rsi, rdx, r10, r8, r9. rcx and r11 are clobbered by
// `syscall` itself, so they are declared as clobbers; "memory" keeps the
// compiler from reordering memory accesses across the gate.
long rawSyscall(long number, long a1, long a2, long a3, long a4, long a5,
                long a6) noexcept {
  long result;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8),
                     "r"(r9)
                   : "rcx", "r11", "memory");
  return result;
}

// Syscall numbers (asm/unistd_64.h).
constexpr long kSysRead = 0;
constexpr long kSysWrite = 1;
constexpr long kSysClose = 3;
constexpr long kSysLseek = 8;
constexpr long kSysMmap = 9;
constexpr long kSysExitGroup = 231;
constexpr long kSysOpenAt = 257;

// openat / lseek constants.
constexpr long kAtFdcwd = -100;
constexpr int kOReadOnly = 0;
constexpr int kOWriteOnly = 1;
constexpr int kOCreate = 64;
constexpr int kOTrunc = 512;

// mmap constants.
constexpr long kProtReadWrite = 3;          // PROT_READ | PROT_WRITE
constexpr long kMapPrivateAnon = 0x22;      // MAP_PRIVATE | MAP_ANONYMOUS

// A negative return in the range [-4095, -1] is an errno, not a result.
bool isError(long value) noexcept { return value < 0 && value >= -4095; }

}  // namespace

void PlatformFreestanding::write(int fd, const char* data, std::size_t length) {
  // The contract forbids short writes, so retry until the whole buffer is out
  // (a pipe or a signal can interrupt a large write).
  std::size_t written = 0;
  while (written < length) {
    long n = rawSyscall(kSysWrite, fd, reinterpret_cast<long>(data + written),
                        static_cast<long>(length - written), 0, 0, 0);
    if (isError(n) || n == 0) {
      // No error channel is available below libc; dropping the rest is the
      // only option that cannot recurse into a failing `writeErr`.
      return;
    }
    written += static_cast<std::size_t>(n);
  }
}

void PlatformFreestanding::writeErr(const char* data, std::size_t length) {
  write(Platform::kStderr, data, length);
}

void PlatformFreestanding::flush(int /*fd*/) {
  // Direct syscalls are unbuffered; there is nothing to push.
}

std::size_t PlatformFreestanding::read(int fd, char* buffer, std::size_t length) {
  long n = rawSyscall(kSysRead, fd, reinterpret_cast<long>(buffer),
                      static_cast<long>(length), 0, 0, 0);
  if (isError(n) || n < 0) return 0;
  return static_cast<std::size_t>(n);
}

int PlatformFreestanding::open(const char* path, OpenMode mode) {
  int flags = kOReadOnly;
  if (mode == OpenMode::Write) flags = kOWriteOnly | kOCreate | kOTrunc;
  long fd = rawSyscall(kSysOpenAt, kAtFdcwd, reinterpret_cast<long>(path), flags, 0644, 0, 0);
  if (isError(fd)) return kInvalidFd;
  return static_cast<int>(fd);
}

void PlatformFreestanding::close(int fd) {
  rawSyscall(kSysClose, fd, 0, 0, 0, 0, 0);
}

bool PlatformFreestanding::seek(int fd, std::int64_t offset, SeekOrigin origin) {
  long whence = 0;
  switch (origin) {
    case SeekOrigin::Start: whence = 0; break;
    case SeekOrigin::Current: whence = 1; break;
    case SeekOrigin::End: whence = 2; break;
  }
  long result = rawSyscall(kSysLseek, fd, static_cast<long>(offset), whence, 0, 0, 0);
  return !isError(result);
}

void PlatformFreestanding::exit(int code) {
  rawSyscall(kSysExitGroup, code, 0, 0, 0, 0, 0);
  // `exit_group` never returns; the builtin keeps the compiler happy about the
  // [[noreturn]] contract on every path.
  __builtin_unreachable();
}

void* PlatformFreestanding::alloc(std::size_t size, std::size_t alignment) {
  if (cursor_ == nullptr) {
    long region = rawSyscall(kSysMmap, 0, static_cast<long>(regionSize_),
                             kProtReadWrite, kMapPrivateAnon, -1, 0);
    if (isError(region) || region == 0) return nullptr;
    cursor_ = reinterpret_cast<std::uint8_t*>(region);
    end_ = cursor_ + regionSize_;
  }
  const std::uintptr_t current = reinterpret_cast<std::uintptr_t>(cursor_);
  const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
  const std::uintptr_t aligned = (current + mask) & ~mask;
  const std::size_t actual = size == 0 ? 1 : size;
  if (aligned > reinterpret_cast<std::uintptr_t>(end_) ||
      actual > reinterpret_cast<std::uintptr_t>(end_) - aligned) {
    return nullptr;
  }
  cursor_ = reinterpret_cast<std::uint8_t*>(aligned) + actual;
  return reinterpret_cast<void*>(aligned);
}

void PlatformFreestanding::dealloc(void* /*pointer*/) {
  // Region allocation: individual frees are not part of the model.
}

void* PlatformFreestanding::lookupSymbol(const char* /*name*/) {
  // No dynamic linker in a freestanding binary: `extern "C"` symbols are
  // link-time relocations. Reporting "not found" is the honest answer, and the
  // caller turns it into a clear error rather than a wild call.
  return nullptr;
}

#else  // not Linux x86-64

// The class must compile everywhere the dev library does (Windows and every
// CI compiler), but there is no syscall gate to reach for. Every operation
// fails loudly rather than silently pretending to work.
#include <stdexcept>

namespace {
[[noreturn]] void unsupported() {
  throw std::runtime_error("native/platform_freestanding: only linux-x86_64 is implemented");
}
}  // namespace

void PlatformFreestanding::write(int, const char*, std::size_t) { unsupported(); }
void PlatformFreestanding::writeErr(const char*, std::size_t) { unsupported(); }
void PlatformFreestanding::flush(int) { unsupported(); }
std::size_t PlatformFreestanding::read(int, char*, std::size_t) { unsupported(); }
int PlatformFreestanding::open(const char*, OpenMode) { unsupported(); }
void PlatformFreestanding::close(int) { unsupported(); }
bool PlatformFreestanding::seek(int, std::int64_t, SeekOrigin) { unsupported(); }
void PlatformFreestanding::exit(int) { unsupported(); }
void* PlatformFreestanding::alloc(std::size_t, std::size_t) { unsupported(); }
void PlatformFreestanding::dealloc(void*) { unsupported(); }
void* PlatformFreestanding::lookupSymbol(const char*) { unsupported(); }

#endif
