// Standalone port tests: no legacy code or Daisy hardware is needed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "cloudseed/fast_sin.h"
#include "cloudseed/reverb_controller.h"
#include "cloudseed/response_curves.h"
#include "cloudseed/staging.h"
#include "cloudseed/presets.h"
#include "queued_transport.h"

namespace {
using namespace cloudseed;
constexpr int kRate = 48000;
constexpr int kBlock = kMaxBlockSize;
int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

const presets::Preset* const kPresets[] = {
    &presets::kSmallRoom,         &presets::kMediumSpace,
    &presets::kNoiseInTheHallway, &presets::kHyperplane,
    &presets::kRubiKaFields,      &presets::kThroughTheLookingGlass,
    &presets::kThe90sAreBack,     &presets::kDullEchoes,
    &presets::kChorusDelay,       &presets::kDarkPlate,
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

struct Fixture {
  std::vector<float> storage;
  MemoryPool memory;
  ReverbController reverb;
  Fixture()
      : storage(ReverbController::RequiredPoolFloats(kRate) + 2, 12345.f) {
    memory.Init(storage.data() + 1, storage.size() - 2);
    Check(reverb.Init(kRate, memory), "exact-size pool initializes");
    Check(memory.used() == memory.size(), "pool sizing matches allocations");
  }
  ~Fixture() {
    Check(storage.front() == 12345.f && storage.back() == 12345.f,
          "delay pool guards survive");
  }
  void Load(const presets::Preset& preset, bool no_mod = false) {
    reverb.LoadPreset(preset.values);
    reverb.SetParameter(Parameter::DryOut, 0);
    reverb.SetParameter(Parameter::CutoffEnabled, 1);
    if (no_mod) {
      reverb.SetParameter(Parameter::EarlyDiffusionModAmount, 0);
      reverb.SetParameter(Parameter::LineModAmount, 0);
      reverb.SetParameter(Parameter::LateDiffusionModAmount, 0);
    }
    reverb.ClearBuffers();
  }
};

float Noise(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return (static_cast<float>(state >> 8) / 16777216.f - 0.5f) * 0.5f;
}

void TestLowerRates() {
  for (int rate : {8000, 16000, 32000, 44100, 96000}) {
    // Independently test the shelf's impulse response at and above Nyquist.
    for (double frequency : {rate * .5, rate * .75, 20000.}) {
      Biquad shelf;
      shelf.Init(Biquad::FilterType::HighShelf, rate);
      shelf.SetGain(.5);
      shelf.frequency = frequency;
      shelf.Update();
      bool stable = true;
      double tail_energy = 0;
      for (int i = 0; i < rate; ++i) {
        const float y = shelf.Process(i == 0 ? 1.f : 0.f);
        stable &= std::isfinite(y) && std::fabs(y) <= 2.f;
        if (i > rate / 2) tail_energy += double(y) * y;
      }
      Check(stable && tail_energy < 1e-8, "shelf impulse decays at lower rates");
      Check(shelf.frequency == frequency, "requested frequency survives clamp");
    }
    std::vector<float> storage(ReverbController::RequiredPoolFloats(rate));
    MemoryPool pool; pool.Init(storage.data(), storage.size());
    ReverbController reverb;
    Check(reverb.Init(rate, pool), "lower-rate reverb initializes");
    for (auto* preset : kPresets) {
      reverb.LoadPreset(preset->values);
      reverb.ClearBuffers();
      bool finite = true;
      for (int sample = 0; sample < 2 * rate; sample += kBlock) {
        float in[kBlock] = {}, left[kBlock], right[kBlock];
        if (sample == 0) in[0] = .5f;
        reverb.Process(in, in, left, right, kBlock);
        for (int i = 0; i < kBlock; ++i)
          finite &= std::isfinite(left[i]) && std::isfinite(right[i]);
      }
      if (!finite) std::fprintf(stderr, "%s at %d Hz: ", preset->name, rate);
      Check(finite, "factory preset stays finite at alternate rate");
    }
  }
}

void TestInvalidParameters() {
  Fixture fixture;
  fixture.Load(presets::kMediumSpace);
  for (int i = 0; i < kParameterCount; ++i) {
    const auto p = static_cast<Parameter>(i);
    const double previous = fixture.reverb.GetParameter(p);
    for (double value : {-1., 2., std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
      fixture.reverb.SetParameter(p, value);
      Check(fixture.reverb.GetParameter(p) == previous,
            "invalid normalized parameter leaves DSP unchanged");
    }
  }
}

void TestFilters() {
  for (double cutoff : {20., 400., 1000., 20000.}) {
    Lp1 pos, neg;
    Hp1 hp_pos, hp_neg;
    pos.Init(kRate);
    neg.Init(kRate);
    hp_pos.Init(kRate);
    hp_neg.Init(kRate);
    pos.SetCutoffHz(cutoff);
    neg.SetCutoffHz(cutoff);
    hp_pos.SetCutoffHz(cutoff);
    hp_neg.SetCutoffHz(cutoff);
    const double nn = 2 - std::cos(2 * M_PI * cutoff / kRate);
    const double pole = nn - std::sqrt(nn * nn - 1);
    for (int i = 0; i < 100; i++) {
      const float x = i == 0 ? 1.f : 0.f;
      const float a = pos.Process(x), b = neg.Process(-x);
      const float c = hp_pos.Process(x), d = hp_neg.Process(-x);
      Check(a == -b && c == -d, "one-pole impulse polarity symmetry");
      const double ideal = (1 - pole) * std::pow(pole, i);
      Check(std::fabs(a - ideal) < 1e-7, "LP matches analytical impulse");
      Check(std::fabs(c - (x - ideal)) < 1e-7, "HP matches analytical impulse");
    }
    hp_neg.ClearBuffers();
    Check(hp_neg.Process(0) == 0, "HP clear discards internal state");
  }
}

void TestOnePoleBlockForms() {
  // The block forms of the one-pole filters test the legacy silence
  // condition on the values' bits; the scalar Process() keeps the float
  // form. Both must agree bit for bit, including where the condition
  // decides: exact zeros in the input, and states below, at and above the
  // 1e-12 threshold, of both signs, with the state in either sign.
  uint32_t rng = 77;
  const float specials[] = {0.f,    -0.f,   1e-12f, -1e-12f, 9.9e-13f,
                            -9.9e-13f, 1e-13f, 1e-11f, 2e-38f, -1e-45f};
  for (double cutoff : {20., 1000., 20000.}) {
    Lp1 scalar, block, pair_a, pair_b, single;
    Hp1 hp_scalar, hp_block;
    for (Lp1* f : {&scalar, &block, &pair_a, &pair_b, &single}) {
      f->Init(kRate);
      f->SetCutoffHz(cutoff);
    }
    for (Hp1* f : {&hp_scalar, &hp_block}) {
      f->Init(kRate);
      f->SetCutoffHz(cutoff);
    }
    bool same = true;
    for (int blockno = 0; blockno < 400; blockno++) {
      float input[kBlock], other[kBlock], out[kBlock], out_a[kBlock],
          out_b[kBlock], out_single[kBlock], hp_out[kBlock];
      const int count = 1 + (blockno * 7) % kBlock;
      for (int i = 0; i < count; i++) {
        // Noise, runs of exact zeros (blocks 100..199 and 300..399 are
        // silent, so the states decay through the threshold), and special
        // values at some positions.
        const bool silent = (blockno / 100) % 2 == 1;
        float x = silent ? 0.f : Noise(rng);
        if (!silent && (blockno * 48 + i) % 11 == 0)
          x = specials[(blockno + i) % 10];
        input[i] = x;
        other[i] = silent ? 0.f : Noise(rng) * 1e-6f;
      }
      for (int i = 0; i < count; i++) out[i] = scalar.Process(input[i]);
      block.Process(input, out_single, count);
      Lp1::ProcessPair(pair_a, pair_b, input, other, out_a, out_b, count);
      for (int i = 0; i < count; i++) {
        same &= out[i] == out_single[i] && out[i] == out_a[i];
        same &= FloatBits(out[i]) == FloatBits(out_single[i]);
      }
      // The pair's second filter against a scalar filter over its input.
      for (int i = 0; i < count; i++) same &= single.Process(other[i]) == out_b[i];
      for (int i = 0; i < count; i++) hp_out[i] = hp_scalar.Process(input[i]);
      hp_block.Process(input, out_single, count);
      for (int i = 0; i < count; i++) {
        same &= FloatBits(hp_out[i]) == FloatBits(out_single[i]);
      }
    }
    Check(same, "one-pole block forms match the scalar filters bit for bit");
    // Silence until the states have decayed through the threshold (a 20 Hz
    // pole needs some 10,000 samples): the block forms must zero their
    // state at the same sample as the scalar filters, and stay at zero.
    float zeros[kBlock] = {}, out[kBlock], out_a[kBlock], out_b[kBlock],
          hp_out[kBlock];
    int settled = -1;
    for (int blockno = 0; blockno < 2000 && settled < 0; blockno++) {
      block.Process(zeros, out, kBlock);
      Lp1::ProcessPair(pair_a, pair_b, zeros, zeros, out_a, out_b, kBlock);
      hp_block.Process(zeros, hp_out, kBlock);
      for (int i = 0; i < kBlock; i++) {
        const float expected = scalar.Process(0.f);
        same &= FloatBits(out[i]) == FloatBits(expected) &&
                FloatBits(out_a[i]) == FloatBits(expected);
        same &= FloatBits(hp_out[i]) == FloatBits(hp_scalar.Process(0.f));
        if (settled < 0 && expected == 0.f) settled = blockno * kBlock + i;
      }
    }
    Check(same, "one-pole block forms zero their state with the scalar filters");
    // (A fast pole has settled during the silent blocks above already.)
    Check(settled >= 0 && scalar.output == 0.f && block.output == 0.f &&
              pair_a.output == 0.f && hp_block.output == 0.f,
          "one-pole filters settle to an exact zero after silence");
  }
  // In-place filtering (input and output the same buffer).
  {
    Lp1 a, b;
    a.Init(kRate);
    b.Init(kRate);
    float buffer[kBlock], reference[kBlock];
    for (int i = 0; i < kBlock; i++) buffer[i] = reference[i] = Noise(rng);
    for (int i = 0; i < kBlock; i++) reference[i] = a.Process(reference[i]);
    b.Process(buffer, buffer, kBlock);
    bool same = true;
    for (int i = 0; i < kBlock; i++) same &= buffer[i] == reference[i];
    Check(same, "one-pole block form filters in place");
  }
}

void TestDarkPlateShelves() {
  // CloudSeedCore Programs.h/ScaleParam at deb21ded: the successor's
  // shelves have these actual endpoint gains. Exercise the filter, since
  // copying the same bad conversion to a reference would miss this bug.
  for (bool high : {false, true}) {
    const double db = -20 + 20 * (high ? 0.7680000066757202
                                      : 0.5559999942779541);
    const Parameter param = high ? Parameter::PostHighShelfGain
                                 : Parameter::PostLowShelfGain;
    Biquad filter;
    filter.Init(high ? Biquad::FilterType::HighShelf
                     : Biquad::FilterType::LowShelf, kRate);
    filter.frequency = 1000;
    filter.SetGain(ResponseDec(presets::kDarkPlate.values[int(param)], 2));
    filter.Update();
    double gain = 0;
    for (int i = 0; i < 4096; i++) {
      // DC measures the low shelf; alternating samples measure Nyquist.
      const float input = high && i % 2 ? -1.f : 1.f;
      const float output = filter.Process(input);
      if (i >= 3072) gain += output * input / 1024.0;
    }
    const double actual_db = 20 * std::log10(gain);
    if (std::fabs(actual_db - db) >= 0.012)
      std::fprintf(stderr, "Dark Plate %s shelf: %.6f dB, wanted %.6f dB\n",
                   high ? "high" : "low", actual_db, db);
    // The legacy response table quantizes the converted setting.
    Check(std::fabs(actual_db - db) < 0.012,
          "adapted Dark Plate shelf preserves successor endpoint gain");
  }
}

void TestPool() {
  float data[4] = {};
  MemoryPool pool;
  pool.Init(data, 4);
  Check(pool.Allocate(1) == data, "initial pool allocation");
  Check(pool.Allocate(std::numeric_limits<size_t>::max()) == nullptr,
        "oversized allocation cannot wrap size_t");
  Check(pool.used() == 1 && pool.overflow(),
        "failed allocation preserves cursor");
  std::vector<float> small(ReverbController::RequiredPoolFloats(kRate) - 1);
  pool.Init(small.data(), small.size());
  ReverbController reverb;
  Check(!reverb.Init(kRate, pool), "undersized reverb pool is rejected");
  // Placed buffers start on cache lines, whatever the pool's alignment.
  std::vector<float> raw(64);
  MemoryPool fast;
  fast.Init(raw.data() + 1, 40);
  size_t allocated = 0;
  Check(fast.Fits(9) && !fast.Fits(41), "fast pool reports what fits");
  float* p = fast.AllocateAligned(9, &allocated);
  Check(p != nullptr && reinterpret_cast<uintptr_t>(p) % 32 == 0 &&
            allocated == 16 && fast.used() <= 40,
        "aligned allocation rounds up to cache lines");
  Check(fast.AllocateAligned(40, &allocated) == nullptr && fast.overflow(),
        "exhausted fast pool returns nullptr");
  fast.Reset();
  Check(fast.used() == 0 && !fast.overflow(), "fast pool reset");
  for (size_t excess = 0; excess < MemoryPool::kAlignFloats; excess++) {
    const size_t huge = std::numeric_limits<size_t>::max() - excess;
    allocated = 123;
    Check(!fast.Fits(huge) && fast.AllocateAligned(huge, &allocated) == nullptr,
          "aligned size rounding cannot wrap size_t");
    Check(fast.used() == 0 && allocated == 123,
          "failed aligned allocation preserves cursor and result");
  }
  MemoryPool empty;
  Check(!empty.Fits(0), "uninitialized pool rejects aligned allocation");
}

void TestDelayBoundaries() {
  // Every supported block length, including exact ring boundaries and rings
  // smaller than a block. Integer delays have an independent ramp oracle.
  for (int size : {4, 8, 47, 48, 49, 96, 97, 257}) {
    for (int delay : {0, 1, 2, size - 2}) {
      for (int count : {1, 7, 8, 47, 48}) {
        std::vector<float> data(size);
        MemoryPool pool;
        pool.Init(data.data(), data.size());
        ModulatedDelay d;
        Check(d.Init(pool, size, delay, .5f), "boundary delay init");
        d.ClearBuffers();
        float input[kBlock];
        bool same = true;
        for (int pos = 0; pos < 1000; pos += count) {
          for (int i = 0; i < count; i++) input[i] = pos + i + 1;
          d.Process(input, count);
          for (int i = 0; i < count; i++) {
            const int n = pos + i - delay;
            same &= d.output()[i] == (n < 0 ? 0.f : n + 1.f);
          }
        }
        Check(same, "integer delay matches sample history across ring wraps");
      }
    }
  }
  // Previously the no-wrap path left write_index==96. A crossfade starting
  // in the next call then wrote past the allocation before wrapping it.
  std::vector<float> data(96);
  MemoryPool pool;
  pool.Init(data.data(), data.size());
  ModulatedDelay d;
  Check(d.Init(pool, 96, 8, .5f), "crossfade boundary init");
  d.ClearBuffers();
  float input[kBlock];
  bool same = true;
  for (int pos = 0; pos < 1248; pos += kBlock) {
    if (pos == 96) d.SetDelay(16);
    for (int i = 0; i < kBlock; i++) input[i] = pos + i + 1;
    d.Process(input, kBlock);
    for (int i = 0; i < kBlock; i++) {
      const int n = pos + i;
      float expected = n < 8 ? 0.f : n - 7.f;
      if (n >= 96) {
        const float t = n < 1120 ? (1120 - n) / 1024.f : 0.f;
        expected = (n - 7.f) * t + (n - 15.f) * (1.f - t);
      }
      same &= d.output()[i] == expected;
    }
  }
  Check(same, "crossfade at exact ring end matches analytical ramp");
}

void TestCompactTaps() {
  // The one-tap zero-gain setting is an exact passthrough, even when the
  // length is zero and a block is longer than the old compact allocation.
  std::vector<float> data(1024);
  MemoryPool pool;
  pool.Init(data.data(), data.size());
  MultitapDiffuser tap;
  Check(tap.Init(pool, data.size()), "compact tap init");
  tap.SetTapCount(1);
  tap.SetTapLength(0);
  tap.SetTapGain(0);
  std::vector<float> compact(tap.RequiredSize());
  tap.UseMemory(compact.data(), compact.size());
  tap.ClearBuffers();
  float input[kBlock];
  for (int i = 0; i < kBlock; i++) input[i] = i + 1;
  tap.Process(input, kBlock);
  bool same = true;
  for (int i = 0; i < kBlock; i++) same &= tap.output()[i] == input[i];
  Check(same, "compact zero-length tap is exact passthrough");

  // A one-sample call cannot overwrite unread history. Compare the optimized
  // block path, then increase the length beyond an existing compact ring.
  for (int taps : {1, 2, 5, 50}) {
    for (int length : {0, 1, 47, 48, 97}) {
      std::vector<float> home_a(1024), home_b(1024);
      MemoryPool pa, pb;
      pa.Init(home_a.data(), home_a.size());
      pb.Init(home_b.data(), home_b.size());
      MultitapDiffuser a, b;
      Check(a.Init(pa, 1024) && b.Init(pb, 1024), "paired tap init");
      for (auto* t : {&a, &b}) {
        t->SetTapCount(taps);
        t->SetTapLength(length);
        t->SetTapGain(.6);
      }
      std::vector<float> ca(a.RequiredSize()), cb(b.RequiredSize());
      a.UseMemory(ca.data(), ca.size());
      b.UseMemory(cb.data(), cb.size());
      a.ClearBuffers();
      b.ClearBuffers();
      same = true;
      uint32_t rng = 15;
      for (int block = 0; block < 50; block++) {
        if (block == 25) {
          a.SetTapLength(1000);
          b.SetTapLength(1000);
        }
        for (float& x : input) x = Noise(rng);
        a.Process(input, kBlock);
        for (int i = 0; i < kBlock; i++) {
          b.Process(input + i, 1);
          same &= a.output()[i] == b.output()[0];
        }
      }
      Check(same, "compact and resized taps preserve sample ordering");
    }
  }
}

void TestAllpassRebind() {
  for (bool interpolate : {false, true}) {
    for (int pending = 0; pending < kModulationUpdateRate; pending++) {
      std::vector<float> home(2048), compact(8);
      MemoryPool pool;
      pool.Init(home.data(), home.size());
      ModulatedAllpass a;
      Check(a.Init(pool, 2048, 1000, .5f), "rebound allpass init");
      a.modulation_enabled = true;
      a.interpolation_enabled = interpolate;
      float input[kBlock] = {}, output[kBlock];
      a.ClearBuffers();
      a.Process(input, output, pending);
      a.sample_delay = 4;
      a.UseMemory(compact.data(), compact.size());
      a.ClearBuffers();
      for (int i = 0; i < kBlock; i++) input[i] = i + 1;
      a.Process(input, output, kBlock);
      bool same = true;
      for (int i = 0; i < kBlock; i++)
        same &= output[i] == (i < 4 ? 0.f : i - 3.f);
      Check(same, "allpass rebind rebuilds heads before the next LFO tick");
    }
  }
}

void TestPlacementHistory() {
  Fixture f;
  std::vector<float> a(114688), b(61440);
  f.reverb.AddFastPool(a.data(), a.size());
  f.reverb.AddFastPool(b.data(), b.size());
  size_t expected[kPresetCount][2];
  int unplaced[kPresetCount];
  for (int i = 0; i < kPresetCount; i++) {
    f.Load(*kPresets[i]);
    expected[i][0] = f.reverb.fast_pool_used(0);
    expected[i][1] = f.reverb.fast_pool_used(1);
    unplaced[i] = f.reverb.unplaced_buffers();
  }
  bool same = true;
  for (auto previous : kPresets) {
    for (int i = 0; i < kPresetCount; i++) {
      f.Load(*previous);
      f.Load(*kPresets[i]);
      same &= expected[i][0] == f.reverb.fast_pool_used(0) &&
              expected[i][1] == f.reverb.fast_pool_used(1) &&
              unplaced[i] == f.reverb.unplaced_buffers();
    }
  }
  Check(same, "all preset transitions have history-independent placement");
}

void TestPlacement() {
  // Moving delay buffers into fast pools must not change a single output
  // bit, whatever fits: pools of the firmware's size, small pools that hold a
  // part of every program, and pools too small for anything.
  const size_t sizes[][2] = {{114688, 61440}, {10240, 5120}, {200, 100}};
  for (auto& size : sizes) {
    // Both start from Init(): the LFO phases only agree with equal histories.
    Fixture reference;
    Fixture placed;
    std::vector<float> a(size[0]), b(size[1]);
    placed.reverb.AddFastPool(a.data(), a.size());
    placed.reverb.AddFastPool(b.data(), b.size());
    uint32_t rng = 5;
    for (auto preset : kPresets) {
      reference.Load(*preset);
      placed.Load(*preset);
      Check(placed.reverb.fast_pool_used(0) <= a.size() &&
                placed.reverb.fast_pool_used(1) <= b.size(),
            "placement stays inside the pools");
      if (size[0] == 114688 && kMaxLines <= 4) {
        // Only the long late delay lines may stay behind (the twelve-line
        // build's allpass memory exceeds the pools by design).
        Check(
            placed.reverb.unplaced_buffers() <= 2 * placed.reverb.line_count(),
            "firmware-sized pools hold every allpass, tap and pre-delay");
      }
      float input[kBlock], rl[kBlock], rr[kBlock], pl[kBlock], pr[kBlock];
      bool same = true;
      for (int block = 0; block < 400; block++) {
        if (block % 30 == 0) {
          const double value = (block % 90) / 80.0;
          for (Fixture* f : {&reference, &placed}) {
            f->reverb.SetParameter(Parameter::LineDecay, value);
            f->reverb.SetParameter(Parameter::PostCutoffFrequency, 1 - value);
          }
        }
        if (block % 150 == 0) {
          reference.reverb.SetFrozen((block / 150) % 2);
          placed.reverb.SetFrozen((block / 150) % 2);
        }
        for (float& x : input) x = block < 250 ? Noise(rng) : 0.f;
        reference.reverb.Process(input, input, rl, rr, kBlock);
        placed.reverb.Process(input, input, pl, pr, kBlock);
        for (int i = 0; i < kBlock; i++)
          same &= rl[i] == pl[i] && rr[i] == pr[i];
      }
      Check(same, "placed buffers reproduce the unplaced output");
      reference.reverb.SetFrozen(false);
      placed.reverb.SetFrozen(false);
    }
  }
}

void TestBlockFeedback() {
  // This intentionally records the legacy topology: a 96-sample delay plus
  // one 48-sample feedback block. Changing it silently changes the reverb.
  std::vector<float> data(12000 + kMaxStages * 7200);
  MemoryPool pool;
  pool.Init(data.data(), data.size());
  DelayLine line;
  Check(line.Init(pool, kRate, 12000, 7200), "delay line init");
  line.SetDelay(96);
  line.SetFeedback(0.5);
  line.ClearBuffers();
  float input[kBlock] = {};
  for (int pos = 0; pos < 432; pos += kBlock) {
    for (int i = 0; i < kBlock; i++) input[i] = pos + i == 0 ? 1.f : 0.f;
    line.Process(input, kBlock);
    for (int i = 0; i < kBlock; i++) {
      const int n = pos + i;
      const float expected = n == 96    ? 1.f
                             : n == 240 ? 0.5f
                             : n == 384 ? 0.25f
                                        : 0.f;
      Check(line.output()[i] == expected,
            "legacy feedback echo timing and gain");
    }
  }
}

void TestSeedRefresh() {
  std::vector<float> data(kMaxStages * 7200);
  MemoryPool pool;
  pool.Init(data.data(), data.size());
  AllpassDiffuser actual;
  Check(actual.Init(pool, kRate, 7200), "diffuser init");
  actual.set_stages(4);
  actual.SetDelay(1000);
  actual.SetFeedback(0.6);
  actual.SetModulationEnabled(true);
  actual.SetModAmount(80);
  actual.SetModRate(2);
  // Clone the LFO phases/state, then reuse its external pool sequentially.
  // Explicitly reapplying unchanged modulation must be redundant after a seed
  // change; otherwise presets inherit the previous program's modulation.
  auto expected = actual;
  actual.SetSeed(98765);
  actual.SetCrossSeed(0.7);
  expected.SetSeed(98765);
  expected.SetCrossSeed(0.7);
  expected.SetModAmount(80);
  expected.SetModRate(2);
  std::vector<float> output(4800);
  float input[kBlock] = {};
  actual.ClearBuffers();
  for (int pos = 0; pos < 4800; pos += kBlock) {
    input[0] = pos == 0 ? 1 : 0;
    actual.Process(input, kBlock);
    for (int i = 0; i < kBlock; i++) output[pos + i] = actual.output()[i];
  }
  expected.ClearBuffers();
  bool same = true;
  for (int pos = 0; pos < 4800; pos += kBlock) {
    input[0] = pos == 0 ? 1 : 0;
    expected.Process(input, kBlock);
    for (int i = 0; i < kBlock; i++)
      same &= output[pos + i] == expected.output()[i];
  }
  Check(same, "seed changes refresh modulation amount and rate");
}

void TestFreeze() {
  Fixture a, b;
  a.Load(presets::kNoiseInTheHallway, true);
  b.Load(presets::kNoiseInTheHallway, true);
  a.reverb.SetParameter(Parameter::LineDecay, 0.3);
  b.reverb.SetParameter(Parameter::LineDecay, 0.3);
  float input[kBlock], silence[kBlock] = {}, al[kBlock], ar[kBlock], bl[kBlock],
                       br[kBlock];
  uint32_t rng = 99;
  double held = 0, released = 0;
  bool same = true;
  for (int block = 0; block < 6000; block++) {
    if (block == 1000) {
      a.reverb.SetFrozen(true);
      b.reverb.SetFrozen(true);
    }
    if (block == 4000) {
      a.reverb.SetFrozen(false);
      b.reverb.SetFrozen(false);
    }
    for (float& x : input) x = Noise(rng);
    a.reverb.Process(block < 4000 ? input : silence,
                     block < 4000 ? input : silence, al, ar, kBlock);
    b.reverb.Process(block < 1000 ? input : silence,
                     block < 1000 ? input : silence, bl, br, kBlock);
    for (int i = 0; i < kBlock; i++) {
      if (same && (al[i] != bl[i] || ar[i] != br[i]))
        std::fprintf(stderr, "freeze input first differs at sample %d\n",
                     block * kBlock + i);
      same &= al[i] == bl[i] && ar[i] == br[i];
      if (block >= 3000 && block < 4000) held += double(al[i]) * al[i];
      if (block >= 5000) released += double(al[i]) * al[i];
    }
  }
  Check(same, "new input cannot enter a frozen reverb");
  Check(held > 1e-5 && released < held * 0.01, "freeze holds then releases");
}

void TestPresets() {
  Fixture f;
  uint32_t rng = 1;
  float input[kBlock], left[kBlock], right[kBlock];
  for (auto preset : kPresets) {
    f.Load(*preset);
    const int wanted =
        1 +
        static_cast<int>(preset->values[int(Parameter::LineCount)] * 11.999);
    Check(f.reverb.line_count() == (wanted < kMaxLines ? wanted : kMaxLines),
          "preset line count clamps to firmware budget");
    bool finite = true;
    for (int block = 0; block < 2000; block++) {
      // Endpoints and modulation extremes with continuous CV-like updates.
      if (block % 20 == 0) {
        const double value = (block % 100) / 80.0;
        f.reverb.SetParameter(Parameter::LineDecay, value);
        f.reverb.SetParameter(Parameter::PostCutoffFrequency, 1 - value);
      }
      if (block % 200 == 0) f.reverb.SetFrozen((block / 200) % 2);
      for (float& x : input) x = block < 1500 ? Noise(rng) : 0.f;
      f.reverb.Process(input, input, left, right, kBlock);
      for (int i = 0; i < kBlock; i++)
        finite &= std::isfinite(left[i]) && std::isfinite(right[i]);
    }
    Check(finite, "preset automation stays finite");
    f.reverb.SetFrozen(false);
    f.reverb.ClearBuffers();
    for (float& x : input) x = 0;
    bool silent = true;
    for (int block = 0; block < 200; block++) {
      f.reverb.Process(input, input, left, right, kBlock);
      for (int i = 0; i < kBlock; i++) silent &= left[i] == 0 && right[i] == 0;
    }
    Check(silent, "preset clear leaves no old tail");
  }
}
#if CLOUDSEED_STAGED_MEMORY
void TestStaging() {
  // Serving rings from staging memory (windows and block buffers moved by a
  // transport that defers its copies until Wait) must preserve every bit:
  // with memory for everything, for a part of every program, and for
  // nothing, through ring wraps, pot changes, freezes and reloads with
  // fewer lines.
  const size_t memories[] = {65536, 8192, 100};
  {
    for (size_t memory_floats : memories) {
      Fixture reference;
      Fixture staged;
      std::vector<float> ra(114688), rb(61440), sa(114688), sb(61440);
      reference.reverb.AddFastPool(ra.data(), ra.size());
      reference.reverb.AddFastPool(rb.data(), rb.size());
      staged.reverb.AddFastPool(sa.data(), sa.size());
      staged.reverb.AddFastPool(sb.data(), sb.size());
      std::vector<float> tcm_a(memory_floats + 2, 777.f),
          tcm_b(memory_floats / 2 + 2, 777.f);
      QueuedTransport transport;
      Staging<QueuedTransport> staging;
      staging.Init(&transport);
      staging.AddMemory(tcm_a.data() + 1, memory_floats);
      staging.AddMemory(tcm_b.data() + 1, memory_floats / 2);
      uint32_t rng = 11;
      int total_staged = 0;
      for (int p = 0; p < kPresetCount; p++) {
        const presets::Preset& preset = *kPresets[p];
        for (int reload = 0; reload < 2; reload++) {
          staging.Unplan();
          reference.Load(preset);
          staged.Load(preset);
          if (reload == 1) {
            // The firmware reloads an overloaded program with fewer lines.
            for (Fixture* f : {&reference, &staged}) {
              f->reverb.SetParameter(Parameter::LineCount,
                                     f->reverb.line_count() > 1 ? 0.0 : 1.0);
              f->reverb.PlaceBuffers();
              f->reverb.ClearBuffers();
            }
          }
          Check(staging.Plan(staged.reverb), "staging plan succeeds");
          Check(!staging.failed(), "staging plan does not fail");
          if (memory_floats == 65536) {
            // Only rings whose delay is too short for a window stay behind
            // (a pre-delay of 0, the shortest early stages).
            Check(staging.unstaged_buffers() <= 6,
                  "large staging memory stages every eligible ring");
          } else if (memory_floats == 8192 && reload == 0) {
            // Rings in the Init() memory (the SDRAM on the Seed) are staged
            // first: with 9 KB of staging memory, no late line of a program
            // with unplaced lines may stay behind while pool rings are staged.
            for (int c = 0; c < 2; c++) {
              ReverbChannel& ch = staged.reverb.channel(c);
              Placement list[ReverbChannel::kMaxPlacements];
              const int count =
                  ch.CollectPlacements(list, ReverbChannel::kMaxPlacements);
              bool pool_staged = false, home_unstaged = false;
              for (int i = 0; i < count; i++) {
                if (list[i].kind != Placement::Kind::kDelay) continue;
                auto* d = static_cast<ModulatedDelay*>(list[i].object);
                if (d->delay() < 2 * 2 * kBlock + 16) continue;  // ineligible
                if (list[i].placed && d->staged()) pool_staged = true;
                if (!list[i].placed && !d->staged()) home_unstaged = true;
              }
              Check(!(pool_staged && home_unstaged),
                    "rings in the Init() memory are staged before pool rings");
            }
          }
          Check(staging.memory_used() <= memory_floats + memory_floats / 2,
                "staging stays inside its memory");
          total_staged += staging.staged_buffers();
          float input[kBlock], rl[kBlock], rr[kBlock], sl[kBlock], sr[kBlock];
          bool same = true;
          const int blocks = reload == 0 ? 2500 : 500;
          for (int block = 0; block < blocks; block++) {
            if (block % 30 == 0) {
              const double value = (block % 90) / 80.0;
              for (Fixture* f : {&reference, &staged}) {
                f->reverb.SetParameter(Parameter::LineDecay, value);
                f->reverb.SetParameter(Parameter::PostCutoffFrequency,
                                       1 - value);
              }
            }
            if (block % 150 == 0) {
              reference.reverb.SetFrozen((block / 150) % 2);
              staged.reverb.SetFrozen((block / 150) % 2);
            }
            for (float& x : input) x = block < 500 ? Noise(rng) : 0.f;
            reference.reverb.Process(input, input, rl, rr, kBlock);
            Check(staging.Begin(kBlock), "staged windows are ready");
            staged.reverb.Process(input, input, sl, sr, kBlock);
            Check(staging.End(), "staged write-back is submitted");
            for (int i = 0; i < kBlock; i++)
              same &= rl[i] == sl[i] && rr[i] == sr[i];
          }
          Check(same, "staged output is bit-identical to the ring output");
          Check(!staging.failed(), "staging runs without transport failure");
          Check(!transport.bad_lifetime,
                "pending transport list is never rebuilt");
        }
      }
      Check(tcm_a.front() == 777.f && tcm_a.back() == 777.f &&
                tcm_b.front() == 777.f && tcm_b.back() == 777.f,
            "staging memory guards survive");
      if (memory_floats == 100) {
        // At most a multitap whose taps all read the input history.
        Check(total_staged <= kPresetCount * 2 * 2,
              "tiny staging memory stages nothing else");
      } else {
        Check(total_staged > 0, "staging memory stages a part");
      }
      staging.Unplan();
    }
  }
}

void TestStagingFailures() {
  {
    for (auto failure : {QueuedTransport::Failure::kCopy,
                         QueuedTransport::Failure::kCommit,
                         QueuedTransport::Failure::kWait}) {
      Fixture f;
      f.Load(presets::kMediumSpace);
      std::vector<float> tcm(24000, std::numeric_limits<float>::quiet_NaN());
      QueuedTransport transport;
      Staging<QueuedTransport> s;
      s.Init(&transport);
      s.AddMemory(tcm.data(), tcm.size());
      transport.failure = failure;
      Check(!s.Plan(f.reverb) && s.failed() && !s.planned(),
            "failed prime is rejected without attaching invalid windows");
      if (failure == QueuedTransport::Failure::kCopy)
        Check(transport.commits == 0, "partial list is never committed");
      Check(s.Unplan(), "idle failed transport releases its plan");

      transport.failure = QueuedTransport::Failure::kNone;
      Check(s.Plan(f.reverb), "clean plan after rejected prime");
      float input[kBlock] = {}, left[kBlock], right[kBlock];
      Check(s.Begin(kBlock), "first windows ready");
      f.reverb.Process(input, input, left, right, kBlock);
      Check(s.End(), "first block submitted");
      transport.failure = failure;
      bool ok = s.Begin(kBlock);
      if (ok) {
        f.reverb.Process(input, input, left, right, kBlock);
        ok = s.End();
      }
      Check(!ok && s.failed(), "runtime transport failure reaches caller");
      Check(s.Unplan(), "runtime failure detaches after transport stops");
      Check(!transport.bad_lifetime, "failure never rebuilds an active list");
    }
  }
}

void TestRejectedActiveCommit() {
  for (bool prime : {false, true}) {
    Fixture f;
    f.Load(presets::kMediumSpace);
    std::vector<float> tcm(24000);
    QueuedTransport transport;
    Staging<QueuedTransport> s;
    s.Init(&transport);
    s.AddMemory(tcm.data(), tcm.size());
    if (!prime) Check(s.Plan(f.reverb), "active-commit test plan primes");
    transport.failure = QueuedTransport::Failure::kActiveCommit;
    transport.hold_pending = true;
    if (prime) {
      Check(!s.Plan(f.reverb), "active channel rejects prime commit");
    } else {
      float input[kBlock] = {}, left[kBlock], right[kBlock];
      Check(s.Begin(kBlock), "active-commit test first block begins");
      f.reverb.Process(input, input, left, right, kBlock);
      Check(!s.End(), "active channel rejects runtime commit");
    }
    Check(s.failed() && !transport.Idle(),
          "rejected commit still owns transport memory");
    const size_t used = s.memory_used();
    Check(used > 0, "rejected commit has allocated windows");
    for (int retry = 0; retry < 3; retry++) {
      Check(!s.Unplan() && s.memory_used() == used,
            "rejected active commit keeps windows until abort completes");
      Check(!s.Plan(f.reverb) && s.memory_used() == used,
            "replanning cannot reuse memory held by rejected commit");
    }
    transport.hold_pending = false;
    Check(s.Unplan() && transport.Idle() && s.memory_used() == 0,
          "completed abort releases rejected commit memory");
    Check(!transport.bad_lifetime, "active commit never rebuilds live list");
  }
}

void TestLongStagedHeads() {
  // A physical delay much larger than the staging allocation is valid:
  // only lead - delay is a window offset. Exercise all three kernels and
  // both full and partial LFO runs with an independent sample oracle.
  constexpr int kDelay = 20000;
  std::vector<float> ring(kDelay + 48);
  for (int count : {1, 7, 8, 47, 48}) {
    float window[64], block[kBlock + 2], input[kBlock], output[kBlock];
    for (int i = 0; i < 64; i++) window[i] = static_cast<float>(i);
    for (int i = 0; i < kBlock; i++) input[i] = 100.f + i;
    block[0] = block[kBlock + 1] = 777.f;
    const StagedHead head = {window, kDelay + 4, 64, block + 1};
    for (int mode = 0; mode < 3; mode++) {
      MemoryPool pool;
      pool.Init(ring.data(), ring.size());
      ModulatedAllpass ap;
      Check(ap.Init(pool, ring.size(), kDelay, .25f), "long staged allpass init");
      ap.modulation_enabled = mode != 0;
      ap.interpolation_enabled = mode == 2;
      ap.mod_amount = .25f;
      ap.feedback = .5f;
      ap.ClearBuffers();
      ap.SetStaged(&head);
      ap.Process(input, output, count);
      for (int i = 0; i < count; i++) {
        const float read = window[4 + i] - (mode == 2 ? .25f : 0.f);
        const float written = input[i] + read * .5f;
        Check(output[i] == read - written * .5f && block[i + 1] == written,
              "long staged allpass reads only its small window");
      }
    }
    MemoryPool pool;
    pool.Init(ring.data(), ring.size());
    ModulatedDelay delay;
    Check(delay.Init(pool, ring.size(), kDelay, .25f), "long staged delay init");
    delay.mod_amount = .25f;
    delay.ClearBuffers();
    delay.SetStaged(&head);
    delay.Process(input, count);
    for (int i = 0; i < count; i++)
      Check(delay.output()[i] == window[4 + i] - .25f &&
                block[i + 1] == input[i],
            "long staged delay reads only its small window");
    Check(block[0] == 777.f && block[kBlock + 1] == 777.f,
          "long staged head preserves write guards");
  }
}

void TestStagedBlockSizes() {
  // Blocks shorter than the update rate, and blocks that do not end on an
  // update, take the staged heads through every run shape (a partial first
  // run, runs of the update rate, a partial last run) against the ring.
  Fixture reference;
  Fixture staged;
  std::vector<float> ra(114688), rb(61440), sa(114688), sb(61440);
  reference.reverb.AddFastPool(ra.data(), ra.size());
  reference.reverb.AddFastPool(rb.data(), rb.size());
  staged.reverb.AddFastPool(sa.data(), sa.size());
  staged.reverb.AddFastPool(sb.data(), sb.size());
  std::vector<float> tcm(65536);
  QueuedTransport transport;
  Staging<QueuedTransport> staging;
  staging.Init(&transport);
  staging.AddMemory(tcm.data(), tcm.size());
  const int counts[] = {48, 7, 1, 13, 8, 47, 48, 24, 3, 16, 48, 5};
  uint32_t rng = 23;
  for (const presets::Preset* preset :
       {&presets::kHyperplane, &presets::kThroughTheLookingGlass}) {
    staging.Unplan();
    reference.Load(*preset);
    staged.Load(*preset);
    Check(staging.Plan(staged.reverb), "staging plan for block sizes");
    float input[kBlock], rl[kBlock], rr[kBlock], sl[kBlock], sr[kBlock];
    bool same = true;
    for (int block = 0; block < 600; block++) {
      const int count = counts[block % 12];
      for (int i = 0; i < count; i++) input[i] = block < 300 ? Noise(rng) : 0.f;
      reference.reverb.Process(input, input, rl, rr, count);
      Check(staging.Begin(count), "staged windows are ready (block sizes)");
      staged.reverb.Process(input, input, sl, sr, count);
      Check(staging.End(), "staged write-back is submitted (block sizes)");
      for (int i = 0; i < count; i++) same &= rl[i] == sl[i] && rr[i] == sr[i];
    }
    Check(same, "staged output matches the ring for every block size");
    Check(!staging.failed(), "block sizes run without transport failure");
  }
  staging.Unplan();
}

void TestStagingAllocationFailure() {
  Fixture f;
  f.Load(presets::kSmallRoom);
  // The block pool needs kPools * kPoolBlocks * kBlock floats in one area;
  // the aggregate capacity suffices, but neither area holds it.
  using CpuStaging = Staging<CpuStagingTransport>;
  constexpr size_t kPool = CpuStaging::kPools * CpuStaging::kPoolBlocks * kBlock;
  std::vector<float> a(kPool - 8), b(kPool - 16);
  CpuStagingTransport transport;
  CpuStaging staging;
  staging.Init(&transport);
  staging.AddMemory(a.data(), a.size());
  staging.AddMemory(b.data(), b.size());
  Check(staging.Plan(f.reverb), "fragmented staging pool keeps ring fallback");
  Check(staging.staged_buffers() == 0 && staging.memory_used() == 0,
        "rejected staging entry releases all partial allocations");
  Check(staging.Unplan(), "fragmented plan releases cleanly");
}
#endif
}  // namespace

int main() {
  cloudseed::FastSin::Init();
  TestLowerRates();
  TestInvalidParameters();
  TestFilters();
  TestOnePoleBlockForms();
  TestDarkPlateShelves();
  TestPool();
  TestDelayBoundaries();
  TestCompactTaps();
  TestAllpassRebind();
  TestPlacementHistory();
  TestPlacement();
#if CLOUDSEED_STAGED_MEMORY
  TestStaging();
  TestStagingFailures();
  TestRejectedActiveCommit();
  TestLongStagedHeads();
  TestStagedBlockSizes();
  TestStagingAllocationFailure();
#endif
  TestBlockFeedback();
  TestSeedRefresh();
  TestFreeze();
  TestPresets();
  std::printf("%d-line port regression (staged kernels=%d): %d failures\n",
              cloudseed::kMaxLines, CLOUDSEED_STAGED_MEMORY, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
