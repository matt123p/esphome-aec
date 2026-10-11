#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome::aec_speexdsp {
// Freeze one audio window while evaluating a bounded number of lags per frame.
// Acquisition is exhaustive (correlation is not monotonic). Once locked, use
// local tracking with full reacquisition on weak/edge peaks or periodic refresh.
class ReferenceDelayTracker {
 public:
  static constexpr size_t MAX_WINDOW = 2048;
  static constexpr int MAX_DELAY = 256;
  static constexpr unsigned LAGS_PER_CALL = 8;
  static constexpr int TRACK_RADIUS = 8;
  static constexpr unsigned GLOBAL_REFRESH = 16;
  struct Estimate {
    bool valid{false}, updated{false};
    int delay_samples{0};
    float correlation{0}, confidence{0}, drift_ppm{0};
  };
  struct Work {
    unsigned lags_last_call{0};
    uint32_t full_searches{0}, local_searches{0}, completed_searches{0};
  };

  void configure(int initial_delay, int max_delay = MAX_DELAY,
                 uint32_t update_samples = 8000, size_t window = 1024) {
    max_delay_ = std::clamp(max_delay, 0, MAX_DELAY);
    window_ = std::clamp<size_t>(window, 128, MAX_WINDOW - MAX_DELAY - 1);
    update_samples_ = std::max<uint32_t>(static_cast<uint32_t>(window_), update_samples);
    reset(initial_delay);
  }
  void reset(int delay = 0) {
    // Preserve configuration; no large temporary tracker on the audio stack.
    position_ = filled_ = 0;
    since_update_ = total_samples_ = search_sample_ = last_valid_sample_ = 0;
    searching_ = locked_ = false;
    local_updates_ = 0;
    center_ = last_best_delay_ = std::clamp(delay, 0, max_delay_);
    estimate_ = {};
    estimate_.delay_samples = center_;
    work_ = {};
  }
  const Estimate &estimate() const { return estimate_; }
  const Work &work() const { return work_; }
  void invalidate() {
    discard_capture_();
    estimate_.valid = estimate_.updated = false;
    estimate_.drift_ppm = 0;
    last_valid_sample_ = 0;
  }

