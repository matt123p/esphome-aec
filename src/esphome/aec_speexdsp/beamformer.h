#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace aec_speexdsp {

struct TdoaResult {
  bool valid{false};
  int32_t delay_q15{0};
  int16_t peak_lag{0};
  uint16_t correlation_q15{0};
  uint16_t confidence_q15{0};
};

class AdaptiveDelayAndSumBeamformer {
 public:
  static constexpr uint8_t MAX_MICROPHONES = 4;
  static constexpr uint8_t MAX_LAG = 8;

  void configure(uint8_t microphones, uint8_t max_lag, uint8_t update_frames, uint16_t min_rms,
                 uint8_t min_correlation_percent, uint8_t min_peak_dominance_percent);
  // stride=1 for planar inputs; stride=channel count for interleaved AEC output.
  void process(const int16_t *const *microphones, size_t samples, int16_t *output, size_t stride = 1);
  // Apply another beamformer's current steering, with independent sample
  // history and no localization. Input/output buffers must not overlap.
  void process_with_steering(const AdaptiveDelayAndSumBeamformer &steering,
                            const int16_t *const *microphones, size_t samples, int16_t *output, size_t stride = 1);

  int32_t get_tdoa_q15(uint8_t microphone) const {
    return microphone < MAX_MICROPHONES ? this->tdoa_q15_[microphone] : 0;
  }
  uint16_t get_confidence_q15(uint8_t microphone) const {
    return microphone < MAX_MICROPHONES ? this->confidence_q15_[microphone] : 0;
  }
  uint64_t get_localization_cycles() const { return this->localization_cycles_; }
  uint64_t get_beamforming_cycles() const { return this->beamforming_cycles_; }
  uint32_t get_localization_calls() const { return this->localization_calls_; }
  uint32_t get_beamforming_calls() const { return this->beamforming_calls_; }

  static TdoaResult estimate_tdoa_xcorr(const int16_t *reference, const int16_t *microphone,
                                        size_t samples, uint8_t max_lag, uint16_t min_rms,
                                        uint16_t min_correlation_q15, uint16_t min_dominance_q15, size_t stride = 1);

 protected:
  static constexpr uint8_t HISTORY_SAMPLES = MAX_LAG * 2 + 2;
  static uint64_t isqrt64_(uint64_t value);
  static int16_t saturate16_(int32_t value);
  int16_t delayed_sample_(uint8_t microphone, const int16_t *input, size_t index, int32_t delay_q15, size_t stride) const;
  void update_delays_(const int16_t *const *microphones, size_t samples, size_t stride);
  void apply_delays_(const int16_t *const *microphones, size_t samples, int16_t *output, size_t stride);

  uint8_t microphones_{1};
  uint8_t max_lag_{3};
  uint8_t update_frames_{8};
  uint8_t frame_counter_{0};
  uint16_t min_rms_{120};
  uint16_t min_correlation_q15_{16384};
  uint16_t min_dominance_q15_{1638};
  int32_t tdoa_q15_[MAX_MICROPHONES]{};
  uint16_t confidence_q15_[MAX_MICROPHONES]{};
  int16_t history_[MAX_MICROPHONES][HISTORY_SAMPLES]{};
  uint64_t localization_cycles_{0};
  uint64_t beamforming_cycles_{0};
  uint32_t localization_calls_{0};
  uint32_t beamforming_calls_{0};
};

}  // namespace aec_speexdsp
}  // namespace esphome
