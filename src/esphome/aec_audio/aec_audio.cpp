#include "aec_audio.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aec_audio {

AECAudioComponent *global_aec_audio = nullptr;

static const char *const TAG = "aec_audio";
static const uint32_t SAMPLE_RATE = 16000;
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
static const uint32_t PLAYBACK_RATE = SAMPLE_RATE;
#endif
static const uint32_t PLAYBACK_BUFFER_MS = 1000;
static const uint32_t REFERENCE_BUFFER_MS = 250;
static const size_t TRANSPORT_FRAME_SAMPLES = 512;
static const uint32_t DIAGNOSTIC_LOG_INTERVAL_MS = 5000;
static const uint32_t HANDOFF_LOG_INTERVAL_MS = 1000;
static const uint32_t INPUT_RATE_LOG_INTERVAL_MS = 5000;
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
static const uint32_t PLAYBACK_RATE_MIN = SAMPLE_RATE - 1000;
static const uint32_t PLAYBACK_RATE_MAX = SAMPLE_RATE + 1000;
static const uint32_t PLAYBACK_RATE_ADJUST_STEP = 1;
static const uint32_t PLAYBACK_RATE_MEASURE_MIN_US = 8000;
static const uint32_t PLAYBACK_RATE_LOG_INTERVAL_MS = 1000;
static const uint32_t INPUT_RATE_ESTIMATE_MIN_WINDOW_MS = 3000;
static const uint32_t INPUT_RATE_IDLE_RESET_MS = 1000;
static const size_t INPUT_RATE_ESTIMATE_MIN_FRAMES = SAMPLE_RATE * 2;
#endif

void AECAudioComponent::set_pins(int mclk, int bclk, int lrclk, int din, int dout) {
  this->mclk_pin_ = static_cast<gpio_num_t>(mclk);
  this->bclk_pin_ = static_cast<gpio_num_t>(bclk);
  this->lrclk_pin_ = static_cast<gpio_num_t>(lrclk);
  this->din_pin_ = static_cast<gpio_num_t>(din);
  this->dout_pin_ = static_cast<gpio_num_t>(dout);
}

void AECAudioComponent::setup() {
  global_aec_audio = this;

  std::unique_ptr<ring_buffer::RingBuffer> buffer =
      ring_buffer::RingBuffer::create(SAMPLE_RATE * 2 * 2 * PLAYBACK_BUFFER_MS / 1000);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Could not allocate playback ring buffer");
    this->mark_failed();
    return;
  }
  this->playback_buffer_ = std::shared_ptr<ring_buffer::RingBuffer>(std::move(buffer));
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  this->playback_rate_.store(PLAYBACK_RATE);
  this->input_playback_rate_.store(PLAYBACK_RATE);
  initialise_resampler();
#endif

  if (this->reference_source_ == AEC_AUDIO_REFERENCE_PLAYBACK) {
    std::unique_ptr<ring_buffer::RingBuffer> reference_buffer =
        ring_buffer::RingBuffer::create(SAMPLE_RATE * sizeof(int16_t) * REFERENCE_BUFFER_MS / 1000);
    if (reference_buffer == nullptr) {
      ESP_LOGE(TAG, "Could not allocate playback reference ring buffer");
      this->mark_failed();
      return;
    }
    this->reference_buffer_ = std::shared_ptr<ring_buffer::RingBuffer>(std::move(reference_buffer));

    if (this->reference_delay_samples_ > 0) {
      std::vector<int16_t> zeros(this->reference_delay_samples_, 0);
      size_t written = this->reference_buffer_->write(zeros.data(), zeros.size() * sizeof(int16_t));
      if (written != zeros.size() * sizeof(int16_t)) {
        ESP_LOGW(TAG, "Could only pre-fill %zu samples of reference delay (requested %u)",
                 written / sizeof(int16_t), static_cast<unsigned>(this->reference_delay_samples_));
      } else {
        ESP_LOGI(TAG, "Pre-filled reference buffer with %u samples (%" PRIu32 " ms) of silence",
                 static_cast<unsigned>(this->reference_delay_samples_),
                 static_cast<uint32_t>(this->reference_delay_samples_) * 1000 / SAMPLE_RATE);
      }
    }
  }

  // Capture buffer in PSRAM: CAPTURE_SECONDS * 16000 samples * 2 bytes
  this->capture_buffer_ = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, CAPTURE_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->capture_buffer_ == nullptr) {
    // Non-fatal: capture feature will be unavailable
    ESP_LOGW(TAG, "Could not allocate %u-byte capture buffer in PSRAM — capture disabled",
             static_cast<unsigned>(CAPTURE_FRAMES * sizeof(int16_t)));
  } else {
    ESP_LOGI(TAG, "Capture buffer: %u frames (%u bytes PSRAM)",
             static_cast<unsigned>(CAPTURE_FRAMES),
             static_cast<unsigned>(CAPTURE_FRAMES * sizeof(int16_t)));
  }

#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  ESP_LOGI(TAG, "Playback resampler enabled: initial_rate=%" PRIu32 " Hz", PLAYBACK_RATE);
#endif

  if (!this->start_i2s_() || !this->start_afe_()) {
    this->mark_failed();
    return;
  }

  if (xTaskCreatePinnedToCore(AECAudioComponent::playback_task, "aec_playback", 4096, this, 20, &this->playback_task_handle_, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "Could not start playback task");
    this->mark_failed();
    return;
  }
  if (xTaskCreatePinnedToCore(AECAudioComponent::audio_task, "aec_audio", 8192, this, 19, &this->audio_task_handle_, 0) != pdPASS) {
    ESP_LOGE(TAG, "Could not start audio task");
    this->mark_failed();
  }
}

void AECAudioComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "AEC Audio:\n"
                "  I2S port: %u\n"
                "  Pins: MCLK=%d BCLK=%d LRCLK=%d DIN=%d DOUT=%d\n"
                "  TDM slots: %u\n"
                "  Microphone slots: %u, %u\n"
                "  Reference: native %s, slot %u\n"
                "  TX slots: %u, %u\n"
                "  Diagnostic raw slot: %d\n"
                "  Processing: ESP-SR dual-microphone %s AFE, mono output duplicated to stereo\n"
                "  AGC: %s\n"
                "  AEC filter length: %u",
                this->i2s_port_, this->mclk_pin_, this->bclk_pin_, this->lrclk_pin_, this->din_pin_, this->dout_pin_,
                this->tdm_slots_, this->microphone_slots_[0], this->microphone_slots_[1],
                this->reference_source_ == AEC_AUDIO_REFERENCE_PLAYBACK ? "playback buffer" : "analog TDM",
                this->reference_slot_, this->tx_slots_[0], this->tx_slots_[1], this->diagnostic_raw_slot_.load(),
                this->afe_include_unused_channel_ ? "MMNR" : "MMR",
                YESNO(this->agc_enabled_),
                this->filter_length_);
}

