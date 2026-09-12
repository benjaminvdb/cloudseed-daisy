#pragma once

// System settings of the Daisy Seed (STM32H750) that the reverb's
// performance depends on, as free functions an application calls after
// libDaisy's DaisySeed::Init(): the 480 MHz clock where the silicon allows
// it, the cache policy of the internal SRAM and the SDRAM, and the SDRAM's
// refresh count and row timings, which libDaisy's driver programs wrongly
// for the Seed's part. None of them is specific to the reverb; the
// reference firmware's hardware class applies them for every firmware of
// its module. Everything here needs the STM32 HAL, so it is only compiled
// for the target. See README.md, "The Seed's system settings".

#ifdef STM32H750xx

#include <stdint.h>

#include "stm32h7xx_hal.h"

namespace cloudseed_daisy {

// Whether the silicon supports the 480 MHz clock (VOS0), for
// DaisySeed::Init(boost). Revision IDs of the STM32H750 (DBGMCU_IDCODE
// [31:16], stm32h7xx_hal.h): Y is 0x1003, B 0x2000, X 0x2001 and V 0x2003.
// ES0392 section 2.2.21 ("480 MHz maximum CPU frequency not available")
// limits revisions Y and W to 400 MHz and says to use revision V or X.
inline bool SupportsBoost() { return HAL_GetREVID() >= 0x2001; }

// Maps the SDRAM (libDaisy's MPU region 1) as write-back with write and
// read allocation (TEX 1, C 1, B 1; PM0253, "TEX, C, B encoding") instead of
// libDaisy's write-back without write allocation (TEX 0, C 1, B 1). A delay
// line that writes one sample at a time can then reuse a filled cache line
// and defer writes until eviction; the gain depends on cache misses. Only
// for firmwares whose SDRAM is used by the CPU alone (no DMA).
inline void ConfigureSdramWriteAllocate() {
  MPU_Region_InitTypeDef region = {};
  region.Enable = MPU_REGION_ENABLE;
  region.Number = MPU_REGION_NUMBER1;
  region.BaseAddress = 0xC0000000;
  region.Size = MPU_REGION_SIZE_64MB;
  region.SubRegionDisable = 0x00;
  region.TypeExtField = MPU_TEX_LEVEL1;
  region.AccessPermission = MPU_REGION_FULL_ACCESS;
  region.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  region.IsCacheable = MPU_ACCESS_CACHEABLE;
  region.IsBufferable = MPU_ACCESS_BUFFERABLE;
  SCB_CleanInvalidateDCache();
  HAL_MPU_Disable();
  HAL_MPU_ConfigRegion(&region);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

// Maps the internal SRAM (the 512 KB AXI SRAM and the cacheable part of the
// D2 SRAM) as write-back without write allocation (TEX 0, C 1, B 1, the
// encoding libDaisy uses for the SDRAM) instead of the Cortex-M7's default
// write-back with write allocation. A store that misses the cache then goes
// to the memory through the store buffer, which merges consecutive stores
// to a line into one bus burst, instead of first fetching the line from the
// memory: what a firmware wants for sample streams written once and read
// long after. libDaisy's regions 0 (D2 SRAM, first 32 KB, not cacheable, for
// its DMA buffers), 1 (SDRAM) and 2 (backup SRAM) stay; a region with a
// higher number wins where they overlap, so the D2 region leaves its first
// 32 KB (subregion 0 of eight) to region 0. Write-through is not used: on
// the STM32H750's Cortex-M7 (r1p1) it can return stale data (ES0392 section
// 2.1.1, Arm erratum 1259864).
inline void ConfigureSramNoWriteAllocate() {
  MPU_Region_InitTypeDef region = {};
  region.Enable = MPU_REGION_ENABLE;
  region.AccessPermission = MPU_REGION_FULL_ACCESS;
  region.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  region.IsCacheable = MPU_ACCESS_CACHEABLE;
  region.IsBufferable = MPU_ACCESS_BUFFERABLE;
  region.TypeExtField = MPU_TEX_LEVEL0;

  SCB_CleanInvalidateDCache();
  HAL_MPU_Disable();

  region.Number = MPU_REGION_NUMBER3;  // AXI SRAM, 512 KB
  region.BaseAddress = 0x24000000;
  region.Size = MPU_REGION_SIZE_512KB;
  region.SubRegionDisable = 0x00;
  HAL_MPU_ConfigRegion(&region);

  region.Number = MPU_REGION_NUMBER4;  // D2 SRAM 32 KB..256 KB
  region.BaseAddress = 0x30000000;
  region.Size = MPU_REGION_SIZE_256KB;
  region.SubRegionDisable = 0x01;  // the non-cacheable DMA window
  HAL_MPU_ConfigRegion(&region);

  region.Number = MPU_REGION_NUMBER5;  // D2 SRAM 256 KB..288 KB
  region.BaseAddress = 0x30040000;
  region.Size = MPU_REGION_SIZE_32KB;
  region.SubRegionDisable = 0x00;
  HAL_MPU_ConfigRegion(&region);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

// Reprograms the SDRAM's row and column timings. The Seed's SDRAM is an
// Alliance Memory AS4C16M32MSA-6 (166 MHz grade), clocked at 100 MHz by
// libDaisy (PLL2 at 200 MHz, FMC_SDRAM_CLOCK_PERIOD_2); the HAL's fields are
// SDRAM clock cycles of 10 ns. libDaisy's driver (src/dev/sdram.cpp)
// programs RCDDelay 10 and RPDelay 16 where the datasheet needs 18 ns each;
// ST's own STM32H7 examples for 100 MHz SDRAMs use 2 and 2. tRC is 60 ns,
// tRFC 80 ns, tRAS 48 ns, tWR 15 ns, tXSR 80 ns and tMRD 2 cycles
// (AS4C16M32MSA-6BIN, Rev. 1.0, pp. 17-18). RM0433's FMC_SDTR TRC must
// satisfy BOTH tRC and tRFC. TWR must also satisfy TRAS-TRCD and
// TRC-TRCD-TRP: max(2, 5-2, 8-2-2) = 4 cycles.
//
// datasheet true programs the part's own delays (test the memory
// afterwards, e.g. Engine::TestDelayMemory(), and fall back on failure);
// false keeps libDaisy's conservative row and column delays but meets the
// part's minimum tXSR and tRAS, which the driver cuts short (7 and 4 cycles
// where 8 and 5 are needed). Only while nothing uses the SDRAM.
inline void SetSdramTiming(bool datasheet) {
  FMC_SDRAM_TimingTypeDef timing;
  if (datasheet) {
    timing.LoadToActiveDelay = 2;     // tMRD
    timing.ExitSelfRefreshDelay = 8;  // tXSR
    timing.SelfRefreshTime = 5;       // tRAS
    timing.RowCycleDelay = 8;         // max(tRC, tRFC)
    timing.WriteRecoveryTime = 4;     // tWR and FMC timing constraints
    timing.RPDelay = 2;               // tRP
    timing.RCDDelay = 2;              // tRCD
  } else {
    timing.LoadToActiveDelay = 2;
    timing.ExitSelfRefreshDelay = 8;
    timing.SelfRefreshTime = 5;
    timing.RowCycleDelay = 8;
    timing.WriteRecoveryTime = 3;
    timing.RPDelay = 16;
    timing.RCDDelay = 10;
  }
  FMC_SDRAM_Timing_Init(FMC_SDRAM_DEVICE, &timing, FMC_SDRAM_BANK1);
}

// Corrects the SDRAM's refresh count for the Seed's AS4C16M32MSA at its
// 100 MHz SDCLK: 8192 refreshes per 64 ms with ST's 20-cycle reserve (RM0433
// FMC_SDRTR: floor(64 ms * SDCLK / 8192 rows) - 20 = 761). libDaisy's
// 0x81A - 20 refreshes every 20.5 us, 2.6 times too slowly for the part.
// After DaisySeed::Init(), before the SDRAM is used.
inline void ConfigureSdramRefresh() {
  constexpr uint32_t kRefreshCount = 6400000u / 8192u - 20u;
  FMC_SDRAM_ProgramRefreshRate(FMC_SDRAM_DEVICE, kRefreshCount);
}

}  // namespace cloudseed_daisy

#endif  // STM32H750xx
