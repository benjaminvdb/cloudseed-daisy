#pragma once

// The Cloud Seed reverb on the Daisy Seed: one object that owns the reverb,
// its delay memory in every memory of the STM32H750 (SDRAM, AXI SRAM, D2
// SRAM, the tightly coupled memories), the staging of that memory through
// the MDMA, the loading of programs outside the audio interrupt, and the
// recovery from CPU overload and transport faults. The application maps its
// controls onto it and mixes the wet signal it renders. See README.md for
// the usage and TECHNICAL.md for what it does and why.
//
//   cloudseed_daisy::Engine engine;   // place it in the DTCM, see below
//
//   void AudioCallback(in, out, size) {
//     engine.BeginBlock();
//     engine.RequestProgram(program);            // every block
//     engine.SetParameter(Parameter::LineDecay, decay, 0.001f);
//     engine.SetFrozen(gate);
//     const bool wet = engine.Process(in[0], in[1], wet_l, wet_r, size);
//     ... mix wet_l/wet_r with the dry input, or pass the dry input alone
//         when wet is false ...
//     engine.EndBlock();
//   }
//
//   int main() {
//     seed.Init(true);
//     Engine::Config config; config.programs = ...; ...
//     if (!engine.Init(config)) ...;
//     engine.Start(0);
//     seed.StartAudio(AudioCallback);
//     for (;;) { engine.Service(); ... }
//   }
//
// The callback and the main loop share the reverb through one lock-free
// state (State): the callback owns it while it runs, main owns it while it
// loads a program or recovers, and the callback fades the wet signal out
// before handing over and back in afterwards. Everything the application
// calls from the callback is documented as such; everything else belongs
// to the main loop.

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "cloudseed/config.h"
#include "cloudseed/parameter.h"
#include "cloudseed/presets.h"
#include "cloudseed/reverb_controller.h"
#include "util/CpuLoadMeter.h"

// Build options (see cloudseed.mk and README.md, "Build options").
#ifndef CLOUDSEED_PROFILE
#define CLOUDSEED_PROFILE 0  // log the CPU load over USB serial
#endif
#ifndef CLOUDSEED_STAGING
#define CLOUDSEED_STAGING 1  // 0 off, 1 the MDMA stages the delay memory, 2 the CPU
#endif

// Section attribute for the engine object (and anything else the audio
// callback touches every block): the DTCM is neither cached nor subject to
// wait states, so the callback's own state never competes with the delay
// memory for the data cache. libDaisy's start-up code does not clear that
// section; the engine does, before any object placed there is constructed.
#ifdef STM32H750xx
#define CLOUDSEED_DAISY_DTCM __attribute__((section(".dtcmram_bss")))
#else
#define CLOUDSEED_DAISY_DTCM
#endif

namespace cloudseed_daisy {

// A program: a preset and the number of late lines per channel it runs with
// (1 to cloudseed::kMaxLines; the build's cap applies, and the overload
// recovery may reduce it further at run time).
struct Program {
  const cloudseed::presets::Preset* preset;
  int lines;
};

// Who owns the reverb: the audio callback, or the main loop while it loads a
// program or recovers.
enum class State : unsigned int {
  kRunning,       // the callback renders the reverb
  kFadingOut,     // another program was requested; the wet signal fades out
  kSwitching,     // main loads the requested program
  kFadingIn,      // loaded; the wet signal fades back in
  kOverloaded,    // a block exceeded the CPU budget; main reduces the lines
  kStagingFault,  // the transport failed; main reloads onto the CPU paths
  kBypassed       // the program exceeded the budget at one line: dry only
};

// A sink for the engine's diagnostic text (profiling builds): called with
// pieces of at most 120 bytes, a line ending in "\r\n" spread over several
// calls. Print them as they come, e.g. seed.Print("%s", text).
using PrintFn = void (*)(const char* text);

#if CLOUDSEED_PROFILE
// One second of the profiling build's measurements (see README.md,
// "Measuring the load").
struct ProfileReport {
  static constexpr int kMaxControls = 12;
  uint32_t cycles[cloudseed::kProfileSectionCount];
  uint32_t blocks;                          // all blocks of the second
  uint32_t reverb_blocks;                   // blocks in which the reverb ran
  unsigned int avg_permille, max_permille;  // load of the reverb blocks
  unsigned int updates;                     // parameter updates applied
  unsigned int overloads;                   // budget breaches
  bool frozen;
  int program, lines;
  bool mixed;  // program, line count or freeze changed during the interval
  unsigned int controls[kMaxControls];  // SetProfileControls(), 0..1000
  int control_count;
  unsigned int copies;  // staging copies of the last block
  unsigned int staging_failures;
  // The MDMA transport (zero without it): segments started, errors, the
  // error status register, the most polls in a wait; and the lists of the
  // interval: how many were timed, their mean and longest duration in
  // microseconds, how many were still running when the next block waited
  // for them, how often the transfer-complete interrupt found its list
  // already handled by that wait.
  uint32_t mdma_lists, mdma_errors, mdma_status, mdma_maxpolls;
  uint32_t mdma_timed, mdma_list_avg_us, mdma_list_max_us;
  uint32_t mdma_late, mdma_stale;
  unsigned int stack_free;  // bytes of stack never written since start-up
};
#endif

class Engine {
 public:
  // One Engine per application: the DSP, pools and transport are shared
  // statically. Call Init once, before audio starts.
  static constexpr int kMaxPrograms = 32;

