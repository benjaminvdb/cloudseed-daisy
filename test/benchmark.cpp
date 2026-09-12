// Relative desktop timings only. These do not establish the MCU deadline
// margin.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cloudseed/fast_sin.h"
#include "cloudseed/reverb_controller.h"
#include "cloudseed/presets.h"

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  const bool stress = argc == 2 && std::strcmp(argv[1], "--stress") == 0;
  cloudseed::FastSin::Init();
  std::vector<float> pool(
      cloudseed::ReverbController::RequiredPoolFloats(48000));
  cloudseed::MemoryPool memory;
  memory.Init(pool.data(), pool.size());
  cloudseed::ReverbController reverb;
  if (!reverb.Init(48000, memory)) return 1;
  // The default MDMA firmware's internal-RAM pools (448 KiB + 232 KiB).
  std::vector<float> fast_a(448 * 1024 / 4), fast_b(232 * 1024 / 4);
  reverb.AddFastPool(fast_a.data(), fast_a.size());
  reverb.AddFastPool(fast_b.data(), fast_b.size());
  reverb.LoadPreset(cloudseed::presets::kMediumSpace.values);
  reverb.ClearBuffers();
  for (auto parameter : {cloudseed::Parameter::LineDecay,
                         cloudseed::Parameter::PostCutoffFrequency}) {
    const auto start = Clock::now();
    for (int i = 0; i < 200000; i++)
      reverb.SetParameter(parameter, (i % 1000) / 1000.0);
    std::printf(
        "control %d: %.1f ns/update\n", int(parameter),
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
            200000);
  }
  const cloudseed::presets::Preset* presets[] = {
      &cloudseed::presets::kSmallRoom,
      &cloudseed::presets::kMediumSpace,
      &cloudseed::presets::kNoiseInTheHallway,
      &cloudseed::presets::kHyperplane,
      &cloudseed::presets::kRubiKaFields,
      &cloudseed::presets::kThroughTheLookingGlass,
      &cloudseed::presets::kThe90sAreBack,
      &cloudseed::presets::kDullEchoes,
      &cloudseed::presets::kChorusDelay,
      &cloudseed::presets::kDarkPlate,
  };
  float left[48], right[48], out_l[48], out_r[48];
  const int blocks = stress ? 120000 : 5000;
  for (auto preset : presets) {
    reverb.SetFrozen(false);
    reverb.LoadPreset(preset->values);
    reverb.SetParameter(cloudseed::Parameter::DryOut, 0);
    reverb.SetParameter(cloudseed::Parameter::CutoffEnabled, 1);
    reverb.ClearBuffers();
    for (int i = 0; i < 48; i++) {
      left[i] = 0.1f * std::sin(i * 0.37f);
      right[i] = 0.1f * std::cos(i * 0.29f);
    }
    double peak = 0, end_energy = 0;
    const auto start = Clock::now();
    for (int block = 0; block < blocks; block++) {
      if (stress && block == 1000) {
        reverb.SetFrozen(true);
        for (int i = 0; i < 48; i++) left[i] = right[i] = 0;
      }
      reverb.Process(left, right, out_l, out_r, 48);
      for (int i = 0; i < 48; i++) {
        if (!std::isfinite(out_l[i]) || !std::isfinite(out_r[i])) {
          std::fprintf(stderr, "Nonfinite output: %s block %d\n", preset->name,
                       block);
          return 1;
        }
        peak = std::fmax(peak,
                         std::fmax(std::fabs(out_l[i]), std::fabs(out_r[i])));
        if (block >= blocks - 1000)
          end_energy +=
              double(out_l[i]) * out_l[i] + double(out_r[i]) * out_r[i];
      }
    }
    std::printf(
        "%-28s %.1f ns/block, peak %.5f, final RMS %.7f%s\n", preset->name,
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
            blocks,
        peak, std::sqrt(end_energy / 96000),
        stress ? " (120 s, frozen after 1 s)" : "");
  }
}
