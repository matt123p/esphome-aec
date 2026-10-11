#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::aec_speexdsp {

// Continuous, allocation-free reference alignment.  First-difference
// pre-whitening makes the bounded normalized correlation insensitive to
// speech spectral colour in the same spirit as GCC-PHAT, while avoiding an
// additional FFT in the audio task.  A slow delay slope is reported as clock
// drift; the integer delay output follows it one sample at a time.
class ReferenceDelayTracker {
 public:
  static constexpr size_t MAX_WINDOW = 2048;
  static constexpr int MAX_DELAY = 256;

  struct Estimate {
    bool valid{false};
    int delay_samples{0};
    float correlation{0};
    float confidence{0};
    float drift_ppm{0};
  };

  void configure(int initial_delay, int max_delay = MAX_DELAY,
                 uint32_t update_samples = 8000, size_t window = 1024) {
    max_delay_ = std::clamp(max_delay, 0, MAX_DELAY);
    update_samples_ = std::max<uint32_t>(window, update_samples);
    window_ = std::clamp<size_t>(window, 128, MAX_WINDOW - MAX_DELAY - 1);
    estimate_.delay_samples = std::clamp(initial_delay, 0, max_delay_);
  }

  void reset(int delay = 0) {
    const int maximum = max_delay_;
    const uint32_t update = update_samples_;
    const size_t window = window_;
    *this = ReferenceDelayTracker{};
    configure(delay, maximum, update, window);
  }

  const Estimate &estimate() const { return estimate_; }

  Estimate process(const int16_t *microphone, size_t mic_stride,
                   const int16_t *reference, size_t samples, bool excited) {
    if (microphone == nullptr || reference == nullptr || mic_stride == 0 ||
        samples == 0 || samples > 1024) {
      estimate_.valid = false;
      return estimate_;
    }
    for (size_t n = 0; n < samples; ++n) {
      mic_[position_] = microphone[n * mic_stride];
      ref_[position_] = reference[n];
      position_ = (position_ + 1) % mic_.size();
      filled_ = std::min(filled_ + 1, mic_.size());
    }
    since_update_ += static_cast<uint32_t>(samples);
    total_samples_ += samples;
    if (!excited) {
      estimate_.valid = false;
      return estimate_;
    }
    if (since_update_ < update_samples_ ||
        filled_ < window_ + static_cast<size_t>(max_delay_) + 1) {
      return estimate_;
    }
    since_update_ = 0;

    float best = -1.0f, second = -1.0f;
    int best_delay = estimate_.delay_samples;
    for (int delay = 0; delay <= max_delay_; ++delay) {
      double cross = 0, mic_energy = 0, ref_energy = 0;
      for (size_t i = 1; i < window_; ++i) {
        const size_t mic_index = ring_index_(window_ - i);
        const size_t mic_previous = ring_index_(window_ - i + 1);
        const size_t ref_index = ring_index_(window_ - i + delay);
        const size_t ref_previous = ring_index_(window_ - i + delay + 1);
        const double m = static_cast<double>(mic_[mic_index]) - mic_[mic_previous];
        const double r = static_cast<double>(ref_[ref_index]) - ref_[ref_previous];
        cross += m * r;
        mic_energy += m * m;
        ref_energy += r * r;
      }
      const float correlation = mic_energy > 1 && ref_energy > 1
                                    ? static_cast<float>(std::abs(cross) / std::sqrt(mic_energy * ref_energy))
                                    : 0.0f;
      if (correlation > best) {
        second = best;
        best = correlation;
        best_delay = delay;
      } else if (correlation > second && std::abs(delay - best_delay) > 1) {
        second = correlation;
      }
    }
    estimate_.correlation = std::max(0.0f, best);
    estimate_.confidence = best > 0 ? std::clamp((best - std::max(0.0f, second)) / best, 0.0f, 1.0f) : 0;
    estimate_.valid = best >= 0.35f && estimate_.confidence >= 0.015f;
    if (!estimate_.valid)
      return estimate_;

    const int previous = estimate_.delay_samples;
    const int delta = best_delay - previous;
    estimate_.delay_samples += std::clamp(delta, -1, 1);
    if (best >= 0.8f && estimate_.confidence >= 0.1f && std::abs(delta) > 8)
      estimate_.delay_samples = best_delay;
    if (last_valid_sample_ != 0 && total_samples_ > last_valid_sample_) {
      const float instantaneous = static_cast<float>(best_delay - last_best_delay_) * 1000000.0f /
                                  static_cast<float>(total_samples_ - last_valid_sample_);
      estimate_.drift_ppm = std::clamp(0.9f * estimate_.drift_ppm + 0.1f * instantaneous,
                                       -2000.0f, 2000.0f);
    }
    last_best_delay_ = best_delay;
    last_valid_sample_ = total_samples_;
    return estimate_;
  }

 private:
  size_t ring_index_(size_t samples_ago) const {
    return (position_ + mic_.size() - 1 - samples_ago) % mic_.size();
  }

  std::array<int16_t, MAX_WINDOW + MAX_DELAY + 2> mic_{}, ref_{};
  size_t position_{0}, filled_{0}, window_{1024};
  int max_delay_{MAX_DELAY}, last_best_delay_{0};
  uint32_t update_samples_{8000}, since_update_{0};
  uint64_t total_samples_{0}, last_valid_sample_{0};
  Estimate estimate_{};
};

}  // namespace esphome::aec_speexdsp
