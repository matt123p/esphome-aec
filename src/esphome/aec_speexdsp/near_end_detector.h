#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "acoustic_observer.h"
#include "reference_tracker.h"
#include "cancellation_quality.h"
#include "echo_baseline.h"

namespace esphome::aec_speexdsp {

// A conservative double-talk candidate, not speaker identification. All inputs
// describe the same DSP frame, before AGC. Never compare spectral power to RMS.
class NearEndDetector {
 public:
  void set_observation_enabled(bool enabled) { observation_enabled_ = enabled; observer_.reset(); echo_baseline_.reset(); }
  void set_adaptive_enabled(bool enabled) { adaptive_enabled_ = enabled; echo_baseline_.reset(); }
  bool adaptive_used() const { return adaptive_used_; }
  float adaptive_threshold_rms() const { return adaptive_threshold_rms_; }
  float adaptive_pre_agc_threshold_rms() const { return adaptive_pre_agc_threshold_rms_; }
  const EchoBaseline::Result &echo_baseline() const { return echo_baseline_.result(); }
  const EchoBaseline::Counters &echo_learning_counters() const { return echo_baseline_.counters(); }
  const AcousticObserver &observer() const { return observer_; }
  bool observation_enabled() const { return observation_enabled_; }
  void discard_pending_observation() { observer_.process(AcousticObserver::Scene::UNKNOWN, 0, 0); echo_baseline_.discard_pending(); }
  void reset() {
    active_ = candidate_ = reference_active_ = echo_dominated_ = adaptive_used_ = false;
    above_ = below_ = 0;
    adaptive_threshold_rms_ = adaptive_pre_agc_threshold_rms_ = 0;
    observer_.reset();
    echo_baseline_.reset();
  }
  void configure(float min_rms, float margin_db, float echo_floor_db, uint32_t onset_ms,
                 uint32_t release_ms, uint32_t startup_ms) {
    min_rms_ = min_rms;
    margin_ = std::pow(10.0f, margin_db / 10.0f);
    echo_floor_ = std::pow(10.0f, echo_floor_db / 20.0f);
    onset_samples_ = onset_ms * 16;
    release_samples_ = release_ms * 16;
    reference_tracker_.configure(200, 100, 250, startup_ms);
  }