void AECAudioComponent::loop() {
  if (millis() - this->last_input_rate_log_ >= INPUT_RATE_LOG_INTERVAL_MS) {
    const uint32_t now = millis();
    const size_t requested_bytes = this->play_requested_bytes_.load();
    const size_t accepted_bytes = this->play_source_bytes_.load();
    const size_t requested_delta = requested_bytes - this->last_logged_play_requested_bytes_;
    const size_t accepted_delta = accepted_bytes - this->last_logged_input_rate_accepted_bytes_;
    const uint32_t elapsed_ms = now - this->last_input_rate_log_;
    if (requested_delta > 0 && elapsed_ms > 0) {
      uint8_t channels = this->speaker_ == nullptr ? 1 : this->speaker_->get_audio_stream_info().get_channels();
      if (channels == 0)
        channels = 1;
      const float seconds = elapsed_ms / 1000.0f;
      const float requested_bps = requested_delta / seconds;
      const float accepted_bps = accepted_delta / seconds;
      const float bytes_per_frame = channels * sizeof(int16_t);
      ESP_LOGI(TAG,
               "Playback input rate: offered=%u bytes %.1f B/s %.1f Hz accepted=%u bytes %.1f B/s %.1f Hz "
               "window=%" PRIu32 " ms channels=%u",
               static_cast<unsigned>(requested_delta), requested_bps, requested_bps / bytes_per_frame,
               static_cast<unsigned>(accepted_delta), accepted_bps, accepted_bps / bytes_per_frame, elapsed_ms,
               static_cast<unsigned>(channels));
      this->last_logged_play_requested_bytes_ = requested_bytes;
      this->last_logged_input_rate_accepted_bytes_ = accepted_bytes;
    }
    this->last_input_rate_log_ = now;
  }

  if (millis() - this->last_handoff_log_ >= HANDOFF_LOG_INTERVAL_MS) {
    const uint32_t play_calls = this->play_calls_.load();
    const uint32_t play_zero_writes = this->play_zero_writes_.load();
    const size_t play_source_bytes = this->play_source_bytes_.load();
    const size_t play_enqueued_bytes = this->play_enqueued_bytes_.load();
    const size_t playback_drained_bytes = this->playback_drained_bytes_.load();
    const bool changed = play_calls != this->last_logged_play_calls_ ||
                         play_zero_writes != this->last_logged_play_zero_writes_ ||
                         play_source_bytes != this->last_logged_play_source_bytes_ ||
                         play_enqueued_bytes != this->last_logged_play_enqueued_bytes_ ||
                         playback_drained_bytes != this->last_logged_playback_drained_bytes_;
    if (changed && (play_zero_writes != this->last_logged_play_zero_writes_ ||
                    (this->playback_buffer_ != nullptr && this->playback_buffer_->available() > 0))) {
      this->last_handoff_log_ = millis();
      ESP_LOGI(TAG,
               "Playback handoff: calls=%" PRIu32 " zero=%" PRIu32
               " accepted=%u enqueued=%u drained=%u ring=%u free=%u buffering=%s",
               play_calls - this->last_logged_play_calls_,
               play_zero_writes - this->last_logged_play_zero_writes_,
               static_cast<unsigned>(play_source_bytes - this->last_logged_play_source_bytes_),
               static_cast<unsigned>(play_enqueued_bytes - this->last_logged_play_enqueued_bytes_),
               static_cast<unsigned>(playback_drained_bytes - this->last_logged_playback_drained_bytes_),
               this->playback_buffer_ == nullptr ? 0 : this->playback_buffer_->available(),
               this->playback_buffer_ == nullptr ? 0 : this->playback_buffer_->free(),
               this->buffering_.load() ? "true" : "false");
      this->last_logged_play_calls_ = play_calls;
      this->last_logged_play_zero_writes_ = play_zero_writes;
      this->last_logged_play_source_bytes_ = play_source_bytes;
      this->last_logged_play_enqueued_bytes_ = play_enqueued_bytes;
      this->last_logged_playback_drained_bytes_ = playback_drained_bytes;
    }
  }
#ifdef USE_AEC_AUDIO_DIAGNOSTICS
  if (millis() - this->last_diagnostic_log_ >= DIAGNOSTIC_LOG_INTERVAL_MS) {
    this->last_diagnostic_log_ = millis();
    ESP_LOGD(TAG,
             "rx_errors=%" PRIu32 " tx_errors=%" PRIu32 " underruns=%" PRIu32 " dropped=%" PRIu32
             " ref_underruns=%" PRIu32 " ref_overflows=%" PRIu32 " max_processing_us=%" PRIu32
             " playback_bytes=%u reference_bytes=%u reference_rms=%.1f reference_peak=%" PRIu32,
             this->rx_errors_.load(), this->tx_errors_.load(), this->playback_underruns_.load(),
             this->dropped_frames_.load(), this->reference_underruns_.load(), this->reference_overflows_.load(),
             this->max_processing_us_.load(), this->playback_buffer_ == nullptr ? 0 : this->playback_buffer_->available(),
             this->reference_buffer_ == nullptr ? 0 : this->reference_buffer_->available(), this->reference_rms_.load(),
             this->reference_peak_.load());
  }
#endif
}

