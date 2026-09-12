// Compares the Löwenzahnhonig port (single precision, dsp/) with the
// corrected legacy reference (CloudSeed.Native, double precision) on the
// same input. See prepare_reference.py for explicit reference corrections.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

// Original
#include "AudioLib/ValueTables.h"
#include "FastSin.h"
#include "ReverbController.h"

// Port
#include "cloudseed/fast_sin.h"
#include "cloudseed/memory_pool.h"
#include "cloudseed/reverb_controller.h"
#include "cloudseed/presets.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 48;
constexpr int kSeconds = 4;
constexpr int kLength = kSampleRate * kSeconds;

struct Stereo {
  std::vector<float> l, r;
};

void MakeInput(std::vector<float>& l, std::vector<float>& r) {
  l.assign(kLength, 0.f);
  r.assign(kLength, 0.f);
  l[100] = 0.5f;
  r[100] = 0.5f;
  uint32_t state = 12345;
  for (int i = kSampleRate / 2; i < kSampleRate; i++) {
    state = state * 1664525u + 1013904223u;
    float n = (static_cast<float>(state >> 8) / 16777216.f) * 2.f - 1.f;
    l[i] = 0.25f * n;
    state = state * 1664525u + 1013904223u;
    n = (static_cast<float>(state >> 8) / 16777216.f) * 2.f - 1.f;
    r[i] = 0.25f * n;
  }
}

Stereo RunReference(const double* values, const std::vector<float>& in_l,
                    const std::vector<float>& in_r) {
  auto* rev = new CloudSeed::ReverbController(kSampleRate);
  for (int i = 0; i < (int)Parameter::Count; i++) {
    double value = values[i];
    if (i == static_cast<int>(Parameter::LineCount)) {
      const int lines = 1 + static_cast<int>(value * 11.999);
      if (lines > cloudseed::kMaxLines)
        value = (cloudseed::kMaxLines - 0.5) / 11.999;
    }
    rev->SetParameter(static_cast<Parameter>(i), value);
  }
  rev->ClearBuffers();

  Stereo out;
  out.l.assign(kLength, 0.f);
  out.r.assign(kLength, 0.f);
  double bl[kBlock], br[kBlock], ol[kBlock], orr[kBlock];
  double* ins[2] = {bl, br};
  double* outs[2] = {ol, orr};
  for (int pos = 0; pos + kBlock <= kLength; pos += kBlock) {
    for (int i = 0; i < kBlock; i++) {
      bl[i] = in_l[pos + i];
      br[i] = in_r[pos + i];
    }
    rev->Process(ins, outs, kBlock);
    for (int i = 0; i < kBlock; i++) {
      out.l[pos + i] = static_cast<float>(ol[i]);
      out.r[pos + i] = static_cast<float>(orr[i]);
    }
  }
  delete rev;
  return out;
}

Stereo RunPort(const double* values, const std::vector<float>& in_l,
               const std::vector<float>& in_r, bool sweep_size = false) {
  const size_t floats =
      cloudseed::ReverbController::RequiredPoolFloats(kSampleRate);
  std::vector<float> pool(floats, 12345.f);  // garbage, like uncleared SDRAM
  cloudseed::MemoryPool memory;
  memory.Init(pool.data(), floats);
  auto* rev = new cloudseed::ReverbController();
  if (!rev->Init(kSampleRate, memory)) {
    fprintf(stderr, "port: Init failed (pool %zu floats, used %zu)\n", floats,
            memory.used());
    exit(1);
  }
  // The Daisy firmware's internal-RAM pools (448 KB + 240 KB), so that the
  // placed configuration is what is compared with the reference.
  static std::vector<float> fast_a(114688), fast_b(61440);
  rev->AddFastPool(fast_a.data(), fast_a.size());
  rev->AddFastPool(fast_b.data(), fast_b.size());
  rev->LoadPreset(values);
  rev->ClearBuffers();

  Stereo out;
  out.l.assign(kLength, 0.f);
  out.r.assign(kLength, 0.f);
  for (int pos = 0; pos + kBlock <= kLength; pos += kBlock) {
    if (sweep_size) {
      // Exercise DSP delay/decay/tone automation (delay is not a panel pot).
      double t = static_cast<double>(pos) / kLength;
      rev->SetParameter(cloudseed::Parameter::LineDelay,
                        0.5 + 0.5 * std::sin(t * 20.0));
      rev->SetParameter(cloudseed::Parameter::LineDecay,
                        0.5 + 0.5 * std::sin(t * 7.0));
      rev->SetParameter(cloudseed::Parameter::PostCutoffFrequency,
                        0.5 + 0.5 * std::cos(t * 13.0));
    }
    rev->Process(&in_l[pos], &in_r[pos], &out.l[pos], &out.r[pos], kBlock);
  }
  delete rev;
  return out;
}

