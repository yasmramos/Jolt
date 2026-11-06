#pragma once

#include <cstddef>
#include <cstdint>

// Bump (region) allocator for the embedded AOT runtime (PHASE 7c).
//
// The generated binary has no libc and no `malloc`: every object the runtime
// creates (strings, arrays, maps, frames) is carved out of one contiguous
// block with a moving cursor. Freeing is all-or-nothing (`reset`), which is
// what a region model gives and what the AOT runtime needs -- reference
// counting and cycle collection are the hosted engine's concern, not the
// freestanding one.
//
// The arena never owns the block: the caller maps it (mmap in the generated
// binary, any buffer in tests) and keeps it alive. Allocation is O(1), has no
// per-object header, and only ever moves the cursor forward.
//
// Failure is reported as `nullptr`, never as an exception: this header is
// meant to compile into a binary that has no libstdc++ to throw from. It
// mirrors `Platform::alloc`, which has the same convention.
class Arena {
 public:
  // `block` is the start of the region, `size` its length in bytes. A null
  // block or a zero size yields an arena that fails every allocation.
  Arena(void* block, std::size_t size) noexcept;

  // Returns a pointer to `size` bytes aligned to `alignment` (a power of two,
  // at most 16), or `nullptr` when the region is exhausted.
  void* allocate(std::size_t size, std::size_t alignment = 8) noexcept;

  // Bytes handed out so far and the region's total size, used by tests and by
  // the runtime's diagnostics.
  std::size_t used() const noexcept { return static_cast<std::size_t>(cursor_ - base_); }
  std::size_t capacity() const noexcept { return static_cast<std::size_t>(end_ - base_); }

  // Reclaims the whole region. The block's contents are not zeroed, matching
  // a bump allocator that trusts its caller.
  void reset() noexcept { cursor_ = base_; }

 private:
  std::uint8_t* base_ = nullptr;
  std::uint8_t* cursor_ = nullptr;
  std::uint8_t* end_ = nullptr;
};
