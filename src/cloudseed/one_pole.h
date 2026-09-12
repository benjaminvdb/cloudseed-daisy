#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fdlibm_trig.h"
#include "utils.h"

namespace cloudseed {

// The one-pole filters keep the plugin's silence test: a zero input with
// the state below 1e-12 (in magnitude, see below) sets the state to zero
// instead of letting it decay for ever. The block loops test it on the
// values' bits (FloatBits): a sample is zero exactly when its bits without
// the sign are zero, and for a state that is not NaN the magnitude is below
// the threshold exactly when its bits without the sign are below the
// threshold's. Both tests then run in the integer pipeline; the float
// compares they replace each needed a flag transfer (VMRS) that waits for
// the filter's own arithmetic to finish, twice per sample (TECHNICAL.md,
// "DSP kernels"). The sample's bits come straight from memory (an integer
// load beside the float load, no register transfer); the state's are only
// looked at when the sample is zero. Process(float) keeps the float form
// as the reference the tests compare the block forms with.
static inline int32_t MagnitudeBits(const float* sample) {
  int32_t bits;
  memcpy(&bits, sample, sizeof bits);
  return bits & 0x7fffffff;
}
static inline bool SilentState(float state) {
  return (FloatBits(state) & 0x7fffffff) < FloatBits(1e-12f);
}

// First-order low-pass filter (CloudSeed's AudioLib::Lp1).
class Lp1 {
 public:
  struct Coefficients {
    float a1, b0;
  };

  static Coefficients Calculate(double hz, double sample_rate) {
    if (hz >= sample_rate * 0.5) hz = sample_rate * 0.499;
    const double x = 2 * M_PI * hz / sample_rate;
    const double nn = 2 - trig::Cos(x);
    const double alpha = nn - sqrt(nn * nn - 1);
    return {static_cast<float>(alpha), static_cast<float>(1 - alpha)};
  }

  // Coefficients can be shared by parallel filters; their state stays local.
  void SetCoefficients(Coefficients coefficients) {
    a1_ = coefficients.a1;
    b0_ = coefficients.b0;
  }

  void Init(double sample_rate) {
    fs_ = sample_rate;
    cutoff_ = sample_rate * 0.25;
    output = 0.f;
    Update();
  }

  void SetSamplerate(double sample_rate) { fs_ = sample_rate; }

  void SetCutoffHz(double hz) {
    cutoff_ = hz;
    Update();
  }

  void Update() { SetCoefficients(Calculate(cutoff_, fs_)); }

  inline float Process(float input) {
    // Test magnitude: the legacy signed comparison truncates negative tails.
    if (input == 0.f && fabsf(output) < 1e-12f) {
      output = 0.f;
    } else {
      output = b0_ * input + a1_ * output;
    }
    return output;
  }

  // Block form with the state in locals (see Biquad::Process) and the
  // silence test on the bits (see above); input and output may be the same
  // buffer.
  __attribute__((noinline)) void Process(const float* input, float* out,
                                         int len) {
    const float b0 = b0_;
    const float a1 = a1_;
    float y = output;
    for (int i = 0; i < len; i++) {
      const float x = input[i];
      float next = b0 * x + a1 * y;
      if (MagnitudeBits(input + i) == 0 && SilentState(y)) next = 0.f;
      y = next;
      out[i] = y;
    }
    output = y;
  }

  // Two filters over two blocks in one loop (see Biquad::ProcessPair), each
  // with the arithmetic of Process().
  __attribute__((noinline)) static void ProcessPair(Lp1& fa, Lp1& fb,
                                                    const float* input_a,
                                                    const float* input_b,
                                                    float* out_a, float* out_b,
                                                    int len) {
    const float b0a = fa.b0_, a1a = fa.a1_;
    const float b0b = fb.b0_, a1b = fb.a1_;
    float ya = fa.output;
    float yb = fb.output;
    for (int i = 0; i < len; i++) {
      const float xa = input_a[i];
      const float xb = input_b[i];
      float next_a = b0a * xa + a1a * ya;
      float next_b = b0b * xb + a1b * yb;
      if (MagnitudeBits(input_a + i) == 0 && SilentState(ya)) next_a = 0.f;
      if (MagnitudeBits(input_b + i) == 0 && SilentState(yb)) next_b = 0.f;
      ya = next_a;
      yb = next_b;
      out_a[i] = ya;
      out_b[i] = yb;
    }
    fa.output = ya;
    fb.output = yb;
  }

  float output = 0.f;

 private:
  double fs_ = 48000.0;
  double cutoff_ = 12000.0;
  float b0_ = 1.f;
  float a1_ = 0.f;
};

// First-order high-pass filter (CloudSeed's AudioLib::Hp1): the input minus
// a first-order low-pass of it.
class Hp1 {
 public:
  void Init(double sample_rate) {
    fs_ = sample_rate;
    cutoff_ = sample_rate * 0.25;
    output = 0.f;
    lp_out_ = 0.f;
    Update();
  }

  void SetSamplerate(double sample_rate) { fs_ = sample_rate; }

  void SetCutoffHz(double hz) {
    cutoff_ = hz;
    Update();
  }

  void Update() {
    if (cutoff_ >= fs_ * 0.5) cutoff_ = fs_ * 0.499;
    const double x = 2 * M_PI * cutoff_ / fs_;
    const double nn = (2 - trig::Cos(x));
    const double alpha = nn - sqrt(nn * nn - 1);
    a1_ = static_cast<float>(alpha);
    b0_ = static_cast<float>(1 - alpha);
  }

  inline float Process(float input) {
    if (input == 0.f && fabsf(lp_out_) < 1e-12f) {
      lp_out_ = 0.f;
      output = 0.f;
    } else {
      lp_out_ = b0_ * input + a1_ * lp_out_;
      output = input - lp_out_;
    }
    return output;
  }

  // Block form with the state in locals and the silence test on the bits
  // (see Lp1); input and output may be the same buffer.
  void Process(const float* input, float* out, int len) {
    const float b0 = b0_;
    const float a1 = a1_;
    float lp = lp_out_;
    float y = output;
    for (int i = 0; i < len; i++) {
      const float x = input[i];
      float next_lp = b0 * x + a1 * lp;
      float next_y = x - next_lp;
      if (MagnitudeBits(input + i) == 0 && SilentState(lp)) {
        next_lp = 0.f;
        next_y = 0.f;
      }
      lp = next_lp;
      y = next_y;
      out[i] = y;
    }
    lp_out_ = lp;
    output = y;
  }

  void ClearBuffers() {
    output = 0.f;
    lp_out_ = 0.f;
  }

  float output = 0.f;

 private:
  double fs_ = 48000.0;
  double cutoff_ = 12000.0;
  float b0_ = 1.f;
  float a1_ = 0.f;
  float lp_out_ = 0.f;
};

}  // namespace cloudseed
