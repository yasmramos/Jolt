// Behavioral tests for the freestanding float-to-text routine (PHASE 7c.1d.3).
//
// The emitted machine code is mapped as executable memory and called through a
// function pointer, exactly like the hosted engine would call it. The contract
// is Jolt's `str()`: the shortest text that round-trips, matching
// `std::to_chars` byte for byte, including the fixed/scientific choice.
//
// On any platform other than Linux x86-64 the execution section is skipped.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "native/codegen.h"
#include "native/dtoa.h"
#include "native/value.h"

#if defined(__linux__) && defined(__x86_64__)
#define DTOA_EXEC_TESTS 1
#include <charconv>
#include <cstdlib>
#include <random>
#include <sys/mman.h>
#endif

static int g_failures = 0;

static void check(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

#if defined(DTOA_EXEC_TESTS)
using FormatFn = std::int64_t (*)(std::int64_t, std::int64_t, char*, void*);

// C11 requires `aligned_alloc`'s size to be a multiple of the alignment;
// `kScratchBytes + 64` is not necessarily one, and passing it anyway is
// undefined behavior (glibc tolerates it, the sanitizers reject it).
static void* allocScratch() {
  const std::size_t alignment = 64;
  std::size_t bytes = dtoa::kScratchBytes + alignment;
  bytes = (bytes + alignment - 1) / alignment * alignment;
  return std::aligned_alloc(alignment, bytes);
}

static FormatFn emitFormat() {
  CodeGen gen;
  dtoa::DtoaHandles handles = dtoa::newDtoaHandles(gen);
  dtoa::emitDtoa(gen, handles);
  gen.finalize();
  const std::vector<std::uint8_t>& bytes = gen.code();

  std::size_t span = bytes.size();
  void* mem = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    std::perror("mmap");
    std::exit(2);
  }
  std::memcpy(mem, bytes.data(), span);
  if (mprotect(mem, span, PROT_READ | PROT_EXEC) != 0) {
    std::perror("mprotect");
    std::exit(2);
  }
  auto base = reinterpret_cast<std::uint8_t*>(mem);
  return reinterpret_cast<FormatFn>(base + gen.labelOffset(handles.formatDouble));
}

static std::string reference(double v) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

static std::string formatWith(FormatFn fn, void* scratch, std::uint64_t tag, double v) {
  char out[512];
  std::uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  std::int64_t n = fn(static_cast<std::int64_t>(tag), static_cast<std::int64_t>(bits), out, scratch);
  return std::string(out, static_cast<std::size_t>(n));
}

static std::string reference32(float f) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), f);
  return std::string(buf, r.ptr);
}

static void test_random_doubles() {
  FormatFn fn = emitFormat();
  void* scratch = allocScratch();
  std::mt19937_64 rng(20240607);
  long mismatches = 0;
  long total = 0;
  std::string firstBad;
  for (long i = 0; i < 3000000; ++i) {
    std::uint64_t bits = rng();
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    if (v != v || v - v != 0) continue;  // skip nan/inf here
    std::string got = formatWith(fn, scratch, static_cast<std::uint64_t>(AotTag::Double), v);
    std::string want = reference(v);
    ++total;
    if (got != want) {
      ++mismatches;
      if (mismatches <= 10) {
        firstBad += "\n  bits=" + std::to_string(bits) + " got=" + got + " want=" + want;
      }
    }
  }
  check(mismatches == 0,
        "double: " + std::to_string(total) + " values match to_chars (bad=" +
            std::to_string(mismatches) + ")" + firstBad);
  std::free(scratch);
}

static void test_random_floats() {
  FormatFn fn = emitFormat();
  void* scratch = allocScratch();
  std::mt19937_64 rng(424242);
  long mismatches = 0;
  long total = 0;
  std::string firstBad;
  for (long i = 0; i < 3000000; ++i) {
    std::uint32_t bits32 = static_cast<std::uint32_t>(rng());
    float f;
    std::memcpy(&f, &bits32, sizeof(f));
    if (f != f || f - f != 0) continue;
    // The runtime stores a float as a binary64 holding exactly a binary32 value.
    double v = static_cast<double>(f);
    std::string got = formatWith(fn, scratch, static_cast<std::uint64_t>(AotTag::Float), v);
    std::string want = reference32(f);
    ++total;
    if (got != want) {
      ++mismatches;
      if (mismatches <= 10) {
        firstBad += "\n  bits=" + std::to_string(bits32) + " got=" + got + " want=" + want;
      }
    }
  }
  check(mismatches == 0,
        "float: " + std::to_string(total) + " values match to_chars (bad=" +
            std::to_string(mismatches) + ")" + firstBad);
  std::free(scratch);
}

static void test_specials() {
  FormatFn fn = emitFormat();
  void* scratch = allocScratch();
  auto D = static_cast<std::uint64_t>(AotTag::Double);
  check(formatWith(fn, scratch, D, 0.0) == "0", "double 0 -> 0");
  check(formatWith(fn, scratch, D, -0.0) == "-0", "double -0 -> -0");
  check(formatWith(fn, scratch, D, 1.0) == "1", "double 1 -> 1");
  check(formatWith(fn, scratch, D, -1.5) == "-1.5", "double -1.5 -> -1.5");
  check(formatWith(fn, scratch, D, 1000000.0) == "1e+06", "double 1e6 -> 1e+06");
  check(formatWith(fn, scratch, D, 1e-4) == "1e-04", "double 1e-4 -> 1e-04");
  check(formatWith(fn, scratch, D, 10000.0) == "10000", "double 1e4 -> 10000");
  check(formatWith(fn, scratch, D, 123456789012345664.0) == "123456789012345664",
        "large integer keeps its exact digits");
  std::free(scratch);
}

#endif  // DTOA_EXEC_TESTS

int main() {
#if defined(DTOA_EXEC_TESTS)
  test_specials();
  test_random_doubles();
  test_random_floats();
#else
  std::printf("dtoa execution tests skipped (not Linux x86-64)\n");
#endif
  if (g_failures == 0) {
    std::printf("dtoa tests passed\n");
    return 0;
  }
  std::printf("%d dtoa test(s) failed\n", g_failures);
  return 1;
}
