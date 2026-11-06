// Tests for the freestanding AOT runtime (PHASE 7c.0).
//
// Three levels:
//   1. Arena: alignment, exhaustion, reset, null block -- pure, portable.
//   2. PlatformFreestanding: a real write/read file roundtrip through direct
//      syscalls, plus alloc/dealloc.
//   3. End to end: build a real ELF from emitted machine code (a `main` that
//      writes a string and returns 0), write it to a temp file, run it, and
//      check its stdout, exit code and that it has no dynamic dependencies.
//
// Levels 2 and 3 need Linux x86-64; elsewhere they are skipped and only the
// arena runs.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "native/arena.h"
#include "native/entry.h"
#include "native/elf.h"
#include "native/platform_freestanding.h"

static int g_failures = 0;

static void check(bool condition, const char* what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}

// --------------------------------------------------------------------- arena

static void test_arena() {
  alignas(64) unsigned char block[256];
  Arena arena(block, sizeof block);

  check(arena.capacity() == sizeof block, "arena reports its capacity");
  check(arena.used() == 0, "arena starts empty");

  void* a = arena.allocate(1, 1);
  check(a == block, "first allocation is at the block start");
  check(reinterpret_cast<std::uintptr_t>(a) % 1 == 0, "1-byte alignment");

  void* b = arena.allocate(8, 8);
  check(reinterpret_cast<std::uintptr_t>(b) % 8 == 0, "8-byte alignment honoured");
  check(b != a, "distinct allocations do not overlap");

  void* c = arena.allocate(16, 16);
  check(reinterpret_cast<std::uintptr_t>(c) % 16 == 0, "16-byte alignment honoured");

  // Zero-size allocations still get distinct addresses.
  void* z1 = arena.allocate(0, 8);
  void* z2 = arena.allocate(0, 8);
  check(z1 != nullptr && z2 != nullptr && z1 != z2, "zero-size allocations are distinct");

  // Exhaustion returns nullptr instead of throwing.
  void* huge = arena.allocate(1u << 20, 8);
  check(huge == nullptr, "exhausted arena returns nullptr");

  arena.reset();
  check(arena.used() == 0, "reset reclaims the region");
  void* again = arena.allocate(4, 4);
  check(again == block, "allocation after reset reuses the block start");

  // A bad alignment is rejected.
  Arena empty(nullptr, 0);
  check(empty.allocate(8, 8) == nullptr, "null-block arena fails every allocation");
  check(empty.allocate(8, 3) == nullptr, "non-power-of-two alignment rejected");
}

// --------------------------------------------------------------- platform

#if defined(__linux__) && defined(__x86_64__)
#define FREESTANDING_EXEC_TESTS 1
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef FREESTANDING_EXEC_TESTS

static void test_platform_roundtrip() {
  PlatformFreestanding platform;
  const char* path = "/tmp/jolt_freestanding_roundtrip.txt";
  const char* payload = "hello freestanding\n";

  int fd = platform.open(path, Platform::OpenMode::Write);
  check(fd != Platform::kInvalidFd, "open(Write) returns a descriptor");
  platform.write(fd, payload, std::strlen(payload));
  platform.flush(fd);
  platform.close(fd);

  fd = platform.open(path, Platform::OpenMode::Read);
  check(fd != Platform::kInvalidFd, "open(Read) returns a descriptor");
  char buffer[64] = {0};
  std::size_t n = platform.read(fd, buffer, sizeof buffer);
  platform.close(fd);
  check(n == std::strlen(payload), "read returns the written length");
  check(std::strcmp(buffer, payload) == 0, "read returns the written bytes");

  // seek back to the start and re-read.
  fd = platform.open(path, Platform::OpenMode::Read);
  check(platform.seek(fd, 0, Platform::SeekOrigin::Start), "seek(Start) succeeds");
  char first = 0;
  check(platform.read(fd, &first, 1) == 1 && first == 'h', "seek repositions the cursor");
  platform.close(fd);

  // Missing file: Read does not create.
  check(platform.open("/tmp/jolt_does_not_exist_12345", Platform::OpenMode::Read) ==
            Platform::kInvalidFd,
        "open(Read) on a missing file fails");

  // Allocator hands out aligned, distinct memory and frees are no-ops.
  void* p1 = platform.alloc(32, 16);
  void* p2 = platform.alloc(32, 16);
  check(p1 != nullptr && p2 != nullptr && p1 != p2, "platform alloc returns distinct blocks");
  check(reinterpret_cast<std::uintptr_t>(p1) % 16 == 0, "platform alloc honours alignment");
  platform.dealloc(p1);  // no-op, must not crash
}

