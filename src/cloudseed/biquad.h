#pragma once

namespace cloudseed {

// Direct form I biquad with the usual cookbook responses (CloudSeed's
// AudioLib::Biquad). Coefficients are computed in double precision and the
// signal path runs in single precision.
class Biquad {
 public:
  enum class FilterType {
    LowPass = 0,
    HighPass,
    BandPass,
    Notch,
    Peak,
    LowShelf,
    HighShelf
  };

  void Init(FilterType filter_type, double sample_rate);

  void SetSamplerate(double sample_rate);
  double GetGainDb() const;
  void SetGainDb(double value);
  double GetGain() const { return gain_; }
  void SetGain(double value);
  double GetQ() const { return q_; }
  void SetQ(double value);

  // Recomputes the coefficients from type, frequency, slope, gain and Q.
  void Update();

  inline float Process(float x) {
    const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    y2_ = y1_;
    x1_ = x;
    y1_ = y;
    return y;
  }

  // Block form with the coefficients and the state in locals: a store into
  // output could alias the members, which would make GCC reload them after
  // every sample. Input and output may be the same buffer. Out of line:
  // one copy of the loop per image, not one per call site.
  __attribute__((noinline)) void Process(const float* input, float* output,
                                         int len) {
    const float b0 = b0_, b1 = b1_, b2 = b2_, a1 = a1_, a2 = a2_;
    float x1 = x1_, x2 = x2_, y1 = y1_, y2 = y2_;
    for (int i = 0; i < len; i++) {
      const float x = input[i];
      const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      y2 = y1;
      x1 = x;
      y1 = y;
      output[i] = y;
    }
    x1_ = x1;
    x2_ = x2;
    y1_ = y1;
    y2_ = y2;
  }

  // Two filters over two blocks in one loop, each with the arithmetic of
  // Process(): a biquad's next output waits for the last one through the
  // FPU's dependent latencies (Cortex-M7, in order), and the other
  // filter's samples fill that wait. Four samples per iteration, written
  // out: the state then moves through the registers twice per iteration
  // instead of once per sample (a loop of one sample rotates the four
  // state values of each filter with eight register moves, as many issue
  // slots as the arithmetic takes). The arithmetic of every sample is
  // that of Process(). Input and output of a filter may be the same
  // buffer.
  __attribute__((noinline)) static void ProcessPair(Biquad& fa, Biquad& fb,
                                                    const float* input_a,
                                                    const float* input_b,
                                                    float* output_a,
                                                    float* output_b, int len) {
    const float b0a = fa.b0_, b1a = fa.b1_, b2a = fa.b2_, a1a = fa.a1_,
                a2a = fa.a2_;
    const float b0b = fb.b0_, b1b = fb.b1_, b2b = fb.b2_, a1b = fb.a1_,
                a2b = fb.a2_;
    float x1a = fa.x1_, x2a = fa.x2_, y1a = fa.y1_, y2a = fa.y2_;
    float x1b = fb.x1_, x2b = fb.x2_, y1b = fb.y1_, y2b = fb.y2_;
    int i = 0;
    for (; i + 3 < len; i += 4) {
      const float xa0 = input_a[i], xa1 = input_a[i + 1];
      const float xa2 = input_a[i + 2], xa3 = input_a[i + 3];
      const float xb0 = input_b[i], xb1 = input_b[i + 1];
      const float xb2 = input_b[i + 2], xb3 = input_b[i + 3];
      const float ya0 =
          b0a * xa0 + b1a * x1a + b2a * x2a - a1a * y1a - a2a * y2a;
      const float yb0 =
          b0b * xb0 + b1b * x1b + b2b * x2b - a1b * y1b - a2b * y2b;
      const float ya1 =
          b0a * xa1 + b1a * xa0 + b2a * x1a - a1a * ya0 - a2a * y1a;
      const float yb1 =
          b0b * xb1 + b1b * xb0 + b2b * x1b - a1b * yb0 - a2b * y1b;
      const float ya2 =
          b0a * xa2 + b1a * xa1 + b2a * xa0 - a1a * ya1 - a2a * ya0;
      const float yb2 =
          b0b * xb2 + b1b * xb1 + b2b * xb0 - a1b * yb1 - a2b * yb0;
      const float ya3 =
          b0a * xa3 + b1a * xa2 + b2a * xa1 - a1a * ya2 - a2a * ya1;
      const float yb3 =
          b0b * xb3 + b1b * xb2 + b2b * xb1 - a1b * yb2 - a2b * yb1;
      output_a[i] = ya0;
      output_a[i + 1] = ya1;
      output_a[i + 2] = ya2;
      output_a[i + 3] = ya3;
      output_b[i] = yb0;
      output_b[i + 1] = yb1;
      output_b[i + 2] = yb2;
      output_b[i + 3] = yb3;
      x2a = xa2;
      x1a = xa3;
      y2a = ya2;
      y1a = ya3;
      x2b = xb2;
      x1b = xb3;
      y2b = yb2;
      y1b = yb3;
    }
    for (; i < len; i++) {
      const float xa = input_a[i];
      const float xb = input_b[i];
      const float ya = b0a * xa + b1a * x1a + b2a * x2a - a1a * y1a - a2a * y2a;
      const float yb = b0b * xb + b1b * x1b + b2b * x2b - a1b * y1b - a2b * y2b;
      x2a = x1a;
      y2a = y1a;
      x1a = xa;
      y1a = ya;
      x2b = x1b;
      y2b = y1b;
      x1b = xb;
      y1b = yb;
      output_a[i] = ya;
      output_b[i] = yb;
    }
    fa.x1_ = x1a;
    fa.x2_ = x2a;
    fa.y1_ = y1a;
    fa.y2_ = y2a;
    fb.x1_ = x1b;
    fb.x2_ = x2b;
    fb.y1_ = y1b;
    fb.y2_ = y2b;
  }

  void ClearBuffers();

  FilterType type = FilterType::LowPass;
  double frequency = 1000.0;
  double slope = 1.0;

 private:
  double sample_rate_ = 48000.0;
  double q_ = 0.5;
  double gain_ = 1.0;
  float a1_ = 0.f, a2_ = 0.f, b0_ = 1.f, b1_ = 0.f, b2_ = 0.f;
  float x1_ = 0.f, x2_ = 0.f, y1_ = 0.f, y2_ = 0.f;
};

}  // namespace cloudseed