bool AECAudioComponent::start_i2s_() {
  i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(static_cast<i2s_port_t>(this->i2s_port_), I2S_ROLE_MASTER);
  channel_config.dma_desc_num = 6;
  channel_config.dma_frame_num = 256;
  channel_config.auto_clear = true;

  esp_err_t err = i2s_new_channel(&channel_config, &this->tx_handle_, &this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not allocate paired I2S channels: %s", esp_err_to_name(err));
    return false;
  }

  i2s_tdm_config_t config = {
      .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                       static_cast<i2s_tdm_slot_mask_t>(0x0F)),
      .gpio_cfg =
          {
              .mclk = this->mclk_pin_,
              .bclk = this->bclk_pin_,
              .ws = this->lrclk_pin_,
              .dout = this->dout_pin_,
              .din = this->din_pin_,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  config.slot_cfg.total_slot = this->tdm_slots_;

  err = i2s_channel_init_tdm_mode(this->tx_handle_, &config);
  if (err == ESP_OK)
    err = i2s_channel_init_tdm_mode(this->rx_handle_, &config);
  if (err == ESP_OK)
    err = i2s_channel_enable(this->tx_handle_);
  if (err == ESP_OK)
    err = i2s_channel_enable(this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not initialize full-duplex TDM: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool AECAudioComponent::start_afe_() {
  const char *input_format = this->afe_include_unused_channel_ ? "MMNR" : "MMR";
  afe_config_t *config = afe_config_init(
      input_format, nullptr, this->wakenet_enabled_ ? AFE_TYPE_SR : AFE_TYPE_FD,
      this->aec_mode_ == AEC_AUDIO_MODE_FD_HIGH_PERF ? AFE_MODE_HIGH_PERF : AFE_MODE_LOW_COST);
  if (config == nullptr) {
    ESP_LOGE(TAG, "Could not allocate ESP-SR AFE configuration");
    return false;
  }

  config->aec_init = true;
  config->aec_mode =
      this->aec_mode_ == AEC_AUDIO_MODE_FD_HIGH_PERF ? AEC_MODE_FD_HIGH_PERF : AEC_MODE_FD_LOW_COST;
  config->aec_filter_length = this->filter_length_;
  config->aec_nlp_level = this->nlp_level_ == AEC_AUDIO_NLP_NORMAL
                              ? AEC_NLP_LEVEL_NORMAL
                              : (this->nlp_level_ == AEC_AUDIO_NLP_VERY_AGGRESSIVE ? AEC_NLP_LEVEL_VERYAGGR
                                                                                   : AEC_NLP_LEVEL_AGGR);
  config->se_init = this->speech_enhancement_enabled_;
  config->ns_init = this->noise_suppression_enabled_;
  config->wakenet_init = this->wakenet_enabled_;
  config->vad_init = this->wakenet_enabled_;
  config->agc_init = this->agc_enabled_;
  config->fixed_output_channel = !this->wakenet_enabled_;
  config->output_playback_channel = false;
  config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
  config = afe_config_check(config);
  if (config == nullptr) {
    ESP_LOGE(TAG, "ESP-SR rejected the AFE configuration");
    return false;
  }

  // afe_config_check() normalises the pipeline and may restore stage defaults.
  // Apply the explicitly requested optional stages to the checked config so
  // the instance that is created matches the YAML configuration.
  config->se_init = this->speech_enhancement_enabled_;
  config->ns_init = this->noise_suppression_enabled_;
  config->wakenet_init = this->wakenet_enabled_;
  config->vad_init = this->wakenet_enabled_;
  config->agc_init = this->agc_enabled_;
  config->fixed_output_channel = !this->wakenet_enabled_;
  config->output_playback_channel = false;
  ESP_LOGI(TAG,
           "Final ESP-SR stages: AEC=%s SE=%s NS=%s VAD=%s WakeNet=%s AGC=%s "
           "AGC target=-%d dBFS compression=%d dB linear_gain=%.3f",
           YESNO(config->aec_init), YESNO(config->se_init), YESNO(config->ns_init), YESNO(config->vad_init),
           YESNO(config->wakenet_init), YESNO(config->agc_init), config->agc_target_level_dbfs,
           config->agc_compression_gain_db, config->afe_linear_gain);
  afe_config_print(config);
  this->afe_iface_ = esp_afe_handle_from_config(config);
  this->afe_data_ = this->afe_iface_ == nullptr ? nullptr : this->afe_iface_->create_from_config(config);
  afe_config_free(config);
  if (this->afe_iface_ == nullptr || this->afe_data_ == nullptr) {
    ESP_LOGE(TAG, "Could not initialize ESP-SR dual-microphone MMNR AFE");
    return false;
  }

  this->afe_feed_chunksize_ = this->afe_iface_->get_feed_chunksize(this->afe_data_);
  this->afe_fetch_chunksize_ = this->afe_iface_->get_fetch_chunksize(this->afe_data_);
  const int feed_channels = this->afe_iface_->get_feed_channel_num(this->afe_data_);
  const int fetch_channels = this->afe_iface_->get_fetch_channel_num(this->afe_data_);
  const int expected_feed_channels = this->afe_include_unused_channel_ ? 4 : 3;
  if (this->afe_feed_chunksize_ == 0 || this->afe_fetch_chunksize_ == 0 ||
      this->afe_feed_chunksize_ % this->afe_fetch_chunksize_ != 0 || feed_channels != expected_feed_channels ||
      fetch_channels != 1) {
    ESP_LOGE(TAG, "Unsupported ESP-SR AFE shape: feed=%u/%dch fetch=%u/%dch",
             static_cast<unsigned>(this->afe_feed_chunksize_), feed_channels,
             static_cast<unsigned>(this->afe_fetch_chunksize_), fetch_channels);
    return false;
  }
  ESP_LOGI(TAG, "ESP-SR dual-microphone %s AFE initialized: feed=%u/%dch fetch=%u/%dch AGC=%s",
           input_format,
           static_cast<unsigned>(this->afe_feed_chunksize_), feed_channels,
           static_cast<unsigned>(this->afe_fetch_chunksize_), fetch_channels, YESNO(this->agc_enabled_));
  this->afe_iface_->print_pipeline(this->afe_data_);
  return true;
}

void AECAudioComponent::audio_task(void *params) {
  static_cast<AECAudioComponent *>(params)->run_audio_task_();
  vTaskDelete(nullptr);
}

void AECAudioComponent::playback_task(void *params) {
  static_cast<AECAudioComponent *>(params)->run_playback_task_();
  vTaskDelete(nullptr);
}

void AECAudioComponent::run_audio_task_() {
  const size_t frame_size = this->afe_feed_chunksize_;
  const size_t raw_samples = frame_size * this->tdm_slots_;
  const size_t raw_bytes = raw_samples * sizeof(int16_t);
  const size_t planar_samples = frame_size * 2;

  auto *raw = static_cast<int16_t *>(heap_caps_aligned_alloc(16, raw_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *mic = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, planar_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *ref =
      static_cast<int16_t *>(heap_caps_aligned_alloc(16, frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *out = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, planar_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  const size_t afe_channels = this->afe_include_unused_channel_ ? 4 : 3;
  auto *afe_input = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, frame_size * afe_channels * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (raw == nullptr || mic == nullptr || ref == nullptr || out == nullptr || afe_input == nullptr) {
    ESP_LOGE(TAG, "Could not allocate audio frame buffers");
    this->dropped_frames_++;
    return;
  }

  uint32_t last_slot_log = 0;
#ifdef USE_AEC_AUDIO_TELEMETRY
  uint64_t effect_samples = 0;
  uint64_t effect_ref_energy = 0;
  uint64_t effect_raw_energy[2]{0, 0};
  uint64_t effect_out_energy[2]{0, 0};
  int64_t effect_raw_ref_cross[2]{0, 0};
  int64_t effect_out_ref_cross[2]{0, 0};
#endif
  while (true) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(this->rx_handle_, raw, raw_bytes, &bytes_read, 100);
    if (err != ESP_OK || bytes_read != raw_bytes) {
      this->rx_errors_++;
      continue;
    }

    const bool log_slots = millis() - last_slot_log >= DIAGNOSTIC_LOG_INTERVAL_MS;
    if (log_slots)
      last_slot_log = millis();

#ifdef USE_AEC_AUDIO_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_raw(raw, frame_size, this->tdm_slots_);
#endif

#ifdef USE_AEC_AUDIO_SLOT_LOGS
    if (log_slots) {
      for (uint8_t slot = 0; slot < this->tdm_slots_; slot++) {
        uint64_t sum_sq = 0;
        int32_t peak = 0;
        for (size_t frame = 0; frame < frame_size; frame++) {
          const int32_t sample = raw[frame * this->tdm_slots_ + slot];
          const int32_t magnitude = sample == INT16_MIN ? 32768 : std::abs(sample);
          peak = std::max(peak, magnitude);
          sum_sq += static_cast<uint64_t>(sample * sample);
        }
        const float rms = std::sqrt(static_cast<float>(sum_sq) / frame_size);
        ESP_LOGD(TAG, "slot %u rms=%.1f peak=%ld", slot, rms, static_cast<long>(peak));
      }
    }
#endif

    for (size_t frame = 0; frame < frame_size; frame++) {
      mic[frame] = raw[frame * this->tdm_slots_ + this->microphone_slots_[0]];
      mic[frame_size + frame] = raw[frame * this->tdm_slots_ + this->microphone_slots_[1]];
      if (this->reference_source_ == AEC_AUDIO_REFERENCE_ANALOG_SLOT)
        ref[frame] = raw[frame * this->tdm_slots_ + this->reference_slot_];
    }
    if (this->reference_source_ == AEC_AUDIO_REFERENCE_PLAYBACK) {
      const size_t reference_bytes = frame_size * sizeof(int16_t);
      const size_t bytes_read = this->reference_buffer_->read(ref, reference_bytes, pdMS_TO_TICKS(100));
      if (bytes_read != reference_bytes) {
        std::memset(reinterpret_cast<uint8_t *>(ref) + bytes_read, 0, reference_bytes - bytes_read);
        this->reference_underruns_++;
      }
    }

    uint64_t reference_sum_sq = 0;
    int32_t reference_peak = 0;
    for (size_t frame = 0; frame < frame_size; frame++) {
      const int32_t sample = ref[frame];
      const int32_t magnitude = sample == INT16_MIN ? 32768 : std::abs(sample);
      reference_peak = std::max(reference_peak, magnitude);
      reference_sum_sq += static_cast<uint64_t>(sample * sample);
    }
    this->reference_rms_.store(std::sqrt(static_cast<float>(reference_sum_sq) / frame_size));
    this->reference_peak_.store(reference_peak);

#ifdef USE_AEC_AUDIO_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_reference(ref, frame_size);
#endif

    int64_t process_start = esp_timer_get_time();
    const int8_t diagnostic_raw_slot = this->diagnostic_raw_slot_.load();
    for (size_t frame = 0; frame < frame_size; frame++) {
      const size_t offset = frame * afe_channels;
      afe_input[offset] = mic[frame];
      afe_input[offset + 1] = mic[frame_size + frame];
      if (this->afe_include_unused_channel_) {
        afe_input[offset + 2] = 0;
        afe_input[offset + 3] = ref[frame];
      } else {
        afe_input[offset + 2] = ref[frame];
      }
    }
    this->afe_iface_->feed(this->afe_data_, afe_input);
    bool afe_result_valid = true;
    const size_t fetch_count = frame_size / this->afe_fetch_chunksize_;
    for (size_t fetch = 0; fetch < fetch_count; fetch++) {
      afe_fetch_result_t *afe_result = this->afe_iface_->fetch_with_delay(this->afe_data_, pdMS_TO_TICKS(100));
      if (afe_result == nullptr || afe_result->ret_value != ESP_OK || afe_result->data == nullptr ||
          afe_result->data_size != this->afe_fetch_chunksize_ * sizeof(int16_t)) {
        afe_result_valid = false;
        break;
      }

      this->vad_state_ = (afe_result->vad_state == 1);

      const size_t output_offset = fetch * this->afe_fetch_chunksize_;
      for (size_t frame = 0; frame < this->afe_fetch_chunksize_; frame++) {
        out[output_offset + frame] = afe_result->data[frame];
        out[frame_size + output_offset + frame] = afe_result->data[frame];
      }
    }
    if (diagnostic_raw_slot >= 0) {
      for (size_t frame = 0; frame < frame_size; frame++) {
        int16_t sample = raw[frame * this->tdm_slots_ + diagnostic_raw_slot];
        out[frame] = sample;
        out[frame_size + frame] = sample;
      }
    } else if (!afe_result_valid) {
      std::memcpy(out, mic, planar_samples * sizeof(int16_t));
      this->dropped_frames_++;
    }

#ifdef USE_AEC_AUDIO_SLOT_LOGS
    if (log_slots) {
      uint64_t output_sum_sq = 0;
      uint32_t output_peak = 0;
      size_t output_clipped = 0;
      for (size_t frame = 0; frame < frame_size; frame++) {
        const int32_t sample = out[frame];
        const uint32_t magnitude = sample == INT16_MIN ? 32768U : static_cast<uint32_t>(std::abs(sample));
        output_peak = std::max(output_peak, magnitude);
        output_sum_sq += static_cast<uint64_t>(sample * sample);
        if (magnitude >= 32760U)
          output_clipped++;
      }
      const float output_rms = std::sqrt(static_cast<float>(output_sum_sq) / frame_size);
      ESP_LOGD(TAG, "AFE output rms=%.1f peak=%" PRIu32 " clipped=%.1f%% valid=%s source=%s listeners=%u",
               output_rms, output_peak, 100.0f * output_clipped / frame_size, YESNO(afe_result_valid),
               diagnostic_raw_slot < 0 ? "AFE" : "BYPASS",
               this->microphone_ == nullptr ? 0 : this->microphone_->get_listener_count());
    }
#endif

#ifdef USE_AEC_AUDIO_TELEMETRY
    if (this->reference_rms_.load() > 500.0f) {
      for (size_t frame = 0; frame < frame_size; frame++) {
        const int32_t ref_sample = ref[frame];
        effect_ref_energy += static_cast<uint64_t>(ref_sample * ref_sample);
        for (uint8_t channel = 0; channel < 2; channel++) {
          const int32_t raw_sample = mic[channel * frame_size + frame];
          const int32_t out_sample = out[channel * frame_size + frame];
          effect_raw_energy[channel] += static_cast<uint64_t>(raw_sample * raw_sample);
          effect_out_energy[channel] += static_cast<uint64_t>(out_sample * out_sample);
          effect_raw_ref_cross[channel] += static_cast<int64_t>(raw_sample) * ref_sample;
          effect_out_ref_cross[channel] += static_cast<int64_t>(out_sample) * ref_sample;
        }
      }
      effect_samples += frame_size;
    }


    bool print_stats = false;
    if (this->reference_rms_.load() > 500.0f) {
      if (log_slots && effect_samples > 0) {
        print_stats = true;
      }
    } else if (effect_samples > 0) {
      print_stats = true; // playback just ended, dump stats immediately
    }

    if (print_stats) {
      for (uint8_t channel = 0; channel < 2; channel++) {
        const double raw_rms = std::sqrt(static_cast<double>(effect_raw_energy[channel]) / effect_samples);
        const double out_rms = std::sqrt(static_cast<double>(effect_out_energy[channel]) / effect_samples);
        const double attenuation_db =
            raw_rms > 0.0 && out_rms > 0.0 ? 20.0 * std::log10(out_rms / raw_rms) : 0.0;
        const double raw_correlation =
            effect_raw_energy[channel] > 0 && effect_ref_energy > 0
                ? static_cast<double>(effect_raw_ref_cross[channel]) /
                      std::sqrt(static_cast<double>(effect_raw_energy[channel]) * effect_ref_energy)
                : 0.0;
        const double out_correlation =
            effect_out_energy[channel] > 0 && effect_ref_energy > 0
                ? static_cast<double>(effect_out_ref_cross[channel]) /
                      std::sqrt(static_cast<double>(effect_out_energy[channel]) * effect_ref_energy)
                : 0.0;
        ESP_LOGI(TAG,
                 "AEC_EFFECT channel=%u mode=%s samples=%u raw_rms=%.1f cleaned_rms=%.1f attenuation_db=%+.2f "
                 "raw_ref_correlation=%+.4f cleaned_ref_correlation=%+.4f",
                 channel, diagnostic_raw_slot < 0 ? "AFE" : "BYPASS",
                 static_cast<unsigned>(effect_samples), raw_rms, out_rms, attenuation_db, raw_correlation,
                 out_correlation);
      }
      effect_samples = 0;
      effect_ref_energy = 0;
      std::fill(std::begin(effect_raw_energy), std::end(effect_raw_energy), 0);
      std::fill(std::begin(effect_out_energy), std::end(effect_out_energy), 0);
      std::fill(std::begin(effect_raw_ref_cross), std::end(effect_raw_ref_cross), 0);
      std::fill(std::begin(effect_out_ref_cross), std::end(effect_out_ref_cross), 0);
    }
#endif
    uint32_t processing_us = esp_timer_get_time() - process_start;
    uint32_t previous_max = this->max_processing_us_.load();
    while (processing_us > previous_max && !this->max_processing_us_.compare_exchange_weak(previous_max, processing_us)) {
    }

#ifdef USE_AEC_AUDIO_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_output(out, frame_size);
#endif
    this->publish_frame_(out, frame_size);
    this->capture_frame_(out, frame_size);   // capture mono channel 0 (AFE output)
  }
}

void AECAudioComponent::run_playback_task_() {
  const size_t frame_size = TRANSPORT_FRAME_SAMPLES;
  const size_t raw_samples = frame_size * this->tdm_slots_;
  const size_t raw_bytes = raw_samples * sizeof(int16_t);
  const size_t planar_samples = frame_size * 2;
  auto *tx = static_cast<int16_t *>(heap_caps_aligned_alloc(16, raw_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *playback = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, planar_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *reference = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (tx == nullptr || playback == nullptr || reference == nullptr) {
    ESP_LOGE(TAG, "Could not allocate playback frame buffers");
    this->dropped_frames_++;
    return;
  }

  while (true) {
    std::memset(playback, 0, planar_samples * sizeof(int16_t));
    uint8_t channels = this->speaker_ == nullptr ? 1 : this->speaker_->get_audio_stream_info().get_channels();
    size_t requested_playback_bytes = frame_size * channels * sizeof(int16_t);

    size_t playback_bytes = 0;
    if (this->playback_buffer_ != nullptr) {
      const size_t available = this->playback_buffer_->available();
      if (available > 0) {
        playback_bytes = this->playback_buffer_->read(playback, std::min(available, requested_playback_bytes), 0);
      }
      if (playback_bytes > 0) {
        this->playback_drained_bytes_.fetch_add(playback_bytes);
      }
      this->buffering_ = playback_bytes == 0;
      if (playback_bytes == 0 && available > 0) {
        this->playback_underruns_++;
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
        initialise_resampler();
#endif
      }
    }

    std::memset(tx, 0, raw_bytes);
    std::memset(reference, 0, frame_size * sizeof(int16_t));
    size_t available_frames = playback_bytes / (channels * sizeof(int16_t));
    for (size_t frame = 0; frame < available_frames; frame++) {
      if (channels == 1) {
        tx[frame * this->tdm_slots_ + this->tx_slots_[0]] = playback[frame];
        tx[frame * this->tdm_slots_ + this->tx_slots_[1]] = playback[frame];
        reference[frame] = playback[frame];
      } else {
        tx[frame * this->tdm_slots_ + this->tx_slots_[0]] = playback[frame * 2];
        tx[frame * this->tdm_slots_ + this->tx_slots_[1]] = playback[frame * 2 + 1];
        reference[frame] =
            static_cast<int16_t>((static_cast<int32_t>(playback[frame * 2]) + playback[frame * 2 + 1]) / 2);
      }
    }

    if (this->reference_buffer_ != nullptr) {
      const size_t reference_bytes = frame_size * sizeof(int16_t);
      if (this->reference_buffer_->free() < reference_bytes)
        this->reference_overflows_++;
      this->reference_buffer_->write(reference, reference_bytes);
    }

    size_t bytes_written = 0;
    int64_t write_start = esp_timer_get_time();
    esp_err_t err = i2s_channel_write(this->tx_handle_, tx, raw_bytes, &bytes_written, 100);
    uint32_t write_us = static_cast<uint32_t>(esp_timer_get_time() - write_start);
    if (err != ESP_OK || bytes_written != raw_bytes) {
      this->tx_errors_++;
    } else {
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
      if (available_frames == frame_size)
        this->update_playback_rate_(write_us, frame_size);
#endif
      if (available_frames > 0 && this->speaker_ != nullptr)
        this->speaker_->notify_output(available_frames, esp_timer_get_time());
    }
  }
}

void AECAudioComponent::publish_frame_(const int16_t *planar, size_t frames) {
  if (this->microphone_ == nullptr)
    return;
  this->microphone_->publish(reinterpret_cast<const uint8_t *>(planar), frames * sizeof(int16_t));
}

void AECAudioComponent::capture_frame_(const int16_t *planar, size_t frames) {
  if (this->capture_buffer_ == nullptr)
    return;
  const uint8_t state = this->capture_state_.load();
  if (state != AEC_CAPTURE_CAPTURING)
    return;

  size_t written = this->capture_samples_written_.load();
  const size_t target_frames = this->capture_target_frames_.load();
  const size_t remaining = target_frames - written;
  const size_t to_copy = std::min(frames, remaining);

  // Copy mono channel 0 from planar layout [ch0..ch0 | ch1..ch1]
  std::memcpy(this->capture_buffer_ + written, planar, to_copy * sizeof(int16_t));
  written += to_copy;
  this->capture_samples_written_.store(written);

  if (written >= target_frames) {
    this->capture_state_.store(AEC_CAPTURE_READY);
    ESP_LOGI(TAG, "Capture complete: %u frames (%.1fs)",
             static_cast<unsigned>(written),
             static_cast<float>(written) / 16000.0f);
  }
}

size_t AECAudioComponent::play_capture() {
  if (this->capture_buffer_ == nullptr || this->playback_buffer_ == nullptr)
    return 0;
  if (this->capture_state_.load() != AEC_CAPTURE_READY)
    return 0;

  const size_t captured_frames = this->capture_samples_written_.load();
  if (captured_frames == 0)
    return 0;

  // The playback pipeline reads mono (1-channel) from the ring buffer when
  // speaker_->get_audio_stream_info().get_channels() == 1.
  const size_t mono_bytes = captured_frames * sizeof(int16_t);
  const size_t chunk_bytes = 512 * sizeof(int16_t);
  size_t offset = 0;
  while (offset < mono_bytes) {
    const size_t to_write = std::min(mono_bytes - offset, chunk_bytes);
    const size_t written = this->play(
        reinterpret_cast<const uint8_t *>(this->capture_buffer_) + offset, to_write, pdMS_TO_TICKS(200));
    offset += written;
    if (written == 0)
      break;  // ring buffer full — caller can retry
  }

  ESP_LOGI(TAG, "Queued %u capture frames for playback", static_cast<unsigned>(captured_frames));
  return captured_frames;
}

size_t AECAudioComponent::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->playback_buffer_ == nullptr)
    return 0;

  this->play_calls_.fetch_add(1);
  this->play_requested_bytes_.fetch_add(length);

#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  uint8_t channels = this->speaker_ == nullptr ? 1 : this->speaker_->get_audio_stream_info().get_channels();
  size_t frames = length / (channels * sizeof(int16_t));
  if (frames == 0)
    return 0;

  const auto *samples = reinterpret_cast<const int16_t *>(data);
  size_t consumed_frames = 0;
  size_t enqueued_bytes = 0;
  while (consumed_frames < frames) {
    const size_t chunk_frames = std::min(frames - consumed_frames, TRANSPORT_FRAME_SAMPLES);
    uint32_t previous_phase_accumulator = this->phase_accumulator_;
    int16_t previous_last_samples[2]{this->last_samples_[0], this->last_samples_[1]};

    auto resampled = this->resample(samples + consumed_frames * channels, chunk_frames, channels);
    size_t resampled_bytes = resampled.size() * sizeof(int16_t);
    if (resampled_bytes == 0) {
      consumed_frames += chunk_frames;
      continue;
    }

    size_t written = this->playback_buffer_->write_without_replacement(resampled.data(), resampled_bytes, ticks_to_wait,
                                                                       false);
    if (written != resampled_bytes) {
      this->phase_accumulator_ = previous_phase_accumulator;
      this->last_samples_[0] = previous_last_samples[0];
      this->last_samples_[1] = previous_last_samples[1];
      break;
    }

    enqueued_bytes += written;
    consumed_frames += chunk_frames;
  }

  const size_t accepted_bytes = consumed_frames * channels * sizeof(int16_t);
  if (accepted_bytes == 0)
    this->play_zero_writes_.fetch_add(1);
  this->play_source_bytes_.fetch_add(accepted_bytes);
  this->play_enqueued_bytes_.fetch_add(enqueued_bytes);
  if (accepted_bytes > 0) {
    this->record_playback_input_rate_(accepted_bytes, channels);
  }
  return accepted_bytes;
#else
  const size_t written = this->playback_buffer_->write_without_replacement(data, length, ticks_to_wait);
  if (written == 0)
    this->play_zero_writes_.fetch_add(1);
  this->play_source_bytes_.fetch_add(written);
  this->play_enqueued_bytes_.fetch_add(written);
  return written;
#endif
}


#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
void AECAudioComponent::initialise_resampler() {
  const uint32_t playback_rate = this->playback_rate_.load();
  this->phase_increment_.store((static_cast<uint64_t>(SAMPLE_RATE) << 16) / playback_rate);
  phase_accumulator_ = 0;
  last_samples_[0] = 0;
  last_samples_[1] = 0;
}

void AECAudioComponent::record_playback_input_rate_(size_t accepted_bytes, uint8_t channels) {
  if (accepted_bytes == 0)
    return;
  if (channels == 0)
    channels = 1;

  const uint32_t now = millis();
  if (this->input_rate_window_start_ms_ == 0 || this->input_rate_channels_ != channels ||
      now - this->input_rate_last_ms_ > INPUT_RATE_IDLE_RESET_MS) {
    // Start a fresh observation window after idle, but keep any locked rate so
    // the next playback can use it immediately while gathering a new estimate.
    this->input_rate_window_start_ms_ = now;
    this->input_rate_window_bytes_ = 0;
    this->input_rate_channels_ = channels;
  }

  this->input_rate_last_ms_ = now;
  this->input_rate_window_bytes_ += accepted_bytes;

  const uint32_t elapsed_ms = now - this->input_rate_window_start_ms_;
  const size_t bytes_per_frame = channels * sizeof(int16_t);
  const size_t frames = this->input_rate_window_bytes_ / bytes_per_frame;
  if (elapsed_ms < INPUT_RATE_ESTIMATE_MIN_WINDOW_MS || frames < INPUT_RATE_ESTIMATE_MIN_FRAMES)
    return;

  uint32_t source_rate =
      static_cast<uint32_t>(((static_cast<uint64_t>(frames) * 1000ULL) + (elapsed_ms / 2)) / elapsed_ms);
  source_rate = std::clamp<uint32_t>(source_rate, PLAYBACK_RATE_MIN, PLAYBACK_RATE_MAX);
  const uint32_t playback_rate = std::clamp<uint32_t>(
      static_cast<uint32_t>(((static_cast<uint64_t>(SAMPLE_RATE) * SAMPLE_RATE) + (source_rate / 2)) / source_rate),
      PLAYBACK_RATE_MIN, PLAYBACK_RATE_MAX);

  this->input_source_rate_.store(source_rate);
  this->input_playback_rate_.store(playback_rate);
  this->input_rate_valid_.store(true);

  this->input_rate_window_start_ms_ = now;
  this->input_rate_window_bytes_ = 0;
}

void AECAudioComponent::update_playback_rate_(uint32_t write_us, size_t frames) {
  const uint32_t expected_us = static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000ULL) / SAMPLE_RATE);
  if (write_us < PLAYBACK_RATE_MEASURE_MIN_US)
    return;

  uint32_t measured_rate =
      static_cast<uint32_t>(((static_cast<uint64_t>(frames) * 1000000ULL) + (write_us / 2)) / write_us);
  measured_rate = std::clamp<uint32_t>(measured_rate, PLAYBACK_RATE_MIN, PLAYBACK_RATE_MAX);
  const bool input_rate_valid = this->input_rate_valid_.load();
  const uint32_t input_source_rate = this->input_source_rate_.load();
  const uint32_t input_playback_rate = this->input_playback_rate_.load();
  const uint32_t target_rate = input_rate_valid ? input_playback_rate : measured_rate;
  uint32_t playback_rate = this->playback_rate_.load();
  const uint32_t previous_playback_rate = playback_rate;

  if (target_rate + PLAYBACK_RATE_ADJUST_STEP < playback_rate) {
    playback_rate -= PLAYBACK_RATE_ADJUST_STEP;
  } else if (target_rate > playback_rate + PLAYBACK_RATE_ADJUST_STEP) {
    playback_rate = std::min<uint32_t>(PLAYBACK_RATE_MAX, playback_rate + PLAYBACK_RATE_ADJUST_STEP);
  } else {
    playback_rate = target_rate;
  }
  if (playback_rate == previous_playback_rate)
    return;

  this->playback_rate_.store(playback_rate);
  this->phase_increment_.store((static_cast<uint64_t>(SAMPLE_RATE) << 16) / playback_rate);

  const uint32_t now = millis();
  if (now - this->last_playback_rate_log_ >= PLAYBACK_RATE_LOG_INTERVAL_MS) {
    this->last_playback_rate_log_ = now;
    if (input_rate_valid) {
      ESP_LOGI(TAG,
               "Playback rate adjust: write_us=%" PRIu32 " expected_us=%" PRIu32 " measured=%" PRIu32
               " Hz input=%" PRIu32 " Hz target=%" PRIu32 " Hz rate=%" PRIu32 " Hz",
               write_us, expected_us, measured_rate, input_source_rate, target_rate, playback_rate);
    } else {
      ESP_LOGI(TAG, "Playback rate adjust: write_us=%" PRIu32 " expected_us=%" PRIu32 " measured=%" PRIu32
                    " Hz target=measured rate=%" PRIu32 " Hz",
               write_us, expected_us, measured_rate, playback_rate);
    }
  }
}

// Continuous Fractional Phase Accumulator
// Pass in the network buffer, returns a slightly smaller/larger buffer for I2S
std::vector<int16_t> AECAudioComponent::resample(const int16_t *input, size_t frames, uint8_t channels) {
  // Pre-calculate approximate output size to prevent reallocation overhead
  const uint32_t phase_increment = this->phase_increment_.load();
  size_t expected_out_frames = (frames * (1ULL << 16)) / phase_increment + 2;

  std::vector<int16_t> output;
  output.reserve(expected_out_frames * channels);

  // Extract integer index and fractional remainder
  uint32_t idx = phase_accumulator_ >> 16;

  // Process until our integer index walks off the end of the input block
  while (idx < frames) {
    uint32_t fraction = phase_accumulator_ & 0xFFFF;

    for (uint8_t channel = 0; channel < channels; channel++) {
      // If idx == 0, s1 is the last sample from the previous packet.
      int16_t s1 = (idx == 0) ? last_samples_[channel] : input[(idx - 1) * channels + channel];
      int16_t s2 = input[idx * channels + channel];

      int32_t diff = s2 - s1;
      int16_t interpolated = s1 + static_cast<int16_t>((diff * static_cast<int32_t>(fraction)) >> 16);
      output.push_back(interpolated);
    }

    phase_accumulator_ += phase_increment;
    idx = phase_accumulator_ >> 16;
  }

  // Save the last frame of this block to stitch seamlessly into the next packet.
  if (frames > 0) {
    for (uint8_t channel = 0; channel < channels; channel++)
      last_samples_[channel] = input[(frames - 1) * channels + channel];
  }

  phase_accumulator_ -= (frames << 16);

  return output;
}
#endif

bool AECAudioComponent::has_buffered_data() const {
  return this->playback_buffer_ != nullptr && this->playback_buffer_->available() > 0;
}

void AECAudioComponent::clear_playback() {
  if (this->playback_buffer_ != nullptr)
    this->playback_buffer_->reset();

  // A playback-backed AEC reference belongs to the same stream as the PCM
  // above. Do not let reference audio from an interrupted stream bleed into
  // the next one; restore only the configured acoustic delay silence.
  if (this->reference_buffer_ != nullptr) {
    this->reference_buffer_->reset();
    if (this->reference_delay_samples_ > 0) {
      std::vector<int16_t> zeros(this->reference_delay_samples_, 0);
      this->reference_buffer_->write(zeros.data(), zeros.size() * sizeof(int16_t));
    }
  }
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  // The phase accumulator and previous sample are stream-local. Retaining
  // them can interpolate the first sample of a new stream with the tail of
  // the previous stream.
  this->initialise_resampler();
  // Do not clear input_rate_valid_/input_playback_rate_: once a source rate has
  // been learned, keep using it for the next playback until a new window updates it.
  this->input_rate_window_start_ms_ = 0;
  this->input_rate_last_ms_ = 0;
  this->input_rate_window_bytes_ = 0;
  this->input_rate_channels_ = 0;
#endif
  this->buffering_ = true;
}

AECAudioMicrophone::~AECAudioMicrophone() {
  heap_caps_free(this->buffer_);
  if (this->buffer_mutex_ != nullptr)
    vSemaphoreDelete(this->buffer_mutex_);
}

void AECAudioMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, SAMPLE_RATE);
  this->buffer_mutex_ = xSemaphoreCreateMutex();
  this->buffer_ = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_BYTES + HISTORY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->buffer_ == nullptr || this->buffer_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate microphone handoff storage");
    this->mark_failed();
    return;
  }
  this->vec.reserve(DELIVERY_BYTES);
}

void AECAudioMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "AFE microphone: enhanced 16-bit mono at 16000 Hz");
  ESP_LOGCONFIG(TAG, "  Rolling history: 1000 ms; utterance queue: 4000 ms in PSRAM");
}

