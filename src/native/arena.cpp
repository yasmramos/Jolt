#include "native/arena.h"

#include <cstdint>

namespace {

// Rounds `value` up to the next multiple of `alignment` (a power of two).
// Kept in unsigned arithmetic: the intermediate cannot overflow for any block
// a real process can map.
std::uintptr_t alignUp(std::uintptr_t value, std::uintptr_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

Arena::Arena(void* block, std::size_t size) noexcept {
  if (block == nullptr || size == 0) return;
  base_ = static_cast<std::uint8_t*>(block);
  cursor_ = base_;
  end_ = base_ + size;
}

void* Arena::allocate(std::size_t size, std::size_t alignment) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    return nullptr;
  }
  // The alignment is only meaningful if the region itself starts aligned;
  // the caller guarantees that (mmap and every test buffer are page- or at
  // least pointer-aligned). Align the cursor, not the size.
  const std::uintptr_t current = reinterpret_cast<std::uintptr_t>(cursor_);
  const std::uintptr_t aligned = alignUp(current, static_cast<std::uintptr_t>(alignment));
  // `size == 0` still consumes one aligned slot so two live allocations never
  // share an address (the runtime relies on distinct pointers for identity).
  const std::size_t actual = size == 0 ? 1 : size;
  if (aligned > reinterpret_cast<std::uintptr_t>(end_) ||
      actual > reinterpret_cast<std::uintptr_t>(end_) - aligned) {
    return nullptr;
  }
  cursor_ = reinterpret_cast<std::uint8_t*>(aligned) + actual;
  return reinterpret_cast<void*>(aligned);
}
