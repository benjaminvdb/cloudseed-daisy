#include "engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cloudseed/fast_sin.h"
#include "cloudseed/memory_pool.h"
#include "cloudseed/staging.h"
#include "cloudseed/utils.h"
#include "sys/system.h"
#ifdef STM32H750xx
#include "daisy_seed.h"
#if CLOUDSEED_STAGING == 1
#include "mdma_transport.h"
#endif
#endif

// Build options (see cloudseed.mk and README.md, "Build options").
#ifndef CLOUDSEED_AHBS_INITCOUNT
#define CLOUDSEED_AHBS_INITCOUNT 1  // Cortex-M7 AHBSCR.INITCOUNT (1: reset)
#endif
#ifndef CLOUDSEED_PLACEMENT
#define CLOUDSEED_PLACEMENT 1  // per-program delay memory in internal RAM
#endif
#ifndef CLOUDSEED_SAMPLE_RATE
#define CLOUDSEED_SAMPLE_RATE 48000  // the delay memory is sized for it
#endif
#ifndef CLOUDSEED_AXI_POOL_KB
#define CLOUDSEED_AXI_POOL_KB 448  // of the 512 KB AXI SRAM
#endif
#ifndef CLOUDSEED_D2_POOL_KB
#define CLOUDSEED_D2_POOL_KB 232  // of the 288 KB D2 SRAM
#endif
#ifndef CLOUDSEED_DTCM_STAGING_KB
#define CLOUDSEED_DTCM_STAGING_KB 24  // staging memory in the DTCM
#endif

// Memory placement on the Seed. The reverb object (filter state and block
// buffers) and the sine table live in the DTCM: zero wait states, not
// cached, so they do not compete with the delay memory for the 16 KB data
// cache. The per-program delay memory pools take most of the AXI SRAM and
// the D2 SRAM, and the worst-case delay memory stays in the SDRAM.
// The D2 pool's section name sorts it behind libDaisy's audio DMA buffers,
// which use the plain ".sram1_bss" name and must stay in the first 32 KB of
// the D2 SRAM, the part libDaisy's MPU setup makes non-cacheable.
#ifdef STM32H750xx
#define CLOUDSEED_DTCM CLOUDSEED_DAISY_DTCM
#define CLOUDSEED_SDRAM DSY_SDRAM_BSS
#define CLOUDSEED_D2_SRAM __attribute__((section(".sram1_bss.cloudseed_pool")))
// The MDMA's transfer list: also behind the DMA buffers (a plain
// ".sram1_bss" from this file would come first in the link and push them
// out of the non-cacheable window). Defined before the pool, so that it
// comes first.
#define CLOUDSEED_D2_SRAM_MDMA \
  __attribute__((section(".sram1_bss.cloudseed_mdma")))
#else
#define CLOUDSEED_DTCM
#define CLOUDSEED_SDRAM
#define CLOUDSEED_D2_SRAM
#define CLOUDSEED_D2_SRAM_MDMA
#endif

namespace cloudseed_daisy {

using cloudseed::ClampUnit;
using cloudseed::Parameter;
using cloudseed::ReverbController;

static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "Audio handoff requires lock-free atomics");

namespace {

// The delay memory of the reverb is sized for the build's sample rate.
constexpr size_t kPoolFloats =
    ReverbController::RequiredPoolFloats(CLOUDSEED_SAMPLE_RATE);

// Internal-RAM pools for the delay memory of the loaded program: the AXI
// SRAM, and the D2 SRAM behind libDaisy's audio DMA buffers and the MDMA's
// transfer list. Larger line counts can exceed these pools; buffers that do
// not fit stay in the SDRAM backing store.
constexpr size_t kAxiPoolFloats = CLOUDSEED_AXI_POOL_KB * 1024 / sizeof(float);
constexpr size_t kD2PoolFloats = CLOUDSEED_D2_POOL_KB * 1024 / sizeof(float);

// Staging memory (see cloudseed/staging.h): the whole ITCM, which nothing
// else uses, and this much of the DTCM after the reverb object; the rest of
// the DTCM is the stack. The first 32 bytes of the ITCM stay untouched so
// that no buffer starts at the null address.
constexpr size_t kDtcmStagingFloats =
    CLOUDSEED_DTCM_STAGING_KB * 1024 / sizeof(float);
constexpr uintptr_t kItcmBase = 0x00000000u;
constexpr size_t kItcmBytes = 64 * 1024;
constexpr size_t kItcmSkip = 32;

CLOUDSEED_DTCM ReverbController reverb_object;
cloudseed::MemoryPool memory;
float CLOUDSEED_SDRAM sdram_pool[kPoolFloats];
#if CLOUDSEED_PLACEMENT
alignas(32) float axi_pool[kAxiPoolFloats];
alignas(32) float CLOUDSEED_D2_SRAM d2_pool[kD2PoolFloats];
#endif

// The staging of the delay memory in the tightly coupled memories, and its
// transport: the MDMA on the Seed (its transfer list sits in the D2 SRAM
// with libDaisy's DMA buffers, outside the data cache; the transport object
// itself stays in cached memory, see mdma_transport.h), CPU copies on the
// host or when asked for, or the transport a host test names.
#if CLOUDSEED_STAGING
#if CLOUDSEED_STAGING == 1 && defined(STM32H750xx)
using Transport = MdmaStagingTransport;
CLOUDSEED_D2_SRAM_MDMA MdmaStagingTransport::Nodes mdma_nodes;
#elif defined(CLOUDSEED_HOST_TRANSPORT)
using Transport = CLOUDSEED_HOST_TRANSPORT;  // test/engine.cpp
#else
using Transport = cloudseed::CpuStagingTransport;
#endif
Transport staging_transport;
cloudseed::Staging<Transport> staging;
CLOUDSEED_DTCM float dtcm_staging[kDtcmStagingFloats];
#ifndef STM32H750xx
float itcm_staging[kItcmBytes / sizeof(float)];  // the host has no ITCM
#endif
#endif

}  // namespace