// Builds a tiny program in `code` that writes `message` (a rodata offset known
// to the caller) and returns `exitCode`, then appends the freestanding runtime.
// Returns the entry offset and the text/rodata images.
struct BuiltProgram {
  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> rodata;
  std::uint64_t entry = 0;
};

// `main(argc, argv)`: jolt_write(1, messageAddress, messageLength); return N.
static BuiltProgram buildHello(std::int64_t messageAddress, std::int64_t messageLength,
                               std::int64_t exitCode) {
  CodeGen code;
  RuntimeHandles handles = newRuntimeHandles(code);

  CodeGen::Label mainLabel = code.newLabel();
  code.bind(mainLabel);

  // jolt_write(1, message, length)
  code.movImm(X64Reg::Rdi, 1);
  code.movImm(X64Reg::Rsi, messageAddress);
  code.movImm(X64Reg::Rdx, messageLength);
  code.callLabel(handles.write);

  code.movImm(X64Reg::Rax, exitCode);
  code.retInsn();

  std::uint64_t entry = emitFreestandingRuntime(code, handles, mainLabel);
  code.finalize();

  BuiltProgram built;
  built.text = code.code();
  built.entry = entry;
  return built;
}

// Runs `path` and captures its stdout and exit code.
struct RunResult {
  std::string output;
  int exitCode = -1;
};

static RunResult runProgram(const std::string& path) {
  std::string command = path + " > /tmp/jolt_freestanding_out.txt 2>&1";
  int status = std::system(command.c_str());
  RunResult result;
  result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  std::FILE* file = std::fopen("/tmp/jolt_freestanding_out.txt", "rb");
  if (file) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
      result.output.append(buffer, n);
    }
    std::fclose(file);
  }
  return result;
}

static void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return;
  std::fwrite(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  chmod(path.c_str(), 0755);
}

static void test_end_to_end() {
  // The .rodata section is loaded at 0x600000 (see elf.cpp); the message sits
  // at its very start.
  const char* message = "hello from a Jolt AOT binary\n";
  std::vector<std::uint8_t> rodata(message, message + std::strlen(message));
  const std::int64_t messageAddress = 0x600000;

  BuiltProgram built = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 0);
  built.rodata = rodata;

  std::vector<std::uint8_t> elf =
      buildStaticElf(Target::linuxX64(), built.text, built.rodata, built.entry);
  const std::string path = "/tmp/jolt_freestanding_hello";
  writeFile(path, elf);

  RunResult result = runProgram(path);
  check(result.output == message, "the generated binary prints the expected bytes");
  check(result.exitCode == 0, "the generated binary exits with the program's code");

  // A non-zero return value must become the process exit code.
  BuiltProgram failing = buildHello(messageAddress, static_cast<std::int64_t>(rodata.size()), 7);
  failing.rodata = rodata;
  std::vector<std::uint8_t> elfFail =
      buildStaticElf(Target::linuxX64(), failing.text, failing.rodata, failing.entry);
  writeFile("/tmp/jolt_freestanding_exit7", elfFail);
  RunResult failed = runProgram("/tmp/jolt_freestanding_exit7");
  check(failed.exitCode == 7, "the program's return value becomes the exit code");

  // No dynamic dependencies: this is a static, freestanding binary.
  std::string ldd = "ldd " + path + " 2>&1";
  std::FILE* pipe = popen(ldd.c_str(), "r");
  std::string lddOut;
  if (pipe) {
    char buffer[256];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
      lddOut.append(buffer, n);
    }
    pclose(pipe);
  }
  check(lddOut.find("not a dynamic executable") != std::string::npos ||
            lddOut.find("statically linked") != std::string::npos,
        "the generated binary has no dynamic dependencies");
}

#endif  // FREESTANDING_EXEC_TESTS

int main() {
  test_arena();
#ifdef FREESTANDING_EXEC_TESTS
  test_platform_roundtrip();
  test_end_to_end();
#else
  std::printf("skipped: platform and execution tests require Linux x86-64\n");
#endif
  if (g_failures != 0) {
    std::printf("%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all freestanding tests passed\n");
  return 0;
}
