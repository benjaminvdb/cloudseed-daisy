#pragma once

// Keep the pinned STM32 register layouts and bit definitions. Only replace
// the MMIO address, cache/barrier operations and free-running hardware clock.
#include "stm32h7xx_hal.h"

extern MDMA_Channel_TypeDef test_channel;
extern unsigned int test_barriers, test_clean_calls;
// The interrupt mask around the words the transport shares with its
// interrupt: counted, and checked to be balanced.
extern unsigned int test_irq_masked, test_irq_mask_calls;
#define MDMA_STAGING_IRQ_MASK() (++test_irq_mask_calls, ++test_irq_masked, 0u)
#define MDMA_STAGING_IRQ_RESTORE(mask) ((void)(mask), --test_irq_masked)
extern uintptr_t test_clean_address;
extern int32_t test_clean_bytes;

#undef MDMA_Channel0
#define MDMA_Channel0 (&test_channel)
#undef __DSB
#define __DSB() (++test_barriers)
#undef SCB_CleanDCache_by_Addr
#define SCB_CleanDCache_by_Addr(address, bytes)               \
  (++test_clean_calls,                                        \
   test_clean_address = reinterpret_cast<uintptr_t>(address), \
   test_clean_bytes = (bytes))
#undef __HAL_RCC_MDMA_CLK_ENABLE
#define __HAL_RCC_MDMA_CLK_ENABLE() ((void)0)

namespace daisy {
struct System {
  static uint32_t ticks;
  static unsigned int reads;
  static void (*on_tick)();
  static uint32_t GetTick() {
    ++reads;
    ticks += 100u;
    if (on_tick) on_tick();
    return ticks;
  }
  static uint32_t GetTickFreq() { return 1000000; }
};
}  // namespace daisy
