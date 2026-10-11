#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace esphome::aec_speexdsp {

// Observation only: these estimates never change gain, suppression or VAD.
// Classes are conservative hypotheses, not ground truth or speaker identity.
// DSP-task owned. No allocations, retained PCM or cross-task mutable access.
class AcousticObserver {
 public:
  enum class Scene : uint8_t { UNKNOWN, NOISE, NEAR_ONLY, FAR_ONLY };
  struct Distribution {
    std::array<uint32_t, 32> bins{};  // linear-AEC RMS, approximately 3 dB/bin
    uint64_t accepted_samples{0};
    uint32_t weight{0};
    float percentile(float fraction) const {
      if (!weight || !std::isfinite(fraction)) return 0;
      fraction = std::clamp(fraction, 0.0f, 1.0f);
      uint32_t cumulative = 0;
      const uint32_t target = std::max<uint32_t>(1, static_cast<uint32_t>(weight * fraction));
      for (size_t i = 0; i < bins.size(); ++i) {
        cumulative += bins[i];
        if (cumulative >= target) return std::pow(2.0f, static_cast<float>(i) / 2.0f);
      }
      return 32768;
    }
  };

  void reset() { *this = AcousticObserver{}; }
  const Distribution &distribution(Scene scene) const { return distributions_[static_cast<size_t>(scene)]; }

  void process(Scene scene, float linear_rms, uint32_t samples) {
    if (!std::isfinite(linear_rms) || linear_rms < 0 || linear_rms >= 32767 || samples == 0 || samples > 1024)
      scene = Scene::UNKNOWN;
    if (scene != scene_ || scene == Scene::UNKNOWN) {
      scene_ = scene;
      pending_ = {};
      staged_ = {};
      elapsed_ = 0;
      // Discard the transition frame as well as the uncommitted window.
      return;
    }
    const size_t bin = std::min<size_t>(31, static_cast<size_t>(
        2 * std::log2(std::max(1.0f, linear_rms))));
    pending_[bin] += samples;
    elapsed_ += samples;
    if (elapsed_ < 8000) return;  // half-second windows at 16 kHz
    auto &stats = distributions_[static_cast<size_t>(scene)];
    // Commit the PREVIOUS window only after a further stable half-second.
    // Any intervening uncertainty drops both windows, not just its own frame.
    if (stats.weight >= 16000 * 60) {
      stats.weight = 0;
      for (auto &count : stats.bins) { count /= 2; stats.weight += count; }
    }
    for (size_t i = 0; i < staged_.size(); ++i) {
      stats.bins[i] += staged_[i];
      stats.weight += staged_[i];
      stats.accepted_samples += staged_[i];
    }
    staged_ = pending_;
    pending_ = {};
    elapsed_ = 0;
  }

 private:
  std::array<Distribution, 4> distributions_{};
  std::array<uint32_t, 32> pending_{}, staged_{};
  Scene scene_{Scene::UNKNOWN};
  uint32_t elapsed_{0};
};

}  // namespace esphome::aec_speexdsp
