#include "beamformer.h"

#include <algorithm>
#include <climits>
#include <esp_cpu.h>

namespace esphome {
namespace aec_speexdsp {

void AdaptiveDelayAndSumBeamformer::configure(uint8_t microphones, uint8_t max_lag, uint8_t update_frames,
                                               uint16_t min_rms, uint8_t min_correlation_percent,
                                               uint8_t min_peak_dominance_percent) {
  this->microphones_ = std::max<uint8_t>(1, std::min<uint8_t>(microphones, MAX_MICROPHONES));
  this->max_lag_ = std::max<uint8_t>(1, std::min<uint8_t>(max_lag, MAX_LAG));
  this->update_frames_ = std::max<uint8_t>(1, update_frames);
  this->min_rms_ = min_rms;
  this->min_correlation_q15_ = static_cast<uint16_t>(min_correlation_percent * 32768U / 100U);
  this->min_dominance_q15_ = static_cast<uint16_t>(min_peak_dominance_percent * 32768U / 100U);
}

uint64_t AdaptiveDelayAndSumBeamformer::isqrt64_(uint64_t value) {
  uint64_t result = 0;
  uint64_t bit = UINT64_C(1) << 62;
  while (bit > value)
    bit >>= 2;
  while (bit != 0) {
    if (value >= result + bit) {
      value -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

TdoaResult AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(
    const int16_t *reference, const int16_t *microphone, size_t samples, uint8_t max_lag, uint16_t min_rms,
    uint16_t min_correlation_q15, uint16_t min_dominance_q15, size_t stride) {
  TdoaResult result;
  if (reference == nullptr || microphone == nullptr || max_lag == 0 || max_lag > MAX_LAG ||
      samples <= 2U * max_lag || stride == 0)
    return result;

  int64_t sum_a = 0;
  int64_t sum_b = 0;
  for (size_t i = 0; i < samples; i++) {
    sum_a += reference[i * stride];
    sum_b += microphone[i * stride];
  }
  const int32_t mean_a = static_cast<int32_t>(sum_a / static_cast<int64_t>(samples));
  const int32_t mean_b = static_cast<int32_t>(sum_b / static_cast<int64_t>(samples));
  const size_t start = max_lag;
  const size_t end = samples - max_lag;
  const size_t count = end - start;
  uint16_t correlations[2 * MAX_LAG + 1]{};
  uint64_t energy_a_center = 0;
  uint64_t energy_b_center = 0;

  // Reference window is fixed for all lags. The microphone window slides
  // one sample per lag, so update its energy with an exact subtract/add.
  uint64_t energy_a = 0, energy_b = 0;
  for (size_t n = start; n < end; n++) {
    const int64_t a = int32_t(reference[n * stride]) - mean_a;
    const int64_t b = int32_t(microphone[(n-start) * stride]) - mean_b;
    energy_a += uint64_t(a*a);
    energy_b += uint64_t(b*b);
  }

  for (int lag = -max_lag; lag <= max_lag; lag++) {
    int64_t cross = 0;
    if (lag > -max_lag) {
      const int64_t outgoing = int32_t(microphone[(int(start)+lag-1) * stride]) - mean_b;
      const int64_t incoming = int32_t(microphone[(int(end)+lag-1) * stride]) - mean_b;
      energy_b -= uint64_t(outgoing*outgoing);
      energy_b += uint64_t(incoming*incoming);
    }
    for (size_t n = start; n < end; n++) {
      const int32_t a = static_cast<int32_t>(reference[n * stride]) - mean_a;
      const int32_t b = static_cast<int32_t>(microphone[(static_cast<int>(n) + lag) * stride]) - mean_b;
      cross += static_cast<int64_t>(a) * b;
    }
    if (lag == 0) {
      energy_a_center = energy_a;
      energy_b_center = energy_b;
    }
    if (cross <= 0 || energy_a == 0 || energy_b == 0)
      continue;

    const uint64_t largest = std::max<uint64_t>(static_cast<uint64_t>(cross), std::max(energy_a, energy_b));
    const unsigned bits = largest == 0 ? 0U : 64U - static_cast<unsigned>(__builtin_clzll(largest));
    const unsigned shift = bits > 30 ? bits - 30 : 0;
    const uint64_t scaled_cross = static_cast<uint64_t>(cross) >> shift;
    const uint64_t scaled_a = energy_a >> shift;
    const uint64_t scaled_b = energy_b >> shift;
    const uint64_t denominator = isqrt64_(scaled_a * scaled_b);
    if (denominator != 0)
      correlations[lag + max_lag] = static_cast<uint16_t>(
          std::min<uint64_t>(32767, (scaled_cross << 15) / denominator));
  }

  const uint64_t min_energy = static_cast<uint64_t>(min_rms) * min_rms * count;
  if (energy_a_center < min_energy || energy_b_center < min_energy)
    return result;

  int best_index = 0;
  for (int i = 1; i <= 2 * max_lag; i++)
    if (correlations[i] > correlations[best_index])
      best_index = i;
  const int best_lag = best_index - max_lag;
  result.peak_lag = best_lag;
  result.correlation_q15 = correlations[best_index];
  if (best_lag == -max_lag || best_lag == max_lag || result.correlation_q15 < min_correlation_q15)
    return result;

  uint16_t second = 0;
  for (int i = 0; i <= 2 * max_lag; i++)
    if (i < best_index - 1 || i > best_index + 1)
      second = std::max(second, correlations[i]);
  const uint16_t dominance = result.correlation_q15 > second ? result.correlation_q15 - second : 0;
  result.confidence_q15 = std::min(result.correlation_q15, dominance);
  if (dominance < min_dominance_q15)
    return result;

  const int64_t left = correlations[best_index - 1];
  const int64_t center = correlations[best_index];
  const int64_t right = correlations[best_index + 1];
  const int64_t denominator = left - 2 * center + right;
  int32_t fractional_q15 = 0;
  if (denominator != 0)
    fractional_q15 = static_cast<int32_t>(((left - right) * 16384) / denominator);
  fractional_q15 = std::max<int32_t>(-16384, std::min<int32_t>(16384, fractional_q15));
  result.delay_q15 = best_lag * 32768 + fractional_q15;
  result.valid = true;
  return result;
}

void AdaptiveDelayAndSumBeamformer::update_delays_(const int16_t *const *microphones, size_t samples, size_t stride) {
  for (uint8_t microphone = 1; microphone < this->microphones_; microphone++) {
    const TdoaResult estimate = estimate_tdoa_xcorr(
        microphones[0], microphones[microphone], samples, this->max_lag_, this->min_rms_,
        this->min_correlation_q15_, this->min_dominance_q15_, stride);
    if (!estimate.valid)
      continue;
    // Positive TDOA means this microphone received the sound after mic 0.
    // Alpha=1/8 prevents block-to-block steering jumps.
    this->tdoa_q15_[microphone] += (estimate.delay_q15 - this->tdoa_q15_[microphone]) >> 3;
    this->confidence_q15_[microphone] = estimate.confidence_q15;
  }
}

int16_t AdaptiveDelayAndSumBeamformer::delayed_sample_(uint8_t microphone, const int16_t *input, size_t index,
                                                       int32_t delay_q15, size_t stride) const {
  const int integer = delay_q15 >> 15;
  const uint32_t fraction = static_cast<uint32_t>(delay_q15 - integer * 32768);
  const int current_index = static_cast<int>(index) - integer;
  auto sample_at = [&](int position) -> int32_t {
    if (position >= 0)
      return input[position * stride];
    const int history_index = HISTORY_SAMPLES + position;
    return history_index >= 0 ? this->history_[microphone][history_index] : 0;
  };
  const int32_t newer = sample_at(current_index);
  const int32_t older = sample_at(current_index - 1);
  return saturate16_(static_cast<int32_t>((static_cast<int64_t>(newer) * (32768 - fraction) +
                                           static_cast<int64_t>(older) * fraction + 16384) >> 15));
}

int16_t AdaptiveDelayAndSumBeamformer::saturate16_(int32_t value) {
  return static_cast<int16_t>(std::max<int32_t>(INT16_MIN, std::min<int32_t>(INT16_MAX, value)));
}

void AdaptiveDelayAndSumBeamformer::process(const int16_t *const *microphones, size_t samples, int16_t *output, size_t stride) {
  if (++this->frame_counter_ >= this->update_frames_) {
    this->frame_counter_ = 0;
    const uint32_t localization_start = esp_cpu_get_cycle_count();
    this->update_delays_(microphones, samples, stride);
    this->localization_cycles_ += static_cast<uint32_t>(esp_cpu_get_cycle_count() - localization_start);
    this->localization_calls_++;
  }

  const uint32_t beamforming_start = esp_cpu_get_cycle_count();
  int32_t latest_q15 = this->tdoa_q15_[0];
  for (uint8_t microphone = 1; microphone < this->microphones_; microphone++)
    latest_q15 = std::max(latest_q15, this->tdoa_q15_[microphone]);

  for (size_t n = 0; n < samples; n++) {
    int32_t sum = 0;
    for (uint8_t microphone = 0; microphone < this->microphones_; microphone++) {
      const int32_t compensation_q15 = latest_q15 - this->tdoa_q15_[microphone];
      sum += this->delayed_sample_(microphone, microphones[microphone], n, compensation_q15, stride);
    }
    output[n] = saturate16_(sum / this->microphones_);
  }

  for (uint8_t microphone = 0; microphone < this->microphones_; microphone++) {
    if (samples >= HISTORY_SAMPLES) {
      for (size_t i = 0; i < HISTORY_SAMPLES; i++)
        this->history_[microphone][i] = microphones[microphone][(samples - HISTORY_SAMPLES + i) * stride];
    } else {
      const size_t keep = HISTORY_SAMPLES - samples;
      for (size_t i = 0; i < keep; i++)
        this->history_[microphone][i] = this->history_[microphone][i + samples];
      for (size_t i = 0; i < samples; i++)
        this->history_[microphone][keep + i] = microphones[microphone][i * stride];
    }
  }
  this->beamforming_cycles_ += static_cast<uint32_t>(esp_cpu_get_cycle_count() - beamforming_start);
  this->beamforming_calls_++;
}

}  // namespace aec_speexdsp
}  // namespace esphome
