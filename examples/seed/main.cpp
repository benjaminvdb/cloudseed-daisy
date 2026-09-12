// Cloud Seed on a bare Daisy Seed: one program, a fixed dry/wet mix, and
// the engine's overload recovery on the LED. Build with `make` in this
// folder (LIBDAISY_DIR points at a built libDaisy checkout, see the
// Makefile) and flash build/cloudseed_seed.bin with the Daisy Web
// Programmer or `make program-dfu`.
//
// The audio callback hands the engine its block, mixes the wet signal it
// renders with the dry input, and passes the dry input alone while the
// engine loads a program or recovers. The main loop services the engine.
#include "cloudseed_daisy/engine.h"
#include "cloudseed_daisy/seed_system.h"
#include "daisy_seed.h"

using cloudseed::Parameter;
using cloudseed_daisy::Engine;
using daisy::AudioHandle;
using daisy::DaisySeed;
using daisy::System;

namespace {

// The ten programs (see cloudseed/presets.h), each with the late lines per
// channel it runs with on a 480 MHz Seed (TECHNICAL.md, "Measured
// performance"). The engine reduces a program that exceeds the CPU budget.
const cloudseed_daisy::Program kPrograms[] = {
    {&cloudseed::presets::kSmallRoom, 3},
    {&cloudseed::presets::kMediumSpace, 3},
    {&cloudseed::presets::kNoiseInTheHallway, 8},
    {&cloudseed::presets::kHyperplane, 9},
    {&cloudseed::presets::kRubiKaFields, 4},
    {&cloudseed::presets::kThroughTheLookingGlass, 12},
    {&cloudseed::presets::kThe90sAreBack, 9},
    {&cloudseed::presets::kDullEchoes, 12},
    {&cloudseed::presets::kChorusDelay, 12},
    {&cloudseed::presets::kDarkPlate, 12},
};
constexpr int kProgramCount = sizeof(kPrograms) / sizeof(kPrograms[0]);

// The program to run and the mix (0 dry, 1 wet). Wire these to whatever
// controls the board has.
constexpr int kProgram = 1;  // Medium Space
constexpr float kMix = 0.5f;

DaisySeed seed;
// The engine and the callback's buffers in the DTCM, out of the data
// cache's way (see engine.h).
CLOUDSEED_DAISY_DTCM Engine engine;
CLOUDSEED_DAISY_DTCM float wet_l[cloudseed::kMaxBlockSize];
CLOUDSEED_DAISY_DTCM float wet_r[cloudseed::kMaxBlockSize];

// After every program load: this firmware mixes the dry signal itself, so
// the program's own dry level is silenced.
void OnProgramLoaded(cloudseed::ReverbController& reverb, void*) {
  reverb.SetParameter(Parameter::DryOut, 0.0);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out,
                   size_t size) {
  engine.BeginBlock();
  engine.RequestProgram(kProgram);
  const bool wet = engine.Process(in[0], in[1], wet_l, wet_r, size);
  for (size_t i = 0; i < size; i++) {
    out[0][i] = (1.f - kMix) * in[0][i] + (wet ? kMix * wet_l[i] : 0.f);
    out[1][i] = (1.f - kMix) * in[1][i] + (wet ? kMix * wet_r[i] : 0.f);
  }
  engine.EndBlock();
}

}  // namespace

int main() {
  // 480 MHz where the silicon allows it, the SDRAM refreshed and timed as
  // its datasheet requires, and the internal SRAM without write allocation
  // (see seed_system.h): what the measured performance assumes.
  seed.Init(cloudseed_daisy::SupportsBoost());
  cloudseed_daisy::ConfigureSdramRefresh();
  cloudseed_daisy::SetSdramTiming(false);
  cloudseed_daisy::ConfigureSramNoWriteAllocate();
  seed.SetAudioBlockSize(cloudseed::kMaxBlockSize);

  Engine::Config config;
  config.programs = kPrograms;
  config.program_count = kProgramCount;
  config.sample_rate = seed.AudioSampleRate();
  config.block_size = seed.AudioBlockSize();
  config.on_program_loaded = OnProgramLoaded;
  if (!engine.Init(config)) {
    // The delay memory does not fit the sample rate: blink forever.
    for (;;) {
      seed.SetLed(true);
      System::Delay(100);
      seed.SetLed(false);
      System::Delay(100);
    }
  }
  engine.Start(kProgram);
  seed.StartAudio(AudioCallback);

  for (;;) {
    engine.Service();
    // Lit while the program is reduced, bypassed or being recovered.
    seed.SetLed(engine.overloaded());
    System::Delay(2);
  }
}