  struct Config {
    // The programs, in the order the application numbers them.
    const Program* programs = nullptr;
    int program_count = 0;
    // The audio configuration of the Seed. The delay memory is sized for
    // CLOUDSEED_SAMPLE_RATE at build time; Init() requires a positive whole
    // number of Hz no higher than that rate.
    // Process() renders in chunks of cloudseed::kMaxBlockSize (48) samples:
    // use that block size, or a divisor or multiple of it, fixed for every
    // callback. Its cycle period must fit a 32-bit counter.
    float sample_rate = 48000.f;
    size_t block_size = cloudseed::kMaxBlockSize;
    // The wet signal fades out and back in around a program change.
    // Finite and nonnegative; zero switches without a fade. Init rejects a
    // duration whose per-sample float step cannot move unity toward zero.
    float fade_seconds = 0.01f;
    // The share of the block period a block may take before the engine
    // stops the program and main reduces its line count. What is left
    // covers libDaisy's sample conversions, interrupt overhead and main.
    float cpu_budget = 0.9f;  // finite, greater than zero and at most one
    // Called with the reverb right after every program load, while main
    // owns it: for the parameters the application fixes or controls itself
    // (e.g. DryOut 0 when the application mixes the dry signal, or
    // CutoffEnabled 1 when a control drives the cutoff). Delay geometry may
    // be changed here: placement and staging follow the hook.
    void (*on_program_loaded)(cloudseed::ReverbController& reverb,
                              void* context) = nullptr;
    void* context = nullptr;
  };

  // Main, before StartAudio(): initializes the delay memory in every
  // memory, the reverb, the staging and its transport, and the load meter.
  // Returns false for invalid config fields, null presets/names, preset
  // values outside finite 0..1, or insufficient delay memory.
  // The SDRAM must be initialized (the Seed's
  // Init() does that).
  bool Init(const Config& config);

  // Main, after Init() and before StartAudio(): loads the first program.
  // Returns false when the staging failed to prime its windows; the engine
  // then plays dry until Service() has reloaded the program onto the CPU
  // paths, as after any transport fault.
  bool Start(int program);

  // Main, before Init(): writes a pattern over the whole delay memory and
  // reads it back, for a check of the SDRAM's timings. Returns false when a
  // word came back changed.
  static bool TestDelayMemory();

  // Profiling builds: fills the unused stack with a pattern so that the
  // report can tell how much of it was never needed. First thing in main().
  static void FillStack();

  // --- The audio callback --------------------------------------------------

  // Starts the block: the load measurement, flush-to-zero for the interrupt
  // context and the profiling. Call first, before reading the controls.
  void BeginBlock();

  // Selects the program; every block, with the control's current value. A
  // change fades the wet signal out, main loads the program, and the wet
  // signal fades back in. Values outside the program table are clamped.
  void RequestProgram(int program);

  // Sets a parameter (a normalized 0..1 value) when the callback owns the
  // reverb and the value moved by more than threshold since the last value
  // applied (or a program was loaded since). Returns whether it was applied.
  // Supports InputMix, HighPass, LowPass, DiffusionFeedback, LineDecay,
  // LateDiffusionFeedback, the five Post* tone parameters, the four output
  // gains and the five filter-enable switches. All other parameters require
  // a preset or on_program_loaded hook; changing their geometry while the
  // transport owns memory is unsafe. They return false in every build.
  // Non-finite/out-of-range values and non-finite/negative thresholds also
  // return false. A threshold avoids unnecessary coefficient recomputation.
  bool SetParameter(cloudseed::Parameter parameter, double value,
                    float threshold = 0.f);

  // Freezes or releases the late reverb (see ReverbController::SetFrozen).
  void SetFrozen(bool frozen);