double Rms(const std::vector<float>& a, int from, int to) {
  double acc = 0;
  for (int i = from; i < to; i++) acc += (double)a[i] * a[i];
  return std::sqrt(acc / (to - from));
}

bool Compare(const char* name, const Stereo& ref, const Stereo& port, int lines,
             bool modulation_off) {
  double max_diff = 0, ref_rms = 0, diff_rms = 0, env_dev = 0;
  bool finite = true;
  for (int ch = 0; ch < 2; ch++) {
    const std::vector<float>& a = ch ? ref.r : ref.l;
    const std::vector<float>& b = ch ? port.r : port.l;
    double sa = 0, sd = 0;
    for (int i = 0; i < kLength; i++) {
      if (!std::isfinite(a[i]) || !std::isfinite(b[i])) finite = false;
      double d = (double)a[i] - b[i];
      sa += (double)a[i] * a[i];
      sd += d * d;
      if (std::fabs(d) > max_diff) max_diff = std::fabs(d);
    }
    ref_rms += std::sqrt(sa / kLength) / 2;
    diff_rms += std::sqrt(sd / kLength) / 2;
    // Energy envelope in 100 ms windows (for the modulated presets, whose
    // LFO start phases differ between the two implementations).
    const int win = kSampleRate / 10;
    for (int w = 0; w + win <= kLength; w += win) {
      double ea = Rms(a, w, w + win), eb = Rms(b, w, w + win);
      if (ea > 1e-4) {
        double dev = std::fabs(20 * std::log10(eb / ea));
        if (dev > env_dev) env_dev = dev;
      }
    }
  }
  printf(
      "%-28s lines=%2d  ref_rms=%.4f  diff_rms=%.2e (%.1f dB)  max_diff=%.2e  "
      "envelope_dev=%.2f dB  %s\n",
      name, lines, ref_rms, diff_rms,
      20 * std::log10(diff_rms / ref_rms + 1e-30), max_diff, env_dev,
      finite ? "finite" : "NOT FINITE");
  // Float roundoff at low shelf frequencies dominates the deterministic
  // comparison. Modulated signals differ in phase; compare their envelopes.
  return finite && ref_rms > 0 &&
         (modulation_off ? diff_rms / ref_rms < 2e-4 : env_dev < 6.0);
}

}  // namespace

