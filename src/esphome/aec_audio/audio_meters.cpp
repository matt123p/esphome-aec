#include "aec_audio.h"
#include "esphome/core/log.h"

#if defined(USE_ESP32) && defined(USE_AEC_AUDIO_METERS)

#include <esp_heap_caps.h>

namespace esphome {
namespace aec_audio {

static const char *const TAG = "audio_meters";
static const uint32_t SAMPLE_RATE = 16000;

void AECAudioMetersComponent::setup() {
  // FFT buffers for spectrum analyser (PSRAM, non-fatal if unavailable)
  this->fft_re_ = static_cast<float *>(
      heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->fft_im_ = static_cast<float *>(
      heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->hann_win_ = static_cast<float *>(
      heap_caps_aligned_alloc(16, SPECTRUM_FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->fft_re_ == nullptr || this->fft_im_ == nullptr || this->hann_win_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate FFT buffers in PSRAM - spectrum analyser disabled");
  } else {
    this->init_spectrum_();
    ESP_LOGI(TAG, "Spectrum analyser: %u-point FFT, %u log-spaced display bins (80-8000 Hz)",
             SPECTRUM_FFT_SIZE, SPECTRUM_BINS);
  }
}

void AECAudioMetersComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Audio Meters component initialized");
}

void AECAudioMetersComponent::process_raw(const int16_t *raw, size_t frames, uint8_t slots) {
  for (uint8_t slot = 0; slot < slots; slot++) {
    uint64_t sum_sq = 0;
    int32_t peak = 0;
    for (size_t frame = 0; frame < frames; frame++) {
      const int32_t sample = raw[frame * slots + slot];
      const int32_t magnitude = sample == INT16_MIN ? 32768 : std::abs(sample);
      peak = std::max(peak, magnitude);
      sum_sq += static_cast<uint64_t>(sample * sample);
    }
    const float rms = std::sqrt(static_cast<float>(sum_sq) / frames);
    if (slot < 4) {
      this->slot_rms_[slot].store(rms);
      this->slot_peak_[slot].store(peak);
    }
  }
}

void AECAudioMetersComponent::process_reference(const int16_t *ref, size_t frames) {
  uint64_t reference_sum_sq = 0;
  int32_t reference_peak = 0;
  for (size_t frame = 0; frame < frames; frame++) {
    const int32_t sample = ref[frame];
    const int32_t magnitude = sample == INT16_MIN ? 32768 : std::abs(sample);
    reference_peak = std::max(reference_peak, magnitude);
    reference_sum_sq += static_cast<uint64_t>(sample * sample);
  }
  this->reference_rms_.store(std::sqrt(static_cast<float>(reference_sum_sq) / frames));
  this->reference_peak_.store(reference_peak);
}

void AECAudioMetersComponent::process_output(const int16_t *planar, size_t frames) {
  // 1. Level meters (VU)
  uint64_t sum_sq = 0;
  uint32_t peak = 0;
  size_t clipped = 0;
  size_t alternating = 0;
  for (size_t sample = 0; sample < frames * 2; sample++) {
    const int32_t value = planar[sample];
    const uint32_t magnitude = value == INT16_MIN ? 32768U : static_cast<uint32_t>(std::abs(value));
    peak = std::max(peak, magnitude);
    if (magnitude >= 32760U)
      clipped++;
    if (sample > 0 && ((value < 0) != (planar[sample - 1] < 0)))
      alternating++;
    sum_sq += static_cast<uint64_t>(value * value);
  }
  const float rms = std::sqrt(static_cast<float>(sum_sq) / (frames * 2));
  this->output_meter_history_[this->output_meter_history_index_] = rms;
  this->output_meter_history_index_ = (this->output_meter_history_index_ + 1) % OUTPUT_METER_HISTORY_SIZE;
  this->output_meter_history_count_ = std::min(this->output_meter_history_count_ + 1, OUTPUT_METER_HISTORY_SIZE);

  float minimum = this->output_meter_history_[0];
  float maximum = minimum;
  for (size_t index = 1; index < this->output_meter_history_count_; index++) {
    minimum = std::min(minimum, this->output_meter_history_[index]);
    maximum = std::max(maximum, this->output_meter_history_[index]);
  }
  auto to_dbfs = [](float value) { return value > 0.0f ? 20.0f * std::log10(value / 32768.0f) : -96.0f; };
  this->output_dbfs_.store(to_dbfs(rms));
  this->output_peak_.store(peak);
  this->output_peak_history_[this->output_peak_history_index_] = peak;
  this->output_peak_history_index_ = (this->output_peak_history_index_ + 1) % OUTPUT_PEAK_HISTORY_SIZE;
  this->output_peak_history_count_ = std::min(this->output_peak_history_count_ + 1, OUTPUT_PEAK_HISTORY_SIZE);
  uint32_t peak_3s = 0;
  for (size_t index = 0; index < this->output_peak_history_count_; index++)
    peak_3s = std::max(peak_3s, this->output_peak_history_[index]);
  this->output_peak_3s_.store(peak_3s);
  this->output_clipped_percent_.store(100.0f * clipped / (frames * 2));
  this->output_alternating_percent_.store(frames > 0 ? 100.0f * alternating / (frames * 2 - 1) : 0.0f);
  this->output_min_dbfs_.store(to_dbfs(minimum));
  this->output_max_dbfs_.store(to_dbfs(maximum));

  // 2. Spectrum analyser (using mono channel 0 from planar output). The FFT is
  // deliberately gated by the UI so it adds no continuous audio-task load.
  if (this->spectrum_enabled_.load())
    this->compute_spectrum_(planar, frames);
}

void AECAudioMetersComponent::init_spectrum_() {
  // Hann window coefficients
  for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
    hann_win_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (SPECTRUM_FFT_SIZE - 1)));

  // Log-spaced display bin boundaries: 80 Hz to 8000 Hz (Nyquist for 16 kHz)
  const float min_freq = 80.0f;
  const float max_freq = 8000.0f;
  const float hz_per_fft_bin = static_cast<float>(SAMPLE_RATE) / SPECTRUM_FFT_SIZE;
  for (int k = 0; k < SPECTRUM_BINS; k++) {
    const float lo = min_freq * std::pow(max_freq / min_freq, static_cast<float>(k) / SPECTRUM_BINS);
    const float hi = min_freq * std::pow(max_freq / min_freq, static_cast<float>(k + 1) / SPECTRUM_BINS);
    spectrum_lo_bin_[k] = static_cast<uint16_t>(lo / hz_per_fft_bin);
    spectrum_hi_bin_[k] = static_cast<uint16_t>(hi / hz_per_fft_bin);
    if (spectrum_hi_bin_[k] <= spectrum_lo_bin_[k])
      spectrum_hi_bin_[k] = spectrum_lo_bin_[k] + 1;
    if (spectrum_hi_bin_[k] > SPECTRUM_FFT_SIZE / 2)
      spectrum_hi_bin_[k] = SPECTRUM_FFT_SIZE / 2;
  }
  for (int k = 0; k < SPECTRUM_BINS; k++)
    spectrum_db_[k] = -80.0f;
}

void AECAudioMetersComponent::compute_spectrum_(const int16_t *mono, size_t frames) {
  if (fft_re_ == nullptr)
    return;

  const size_t n = std::min(frames, static_cast<size_t>(SPECTRUM_FFT_SIZE));

  // Window + copy to FFT buffer (real), zero imaginary
  for (size_t i = 0; i < n; i++) {
    fft_re_[i] = mono[i] * hann_win_[i] / 32768.0f;
    fft_im_[i] = 0.0f;
  }
  for (size_t i = n; i < SPECTRUM_FFT_SIZE; i++) {
    fft_re_[i] = 0.0f;
    fft_im_[i] = 0.0f;
  }

  fft_r2_(fft_re_, fft_im_, SPECTRUM_FFT_SIZE);

  // Map FFT bins -> display bins; use peak-hold with per-frame exponential decay
  // scale: 2/N accounts for one-sided spectrum; Hann window halves amplitude
  const float scale = 2.0f / SPECTRUM_FFT_SIZE;
  static const float decay = 0.965f;  // ~0.6 dB per frame @ 32 ms/frame -> ~2s for -20 dB decay

  for (int k = 0; k < SPECTRUM_BINS; k++) {
    float peak_sq = 0.0f;
    for (int b = spectrum_lo_bin_[k]; b < spectrum_hi_bin_[k]; b++) {
      const float sq = fft_re_[b] * fft_re_[b] + fft_im_[b] * fft_im_[b];
      if (sq > peak_sq)
        peak_sq = sq;
    }
    const float mag = std::sqrt(peak_sq) * scale;
    const float db = mag > 1e-7f ? 20.0f * std::log10(mag) : -80.0f;
    const float current = spectrum_db_[k];
    // Fast attack, slow decay
    spectrum_db_[k] = db > current ? db : current * decay + db * (1.0f - decay);
  }
}

void AECAudioMetersComponent::fft_r2_(float *re, float *im, int n) {
  // Bit-reversal permutation
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  // Butterfly stages
  for (int len = 2; len <= n; len <<= 1) {
    const float ang = -2.0f * static_cast<float>(M_PI) / len;
    const float wr = std::cos(ang);
    const float wi = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int j = 0; j < len / 2; j++) {
        const int a = i + j;
        const int b = a + len / 2;
        const float tr = re[b] * cr - im[b] * ci;
        const float ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr;  im[b] = im[a] - ti;
        re[a] += tr;         im[a] += ti;
        const float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

}  // namespace aec_audio
}  // namespace esphome

#endif
