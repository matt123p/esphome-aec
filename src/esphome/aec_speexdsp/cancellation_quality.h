#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace esphome::aec_speexdsp {
// Shadow measurements, NOT a double-talk classifier or an adaptation control.
// Input and residual must share the same beam steering, before NS/AGC.
class CancellationQuality {
 public:
  struct Snapshot {
    bool valid{false};
    float reduction_db{0}, residual_to_reference_db{0}, residual_correlation{0};
    int lag_samples{0};
    uint32_t active_ms{0};
    float reference_rms{0};
    float input_correlation{0};
  };
  const Snapshot &snapshot() const { return snapshot_; }
  void invalidate() { snapshot_ = {}; history_ = {}; position_ = filled_ = active_samples_ = 0; }
  void process(const int16_t *input, const int16_t *residual, const int16_t *reference,
               uint32_t samples, bool active, bool excited, bool healthy) {
    if (!healthy || samples == 0 || samples > 1024) { invalidate(); return; }
    snapshot_ = {};
    active_samples_ = active ? std::min<uint32_t>(active_samples_ + samples, 16000 * 3600) : 0;
    snapshot_.active_ms = active_samples_ / 16;
    // 0..64 samples (4 ms) causal lag search in 4-sample steps. This is a
    // bounded time-domain correlation diagnostic, NOT spectral coherence.
    struct Moments { double x{0}, y{0}, xx{0}, yy{0}, xy{0}, raw{0}, raw2{0}, xraw{0}; uint32_t n{0}; };
    std::array<Moments, 17> moments{};
    double raw_energy = 0, clean_energy = 0, ref_energy = 0;
    bool clipped = false;
    for (uint32_t n = 0; n < samples; ++n) {
      const double raw = input[n], clean = residual[n], ref = reference[n];
      clipped |= std::abs(raw) >= 32767 || std::abs(clean) >= 32767 || std::abs(ref) >= 32767;
      raw_energy += raw * raw; clean_energy += clean * clean; ref_energy += ref * ref;
      history_[position_] = reference[n];
      filled_ = std::min<uint32_t>(filled_ + 1, history_.size());
      if (n % 4 == 0 && filled_ == history_.size()) {
        for (size_t lag = 0; lag < moments.size(); ++lag) {
          const double x = history_[(position_ + history_.size() - lag * 4) % history_.size()];
          auto &m = moments[lag];
          m.x += x; m.y += clean; m.xx += x*x; m.yy += clean*clean; m.xy += x*clean; ++m.n;
          m.raw += raw; m.raw2 += raw*raw; m.xraw += x*raw;
        }
      }
      position_ = (position_ + 1) % history_.size();
    }
    if (clipped) { invalidate(); return; }
    if (!active || !excited || ref_energy < samples || moments[0].n < 16) return;
    if (moments[0].xx - moments[0].x * moments[0].x / moments[0].n <= 1) return;
    snapshot_.valid = true;
    snapshot_.reference_rms = std::sqrt(ref_energy / samples);
    snapshot_.reduction_db = 10 * std::log10((raw_energy + 1) / (clean_energy + 1));
    snapshot_.residual_to_reference_db = 10 * std::log10((clean_energy + 1) / (ref_energy + 1));
    for (size_t lag = 0; lag < moments.size(); ++lag) {
      const auto &m = moments[lag];
      const double variance = (m.xx - m.x*m.x/m.n) * (m.yy - m.y*m.y/m.n);
      const float correlation = variance > 1 ? std::min(1.0, std::abs(m.xy - m.x*m.y/m.n) / std::sqrt(variance)) : 0;
      const double raw_variance = (m.xx - m.x*m.x/m.n) * (m.raw2 - m.raw*m.raw/m.n);
      const float input_correlation = raw_variance > 1 ?
          std::min(1.0, std::abs(m.xraw - m.x*m.raw/m.n) / std::sqrt(raw_variance)) : 0;
      snapshot_.input_correlation = std::max(snapshot_.input_correlation, input_correlation);
      if (correlation > snapshot_.residual_correlation) {
        snapshot_.residual_correlation = correlation;
        snapshot_.lag_samples = lag * 4;
      }
    }
  }
 private:
  Snapshot snapshot_;
  std::array<int16_t, 65> history_{};
  uint32_t position_{0}, filled_{0}, active_samples_{0};
};
}
