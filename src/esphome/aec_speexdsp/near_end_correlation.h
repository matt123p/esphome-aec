#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::aec_speexdsp {

// Time-domain evidence computed after MDF and before beamforming.  It retains
// smoothed sufficient statistics only; no PCM is retained.  One update per
// microphone is required for each frame, followed by finish_frame().
class NearEndCorrelation {
 public:
  static constexpr size_t MAX_MICROPHONES = 4;

  struct ChannelResult {
    float mic_error_correlation{0};   // C_de
    float echo_error_correlation{0};  // C_ye, y = d - e
    float mic_power{0};
    float error_power{0};
    float removed_power{0};
    float independent_probability{0};
  };

  struct Result {
    std::array<ChannelResult, MAX_MICROPHONES> channels{};
    float mic_error_correlation{0};
    float echo_error_correlation{0};
    float correlation_probability{0};
    float path_change_probability{0};
    uint8_t selected_channel{0};
  };

  void reset() { *this = NearEndCorrelation{}; }

  void process_channel(uint8_t channel, const int16_t *mic, size_t mic_stride,
                       const int16_t *error, size_t error_stride, size_t samples,
                       uint32_t sample_rate = 16000) {
    if (channel >= MAX_MICROPHONES || mic == nullptr || error == nullptr || samples == 0)
      return;
    double dd = 0, ee = 0, yy = 0, de = 0, ye = 0;
    for (size_t n = 0; n < samples; ++n) {
      const double d = mic[n * mic_stride];
      const double e = error[n * error_stride];
      const double y = d - e;
      dd += d * d;
      ee += e * e;
      yy += y * y;
      de += d * e;
      ye += y * e;
    }
    // Equivalent smoothing for all supported frame sizes (75 ms default).
    const float dt = static_cast<float>(samples) / std::max<uint32_t>(1, sample_rate);
    const float a = std::exp(-dt / 0.075f);
    auto &s = state_[channel];
    smooth_(s.dd, dd / samples, a);
    smooth_(s.ee, ee / samples, a);
    smooth_(s.yy, yy / samples, a);
    smooth_(s.de, de / samples, a);
    smooth_(s.ye, ye / samples, a);
    s.valid = true;

    auto &r = result_.channels[channel];
    r.mic_power = static_cast<float>(s.dd);
    r.error_power = static_cast<float>(s.ee);
    r.removed_power = static_cast<float>(s.yy);
    r.mic_error_correlation = normalized_(s.de, s.dd, s.ee);
    r.echo_error_correlation = normalized_(s.ye, s.yy, s.ee);

    // Transparent initial calibration.  Energy confidence prevents a
    // correlation of two nearly-silent signals from becoming evidence.
    const float energy_confidence = static_cast<float>(s.ee / (s.ee + 64.0 * 64.0));
    // Development-set calibration: the useful classical feature is C_de-C_ye,
    // not C_de alone. Echo p90 was 0.12 and near p90 was 0.36 (2026-10-04
    // fixture set); held-out data was not used to choose these bounds.
    const float mic_evidence = smoothstep_(0.06f, 0.30f,
                                           r.mic_error_correlation-r.echo_error_correlation);
    const float path = std::max(smoothstep_(0.45f, 0.85f, r.echo_error_correlation),
                                smoothstep_(0.05f, 0.30f,
                                            r.echo_error_correlation-r.mic_error_correlation));
    r.independent_probability = energy_confidence * mic_evidence * (1.0f - 0.80f * path);
    seen_mask_ |= static_cast<uint8_t>(1U << channel);
  }

  const Result &finish_frame(uint8_t channels) {
    result_.correlation_probability = 0;
    result_.path_change_probability = 0;
    result_.selected_channel = 0;
    const uint8_t count = std::min<uint8_t>(channels, MAX_MICROPHONES);
    for (uint8_t c = 0; c < count; ++c) {
      if (!(seen_mask_ & (1U << c))) continue;
      const auto &r = result_.channels[c];
      const float path = std::max(smoothstep_(0.45f, 0.85f, r.echo_error_correlation),
                                  smoothstep_(0.05f, 0.30f,
                                              r.echo_error_correlation-r.mic_error_correlation));
      result_.path_change_probability = std::max(result_.path_change_probability, path);
      if (r.independent_probability >= result_.correlation_probability) {
        result_.correlation_probability = r.independent_probability;
        result_.selected_channel = c;
      }
    }
    const auto &selected = result_.channels[result_.selected_channel];
    result_.mic_error_correlation = selected.mic_error_correlation;
    result_.echo_error_correlation = selected.echo_error_correlation;
    seen_mask_ = 0;
    return result_;
  }

  const Result &result() const { return result_; }

 private:
  struct State {
    double dd{0}, ee{0}, yy{0}, de{0}, ye{0};
    bool valid{false};
  };

  static void smooth_(double &state, double value, float alpha) {
    state = state == 0 ? value : alpha * state + (1.0f - alpha) * value;
  }
  static float normalized_(double cross, double first, double second) {
    const double denominator = std::sqrt(std::max(0.0, first) * std::max(0.0, second));
    if (denominator <= 1e-9) return 0;
    return std::clamp(static_cast<float>(std::abs(cross) / denominator), 0.0f, 1.0f);
  }
  static float smoothstep_(float low, float high, float value) {
    const float x = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
  }

  std::array<State, MAX_MICROPHONES> state_{};
  Result result_{};
  uint8_t seen_mask_{0};
};

}  // namespace esphome::aec_speexdsp
