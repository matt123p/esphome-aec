#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "cancellation_quality.h"
#include "reference_tracker.h"

namespace esphome::aec_speexdsp {

// Produces a probability, not a hard speech identity decision.  The value is
// used only to interpolate residual-echo suppression; NearEndDetector remains
// the independently debounced barge-in gate.
class SoftDoubleTalkController {
 public:
  struct Result {
    float probability{0};
    float echo_probability{0};
    int suppression_db{15};  // positive attenuation magnitude for Speex ctl
  };

  void configure(int far_end_db, int near_end_db) {
    far_end_db_ = std::max(far_end_db, near_end_db);
    near_end_db_ = std::min(far_end_db, near_end_db);
  }

  void reset() { probability_ = 0; result_ = {}; result_.suppression_db = far_end_db_; }
  const Result &result() const { return result_; }

  Result process(const ReferenceFrame &reference,
                 const CancellationQuality::Snapshot &quality,
                 float speech_probability, uint32_t samples) {
    speech_probability = std::clamp(std::isfinite(speech_probability) ? speech_probability : 0.0f, 0.0f, 1.0f);
    float echo = 0;
    if (reference.active && quality.valid) {
      const float input_coherence = std::clamp(quality.input_correlation, 0.0f, 1.0f);
      const float residual_coherence = std::clamp(quality.residual_correlation, 0.0f, 1.0f);
      const float erle = std::clamp((quality.reduction_db - 2.0f) / 16.0f, 0.0f, 1.0f);
      echo = 0.60f * input_coherence + 0.20f * residual_coherence + 0.20f * erle;
    }
    float target = reference.active ? speech_probability * (1.0f - 0.75f * echo) : speech_probability;
    if (reference.startup)
      target *= 0.25f;
    const float frames = std::clamp(samples / 256.0f, 0.0f, 4.0f);
    const float coefficient = target > probability_ ? 1.0f - std::pow(0.55f, frames)
                                                     : 1.0f - std::pow(0.88f, frames);
    probability_ += coefficient * (target - probability_);
    probability_ = std::clamp(probability_, 0.0f, 1.0f);
    result_.probability = probability_;
    result_.echo_probability = echo;
    result_.suppression_db = static_cast<int>(std::lround(
        far_end_db_ + probability_ * (near_end_db_ - far_end_db_)));
    return result_;
  }

 private:
  int far_end_db_{40}, near_end_db_{6};
  float probability_{0};
  Result result_{};
};

}  // namespace esphome::aec_speexdsp