  Estimate process(const int16_t *microphone, size_t mic_stride,
                   const int16_t *reference, size_t samples, bool excited) {
    estimate_.updated = false;
    work_.lags_last_call = 0;
    if (microphone == nullptr || reference == nullptr || mic_stride == 0 ||
        samples == 0 || samples > 1024) {
      invalidate();
      return estimate_;
    }
    total_samples_ += samples;
    since_update_ = std::min<uint64_t>(since_update_ + samples, update_samples_);
    if (!excited) {
      // Do not finish a stale scan in silence or correlate across input gaps.
      discard_capture_();
      estimate_.valid = false;
      return estimate_;
    }
    if (!searching_) {
      for (size_t n = 0; n < samples; ++n) {
        mic_[position_] = microphone[n * mic_stride];
        ref_[position_] = reference[n];
        if (++position_ == mic_.size()) position_ = 0;
      }
      filled_ = std::min(filled_ + samples, mic_.size());
      if (since_update_ < update_samples_ || filled_ < window_ + static_cast<size_t>(max_delay_) + 1)
        return estimate_;
      since_update_ = 0;
      search_sample_ = total_samples_;
      // Reuse both rings as a frozen chronological snapshot. Capture resumes
      // after the scan; no additional PCM window or allocation is needed.
      std::rotate(mic_.begin(), mic_.begin() + position_, mic_.end());
      std::rotate(ref_.begin(), ref_.begin() + position_, ref_.end());
      base_ = mic_.size() - window_ - 1;
      mic_energy_ = 0;
      for (size_t i = 1; i < window_; ++i) {
        const float m = difference_(mic_, base_ + i);
        mic_energy_ += m * m;
      }
      full_search_ = !locked_ || local_updates_ >= GLOBAL_REFRESH;
      begin_search_(full_search_ ? 0 : std::max(0, center_ - TRACK_RADIUS),
                    full_search_ ? max_delay_ : std::min(max_delay_, center_ + TRACK_RADIUS));
    }
    for (unsigned work = 0; work < LAGS_PER_CALL && next_delay_ <= high_; ++work) {
      const size_t start = base_ - static_cast<size_t>(next_delay_);
      float cross = 0;
      for (size_t i = 1; i < window_; ++i)
        cross += difference_(mic_, base_ + i) * difference_(ref_, start + i);
      scores_[next_delay_] = mic_energy_ > 1 && ref_energy_ > 1
          ? std::min(1.f, std::abs(cross) / std::sqrt(mic_energy_ * ref_energy_)) : 0.f;
      // Adjacent reference windows differ by two first differences.
      const float entering = difference_(ref_, start);
      const float leaving = difference_(ref_, start + window_ - 1);
      ref_energy_ = std::max(0.f, ref_energy_ + entering * entering - leaving * leaving);
      ++next_delay_;
      ++work_.lags_last_call;
    }
    if (next_delay_ <= high_) return estimate_;
    int best_delay = low_;
    for (int d = low_ + 1; d <= high_; ++d)
      if (scores_[d] > scores_[best_delay]) best_delay = d;
    // Exclude adjacent bins only AFTER finding the global maximum. The old
    // one-pass runner-up calculation could accidentally include its neighbour.
    float second = 0;
    for (int d = low_; d <= high_; ++d)
      if (std::abs(d - best_delay) > 1) second = std::max(second, scores_[d]);
    const float best = scores_[best_delay];
    const float confidence = best > 0 ? std::clamp((best - second) / best, 0.f, 1.f) : 0.f;
    const bool valid = best >= .35f && confidence >= .015f;
    const bool at_edge = (best_delay == low_ && low_ > 0) || (best_delay == high_ && high_ < max_delay_);
    if (!full_search_ && (!valid || at_edge)) {
      // Expand on the SAME snapshot, without publishing a suspect local peak.
      full_search_ = true;
      begin_search_(0, max_delay_);
      return estimate_;
    }
    estimate_.correlation = best;
    estimate_.confidence = confidence;
    estimate_.valid = valid;
    estimate_.updated = true;
    ++work_.completed_searches;
    if (valid) {
      center_ = best_delay;
      locked_ = true;
      local_updates_ = full_search_ ? 0 : local_updates_ + 1;
      const int delta = best_delay - estimate_.delay_samples;
      estimate_.delay_samples += std::clamp(delta, -1, 1);
      if (best >= .8f && confidence >= .1f && std::abs(delta) > 8)
        estimate_.delay_samples = best_delay;
      // Use the captured snapshot's timestamp, not the scan completion time.
      if (last_valid_sample_ != 0 && search_sample_ > last_valid_sample_) {
        const float instantaneous = static_cast<float>(best_delay - last_best_delay_) * 1000000.f /
                                    static_cast<float>(search_sample_ - last_valid_sample_);
        estimate_.drift_ppm = std::clamp(.9f * estimate_.drift_ppm + .1f * instantaneous, -2000.f, 2000.f);
      }
      last_best_delay_ = best_delay;
      last_valid_sample_ = search_sample_;
    } else {
      locked_ = false;
    }
    discard_capture_();
    return estimate_;
  }

 private:
  static float difference_(const std::array<int16_t, MAX_WINDOW + MAX_DELAY + 2> &data, size_t index) {
    return static_cast<float>(data[index]) - data[index - 1];
  }
  void discard_capture_() { searching_ = false; position_ = filled_ = 0; }
  void begin_search_(int low, int high) {
    low_ = next_delay_ = low;
    high_ = high;
    searching_ = true;
    ref_energy_ = 0;
    const size_t start = base_ - static_cast<size_t>(low);
    for (size_t i = 1; i < window_; ++i) {
      const float r = difference_(ref_, start + i);
      ref_energy_ += r * r;
    }
    if (full_search_) ++work_.full_searches;
    else ++work_.local_searches;
  }
  std::array<int16_t, MAX_WINDOW + MAX_DELAY + 2> mic_{}, ref_{};
  std::array<float, MAX_DELAY + 1> scores_{};
  size_t position_{0}, filled_{0}, window_{1024}, base_{0};
  int max_delay_{MAX_DELAY}, last_best_delay_{0}, center_{0}, low_{0}, high_{0}, next_delay_{0};
  unsigned local_updates_{0};
  bool searching_{false}, locked_{false}, full_search_{true};
  float mic_energy_{0}, ref_energy_{0};
  uint32_t update_samples_{8000};
  uint64_t since_update_{0}, total_samples_{0}, search_sample_{0}, last_valid_sample_{0};
  Estimate estimate_{};
  Work work_{};
};
}  // namespace esphome::aec_speexdsp
