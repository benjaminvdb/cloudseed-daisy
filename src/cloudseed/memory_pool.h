#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cloudseed {

// Bump allocator over a caller-provided float array. The reverb's delay
// buffers are carved out of it at Init() time, so the firmware can place them
// in the Daisy Seed's SDRAM without any object touching that memory before
// the SDRAM is initialized. The same class serves the internal-RAM pools that
// ReverbController::PlaceBuffers() refills for every preset.
class MemoryPool {
 public:
  // Placed buffers start on a 32-byte boundary, the Cortex-M7 cache line.
  static constexpr size_t kAlignFloats = 8;

  void Init(float* base, size_t size) {
    base_ = base;
    size_ = size;
    used_ = 0;
    overflow_ = false;
  }

  // Returns nullptr (and remembers the overflow) when the pool is exhausted.
  float* Allocate(size_t count) {
    if (base_ == nullptr || count > size_ - used_) {
      overflow_ = true;
      return nullptr;
    }
    float* p = base_ + used_;
    used_ += count;
    return p;
  }

  // Whether AllocateAligned(count) would succeed.
  bool Fits(size_t count) const {
    if (base_ == nullptr || count > SIZE_MAX - (kAlignFloats - 1)) return false;
    const size_t pad = Padding();
    return pad <= size_ - used_ && Rounded(count) <= size_ - used_ - pad;
  }

  // Allocate() with the start rounded up to a cache line and the size to a
  // whole number of cache lines. Returns nullptr when the pool is exhausted;
  // *allocated receives the rounded size.
  float* AllocateAligned(size_t count, size_t* allocated) {
    if (!Fits(count)) {
      overflow_ = true;
      return nullptr;
    }
    const size_t pad = Padding();
    float* p = base_ + used_ + pad;
    *allocated = Rounded(count);
    used_ += pad + *allocated;
    return p;
  }

  // Forgets all allocations.
  void Reset() {
    used_ = 0;
    overflow_ = false;
  }

  size_t used() const { return used_; }
  const float* base() const { return base_; }
  size_t size() const { return size_; }
  bool overflow() const { return overflow_; }

 private:
  static size_t Rounded(size_t count) {
    return (count + kAlignFloats - 1) / kAlignFloats * kAlignFloats;
  }

  // Floats to skip so that the next allocation starts on a cache line.
  size_t Padding() const {
    const uintptr_t line = kAlignFloats * sizeof(float);
    const uintptr_t address = reinterpret_cast<uintptr_t>(base_ + used_);
    return static_cast<size_t>((line - address % line) % line) / sizeof(float);
  }

  float* base_ = nullptr;
  size_t size_ = 0;
  size_t used_ = 0;
  bool overflow_ = false;
};

}  // namespace cloudseed
