#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "cancellation_quality.h"

namespace esphome::aec_speexdsp {
// Bounded echo learner. Qualification limits remain fixed safety policy.
class EchoBaseline {
 public:
  struct Result {
    int band{-1};
    bool qualified{false}, ready{false};
    uint32_t accepted_ms{0}, age_ms{0};
    float coverage{0}, median_db{0}, upper_db{0}, excess_db{0};
  };
  struct Prediction { bool ready{false}; float rms{0}, threshold_db{0}, uncertainty_db{0}, pre_agc_rms{0}; };
  Prediction predict(float reference_rms, uint32_t now) const {
    if (!std::isfinite(reference_rms) || reference_rms < 200) return {};
    const auto &s = bands_[reference_rms < 800 ? 0 : reference_rms < 3200 ? 1 : reference_rms < 12800 ? 2 : 3];
    if (s.weight < 32000 || now - s.last_ms > STALE_MS || !s.initialized) return {};
    const float pre_agc_rms = s.pre_agc_weight >= 32000 ?
        reference_rms * std::pow(10.0f, s.pre_agc_threshold_db / 20.0f) : 0;
    return {true, reference_rms * std::pow(10.0f, s.threshold_db / 20.0f), s.threshold_db,
            s.uncertainty_db, pre_agc_rms};
  }
  const Result &result() const { return result_; }
  // Echo dominance hypothesis, not proof of absent quiet double-talk.
  static bool echo_evidence(const CancellationQuality::Snapshot &q) {
    return q.valid && std::isfinite(q.input_correlation) && q.input_correlation >= 0.8f &&
           q.input_correlation <= 1 && std::isfinite(q.reduction_db) && q.reduction_db >= 6;
  }
  struct Counters { uint32_t qualified{0}, committed{0}, speech_resets{0}, integrity_resets{0},
                              startup_resets{0}, evidence_skips{0}; };
  const Counters &counters() const { return counters_; }
  // Reset in place: a whole-object temporary contains >6 KB of histograms.
  // Inlining that temporary into the audio loop consumes its task stack even
  // when the reset branch is not taken.
  void reset() {
    for (auto &band : bands_) {
      band.bins.fill(0);
      band.pre_agc_bins.fill(0);
      band.weight = band.pre_agc_weight = band.last_ms = band.accepted_ms = 0;
      band.threshold_db = band.uncertainty_db = band.pre_agc_threshold_db = 0;
      band.initialized = false;
    }
    clear_pending_();
    last_frame_ms_ = 0;
    result_ = {};
    counters_ = {};
  }
  void discard_pending() { clear_pending_(); result_ = {}; }
  void process(const CancellationQuality::Snapshot &q, bool healthy, bool adapted,
               bool startup, bool suspect_speech, uint32_t samples, uint32_t now,
               float pre_agc_rms = -1) {
    result_ = {};
    if (!healthy || samples == 0 || samples > 1024 ||
        (pre_agc_rms >= 0 && !std::isfinite(pre_agc_rms)) ||
        (q.valid && (!std::isfinite(q.reference_rms) ||
         !std::isfinite(q.residual_to_reference_db) || !std::isfinite(q.input_correlation) ||
         !std::isfinite(q.reduction_db)))) {
      ++counters_.integrity_resets; discard_pending(); return;
    }
    // Separate playback levels rather than assuming all levels have equal
    // nonlinear echo. Out-of-range ratios are not folded into edge bins.
    const int band = !q.valid || q.reference_rms < 200 ? -1 :
                     q.reference_rms < 800 ? 0 : q.reference_rms < 3200 ? 1 : q.reference_rms < 12800 ? 2 : 3;
    for (auto &stats : bands_)
      if (stats.weight && now - stats.last_ms > STALE_MS) stats = {};
    const bool safe = adapted && !startup && !suspect_speech;
    const bool qualified = safe && band >= 0 && echo_evidence(q) &&
                           q.residual_to_reference_db >= -60 && q.residual_to_reference_db < 4;
    if (qualified) ++counters_.qualified;
    else if (safe) ++counters_.evidence_skips;
    if (!safe) {
      if (suspect_speech) ++counters_.speech_resets;
      else ++counters_.startup_resets;
      clear_pending_();
    } else {
      if (now - last_frame_ms_ > 250) clear_pending_();
      // Weak correlation contributes no data, but does not erase otherwise
      // qualified echo in the same safe window. Suspected speech erases both.
      if (qualified) {
        const size_t bin = static_cast<size_t>(q.residual_to_reference_db + 60);
        pending_[band][bin] += samples;
        if (pre_agc_rms >= 0) {
          const float ratio_db = 20 * std::log10(std::max(1.0f, pre_agc_rms) /
                                                  std::max(1.0f, q.reference_rms));
          if (ratio_db >= -60 && ratio_db < 4)
            pre_agc_pending_[band][static_cast<size_t>(ratio_db + 60)] += samples;
        }
      }
      elapsed_ += samples;
      if (elapsed_ >= WINDOW_SAMPLES) {
        for (size_t b = 0; b < bands_.size(); ++b) {
          auto &stats = bands_[b];
          if (stats.weight >= 16000 * 30) {
            stats.weight = 0;
            for (auto &n : stats.bins) { n /= 2; stats.weight += n; }
            stats.pre_agc_weight = 0;
            for (auto &n : stats.pre_agc_bins) { n /= 2; stats.pre_agc_weight += n; }
          }
          uint32_t committed = 0;
          for (size_t i = 0; i < staged_[b].size(); ++i) {
            stats.bins[i] += staged_[b][i]; committed += staged_[b][i];
          }
          stats.weight += committed;
          uint32_t pre_agc_committed = 0;
          for (size_t i = 0; i < pre_agc_staged_[b].size(); ++i) {
            stats.pre_agc_bins[i] += pre_agc_staged_[b][i];
            pre_agc_committed += pre_agc_staged_[b][i];
          }
          stats.pre_agc_weight += pre_agc_committed;
          if (stats.pre_agc_weight >= 32000) {
            const float median = percentile_bins(stats.pre_agc_bins, stats.pre_agc_weight, .5f);
            const float upper = percentile_bins(stats.pre_agc_bins, stats.pre_agc_weight, .75f);
            stats.pre_agc_threshold_db = std::clamp(2 * upper - median, -54.0f, 16.0f);
          }
          if (committed) {
            stats.last_ms = now; stats.accepted_ms += committed / 16; ++counters_.committed;
            // A measured robust upper fence: median + twice the upper-half
            // interquartile spread. P99 was dominated by rare unmodelled echo
            // bursts and placed the threshold several times above speech RMS.
            // The two quantiles and 1x extrapolation are distribution based,
            // not another room-specific level setting.
            const float median = percentile(stats, .5f);
            const float upper = percentile(stats, .75f);
            stats.uncertainty_db = upper - median;
            const float target = std::clamp(upper + stats.uncertainty_db, -54.0f, 16.0f);
            if (!stats.initialized) { stats.threshold_db = target; stats.initialized = true; }
            else stats.threshold_db += std::clamp(target - stats.threshold_db,
                                                  -static_cast<float>(committed) / 16000.0f, 6 * committed / 16000.0f);
          }
        }
        staged_ = pending_; pending_ = {};
        pre_agc_staged_ = pre_agc_pending_; pre_agc_pending_ = {};
        elapsed_ = 0;
      }
    }
    last_frame_ms_ = now;
    result_.band = band;
    result_.qualified = qualified;
    if (band < 0) return;  // Unexcited frame advances quarantine, contributes no data.
    const auto &stats = bands_[band];
    result_.accepted_ms = stats.accepted_ms;
    result_.age_ms = stats.weight ? now - stats.last_ms : 0;
    result_.coverage = std::min(1.0f, stats.weight / 32000.0f);
    result_.ready = stats.weight >= 32000 && result_.age_ms <= STALE_MS;
    if (stats.weight) {
      result_.median_db = percentile(stats, .5f);
      result_.upper_db = percentile(stats, .95f);
      result_.excess_db = q.residual_to_reference_db - result_.upper_db;
    }
  }
 private:
  void clear_pending_() {
    for (auto &bins : pending_) bins.fill(0);
    for (auto &bins : staged_) bins.fill(0);
    for (auto &bins : pre_agc_pending_) bins.fill(0);
    for (auto &bins : pre_agc_staged_) bins.fill(0);
    elapsed_ = 0;
  }
  static constexpr uint32_t WINDOW_SAMPLES = 4000; // 250 ms + a further window quarantine
  static constexpr uint32_t STALE_MS = 30000;
  struct Band {
    std::array<uint32_t,64> bins{}, pre_agc_bins{};
    uint32_t weight{0}, pre_agc_weight{0}, last_ms{0}, accepted_ms{0};
    float threshold_db{0}, uncertainty_db{0}, pre_agc_threshold_db{0}; bool initialized{false};
  };
  static float percentile_bins(const std::array<uint32_t,64> &bins, uint32_t weight, float fraction) {
    uint32_t sum = 0;
    const uint32_t target = std::max<uint32_t>(1, static_cast<uint32_t>(weight * fraction));
    for (size_t i=0;i<bins.size();++i) { sum += bins[i]; if (sum >= target) return static_cast<float>(i) - 59; }
    return 4;
  }
  static float percentile(const Band &s, float fraction) { return percentile_bins(s.bins, s.weight, fraction); }
  std::array<Band,4> bands_{};
  std::array<std::array<uint32_t,64>,4> pending_{}, staged_{};
  std::array<std::array<uint32_t,64>,4> pre_agc_pending_{}, pre_agc_staged_{};
  uint32_t elapsed_{0}, last_frame_ms_{0};
  Result result_;
  Counters counters_;
};
}