  bool process(bool vad, bool adapted, float reference_rms, float pre_agc_rms,
               float clean_rms, float removed_rms, float input_power,
               float echo_power, float noise_power, uint32_t samples, bool observation_valid = true,
               const ReferenceFrame *shared_reference = nullptr,
               const CancellationQuality::Snapshot *quality = nullptr, uint32_t now_ms = 0) {
    echo_dominated_ = false;
    adaptive_used_ = false; adaptive_threshold_rms_ = adaptive_pre_agc_threshold_rms_ = 0;
    const bool was_active = active_;
    const bool valid = samples > 0 && samples <= 1024 &&
        std::isfinite(reference_rms) && reference_rms >= 0 &&
        std::isfinite(pre_agc_rms) && pre_agc_rms >= 0 &&
        std::isfinite(clean_rms) && clean_rms >= 0 &&
        std::isfinite(removed_rms) && removed_rms >= 0 &&
        std::isfinite(input_power) && input_power >= 0 &&
        std::isfinite(echo_power) && echo_power >= 0 &&
        std::isfinite(noise_power) && noise_power >= 0;
    if (!valid) {
      active_ = candidate_ = false;
      above_ = below_ = 0;
      if (observation_enabled_) observer_.process(AcousticObserver::Scene::UNKNOWN, 0, 0);
      echo_baseline_.discard_pending();
      return false;
    }
    const auto reference = shared_reference != nullptr ? *shared_reference : reference_tracker_.process(reference_rms, samples);
    reference_active_ = reference.active;
    const bool settling = reference.startup;
    // Diagnostic hypothesis ONLY: fixed thresholds, not a calibrated learner.
    // Never use this flag to gate speech, mask PCM or label training data.
    echo_dominated_ = observation_valid && reference_active_ && quality != nullptr && quality->valid &&
                      quality->residual_correlation >= 0.60f && quality->reduction_db >= 6.0f;
    // The removed-echo floor covers uncertainty in Speex's residual estimate.
    // Run 4 showed that the floor plus noise margin alone admits self-audio.
    // Restore the conservative echo-power veto until independently measured
    // echo envelopes have been validated against real near-end speech.
    bool echo_clear = !reference_active_ ||
        (adapted && clean_rms > removed_rms * echo_floor_ &&
         input_power > margin_ * (echo_power + noise_power + 1.0f));
    // Score using PREVIOUSLY committed evidence, never train on a frame then
    // use it to decide whether that same frame was speech.
    bool speech_innovation = false;
    if (adaptive_enabled_ && observation_valid && reference_active_ && adapted && !settling &&
        quality != nullptr && quality->valid) {
      const auto prediction = echo_baseline_.predict(quality->reference_rms, now_ms);
      if (prediction.ready && prediction.pre_agc_rms > 0) {
        adaptive_used_ = true;
        adaptive_threshold_rms_ = prediction.rms;  // comparable linear residual diagnostic
        adaptive_pre_agc_threshold_rms_ = std::max(min_rms_, prediction.pre_agc_rms);
        speech_innovation = pre_agc_rms > adaptive_pre_agc_threshold_rms_;
        // The learned envelope belongs to this same pre-AGC signal. The
        // linear AEC estimate is retained only as a loose independent guard.
        echo_clear = speech_innovation && clean_rms > removed_rms * echo_floor_ * .5f &&
                     input_power > margin_ * (noise_power + 1);
        // A correlated echo burst above the envelope is not permission to open.
        // Use the normal release hangover to bridge brief weak phonemes; do
        // not let persistent echo hold an already-open gate indefinitely.
        if (EchoBaseline::echo_evidence(*quality)) echo_clear = false;
      }
    }
    candidate_ = vad && std::isfinite(pre_agc_rms) && pre_agc_rms >= min_rms_ && echo_clear;
    if (settling) {
      // Retain plausible onset PCM in pre-roll while confirmation is inhibited.
      // Startup is not itself evidence that this frame contains only echo.
      active_ = false;
      above_ = below_ = 0;
    } else if (candidate_) {
      below_ = 0;
      above_ = std::min(above_ + samples, onset_samples_);
      if (above_ >= onset_samples_) active_ = true;
    } else {
      above_ = 0;
      below_ = std::min(below_ + samples, release_samples_);
      if (below_ >= release_samples_) active_ = false;
    }
    if (observation_enabled_ || adaptive_enabled_) {
      // A gate opening is not ground truth: independently measured coherent
      // raw/reference evidence may override it for SHADOW learning, not VAD.
      if (quality != nullptr)
        echo_baseline_.process(*quality, observation_valid && reference_active_, adapted,
                               settling, was_active || active_ || speech_innovation ||
                                   (candidate_ && !EchoBaseline::echo_evidence(*quality)),
                               samples, now_ms, pre_agc_rms);
      else
        echo_baseline_.discard_pending();
      using Scene = AcousticObserver::Scene;
      Scene scene = Scene::UNKNOWN;
      if (!settling && !reference_active_ && reference.quiet) {
        // Use raw speech evidence, not the configured RMS cutoff: otherwise
        // quiet rejected speech could teach the noise floor to reject itself.
        if (vad) scene = Scene::NEAR_ONLY;
        else if (!active_ && input_power <= 2 * (noise_power + 1)) scene = Scene::NOISE;
      } else if (!settling && reference_active_ && reference.excited && adapted &&
                 !vad && !active_ && !candidate_ && !echo_clear) {
        scene = Scene::FAR_ONLY;
      }
      observer_.process(observation_valid ? scene : Scene::UNKNOWN, clean_rms, samples);
    }
    return active_;
  }
  bool reference_active() const { return reference_active_; }
  bool candidate() const { return candidate_; }
  bool echo_dominated() const { return echo_dominated_; }

 private:
  bool observation_enabled_{false};
  bool adaptive_enabled_{false}, adaptive_used_{false};
  float adaptive_threshold_rms_{0};
  float adaptive_pre_agc_threshold_rms_{0};
  AcousticObserver observer_;
  EchoBaseline echo_baseline_;
  float min_rms_{24.0f}, margin_{4.0f}, echo_floor_{0.126f};
  ReferenceTracker reference_tracker_;
  uint32_t onset_samples_{1024}, release_samples_{2560};
  uint32_t above_{0}, below_{0};
  bool echo_dominated_{false};
  bool reference_active_{false}, candidate_{false}, active_{false};
};

}  // namespace esphome::aec_speexdsp
