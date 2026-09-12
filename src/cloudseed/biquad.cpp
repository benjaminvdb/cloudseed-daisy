#include "biquad.h"

#include <math.h>

#include "fdlibm_trig.h"

namespace cloudseed {

void Biquad::Init(FilterType filter_type, double sample_rate) {
  type = filter_type;
  sample_rate_ = sample_rate;
  SetGainDb(0.0);
  frequency = sample_rate / 4;
  SetQ(0.5);
  ClearBuffers();
  Update();
}

void Biquad::SetSamplerate(double sample_rate) {
  sample_rate_ = sample_rate;
  Update();
}

double Biquad::GetGainDb() const { return log10(gain_) * 20; }

void Biquad::SetGainDb(double value) { SetGain(exp(value * (M_LN10 / 20))); }

void Biquad::SetGain(double value) {
  if (value == 0) value = 0.001;  // -60dB
  gain_ = value;
}

void Biquad::SetQ(double value) {
  if (value == 0) value = 1e-12;
  q_ = value;
}

void Biquad::Update() {
  // Presets specify up to 20 kHz even at lower audio rates. Above Nyquist
  // the cookbook's sin(omega) can be negative, moving poles outside the
  // unit circle. Keep the requested frequency for a later rate change.
  const double hz = frequency >= sample_rate_ * 0.5
                        ? sample_rate_ * 0.499 : frequency;
  const double omega = 2 * M_PI * hz / sample_rate_;
  const double sin_omega = trig::Sin(omega);
  const double cos_omega = trig::Cos(omega);
  const double gain = gain_;

  double sqrt_gain = 0.0;
  double alpha = 0.0;

  if (type == FilterType::LowShelf || type == FilterType::HighShelf) {
    alpha = sin_omega / 2 * sqrt((gain + 1 / gain) * (1 / slope - 1) + 2);
    sqrt_gain = sqrt(gain);
  } else {
    alpha = sin_omega / (2 * q_);
  }

  double a0 = 1, a1 = 0, a2 = 0, b0 = 1, b1 = 0, b2 = 0;

  switch (type) {
    case FilterType::LowPass:
      b0 = (1 - cos_omega) / 2;
      b1 = 1 - cos_omega;
      b2 = (1 - cos_omega) / 2;
      a0 = 1 + alpha;
      a1 = -2 * cos_omega;
      a2 = 1 - alpha;
      break;
    case FilterType::HighPass:
      b0 = (1 + cos_omega) / 2;
      b1 = -(1 + cos_omega);
      b2 = (1 + cos_omega) / 2;
      a0 = 1 + alpha;
      a1 = -2 * cos_omega;
      a2 = 1 - alpha;
      break;
    case FilterType::BandPass:
      b0 = alpha;
      b1 = 0;
      b2 = -alpha;
      a0 = 1 + alpha;
      a1 = -2 * cos_omega;
      a2 = 1 - alpha;
      break;
    case FilterType::Notch:
      b0 = 1;
      b1 = -2 * cos_omega;
      b2 = 1;
      a0 = 1 + alpha;
      a1 = -2 * cos_omega;
      a2 = 1 - alpha;
      break;
    case FilterType::Peak:
      b0 = 1 + (alpha * gain);
      b1 = -2 * cos_omega;
      b2 = 1 - (alpha * gain);
      a0 = 1 + (alpha / gain);
      a1 = -2 * cos_omega;
      a2 = 1 - (alpha / gain);
      break;
    case FilterType::LowShelf:
      b0 = gain * ((gain + 1) - (gain - 1) * cos_omega + 2 * sqrt_gain * alpha);
      b1 = 2 * gain * ((gain - 1) - (gain + 1) * cos_omega);
      b2 = gain * ((gain + 1) - (gain - 1) * cos_omega - 2 * sqrt_gain * alpha);
      a0 = (gain + 1) + (gain - 1) * cos_omega + 2 * sqrt_gain * alpha;
      a1 = -2 * ((gain - 1) + (gain + 1) * cos_omega);
      a2 = (gain + 1) + (gain - 1) * cos_omega - 2 * sqrt_gain * alpha;
      break;
    case FilterType::HighShelf:
      b0 = gain * ((gain + 1) + (gain - 1) * cos_omega + 2 * sqrt_gain * alpha);
      b1 = -2 * gain * ((gain - 1) + (gain + 1) * cos_omega);
      b2 = gain * ((gain + 1) + (gain - 1) * cos_omega - 2 * sqrt_gain * alpha);
      a0 = (gain + 1) - (gain - 1) * cos_omega + 2 * sqrt_gain * alpha;
      a1 = 2 * ((gain - 1) - (gain + 1) * cos_omega);
      a2 = (gain + 1) - (gain - 1) * cos_omega - 2 * sqrt_gain * alpha;
      break;
  }

  const double g = 1 / a0;
  b0_ = static_cast<float>(b0 * g);
  b1_ = static_cast<float>(b1 * g);
  b2_ = static_cast<float>(b2 * g);
  a1_ = static_cast<float>(a1 * g);
  a2_ = static_cast<float>(a2 * g);
}

void Biquad::ClearBuffers() {
  x1_ = 0.f;
  x2_ = 0.f;
  y1_ = 0.f;
  y2_ = 0.f;
}

}  // namespace cloudseed
