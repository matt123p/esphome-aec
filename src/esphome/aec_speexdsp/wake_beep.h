#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::aec_speexdsp {
// Requests cross tasks; only the playback task owns sample position.
class WakeBeep {
 public:
  void start() { command_.store(1); }
  void stop() { command_.store(-1); active_.store(false); }
  bool active() const { return command_.load() > 0 || active_.load(); }
  size_t render(int16_t *pcm, size_t frames, unsigned channels) {
    const int command = command_.exchange(0);
    if (command != 0) position_ = command > 0 ? 0 : DURATION_SAMPLES;
    active_.store(position_ < DURATION_SAMPLES);
    const size_t count = std::min(frames, DURATION_SAMPLES - position_);
    for (size_t frame = 0; frame < count; ++frame, ++position_) {
      const float envelope = std::min(1.f, std::min(position_, DURATION_SAMPLES - 1 - position_) / 80.f);
      const int32_t tone = std::lround(8192.f * envelope * std::sin(6.283185307179586f * (position_ % 16) / 16.f));
      for (unsigned channel = 0; channel < channels; ++channel) {
        auto &sample = pcm[frame * channels + channel];
        sample = static_cast<int16_t>(std::clamp<int32_t>(static_cast<int32_t>(sample) + tone, -32768, 32767));
      }
    }
    if (position_ == DURATION_SAMPLES) active_.store(false);
    return count;
  }
 private:
  static constexpr size_t DURATION_SAMPLES = 1600;  // 100 ms, 1 kHz at 16 kHz.
  std::atomic<int> command_{0};
  std::atomic<bool> active_{false};
  size_t position_{DURATION_SAMPLES};
};
}  // namespace esphome::aec_speexdsp