#ifndef STM32H750xx
uint32_t host_cycles = 0;  // the host's cycle counter: the tests advance it
#endif

namespace {

#ifdef STM32H750xx
inline uint32_t Cycles() { return DWT->CYCCNT; }
#else
inline uint32_t Cycles() { return host_cycles; }
#endif

#if defined(STM32H750xx) && CLOUDSEED_STAGING
// Enables the ITCM (CM7 ITCMCR, PM0253) and tests it, so that the staging
// can use its 64 KB as data memory; the Cortex-M7 allows data in either TCM
// (TRM, "TCM interfaces"). Returns false when a write is not read back.
bool EnableItcm() {
  SCB->ITCMCR |= SCB_ITCMCR_EN_Msk;
  __DSB();
  __ISB();
  // Through a volatile so that the compiler does not treat the constant
  // address as an object of no size.
  static volatile uintptr_t itcm_base = kItcmBase + kItcmSkip;
  volatile uint32_t* const words =
      reinterpret_cast<volatile uint32_t*>(itcm_base);
  const size_t count = (kItcmBytes - kItcmSkip) / sizeof(uint32_t);
  uint32_t state = 0x2545F491u;
  for (size_t i = 0; i < count; i++) {
    state = state * 1664525u + 1013904223u;
    words[i] = state;
  }
  __DSB();
  state = 0x2545F491u;
  bool ok = true;
  for (size_t i = 0; i < count; i++) {
    state = state * 1664525u + 1013904223u;
    ok &= words[i] == state;
  }
  for (size_t i = 0; i < count; i++) words[i] = 0;
  return ok;
}
#endif

#ifdef STM32H750xx
// libDaisy's start-up code clears .bss but not .dtcmram_bss. Clear it before
// the constructors of the objects placed there run (priority 101 comes
// before the default priority), as the start-up code does for .bss.
extern "C" char _sdtcmram_bss[], _edtcmram_bss[];
__attribute__((constructor(101))) void ZeroDtcmBss() {
  memset(_sdtcmram_bss, 0, static_cast<size_t>(_edtcmram_bss - _sdtcmram_bss));
}
#endif

#if CLOUDSEED_PROFILE
// The profiling build times the parts of the callback with the Cortex-M7's
// cycle counter (DWT CYCCNT, PM0253): the DSP marks its sections through
// CLOUDSEED_PROFILE_SECTION (see cloudseed/config.h), the engine and the
// application mark their own, and ProfileSection() attributes the cycles
// since the previous mark to the previous section. Once per second the
// totals, the load, the parameter updates and the control values go to the
// main loop, which prints them.
//
// Callback-owned accumulation, in the DTCM: the delay memory streams evict
// everything else from the data cache, so a mark's own counters would miss.
CLOUDSEED_DTCM uint32_t profile_last = 0;
CLOUDSEED_DTCM int profile_section = cloudseed::kProfileOther;
CLOUDSEED_DTCM ProfileReport profile_acc;
CLOUDSEED_DTCM float profile_load_sum = 0.f;
CLOUDSEED_DTCM float profile_load_max = 0.f;
CLOUDSEED_DTCM size_t profile_samples = 0;
// Handed to main once per second.
CLOUDSEED_DTCM ProfileReport profile_report;
// Producer never overwrites an unread report; USB may block main for seconds.
// Release/acquire on both handoffs protects the non-atomic payload.
std::atomic<unsigned int> profile_ready{0};
bool profile_dwt = false;
uint32_t profile_cycles_per_block = 1;
uint32_t profile_cycles_per_us = 1;
#if CLOUDSEED_STAGING == 1 && defined(STM32H750xx)
CLOUDSEED_DTCM uint32_t profile_list_clock = 0, profile_list_count = 0;
CLOUDSEED_DTCM uint32_t profile_list_late = 0, profile_list_stale = 0;
#endif

#ifdef STM32H750xx
extern "C" char _sidata[], _sdata[], _edata[];
// The image runs from the vector table at the start of the flash to the end
// of the initial values of .data, which is exactly what objcopy writes to
// the .bin, so this is the CRC-32 (IEEE 802.3, reflected, as zlib and gzip
// compute it) of that file: `make image-crc` prints it. Bit by bit without
// a table, the flash being full: 130 KB take some 10 ms before the audio
// starts. Only for a program in the internal flash (not the QSPI).
uint32_t ImageCrc32() {
  if (daisy::System::GetProgramMemoryRegion() !=
      daisy::System::MemoryRegion::INTERNAL_FLASH)
    return 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(FLASH_BASE);
  const unsigned char* end =
      reinterpret_cast<const unsigned char*>(_sidata) + (_edata - _sdata);
  uint32_t crc = 0xffffffffu;
  for (; p < end; p++) {
    crc ^= *p;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// The stack lives in the DTCM above the staging memory. At start-up the
// unused part is filled with a pattern; the bytes still holding it are the
// stack that was never needed.
constexpr uint32_t kStackPattern = 0x5AC3F00Du;
extern "C" char _estack[];
unsigned int StackFree() {
  const uint32_t* p = reinterpret_cast<const uint32_t*>(_edtcmram_bss);
  const uint32_t* const top = reinterpret_cast<const uint32_t*>(_estack);
  unsigned int free_bytes = 0;
  while (p < top && *p == kStackPattern) {
    p++;
    free_bytes += sizeof(uint32_t);
  }
  return free_bytes;
}
#else
unsigned int StackFree() { return 0; }
#endif

__attribute__((unused)) uint32_t ProfileCycles() { return Cycles(); }

void ProfileInit(float sample_rate, size_t block_size) {
#ifdef STM32H750xx
  // TRCENA enables the DWT, the M7 needs the lock access key first.
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  profile_dwt = (DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0;
#else
  profile_dwt = true;
#endif
  profile_cycles_per_block = static_cast<uint32_t>(
      static_cast<float>(daisy::System::GetSysClkFreq()) * block_size /
      sample_rate);
  profile_cycles_per_us = daisy::System::GetSysClkFreq() / 1000000u;
  if (profile_cycles_per_us == 0) profile_cycles_per_us = 1;
}

void ProfileBegin() {
  profile_last = Cycles();
  profile_section = cloudseed::kProfileOther;
}

// Section cycles of a report as permille of the block time.
unsigned int Permille(const ProfileReport& r, int section) {
  const uint64_t budget =
      static_cast<uint64_t>(r.blocks) * profile_cycles_per_block;
  return budget ? static_cast<unsigned int>(
                      static_cast<uint64_t>(r.cycles[section]) * 1000 / budget)
                : 0u;
}

// Formats and prints one piece of text (under 120 bytes: libDaisy's logger
// holds 128, and its PrintLine inspects the unbounded vsnprintf return
// before clamping it, so the application prints pieces with Print).
__attribute__((format(printf, 2, 3))) void Emit(PrintFn print,
                                                const char* format, ...) {
  char text[120];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  print(text);
}
#endif

}  // namespace

#if CLOUDSEED_PROFILE
#if CLOUDSEED_STAGING == 1 && defined(STM32H750xx)
extern "C" void MDMA_IRQHandler() { staging_transport.OnComplete(); }
#endif
#endif

bool Engine::TestDelayMemory() {
  uint32_t state = 0x9E3779B9u;
  for (size_t i = 0; i < kPoolFloats; i++) {
    state = state * 1664525u + 1013904223u;
    memcpy(&sdram_pool[i], &state, sizeof(state));
  }
#ifdef STM32H750xx
  // Read back from the SDRAM, not from the data cache.
  SCB_CleanInvalidateDCache();
#endif
  state = 0x9E3779B9u;
  bool ok = true;
  for (size_t i = 0; i < kPoolFloats; i++) {
    state = state * 1664525u + 1013904223u;
    uint32_t actual;
    memcpy(&actual, &sdram_pool[i], sizeof(actual));
    ok &= actual == state;
  }
  return ok;
}

void Engine::FillStack() {
#if CLOUDSEED_PROFILE && defined(STM32H750xx)
  uint32_t* p = reinterpret_cast<uint32_t*>(_edtcmram_bss);
  uint32_t* const top = reinterpret_cast<uint32_t*>(__get_MSP()) - 64;
  while (p < top) *p++ = kStackPattern;
#endif
}

ReverbController& Engine::reverb() { return reverb_object; }

int Engine::line_count() const { return cloudseed_daisy::reverb_object.line_count(); }

int Engine::line_limit(int program) const {
  return program >= 0 && program < config_.program_count ? line_limits_[program]
                                                         : 0;
}

bool Engine::Init(const Config& config) {
  config_ = config;
  if (config_.programs == nullptr || config_.program_count < 1 ||
      config_.program_count > kMaxPrograms)
    return false;
  if (config_.sample_rate <= 0.f || config_.block_size == 0) return false;
  switch_fade_step_ = 1.f / (config_.fade_seconds * config_.sample_rate);
  if (!(switch_fade_step_ > 0.f)) switch_fade_step_ = 1.f;

  // Nothing may touch the pool before the SDRAM is initialized.
  cloudseed::FastSin::Init();
  memory.Init(sdram_pool, kPoolFloats);
  if (!reverb_object.Init(static_cast<int>(config_.sample_rate), memory)) return false;

  // The internal-RAM pools, in which the reverb places the delay memory of
  // every program (see ReverbController::AddFastPool).
#if CLOUDSEED_PLACEMENT
  reverb_object.AddFastPool(axi_pool, kAxiPoolFloats);
#ifdef STM32H750xx
  {
    // libDaisy's MPU region for the audio DMA buffers makes the first 32 KB
    // of the D2 SRAM non-cacheable; delay memory must stay out of it.
    constexpr uintptr_t kD2Cacheable = 0x30008000u;
    uintptr_t start = reinterpret_cast<uintptr_t>(d2_pool);
    const uintptr_t end = start + sizeof(d2_pool);
    if (start < kD2Cacheable) start = kD2Cacheable;
    if (start < end) {
      reverb_object.AddFastPool(reinterpret_cast<float*>(start),
                         (end - start) / sizeof(float));
    }
  }
#else
  reverb_object.AddFastPool(d2_pool, kD2PoolFloats);
#endif
#endif

  // The staging: its transport and its memory.
  staging_enabled_ = CLOUDSEED_STAGING != 0;
  staging_failures_ = 0;
  itcm_state_ = 0;
#if CLOUDSEED_STAGING
  staging.Init(&staging_transport);
#ifdef STM32H750xx
  itcm_state_ = EnableItcm() ? 1 : 2;
  if (itcm_state_ == 1) {
    staging.AddMemory(reinterpret_cast<float*>(kItcmBase + kItcmSkip),
                      (kItcmBytes - kItcmSkip) / sizeof(float));
  }
#if CLOUDSEED_STAGING == 1
  if (!staging_transport.Init(&mdma_nodes)) staging_enabled_ = false;
#if CLOUDSEED_PROFILE
  // The profiling build times every list from its start to the channel's
  // transfer-complete interrupt, below the audio interrupt's priority.
  staging_transport.SetClock(ProfileCycles);
  HAL_NVIC_SetPriority(MDMA_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(MDMA_IRQn);
#endif
#endif
#else
  itcm_state_ = 1;
  staging.AddMemory(itcm_staging, sizeof(itcm_staging) / sizeof(float));
#endif
  staging.AddMemory(dtcm_staging, kDtcmStagingFloats);
#endif

  for (int i = 0; i < config_.program_count; i++) {
    int lines = config_.programs[i].lines;
    if (lines < 1) lines = 1;
    if (lines > cloudseed::kMaxLines) lines = cloudseed::kMaxLines;
    line_limits_[i] = lines;
  }
  current_program_ = -1;
  state_.store(State::kRunning, std::memory_order_relaxed);
  requested_program_.store(0, std::memory_order_relaxed);
  status_.store(0, std::memory_order_relaxed);
  block_open_ = processed_ = false;
  frozen_ = false;
  switch_gain_ = 1.f;
  updates_ = 0;
  overload_samples_ = 0;
  overload_latch_ = false;
  ResetLastValues();

  cpu_load_meter_.Init(config_.sample_rate,
                       static_cast<int>(config_.block_size));
#if CLOUDSEED_PROFILE
  ProfileInit(config_.sample_rate, config_.block_size);
  profile_acc = ProfileReport();
  profile_load_sum = profile_load_max = 0.f;
  profile_samples = 0;
  profile_ready.store(0, std::memory_order_relaxed);
#ifdef STM32H750xx
  image_crc_ = ImageCrc32();
#endif
  printed_program_ = printed_limit_ = -1;
  profile_control_count_ = 0;
#endif

#if defined(STM32H750xx) && CLOUDSEED_AHBS_INITCOUNT != 1
  // The MDMA writes the staging memory while the callback reads it: the
  // fairness counter decides how often the AHB slave port wins a contended
  // TCM cycle (Cortex-M7 TRM, CM7_AHBSCR; 1 alternates, the reset value).
  SCB->AHBSCR = (SCB->AHBSCR & ~SCB_AHBSCR_INITCOUNT_Msk) |
                ((CLOUDSEED_AHBS_INITCOUNT) << SCB_AHBSCR_INITCOUNT_Pos);
#endif
  return true;
}

bool Engine::Start(int program) {
  if (program < 0) program = 0;
  if (program >= config_.program_count) program = config_.program_count - 1;
  switch_gain_ = 1.f;
  const bool loaded = LoadProgram(program);
  requested_program_.store(current_program_, std::memory_order_relaxed);
  if (!loaded) {
    switch_gain_ = 0.f;
    state_.store(State::kStagingFault, std::memory_order_release);
    return false;
  }
  state_.store(State::kRunning, std::memory_order_release);
  return true;
}

void Engine::DisableStaging() {
  if (staging_enabled_) {
    staging_enabled_ = false;
    staging_failures_++;
  }
}

void Engine::ResetLastValues() {
  for (int i = 0; i < cloudseed::kParameterCount; i++)
    has_last_value_[i] = false;
}

// Loads a program. Only called while the audio callback leaves the reverb
// alone.
bool Engine::LoadProgram(int program) {
#if CLOUDSEED_STAGING
  if (!staging.Unplan()) return false;
  if (staging.failed()) DisableStaging();
#ifdef STM32H750xx
  // The transport has relinquished the rings. Discard stale cached reads
  // before the CPU reuses their memory; preserve unrelated dirty CPU data.
  SCB_CleanInvalidateDCache();
#endif
#endif
  current_program_ = program;
  reverb_object.LoadPreset(config_.programs[program].preset->values);
  if (config_.on_program_loaded != nullptr)
    config_.on_program_loaded(reverb_object, config_.context);
  if (reverb_object.line_count() > line_limits_[program]) {
    reverb_object.SetParameter(Parameter::LineCount,
                        double(line_limits_[program] - 1) /
                            (cloudseed::kPluginLineCount - 1));
    // Fewer lines: place the delay memory of the lines that remain.
    reverb_object.PlaceBuffers();
  }
  reverb_object.ClearBuffers();
#if CLOUDSEED_STAGING
#ifdef STM32H750xx
  // The zeroed rings must reach the memory before the transport reads them.
  SCB_CleanDCache();
#endif
  if (staging_enabled_ && !staging.Plan(reverb_object)) {
    DisableStaging();
    return false;
  }
#endif
  ResetLastValues();
  return true;
}

// Called by main only. Switching, overload and staging fault states exclude
// the callback from the reverb_object. Remember reductions per program, so
// revisiting a heavy program does not retry a workload that already
// exceeded the budget.
void Engine::Service() {
  const State current_state = state_.load(std::memory_order_acquire);
  if (current_state != State::kSwitching &&
      current_state != State::kOverloaded &&
      current_state != State::kStagingFault)
    return;

#if CLOUDSEED_STAGING
  // Even an aborted transfer may still own its destination. Keep rendering
  // dry audio until it has stopped, then reload clean DSP state on the CPU.
  if (!staging.Unplan()) {
    DisableStaging();
    state_.store(State::kStagingFault, std::memory_order_release);
    return;
  }
#endif

  if (current_state == State::kOverloaded && current_program_ >= 0)
    line_limits_[current_program_] = reverb_object.line_count() - 1;

  const int program = requested_program_.load(std::memory_order_relaxed);
  if (line_limits_[program] == 0) {
    current_program_ = program;
    state_.store(State::kBypassed, std::memory_order_release);
  } else {
    const bool loaded = LoadProgram(program);
    state_.store(loaded ? State::kFadingIn : State::kStagingFault,
                 std::memory_order_release);
  }
}

void Engine::BeginBlock() {
  if (block_open_) EndBlock();
  block_open_ = true;
  processed_ = false;
  block_samples_ = 0;
  updates_ = 0;
  cpu_load_meter_.OnBlockStart();
  // Measure this block, not a historical peak; recovery must act on the
  // first expensive callback, before sustained IRQ load can starve main.
  cpu_load_meter_.Reset();
#ifdef STM32H750xx
  // Set FZ in the actual interrupt context (PM0253, FPSCR[24]). The input
  // noise gate cannot prevent subnormals inside long feedback/filter tails.
  __set_FPSCR(__get_FPSCR() | (1u << 24));
#endif
#if CLOUDSEED_PROFILE
  ProfileBegin();
  cloudseed::ProfileSection(cloudseed::kProfileControls);
#endif
}

void Engine::RequestProgram(int program) {
  if (program < 0) program = 0;
  if (program >= config_.program_count) program = config_.program_count - 1;
  requested_program_.store(program, std::memory_order_relaxed);
  const State current_state = state_.load(std::memory_order_acquire);
  if ((current_state == State::kRunning ||
       current_state == State::kFadingIn ||
       current_state == State::kBypassed) &&
      program != current_program_) {
    // The selector must work even if main has not run since the last block.
    state_.store(current_state == State::kBypassed ? State::kSwitching
                                                   : State::kFadingOut,
                 std::memory_order_release);
  }
}

bool Engine::SetParameter(Parameter parameter, double value, float threshold) {
  if (!CallbackOwns(state_.load(std::memory_order_acquire))) return false;
  const int index = static_cast<int>(parameter);
  if (index < 0 || index >= cloudseed::kParameterCount) return false;
  if (has_last_value_[index] &&
      fabs(value - last_value_[index]) <= static_cast<double>(threshold))
    return false;
#if CLOUDSEED_PROFILE
  const int caller = cloudseed::ProfileSection(cloudseed::kProfileParameters);
#endif
  reverb_object.SetParameter(parameter, value);
  last_value_[index] = value;
  has_last_value_[index] = true;
  updates_++;
#if CLOUDSEED_PROFILE
  cloudseed::ProfileSection(caller);
#endif
  return true;
}

void Engine::SetFrozen(bool frozen) {
  frozen_ = frozen;
  if (!CallbackOwns(state_.load(std::memory_order_acquire))) return;
  if (reverb_object.frozen() != frozen) reverb_object.SetFrozen(frozen);
}

bool Engine::Process(const float* in_l, const float* in_r, float* wet_l,
                     float* wet_r, size_t size) {
  if (!block_open_) BeginBlock();
  block_samples_ = size;
  const State current_state = state_.load(std::memory_order_acquire);
  if (!CallbackOwns(current_state)) return false;

  const float switch_step = current_state == State::kFadingOut
                                ? -switch_fade_step_
                            : current_state == State::kFadingIn
                                ? switch_fade_step_
                                : 0.f;
  // The reverb processes at most kMaxBlockSize samples at a time.
  for (size_t offset = 0; offset < size; offset += cloudseed::kMaxBlockSize) {
    size_t count = size - offset;
    if (count > static_cast<size_t>(cloudseed::kMaxBlockSize))
      count = cloudseed::kMaxBlockSize;
#if CLOUDSEED_STAGING
    const bool windows_ready = staging.Begin(static_cast<int>(count));
    CLOUDSEED_PROFILE_SECTION(cloudseed::kProfileOther);
#else
    const bool windows_ready = true;
#endif
    bool block_valid = windows_ready;
    if (windows_ready)
      reverb_object.Process(in_l + offset, in_r + offset, wet_l + offset,
                     wet_r + offset, static_cast<int>(count));
#if CLOUDSEED_STAGING
    if (windows_ready) block_valid = staging.End();
#endif
    if (!block_valid) {
      // Do not consume incomplete windows or preserve a possibly damaged
      // feedback history. Main waits for the transport and reloads. This
      // fault is distinct from CPU overload and does not reduce line count.
      DisableStaging();
      switch_gain_ = 0.f;
      state_.store(State::kStagingFault, std::memory_order_release);
      return false;
    }
  }

  // The program fade. No float compare in the loop (each costs a flag
  // transfer that waits for it, TECHNICAL.md, "DSP kernels"): the fade is a
  // step of zero when idle, and its clamp compares the value's bits.
  CLOUDSEED_PROFILE_SECTION(cloudseed::kProfileMix);
  float gain = switch_gain_;
  for (size_t i = 0; i < size; i++) {
    gain = ClampUnit(gain + switch_step);
    wet_l[i] *= gain;
    wet_r[i] *= gain;
  }
  switch_gain_ = gain;
  CLOUDSEED_PROFILE_SECTION(cloudseed::kProfileOther);

  if (current_state == State::kFadingOut && switch_gain_ <= 0.f) {
    state_.store(State::kSwitching, std::memory_order_release);
  } else if (current_state == State::kFadingIn && switch_gain_ >= 1.f) {
    state_.store(State::kRunning, std::memory_order_release);
  }
  processed_ = true;
  return true;
}

void Engine::EndBlock() {
  if (!block_open_) return;
  block_open_ = false;
  cpu_load_meter_.OnBlockEnd();
  const float load = cpu_load_meter_.GetMaxCpuLoad();
  const bool over_budget = processed_ && load > config_.cpu_budget;
  const size_t samples = block_samples_ ? block_samples_ : config_.block_size;
#if CLOUDSEED_PROFILE
  {
    cloudseed::ProfileSection(cloudseed::kProfileOther);
    const int program = processed_ ? current_program_ : -1;
    const int lines = processed_ ? reverb_object.line_count() : 0;
    if (profile_acc.blocks == 0) {
      profile_acc.program = program;
      profile_acc.lines = lines;
      profile_acc.frozen = frozen_;
    } else if (profile_acc.program != program || profile_acc.lines != lines ||
               profile_acc.frozen != frozen_) {
      profile_acc.mixed = true;
    }
    profile_acc.blocks++;
    if (processed_) {
      profile_acc.reverb_blocks++;
      profile_load_sum += load;
      if (load > profile_load_max) profile_load_max = load;
    }
    profile_acc.updates += updates_;
    if (over_budget) profile_acc.overloads++;
    profile_samples += samples;
    if (profile_samples >= static_cast<size_t>(config_.sample_rate)) {
      profile_acc.avg_permille =
          profile_acc.reverb_blocks
              ? static_cast<unsigned int>(profile_load_sum /
                                              profile_acc.reverb_blocks *
                                              1000.f +
                                          0.5f)
              : 0u;
      profile_acc.max_permille =
          static_cast<unsigned int>(profile_load_max * 1000.f + 0.5f);
      profile_acc.frozen = frozen_;
      profile_acc.control_count = profile_control_count_;
      for (int i = 0; i < profile_control_count_; i++)
        profile_acc.controls[i] =
            static_cast<unsigned int>(profile_controls_[i] * 1000.f + 0.5f);
#if CLOUDSEED_STAGING
      profile_acc.copies =
          static_cast<unsigned int>(staging.copies_last_block());
#endif
      profile_acc.staging_failures = staging_failures_;
#if CLOUDSEED_STAGING == 1 && defined(STM32H750xx)
      profile_acc.mdma_lists = staging_transport.lists();
      profile_acc.mdma_errors = staging_transport.errors();
      profile_acc.mdma_status = staging_transport.error_status();
      profile_acc.mdma_maxpolls = staging_transport.max_polls();
      {
        // The blocks' lists (their segments added up) timed since the last
        // interval (the counters run on).
        const uint32_t clock = staging_transport.block_clock_sum();
        const uint32_t count = staging_transport.timed_blocks();
        const uint32_t timed = count - profile_list_count;
        profile_acc.mdma_timed = timed;
        profile_acc.mdma_list_avg_us =
            timed ? (clock - profile_list_clock) / timed / profile_cycles_per_us
                  : 0u;
        profile_acc.mdma_list_max_us =
            staging_transport.block_clock_max() / profile_cycles_per_us;
        staging_transport.ResetBlockClockMax();
        profile_list_clock = clock;
        profile_list_count = count;
        const uint32_t late = staging_transport.late_lists();
        const uint32_t stale = staging_transport.stale_interrupts();
        profile_acc.mdma_late = late - profile_list_late;
        profile_acc.mdma_stale = stale - profile_list_stale;
        profile_list_late = late;
        profile_list_stale = stale;
      }
#endif
      if (profile_ready.load(std::memory_order_acquire) == 0) {
        profile_report = profile_acc;
        profile_ready.store(1, std::memory_order_release);
      }  // Drop this interval if main is still holding the previous one.
      profile_acc = ProfileReport();
      profile_load_sum = profile_load_max = 0.f;
      profile_samples = 0;
    }
  }
#endif
  if (over_budget) {
    // Do not fade through another expensive block. The next callback takes
    // the dry path, allowing pending DMA work to drain and main to reload
    // safely.
    switch_gain_ = 0.f;
    state_.store(State::kOverloaded, std::memory_order_release);
    overload_latch_ = true;
    overload_samples_ = 0;
  }
  overload_samples_ += samples;
  if (overload_samples_ >= static_cast<size_t>(config_.sample_rate)) {
    overload_latch_ = false;
    overload_samples_ = 0;
  }
  const State after = state_.load(std::memory_order_relaxed);
  status_.store((frozen_ ? kStatusFrozen : 0u) |
                    (overload_latch_ || after == State::kBypassed ||
                             after == State::kStagingFault
                         ? kStatusOverloaded
                         : 0u),
                std::memory_order_relaxed);
}

#if CLOUDSEED_PROFILE
void Engine::SetProfileControls(const float* values, int count) {
  if (count > ProfileReport::kMaxControls) count = ProfileReport::kMaxControls;
  if (count < 0) count = 0;
  for (int i = 0; i < count; i++) profile_controls_[i] = values[i];
  profile_control_count_ = count;
}

bool Engine::TakeReport(ProfileReport* report) {
  if (profile_ready.load(std::memory_order_acquire) == 0) return false;
  *report = profile_report;
  profile_ready.store(0, std::memory_order_release);
  // Scanning the unused stack can touch tens of kilobytes. Do it in main,
  // outside the audio deadline, after taking the fixed-size ISR snapshot.
  report->stack_free = StackFree();
  return true;
}

void Engine::PrintBuild(PrintFn print) {
  unsigned int revision = 0;
#ifdef STM32H750xx
  revision = static_cast<unsigned int>(HAL_GetREVID());
#endif
  Emit(print,
       "build maxlines=%d placement=%d staging=%d ahbs=%d dtcm_staging=%dKB "
       "sample_rate=%d",
       cloudseed::kMaxLines, CLOUDSEED_PLACEMENT, CLOUDSEED_STAGING,
       CLOUDSEED_AHBS_INITCOUNT, CLOUDSEED_DTCM_STAGING_KB,
       CLOUDSEED_SAMPLE_RATE);
  Emit(print, " clock=%uMHz rev=0x%04x cyclecounter=%d image=%08lx\r\n",
       static_cast<unsigned int>(daisy::System::GetSysClkFreq() / 1000000u),
       revision, profile_dwt ? 1 : 0, static_cast<unsigned long>(image_crc_));
}

void Engine::PrintProgramIfChanged(PrintFn print) {
  const int program = current_program_;
  if (program < 0) return;
  const int limit = line_limits_[program];
  if (program == printed_program_ && limit == printed_limit_) return;
  printed_program_ = program;
  printed_limit_ = limit;
  const cloudseed::Workload w = reverb_object.GetWorkload();
  Emit(print, "program %d \"%s\" lines=%d limit=%d early=%d late=%d taps=%d ",
       program, config_.programs[program].preset->name, w.lines, limit,
       w.early_stages, w.late_stages, w.taps);
  Emit(print, "predelay=%d image=%08lx\r\n", w.predelay,
       static_cast<unsigned long>(image_crc_));
  Emit(print, "  mod=%c%c%c interp=%d filters=%c%c%c%c%c latetap=%d ",
       w.early_mod ? 'e' : '-', w.line_mod ? 'l' : '-', w.late_mod ? 'd' : '-',
       w.interpolation ? 1 : 0, w.high_pass ? 'h' : '-',
       w.low_pass ? 'l' : '-', w.low_shelf ? 'L' : '-',
       w.high_shelf ? 'H' : '-', w.cutoff ? 'c' : '-', w.late_tap ? 1 : 0);
  const unsigned int placed_axi = static_cast<unsigned int>(
      reverb_object.fast_pool_used(0) * sizeof(float) / 1024);
  const unsigned int placed_d2 = static_cast<unsigned int>(
      reverb_object.fast_pool_used(1) * sizeof(float) / 1024);
  Emit(print, "streams=%d sdram=%d placed=%u+%uKB unplaced=%d clock=%uMHz\r\n",
       w.streams, w.slow_streams, placed_axi, placed_d2,
       reverb_object.unplaced_buffers(),
       static_cast<unsigned int>(daisy::System::GetSysClkFreq() / 1000000u));
#if CLOUDSEED_STAGING
  // The staging: rings served from the TCM (and taps of the multitaps), the
  // rings that stayed with their memory, the staging memory in use.
  Emit(print, "  staging=%s staged=%d unstaged=%d taps=%d copies<=%d ",
       !staging_enabled_        ? "off"
       : CLOUDSEED_STAGING == 1 ? "mdma"
                                : "cpu",
       staging.staged_buffers(), staging.unstaged_buffers(),
       staging.staged_taps(), staging.planned_copies());
  const unsigned int tcm_used =
      static_cast<unsigned int>(staging.memory_used() * sizeof(float) / 1024);
  const unsigned int tcm_size =
      static_cast<unsigned int>(staging.memory_size() * sizeof(float) / 1024);
  Emit(print, "tcm=%u/%uKB itcm=%s\r\n", tcm_used, tcm_size,
       itcm_state_ == 1   ? "ok"
       : itcm_state_ == 2 ? "FAILED"
                          : "unused");
#endif
}

void Engine::PrintReport(const ProfileReport& r, PrintFn print) {
  static const char* const kNames[cloudseed::kProfileSectionCount] = {
      "other",   "input",     "predelay", "taps",     "early",
      "linemix", "linedelay", "linediff", "linefilt", "out",
      "ctrl",    "param",     "clip",     "stage",    "wait"};
  Emit(print, "load program=%d lines=%d mixed=%d avg=%u.%u%% max=%u.%u%% |",
       r.program, r.lines, r.mixed ? 1 : 0, r.avg_permille / 10,
       r.avg_permille % 10, r.max_permille / 10, r.max_permille % 10);
  for (int s = 1; s < cloudseed::kProfileSectionCount; s++) {
    const unsigned int p = Permille(r, s);
    Emit(print, " %s=%u.%u", kNames[s], p / 10, p % 10);
  }
  const unsigned int other = Permille(r, cloudseed::kProfileOther);
  Emit(print, " other=%u.%u | freeze=%d updates=%u overloads=%u blocks=%u/%u%s\r\n",
       other / 10, other % 10, r.frozen ? 1 : 0, r.updates, r.overloads,
       static_cast<unsigned int>(r.reverb_blocks),
       static_cast<unsigned int>(r.blocks),
       profile_dwt ? "" : " (no cycle counter)");
  if (r.control_count > 0) {
    // The values the application handed in with SetProfileControls(), as
    // of the interval's last callback.
    Emit(print, "controls=");
    for (int i = 0; i < r.control_count; i++)
      Emit(print, "%s%u", i ? " " : "", r.controls[i]);
    Emit(print, "\r\n");
  }
  // The staging's transport per block, its failures, and the stack margin.
  Emit(print, "staging copies=%u failures=%u stack_free=%u", r.copies,
       r.staging_failures, r.stack_free);
#if CLOUDSEED_STAGING == 1 && defined(STM32H750xx)
  Emit(print, " segments=%lu mdma_errors=%lu mdma_status=0x%lx mdma_maxpolls=%lu",
       static_cast<unsigned long>(r.mdma_lists),
       static_cast<unsigned long>(r.mdma_errors),
       static_cast<unsigned long>(r.mdma_status),
       static_cast<unsigned long>(r.mdma_maxpolls));
  // The lists of the interval: mean and longest, from the start of the
  // list to its transfer-complete interrupt; the lists the next block had
  // to wait for; the interrupts that found their list handled by a wait.
  Emit(print, " list=%lu/%lu us timed=%lu late=%lu stale=%lu",
       static_cast<unsigned long>(r.mdma_list_avg_us),
       static_cast<unsigned long>(r.mdma_list_max_us),
       static_cast<unsigned long>(r.mdma_timed),
       static_cast<unsigned long>(r.mdma_late),
       static_cast<unsigned long>(r.mdma_stale));
#endif
  Emit(print, "\r\n");
}
#endif  // CLOUDSEED_PROFILE

}  // namespace cloudseed_daisy

#if CLOUDSEED_PROFILE
int cloudseed::ProfileSection(int section) {
  using namespace cloudseed_daisy;
  const uint32_t now = Cycles();
  profile_acc.cycles[profile_section] += now - profile_last;
  profile_last = now;
  const int previous = profile_section;
  profile_section = section;
  return previous;
}
#endif