void AECAudioMicrophone::loop() {
  if (this->is_failed())
    return;
  const uint32_t now = millis();
  if (now - this->last_overflow_log_ms_ >= 5000) {
    this->last_overflow_log_ms_ = now;
    const uint32_t dropped = this->dropped_bytes_.exchange(0);
    if (dropped != 0)
      ESP_LOGW(TAG, "Microphone handoff capacity exceeded: dropped %" PRIu32 " bytes", dropped);
  }
  if (now - this->last_heap_log_ms_ >= 30000) {
    this->last_heap_log_ms_ = now;
    ESP_LOGD(TAG, "Audio heap: internal free=%zu largest=%zu minimum=%zu; PSRAM free=%zu largest=%zu",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  // At most 32 ms per pass, with no catch-up burst after a blocked main loop.
  // A 16 ms minimum allows backlog recovery when the loop has spare capacity.
  if (now - this->last_delivery_ms_ < 16)
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  if (this->listeners_ == 0 || this->pre_roll_requested_) {
    xSemaphoreGive(this->buffer_mutex_);
    return;
  }
  const size_t count = std::min(this->buffered_bytes_, DELIVERY_BYTES);
  if (count == 0) {
    xSemaphoreGive(this->buffer_mutex_);
    return;
  }
  this->vec.resize(count);
  const size_t first = std::min(count, BUFFER_BYTES - this->read_offset_);
  memcpy(this->vec.data(), this->buffer_ + this->read_offset_, first);
  memcpy(this->vec.data() + first, this->buffer_, count - first);
  this->read_offset_ = (this->read_offset_ + count) % BUFFER_BYTES;
  this->buffered_bytes_ -= count;
  this->queue_start_byte_ += count;
  // Before the handoff these callbacks feed the wake-word detector. Record
  // the capture position, not elapsed wall time (the main loop can stall).
  if (!this->utterance_active_) {
    this->last_detector_byte_ = this->queue_start_byte_;
    this->detector_position_valid_ = true;
  }
  xSemaphoreGive(this->buffer_mutex_);
  if (count != 0) {
    this->last_delivery_ms_ = now;
    // Callbacks may start/stop listeners; never hold the queue lock here.
    this->data_callbacks_.call(this->vec);
  }
}

void AECAudioMicrophone::start() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  if (this->listeners_.fetch_add(1) == 0 && !this->pre_roll_requested_) {
    this->read_offset_ = 0;
    this->buffered_bytes_ = 0;
  }
  this->state_ = microphone::STATE_RUNNING;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::request_pre_roll() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  // Start 100 ms before the last sample delivered to the detector, retaining
  // all newer capture, including audio not yet delivered when detection fires.
  constexpr uint64_t HEADROOM_BYTES = SAMPLE_RATE * sizeof(int16_t) / 10;
  const uint64_t end = this->detector_position_valid_ ? this->last_detector_byte_ : this->captured_bytes_;
  const uint64_t requested_start = end > HEADROOM_BYTES ? end - HEADROOM_BYTES : 0;
  const uint64_t history_start = this->captured_bytes_ - this->history_bytes_;
  const uint64_t start = std::max(history_start, requested_start);
  this->read_offset_ = 0;
  this->queue_start_byte_ = start;
  this->buffered_bytes_ = this->captured_bytes_ - start;
  const size_t oldest = (this->history_write_offset_ + HISTORY_BYTES - this->buffered_bytes_) % HISTORY_BYTES;
  const size_t first = std::min(this->buffered_bytes_, HISTORY_BYTES - oldest);
  memcpy(this->buffer_, this->buffer_ + BUFFER_BYTES + oldest, first);
  memcpy(this->buffer_ + first, this->buffer_ + BUFFER_BYTES, this->buffered_bytes_ - first);
  this->utterance_active_ = true;
  this->pre_roll_requested_ = true;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::begin_pre_roll_replay() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->pre_roll_requested_ = false;
  this->utterance_active_ = true;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::discard_pending_audio() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->pre_roll_requested_ = false;
  this->utterance_active_ = false;
  this->read_offset_ = 0;
  this->buffered_bytes_ = 0;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::set_response_playing(bool playing) {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->response_playing_ = playing;
  this->response_tail_ = !playing;
  this->response_end_ms_ = millis();
  this->buffered_bytes_ = 0;
  this->read_offset_ = 0;
  this->history_bytes_ = 0;
  this->history_write_offset_ = 0;
  this->detector_position_valid_ = false;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::stop() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  if (this->listeners_ > 0)
    this->listeners_--;
  if (this->listeners_ == 0) {
    this->state_ = microphone::STATE_STOPPED;
    // Preserve the armed handoff across the MWW -> VA listener gap.
    if (!this->pre_roll_requested_) {
      this->utterance_active_ = false;
      this->buffered_bytes_ = 0;
      this->read_offset_ = 0;
    }
  }
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioMicrophone::publish(const uint8_t *data, const size_t data_size) {
  if (this->buffer_ == nullptr || this->buffer_mutex_ == nullptr)
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->captured_bytes_ += data_size;
  // AEC can leave residual TTS. Exclude response audio and a 300 ms acoustic
  // tail from BOTH live delivery and history, including short responses.
  if (this->response_playing_ || (this->response_tail_ && millis() - this->response_end_ms_ < 300)) {
    xSemaphoreGive(this->buffer_mutex_);
    return;
  }
  this->response_tail_ = false;
  // History is independent of listener lifetime and delivery latency. It is
  // bounded, overwritten in place, and never replayed as a separate stream.
  const size_t history_count = std::min(data_size, HISTORY_BYTES);
  const uint8_t *history_data = data + data_size - history_count;
  const size_t history_first = std::min(history_count, HISTORY_BYTES - this->history_write_offset_);
  memcpy(this->buffer_ + BUFFER_BYTES + this->history_write_offset_, history_data, history_first);
  memcpy(this->buffer_ + BUFFER_BYTES, history_data + history_first, history_count - history_first);
  this->history_write_offset_ = (this->history_write_offset_ + history_count) % HISTORY_BYTES;
  this->history_bytes_ = std::min(HISTORY_BYTES, this->history_bytes_ + history_count);
  if (this->listeners_ == 0 && !this->pre_roll_requested_) {
    xSemaphoreGive(this->buffer_mutex_);
    return;
  }
  // Idle wake-word detection should see recent audio after a UI stall.
  // During an utterance, preserve FIFO order and report capacity exhaustion.
  const size_t capacity = this->utterance_active_ ? BUFFER_BYTES : DELIVERY_BYTES * 2;
  if (!this->utterance_active_ && this->buffered_bytes_ + data_size > capacity) {
    this->buffered_bytes_ = 0;
    this->read_offset_ = 0;
  }
  if (data_size <= capacity - this->buffered_bytes_) {
    if (this->buffered_bytes_ == 0)
      this->queue_start_byte_ = this->captured_bytes_ - data_size;
    const size_t write_offset = (this->read_offset_ + this->buffered_bytes_) % BUFFER_BYTES;
    const size_t first = std::min(data_size, BUFFER_BYTES - write_offset);
    memcpy(this->buffer_ + write_offset, data, first);
    memcpy(this->buffer_, data + first, data_size - first);
    this->buffered_bytes_ += data_size;
  } else {
    this->dropped_bytes_.fetch_add(data_size);
  }
  xSemaphoreGive(this->buffer_mutex_);
}

void AECAudioSpeaker::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, SAMPLE_RATE);
  this->state_ = speaker::STATE_STOPPED;
}

void AECAudioSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "AEC speaker: 16-bit mono/stereo at 16000 Hz"); }