int main(int argc, char** argv) {
  int failures = 0;
  const bool modulation_off = argc > 1 && std::strcmp(argv[1], "nomod") == 0;
  AudioLib::ValueTables::Init();
  CloudSeed::FastSin::Init();
  cloudseed::FastSin::Init();
  std::srand(1);

  std::vector<float> in_l, in_r;
  MakeInput(in_l, in_r);

  using cloudseed::presets::Preset;
  const Preset* all[] = {
      &cloudseed::presets::kNoiseInTheHallway,
      &cloudseed::presets::kSmallRoom,
      &cloudseed::presets::kMediumSpace,
      &cloudseed::presets::kRubiKaFields,
      &cloudseed::presets::kHyperplane,
      &cloudseed::presets::kChorusDelay,
      &cloudseed::presets::kDullEchoes,
      &cloudseed::presets::kThe90sAreBack,
      &cloudseed::presets::kThroughTheLookingGlass,
      &cloudseed::presets::kDarkPlate,
  };

  printf(
      "port compiled with CLOUDSEED_MAX_LINES=%d, %d s of audio, impulse + "
      "noise burst\n",
      cloudseed::kMaxLines, kSeconds);
  for (const Preset* p : all) {
    std::vector<double> values(p->values,
                               p->values + cloudseed::kParameterCount);
    if (modulation_off) {
      // Disable all modulation so that the two implementations are
      // deterministic relative to each other.
      values[(int)cloudseed::Parameter::EarlyDiffusionModAmount] = 0;
      values[(int)cloudseed::Parameter::LineModAmount] = 0;
      values[(int)cloudseed::Parameter::LateDiffusionModAmount] = 0;
    }
    Stereo ref = RunReference(values.data(), in_l, in_r);
    Stereo port = RunPort(values.data(), in_l, in_r);
    int lines =
        1 + (int)(values[(int)cloudseed::Parameter::LineCount] * 11.999);
    if (lines > cloudseed::kMaxLines) lines = cloudseed::kMaxLines;
    if (!Compare(p->name, ref, port, lines, modulation_off)) failures++;
  }

  // Freeze: after the gate goes high the tail must hold its level (no decay)
  // and new input must not enter the reverb; after release it must decay.
  {
    const size_t floats =
        cloudseed::ReverbController::RequiredPoolFloats(kSampleRate);
    std::vector<float> pool(floats, 0.f);
    cloudseed::MemoryPool memory;
    memory.Init(pool.data(), floats);
    cloudseed::ReverbController rev;
    rev.Init(kSampleRate, memory);
    rev.LoadPreset(cloudseed::presets::kMediumSpace.values);
    rev.SetParameter(cloudseed::Parameter::DryOut, 0.0);
    rev.SetParameter(cloudseed::Parameter::LineDecay, 0.3);  // short tail
    rev.ClearBuffers();
    std::vector<float> zl(kLength, 0.f), zr(kLength, 0.f), ol(kLength, 0.f),
        orr(kLength, 0.f);
    // 0..0.5 s: noise burst; 0.5..3 s: frozen, with the burst repeated at
    // 1.5 s to check that it does not enter; 3..4 s: released.
    for (int i = 0; i < kSampleRate / 2; i++) {
      zl[i] = in_l[i + kSampleRate / 2];
      zr[i] = in_r[i + kSampleRate / 2];
      zl[i + kSampleRate * 3 / 2] = zl[i];
      zr[i + kSampleRate * 3 / 2] = zr[i];
    }
    for (int pos = 0; pos + kBlock <= kLength; pos += kBlock) {
      if (pos == kSampleRate / 2) rev.SetFrozen(true);
      if (pos == kSampleRate * 3) rev.SetFrozen(false);
      rev.Process(&zl[pos], &zr[pos], &ol[pos], &orr[pos], kBlock);
    }
    bool finite = true;
    for (int i = 0; i < kLength; i++)
      if (!std::isfinite(ol[i]) || !std::isfinite(orr[i])) finite = false;
    const double held_start = Rms(ol, kSampleRate * 1, kSampleRate * 1.25);
    const double held_end = Rms(ol, kSampleRate * 2.75, kSampleRate * 3);
    const double burst_in = Rms(ol, kSampleRate * 1.5, kSampleRate * 2);
    const double released = Rms(ol, kSampleRate * 3.75, kSampleRate * 4);
    if (!finite || !(held_start > 0) || !(held_end > held_start * 0.25) ||
        !(released < held_end * 0.1))
      failures++;
    printf(
        "freeze on Medium Space: %s, held tail %.4f -> %.4f rms (%.2f dB "
        "over 1.75 s), during repeated burst %.4f, 0.75 s after release "
        "%.4f\n",
        finite ? "finite" : "NOT FINITE", held_start, held_end,
        20 * std::log10(held_end / held_start), burst_in, released);
  }

  // Runtime parameter sweeps must never produce NaN or infinities.
  Stereo swept =
      RunPort(std::vector<double>(std::begin(cloudseed::presets::kMediumSpace.values),
                                  std::end(cloudseed::presets::kMediumSpace.values))
                  .data(),
              in_l, in_r, true);
  bool finite = true;
  double peak = 0;
  for (int i = 0; i < kLength; i++) {
    if (!std::isfinite(swept.l[i]) || !std::isfinite(swept.r[i]))
      finite = false;
    peak = std::fmax(peak,
                     std::fmax(std::fabs(swept.l[i]), std::fabs(swept.r[i])));
  }
  printf("size/decay/tone sweep on Medium Space: %s, peak %.3f\n",
         finite ? "finite" : "NOT FINITE", peak);
  if (!finite) failures++;
  if (failures) fprintf(stderr, "%d fidelity checks failed\n", failures);
  return failures ? 1 : 0;
}
