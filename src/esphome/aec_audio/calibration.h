#pragma once

// Optional on-device analogue-reference delay calibration. Capture and probe
// rendering run in the audio tasks; correlation is time-sliced in loop().
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <vector>
#include <esp_heap_caps.h>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::aec_audio {
class AECCalibration {
 public:
  static constexpr int N = 8192;
  static constexpr int MAX_DELAY = 256;
  static constexpr int MAX_LAG = 2048;

  bool busy() const { return busy_.load(); }
  const char *status() const { return status_; }
  int delay() const { return delay_source_ ? delay_source_->load() : 0; }
  void set_delay_source(std::atomic<int> *source) { delay_source_ = source; }

  void start() {
    if (busy()) return;
    if (!data_)
      data_ = static_cast<int16_t *>(heap_caps_malloc(4 * N * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data_) { snprintf(status_, sizeof(status_), "Failed: no capture PSRAM"); return; }
    original_delay_ = delay();
    candidates_ = {0};
    trial_ = 0;
    best_delay_ = 0;
    best_score_ = baseline_score_ = 1e9f;
    verifying_ = false;
    busy_.store(true);
    begin_trial_(0);
  }

  void cancel() {
    if (!busy()) return;
    store_delay_(original_delay_);
    busy_.store(false);
    snprintf(status_, sizeof(status_), "Cancelled; restored %d samples", original_delay_);
  }

  bool render(int16_t *pcm, size_t frames, uint8_t channels) {
    if (!busy()) return false;
    for (size_t i = 0; i < frames; ++i) {
      rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
      const int32_t noise = static_cast<int32_t>(rng_ >> 18) - 8192;
      filtered_ = std::clamp((filtered_ + noise) / 2, -8191, 8191);
      for (uint8_t ch = 0; ch < channels; ++ch)
        pcm[i * channels + ch] = static_cast<int16_t>(filtered_);
    }
    return true;
  }

  void capture(const int16_t *mic, const int16_t *ref, const int16_t *out, size_t frames, bool valid) {
    const unsigned generation = generation_.load();
    if (generation != audio_generation_) {
      audio_generation_ = generation;
      captured_ = 0;
      warmup_ = 48000;
      invalid_ = false;
    }
    if (!busy() || captured_generation_.load() == generation) return;
    if (!valid) invalid_ = true;
    for (size_t i = 0; i < frames && captured_ < N; ++i) {
      if (warmup_ > 0) { --warmup_; continue; }
      data_[captured_] = ref[i];
      data_[N + captured_] = mic[i];
      data_[2 * N + captured_] = mic[frames + i];
      data_[3 * N + captured_] = out[i];
      ++captured_;
    }
    if (captured_ == N) captured_generation_.store(generation);
  }

  void loop() {
    if (!busy()) return;
    if (millis() - trial_started_ > 20000) { fail_("capture/analysis timeout"); return; }
    if (captured_generation_.load() != generation_.load()) return;
    if (!analysing_) {
      if (invalid_) { fail_("AEC processing failed"); return; }
      bool clipped = false;
      for (int channel = 0; channel < 4; ++channel) {
        int64_t energy = 0;
        int32_t sum = 0, peak = 0;
        for (int i = 0; i < N; ++i) {
          const int32_t sample = data_[channel * N + i];
          sum += sample;
          energy += static_cast<int64_t>(sample) * sample;
          peak = std::max(peak, sample < 0 ? -sample : sample);
        }
        mean_[channel] = static_cast<float>(sum) / N;
        rms_[channel] = std::sqrt(std::max(0.0f, static_cast<float>(energy) / N - mean_[channel] * mean_[channel]));
        peaks_[channel] = peak;
        clipped |= peak >= 32760;
        correlations_[channel] = 0;
        lags_[channel] = 0;
      }
      if (clipped) {
        snprintf(reason_, sizeof(reason_), "clipped input/output (peaks ref=%ld mic1=%ld mic2=%ld out=%ld); lower volume/gain",
                 static_cast<long>(peaks_[0]), static_cast<long>(peaks_[1]),
                 static_cast<long>(peaks_[2]), static_cast<long>(peaks_[3]));
        fail_(reason_); return;
      }
      if (rms_[0] < 30 || rms_[1] < 30 || rms_[2] < 30) { fail_("reference or microphone too quiet"); return; }
      scan_channel_ = 1;
      scan_lag_ = -MAX_DELAY;
      analysing_ = true;
      trial_started_ = millis();
    }
    const uint32_t pass_started = millis();
    for (int work = 0; work < 64 && scan_channel_ <= 3; ++work) {
      const int limit = scan_channel_ == 3 ? MAX_LAG : MAX_DELAY;
      const float corr = correlation_(scan_channel_, scan_lag_);
      if (std::abs(corr) > std::abs(correlations_[scan_channel_])) {
        correlations_[scan_channel_] = corr;
        lags_[scan_channel_] = scan_lag_;
      }
      scan_lag_ += scan_channel_ == 3 ? 4 : 1;
      if (scan_lag_ > limit) { ++scan_channel_; scan_lag_ = scan_channel_ == 3 ? -MAX_LAG : -MAX_DELAY; }
      if (millis() - pass_started >= 10) break;
    }
    if (scan_channel_ <= 3) return;
    const float output_ratio = rms_[3] / std::min(rms_[1], rms_[2]);
    const float score = output_ratio * std::abs(correlations_[3]);
    ESP_LOGI("aec_calibration", "delay=%d mic_lags=%d/%d mic_corr=%+.3f/%+.3f ref_rms=%.1f cleaned_rms=%.1f leakage_db=%.1f",
             delay(), lags_[1], lags_[2], correlations_[1], correlations_[2], rms_[0], rms_[3],
             20 * std::log10(std::max(score, 1e-9f)));
    if (std::abs(correlations_[1]) < 0.25 || std::abs(correlations_[2]) < 0.25 ||
        std::abs(lags_[1]) == MAX_DELAY || std::abs(lags_[2]) == MAX_DELAY) {
      fail_("weak/ambiguous correlation or lag out of range"); return;
    }
    if (verifying_) {
      const bool improved = score < baseline_score_ * 0.9f && output_ratio <= baseline_output_ * 1.05f;
      store_delay_(improved ? best_delay_ : original_delay_);
      snprintf(status_, sizeof(status_), "%s: %d samples (%.2f ms)",
               improved ? "Verified" : "No verified improvement; restored", delay(), delay() / 16.0f);
      busy_.store(false); return;
    }
    if (trial_ == 0) {
      baseline_score_ = score;
      baseline_output_ = output_ratio;
      const int measured = std::min(lags_[1], lags_[2]);
      for (int delta : {-16, -8, 0}) {
        const int candidate = std::clamp(measured + delta, 0, MAX_DELAY);
        if (std::find(candidates_.begin(), candidates_.end(), candidate) == candidates_.end()) candidates_.push_back(candidate);
      }
    }
    if (score < best_score_ && output_ratio <= baseline_output_ * 1.05f) { best_score_ = score; best_delay_ = delay(); }
    if (++trial_ < candidates_.size()) { begin_trial_(candidates_[trial_]); return; }
    verifying_ = true;
    begin_trial_(best_delay_);
  }

 private:
  float correlation_(int channel, int lag) const {
    float xy = 0, xx = 0, yy = 0;
    for (int i = MAX_LAG; i < N - MAX_LAG; i += 2) {
      const float x = data_[i] - mean_[0];
      const float y = data_[channel * N + i + lag] - mean_[channel];
      xy += x * y; xx += x * x; yy += y * y;
    }
    return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
  }
  void begin_trial_(int delay) {
    store_delay_(delay);
    analysing_ = false;
    trial_started_ = millis();
    generation_.fetch_add(1);
    snprintf(status_, sizeof(status_), "%s %d samples; keep silent", verifying_ ? "Verifying" : "Testing", delay);
  }
  void store_delay_(int samples) { if (delay_source_) delay_source_->store(samples); }
  void fail_(const char *reason) {
    store_delay_(original_delay_);
    busy_.store(false);
    snprintf(status_, sizeof(status_), "Failed: %s (restored %d samples)", reason, original_delay_);
    ESP_LOGW("aec_calibration", "%s", status_);
  }

  std::atomic<bool> busy_{false};
  std::atomic<int> *delay_source_{nullptr};
  std::atomic<unsigned> generation_{0}, captured_generation_{0};
  unsigned audio_generation_{0};
  int16_t *data_{nullptr};
  size_t captured_{0}, trial_{0};
  int warmup_{0};
  bool invalid_{false}, analysing_{false}, verifying_{false};
  uint32_t rng_{0x718ac935}, trial_started_{0};
  int32_t filtered_{0}, peaks_[4]{};
  int original_delay_{0}, best_delay_{0}, scan_channel_{1}, scan_lag_{0}, lags_[4]{};
  char reason_[96]{}, status_[180]{"Ready: keep silent and start auto-tune"};
  float baseline_score_{0}, baseline_output_{0}, best_score_{0}, mean_[4]{}, rms_[4]{}, correlations_[4]{};
  std::vector<int> candidates_;
};
}  // namespace esphome::aec_audio