void AECAudioSpeaker::loop() {
  if ((this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STOPPING) &&
      !this->has_buffered_data())
    this->state_ = speaker::STATE_STOPPED;
}

size_t AECAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  this->start();
  return this->parent_ == nullptr ? 0 : this->parent_->play(data, length, ticks_to_wait);
}

size_t AECAudioSpeaker::play(const uint8_t *data, size_t length) { return this->play(data, length, 0); }

void AECAudioSpeaker::start() {
  // finish() deliberately drains the current stream. If another stream starts
  // while that drain is still in progress, the remaining PCM must not be
  // prepended to the new stream. A stopped speaker should likewise never own
  // queued data, so make both transitions explicit stream boundaries.
  if (this->state_ == speaker::STATE_STOPPED || this->state_ == speaker::STATE_STOPPING) {
    if (this->parent_ != nullptr)
      this->parent_->clear_playback();
  }
  this->state_ = speaker::STATE_RUNNING;
}

void AECAudioSpeaker::stop() {
  if (this->parent_ != nullptr)
    this->parent_->clear_playback();
  this->state_ = speaker::STATE_STOPPED;
}

void AECAudioSpeaker::finish() { this->state_ = this->has_buffered_data() ? speaker::STATE_STOPPING : speaker::STATE_STOPPED; }

bool AECAudioSpeaker::has_buffered_data() const {
  return this->parent_ != nullptr && this->parent_->has_buffered_data();
}

}  // namespace aec_audio
}  // namespace esphome

#endif