  // Renders the wet signal of the block into wet_l and wet_r (size floats
  // each) with the program fade applied, and returns true. Returns false
  // without touching them when the reverb is being loaded or recovered: the
  // application then passes the dry signal alone. Call once per callback
  // with the callback's block. A size different from Config::block_size
  // returns false without touching the output or DSP state.
  bool Process(const float* in_l, const float* in_r, float* wet_l,
               float* wet_r, size_t size);

  // Ends the block: the load measurement (the block's rendering, the
  // application's mixing and control processing included), the overload
  // decision and the profiling. Call last.
  void EndBlock();

  // --- The main loop -------------------------------------------------------

  // Loads requested programs, reduces overloaded ones and recovers from
  // transport faults. Call from the main loop, as often as it runs.
  void Service();

  // --- Status (any context) ------------------------------------------------

  State state() const { return state_.load(std::memory_order_relaxed); }
  // The program loaded (main's view; the callback sees it change only
  // after main has handed the reverb back).
  int program() const { return current_program_; }
  // Late lines per channel the loaded program runs with.
  int line_count() const;
  // The lines a program may run with: its configured count, reduced by the
  // overload recovery; 0 when it exceeded the budget at one line.
  int line_limit(int program) const;
  bool frozen() const { return (status_.load(std::memory_order_relaxed) & kStatusFrozen) != 0; }
  // Whether the reverb is not running as configured: for a second after a
  // budget breach, or while a program is bypassed or a transport fault is
  // being recovered from (the reference firmware flashes its LED).
  bool overloaded() const { return (status_.load(std::memory_order_relaxed) & kStatusOverloaded) != 0; }
  // Whether the delay memory is staged in the TCMs (false after a fault).
  bool staging_enabled() const { return staging_enabled_; }
  unsigned int staging_failures() const { return staging_failures_; }
  // The reverb, for diagnostics and the on_program_loaded hook. Only main
  // may change it, and only while it owns it (inside the hook).
  cloudseed::ReverbController& reverb();

#if CLOUDSEED_PROFILE
  // Values (0..1) the report carries along with the load, e.g. the
  // controls: up to ProfileReport::kMaxControls, from the callback.
  void SetProfileControls(const float* values, int count);
  // Main: takes the report of the last second if one is ready.
  bool TakeReport(ProfileReport* report);
  // Main: the build's options, the clock, the silicon revision and the
  // image's CRC-32, once at start-up (after the logger is started).
  void PrintBuild(PrintFn print);
  // Main: the loaded program's workload, printed once per load.
  void PrintProgramIfChanged(PrintFn print);
  // Main: a report, as three lines.
  void PrintReport(const ProfileReport& report, PrintFn print);
  // CRC-32 of the flash image, as `make image-crc` prints it (0 when the
  // program does not run from the internal flash).
  uint32_t image_crc() const { return image_crc_; }
#endif

 private:
  friend struct EngineTest;
  static constexpr unsigned int kStatusFrozen = 1;
  static constexpr unsigned int kStatusOverloaded = 2;

  bool CallbackOwns(State state) const {
    return state == State::kRunning || state == State::kFadingOut ||
           state == State::kFadingIn;
  }
  bool LoadProgram(int program);
  void DisableStaging();
  void ResetLastValues();

  Config config_;
  daisy::CpuLoadMeter cpu_load_meter_;
  std::atomic<State> state_{State::kRunning};
  std::atomic<int> requested_program_{0};
  std::atomic<unsigned int> status_{0};
  // Main writes these only while the callback has relinquished the reverb;
  // the callback reads current_program_ only after acquiring a state that
  // gives it the reverb.
  int current_program_ = -1;
  int line_limits_[kMaxPrograms] = {};
  // Callback-owned.
  bool block_open_ = false;
  bool processed_ = false;  // the block's Process() rendered the reverb
  size_t block_samples_ = 0;
  bool frozen_ = false;
  float switch_gain_ = 1.f;
  float switch_fade_step_ = 1.f;
  unsigned int updates_ = 0;
  size_t overload_samples_ = 0;
  bool overload_latch_ = false;
  // The last value applied per parameter (SetParameter's threshold).
  double last_value_[cloudseed::kParameterCount] = {};
  bool has_last_value_[cloudseed::kParameterCount] = {};
  bool staging_enabled_ = CLOUDSEED_STAGING != 0;
  unsigned int staging_failures_ = 0;
  int itcm_state_ = 0;  // 0 not used, 1 enabled and tested, 2 failed
#if CLOUDSEED_PROFILE
  uint32_t image_crc_ = 0;
  int printed_program_ = -1;
  int printed_limit_ = -1;
  float profile_controls_[ProfileReport::kMaxControls] = {};
  int profile_control_count_ = 0;
#endif
};

}  // namespace cloudseed_daisy
