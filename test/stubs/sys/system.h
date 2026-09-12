#pragma once
#include <cstdint>

namespace daisy {
// A deterministic clock for injecting callback execution time, not measuring
// host DSP speed. CpuLoadMeter itself is the real pinned libDaisy class.
class System {
 public:
  static uint32_t ticks;
  static uint32_t ticks_per_read;
  static uint32_t GetTick() {
    ticks += ticks_per_read;
    return ticks;
  }
  static uint32_t GetTickFreq() { return 1000000; }
  static uint32_t GetSysClkFreq() { return 480000000; }
  static uint32_t GetNow() { return ticks / 1000; }
  static void Delay(uint32_t) {}
};
}  // namespace daisy
