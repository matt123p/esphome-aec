#include "aec_speexdsp.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_cpu.h>
#include <esp_timer.h>

#include "fftwrap.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aec_speexdsp {

AECSpeexDspComponent *global_aec_speexdsp = nullptr;

// Allocation accounting from speexdsp_mem.c (see os_support_custom.h).
extern "C" size_t speexdsp_mem_internal_bytes(void);
extern "C" size_t speexdsp_mem_psram_bytes(void);
extern "C" void speex_preprocess_profile_get(unsigned long long cycles[6], unsigned int *calls);

static const char *const TAG = "aec_speexdsp";
// Sized for two seconds of stereo PCM (four seconds for the mono Assist
// stream). Long live-model replies can arrive with multi-second gaps followed
// by large bursts, so the previous one-second stereo reserve was too small.
static const uint32_t PLAYBACK_BUFFER_MS = 2000;
// Home Assistant's streaming TTS (e.g. gemini live) arrives in 3-4 s bursts
// separated by 2-3.5 s gaps. After an underrun, wait for roughly three seconds
// of PCM before resuming so a resumed stream rides through the following gap
// instead of cutting out again within ~0.3 s. The timeout ensures a short
// final tail is still played without waiting for a full threshold (and bounds
// the extra start-of-response latency at ~0.5 s).
static const size_t PLAYBACK_REBUFFER_BYTES = 96 * 1024;
static const uint32_t PLAYBACK_REBUFFER_MAX_MS = 500;
static const uint32_t REFERENCE_BUFFER_MS = 250;
static const size_t TRANSPORT_FRAME_SAMPLES = 512;
static const uint32_t DIAGNOSTIC_LOG_INTERVAL_MS = 5000;
static const uint32_t HANDOFF_LOG_INTERVAL_MS = 1000;
static const uint32_t INPUT_RATE_LOG_INTERVAL_MS = 5000;
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
static const uint32_t PLAYBACK_RATE_MIN = SAMPLE_RATE - 1000;
static const uint32_t PLAYBACK_RATE_MAX = SAMPLE_RATE + 1000;
static const uint32_t PLAYBACK_RATE_ADJUST_STEP = 1;
static const uint32_t PLAYBACK_RATE_MEASURE_MIN_US = 8000;
static const uint32_t PLAYBACK_RATE_LOG_INTERVAL_MS = 1000;
#endif

void AECSpeexDspComponent::set_pins(int mclk, int bclk, int lrclk, int din, int dout) {
  this->mclk_pin_ = static_cast<gpio_num_t>(mclk);
  this->bclk_pin_ = static_cast<gpio_num_t>(bclk);
  this->lrclk_pin_ = static_cast<gpio_num_t>(lrclk);
  this->din_pin_ = static_cast<gpio_num_t>(din);
  this->dout_pin_ = static_cast<gpio_num_t>(dout);
}

uint8_t AECSpeexDspComponent::processed_channels_() const {
  if (this->beamforming_enabled_)
    return this->microphone_slots_.size();
  return this->output_channel_ == AEC_SPEEXDSP_OUTPUT_MIXED ? 2 : 1;
}

void AECSpeexDspComponent::setup() {
  global_aec_speexdsp = this;

  std::unique_ptr<ring_buffer::RingBuffer> buffer =
      ring_buffer::RingBuffer::create(SAMPLE_RATE * 2 * 2 * PLAYBACK_BUFFER_MS / 1000);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Could not allocate playback ring buffer");
    this->mark_failed();
    return;
  }
  this->playback_buffer_ = std::shared_ptr<ring_buffer::RingBuffer>(std::move(buffer));
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  this->playback_rate_.store(PLAYBACK_RATE);
  initialise_resampler();
#endif

  if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_PLAYBACK) {
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

  if (this->playback_gain_db_ < 0.0f) {
    this->playback_gain_ = std::pow(10.0f, this->playback_gain_db_ / 20.0f);
    ESP_LOGI(TAG, "Playback digital gain: %.1f dB (%.3fx) — keeps amp/reference loopback out of clipping",
             this->playback_gain_db_, this->playback_gain_);
  }

  if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT && this->reference_delay_samples_ > 0) {
    if (this->reference_delay_samples_ > REFERENCE_DELAY_MAX_SAMPLES) {
      ESP_LOGW(TAG, "reference_delay_samples %u exceeds the analog-slot maximum %d; clamping",
               static_cast<unsigned>(this->reference_delay_samples_), REFERENCE_DELAY_MAX_SAMPLES);
      this->reference_delay_samples_ = REFERENCE_DELAY_MAX_SAMPLES;
    }
    // RAM-only runtime delay: applied every frame from the audio task.
    this->reference_delay_.store(this->reference_delay_samples_);
    ESP_LOGI(TAG, "Analog reference delay: %u samples (%" PRIu32 " ms)",
             static_cast<unsigned>(this->reference_delay_samples_),
             static_cast<uint32_t>(this->reference_delay_samples_) * 1000 / SAMPLE_RATE);
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
             static_cast<unsigned>(CAPTURE_FRAMES), static_cast<unsigned>(CAPTURE_FRAMES * sizeof(int16_t)));
  }

#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  ESP_LOGI(TAG, "Playback resampler enabled: initial_rate=%" PRIu32 " Hz", PLAYBACK_RATE);
#endif

  if (!this->start_i2s_() || !this->start_dsp_()) {
    this->mark_failed();
    return;
  }

  // Dual-core targets keep playback beside ESPHome's CPU-1 loopTask while the
  // canceller runs on CPU 0. ESP32-S2 is single-core, so CPU 1 does not exist;
  // run both tasks on CPU 0 there. Playback normally blocks in its I2S write.
#ifdef CONFIG_FREERTOS_UNICORE
  constexpr BaseType_t playback_core = 0;
#else
  constexpr BaseType_t playback_core = 1;
#endif
  if (xTaskCreatePinnedToCore(AECSpeexDspComponent::playback_task, "speexdsp_playback", 4096, this, 20,
                              &this->playback_task_handle_, playback_core) != pdPASS) {
    ESP_LOGE(TAG, "Could not start playback task");
    this->mark_failed();
    return;
  }
  // Priority 4 on core 0 keeps the DSP below the WiFi/API work that shares the
  // core; I2S DMA absorbs the brief scheduling delay. Over-budget frames yield
  // explicitly (see run_audio_task_).
  if (xTaskCreatePinnedToCore(AECSpeexDspComponent::audio_task, "speexdsp_audio", 8192, this, 4,
                              &this->audio_task_handle_, 0) != pdPASS) {
    ESP_LOGE(TAG, "Could not start audio task");
    this->mark_failed();
  }
}

void AECSpeexDspComponent::dump_config() {
  const uint8_t second_slot =
      this->microphone_slots_.size() > 1 ? this->microphone_slots_[1] : this->tdm_slots_;
  ESP_LOGCONFIG(TAG,
                "AEC SpeexDSP:\n"
                "  I2S port: %u\n"
                "  Pins: MCLK=%d BCLK=%d LRCLK=%d DIN=%d DOUT=%d\n"
                "  TDM slots: %u\n"
                "  Microphone slots: %u, %u\n"
                "  Reference: %s, slot %u\n"
                "  TX slots: %u, %u\n"
                "  Diagnostic raw slot: %d\n"
                "  Processing: SpeexDSP AEC + preprocess, output channel: %s\n"
                "  Frame size: %u (%u ms)\n"
                "  AEC filter length: %u (%u ms tail)\n"
                "  Noise suppression: %s (max %u dB)\n"
                "  Residual echo suppression: %u dB idle / %u dB active\n"
                "  AGC: %s (target %.0f%% full scale)\n"
                "  VAD: %s (start threshold %u%%)",
                this->i2s_port_, this->mclk_pin_, this->bclk_pin_, this->lrclk_pin_, this->din_pin_, this->dout_pin_,
                this->tdm_slots_, this->microphone_slots_[0], second_slot,
                this->reference_source_ == AEC_SPEEXDSP_REFERENCE_PLAYBACK ? "playback buffer" : "analog TDM",
                this->reference_slot_, this->tx_slots_[0], this->tx_slots_[1], this->diagnostic_raw_slot_.load(),
                this->output_channel_ == AEC_SPEEXDSP_OUTPUT_MIXED
                    ? "mixed"
                    : (this->output_channel_ == AEC_SPEEXDSP_OUTPUT_SECOND ? "second" : "first"),
                this->frame_size_, static_cast<unsigned>(this->frame_size_ * 1000u / static_cast<unsigned>(SAMPLE_RATE)),
                this->filter_length_,
                static_cast<unsigned>(this->filter_length_ * 1000u / static_cast<unsigned>(SAMPLE_RATE)),
                YESNO(this->noise_suppression_enabled_), this->noise_suppression_level_db_, this->echo_suppress_db_,
                this->echo_suppress_active_db_, YESNO(this->agc_enabled_), this->agc_target_level_ * 100.0f,
                YESNO(this->vad_enabled_), this->vad_threshold_);
}

void AECSpeexDspComponent::loop() {
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
#ifdef USE_AEC_SPEEXDSP_DIAGNOSTICS
  if (millis() - this->last_diagnostic_log_ >= DIAGNOSTIC_LOG_INTERVAL_MS) {
    this->last_diagnostic_log_ = millis();
    ESP_LOGD(TAG,
             "rx_errors=%" PRIu32 " tx_errors=%" PRIu32 " underruns=%" PRIu32 " dropped=%" PRIu32
             " ref_underruns=%" PRIu32 " ref_overflows=%" PRIu32 " max_processing_us=%" PRIu32
             " playback_bytes=%u reference_bytes=%u reference_rms=%.1f reference_peak=%" PRIu32 " vad_prob=%.2f",
             this->rx_errors_.load(), this->tx_errors_.load(), this->playback_underruns_.load(),
             this->dropped_frames_.load(), this->reference_underruns_.load(), this->reference_overflows_.load(),
             this->max_processing_us_.load(), this->playback_buffer_ == nullptr ? 0 : this->playback_buffer_->available(),
             this->reference_buffer_ == nullptr ? 0 : this->reference_buffer_->available(), this->reference_rms_.load(),
             this->reference_peak_.load(), this->vad_probability_.load());
  }
#endif
}

bool AECSpeexDspComponent::start_i2s_() {
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

bool AECSpeexDspComponent::start_dsp_() {
  const int frame = this->frame_size_;
  const int filter = this->filter_length_;

  // Processed-channel mapping: state index -> microphone slot. The primary
  // state (index 0) always drives the published VAD state.
  if (this->beamforming_enabled_) {
    for (uint8_t c = 0; c < this->microphone_slots_.size(); c++)
      this->processed_slots_[c] = this->microphone_slots_[c];
  } else switch (this->output_channel_) {
    case AEC_SPEEXDSP_OUTPUT_SECOND:
      this->processed_slots_[0] = this->microphone_slots_[1];
      break;
    case AEC_SPEEXDSP_OUTPUT_MIXED:
      this->processed_slots_[0] = this->microphone_slots_[0];
      this->processed_slots_[1] = this->microphone_slots_[1];
      break;
    case AEC_SPEEXDSP_OUTPUT_FIRST:
    default:
      this->processed_slots_[0] = this->microphone_slots_[0];
      break;
  }
  const uint8_t channels = this->processed_channels_();

  const bool shared_aec = this->beamforming_enabled_;
  const uint8_t echo_states = shared_aec ? 1 : channels;
  for (uint8_t c = 0; c < echo_states; c++) {
    // Speex's multichannel state shares the reference FFT, reference history,
    // power estimate, and adaptation bookkeeping across all microphones.
    this->echo_state_[c] = shared_aec
                               ? speex_echo_state_init_mc(frame, filter, channels, 1)
                               : speex_echo_state_init(frame, filter);
    if (this->echo_state_[c] == nullptr) {
      ESP_LOGE(TAG, "Could not allocate SpeexDSP echo canceller (channel %u, frame %d, filter %d)", c, frame, filter);
      this->destroy_dsp_();
      return false;
    }
    int rate = SAMPLE_RATE;
    speex_echo_ctl(this->echo_state_[c], SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

    // Beamforming always has one final preprocessor, regardless of AEC topology.
    if (this->beamforming_enabled_ && c != 0)
      continue;

    this->preprocess_state_[c] = speex_preprocess_state_init(frame, SAMPLE_RATE);
    if (this->preprocess_state_[c] == nullptr) {
      ESP_LOGE(TAG, "Could not allocate SpeexDSP preprocessor (channel %u, frame %d)", c, frame);
      this->destroy_dsp_();
      return false;
    }

    int denoise = this->noise_suppression_enabled_ ? 1 : 0;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    int noise_db = this->noise_suppression_level_db_;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_db);

    int agc = this->agc_enabled_ ? 1 : 0;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC_GATE, &this->agc_gate_config_);
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC, &agc);
    if (this->agc_enabled_) {
      float level = this->agc_target_level_ * 32768.0f;
      speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
      speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &this->agc_max_gain_db_);
    }

    // Link the echo state so the preprocessor applies residual echo
    // suppression (speex_echo_get_residual) on top of the NS estimate.
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_STATE, this->echo_state_[c]);
    int echo_db = this->echo_suppress_db_;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_db);
    int echo_active_db = this->echo_suppress_active_db_;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echo_active_db);

    int vad = this->vad_enabled_ ? 1 : 0;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_VAD, &vad);
    int prob_start = this->vad_threshold_;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_PROB_START, &prob_start);
  }

  ESP_LOGI(TAG,
           "SpeexDSP pipeline: AEC(frame=%d filter=%d ms-tail=%u) + preprocess channels=%u beamforming=%s "
           "AEC topology=%s "
           "NS=%s(-%udB max) "
           "residual-echo=-%udB/-%udB AGC=%s(target %.2f) VAD=%s(start %u%%)",
           frame, filter, static_cast<unsigned>(filter * 1000 / SAMPLE_RATE), channels,
           YESNO(this->beamforming_enabled_),
           shared_aec ? "shared-reference multichannel MDF" : "independent mono MDF",
           YESNO(this->noise_suppression_enabled_), this->noise_suppression_level_db_, this->echo_suppress_db_,
           this->echo_suppress_active_db_, YESNO(this->agc_enabled_), this->agc_target_level_,
           YESNO(this->vad_enabled_), this->vad_threshold_);
  ESP_LOGI(TAG, "SpeexDSP state memory: %u KB internal, %u KB PSRAM",
           static_cast<unsigned>(speexdsp_mem_internal_bytes() / 1024),
           static_cast<unsigned>(speexdsp_mem_psram_bytes() / 1024));
  return true;
}

void AECSpeexDspComponent::destroy_dsp_() {
  for (uint8_t c = 0; c < AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES; c++) {
    if (this->echo_state_[c] != nullptr) {
      speex_echo_state_destroy(this->echo_state_[c]);
      this->echo_state_[c] = nullptr;
    }
    if (this->preprocess_state_[c] != nullptr) {
      speex_preprocess_state_destroy(this->preprocess_state_[c]);
      this->preprocess_state_[c] = nullptr;
    }
  }
}

void AECSpeexDspComponent::audio_task(void *params) {
  static_cast<AECSpeexDspComponent *>(params)->run_audio_task_();
  vTaskDelete(nullptr);
}

void AECSpeexDspComponent::playback_task(void *params) {
  static_cast<AECSpeexDspComponent *>(params)->run_playback_task_();
  vTaskDelete(nullptr);
}

void AECSpeexDspComponent::run_audio_task_() {
  const size_t frame_size = this->frame_size_;
  const size_t raw_samples = frame_size * this->tdm_slots_;
  const size_t raw_bytes = raw_samples * sizeof(int16_t);
  const uint8_t channels = this->processed_channels_();

  auto *raw = static_cast<int16_t *>(heap_caps_aligned_alloc(16, raw_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *mic = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, channels * frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *ref =
      static_cast<int16_t *>(heap_caps_aligned_alloc(16, frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *processed = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, channels * frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *out = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, frame_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (raw == nullptr || mic == nullptr || ref == nullptr || processed == nullptr || out == nullptr) {
    ESP_LOGE(TAG, "Could not allocate audio frame buffers");
    this->dropped_frames_++;
    return;
  }

  uint32_t last_slot_log = 0;
  // CPU load accounting. The loop is audio-driven: each iteration consumes
  // exactly frame_size samples, so the frame's real-time budget is
  // frame_size / 16 kHz (16000 us for the default 256). Everything the task
  // does except the blocking wait for DMA audio is "busy" time; the ratio
  // busy/budget is this component's share of one core.
  constexpr uint32_t LOAD_LOG_INTERVAL_MS = 5000;
  const uint32_t frame_budget_us = frame_size * 1000000u / SAMPLE_RATE;
  uint32_t load_frames = 0;
  uint64_t load_processing_us = 0;
  uint32_t load_peak_us = 0;
  uint32_t last_load_log = millis();
#ifdef USE_AEC_SPEEXDSP_PROFILE
  uint64_t profile_echo_cycles = 0;
  uint64_t profile_preprocess_cycles = 0;
  spx_fft_profile_t profile_fft_previous{};
  spx_fft_profile_get(&profile_fft_previous);
  unsigned long long profile_preprocess_stage_previous[6]{};
  unsigned int profile_preprocess_calls_previous = 0;
  speex_preprocess_profile_get(profile_preprocess_stage_previous, &profile_preprocess_calls_previous);
  uint64_t profile_localization_previous = this->beamformer_.get_localization_cycles();
  uint64_t profile_beamforming_previous = this->beamformer_.get_beamforming_cycles();
  uint32_t profile_localization_calls_previous = this->beamformer_.get_localization_calls();
  uint32_t profile_beamforming_calls_previous = this->beamformer_.get_beamforming_calls();
#endif
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
  // Fixed one-second windows include playback tails and pauses. These are
  // stage levels, not delay-aligned ERLE or proof of near-end speech.
  uint32_t stage_last_log = millis();
  uint32_t stage_frames = 0;
  uint32_t stage_reference_frames = 0;
  uint64_t stage_raw[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  uint64_t stage_aec[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  uint64_t stage_pre[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  uint64_t stage_post[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  uint64_t stage_reference = 0, stage_output = 0;
  uint32_t stage_output_peak = 0, stage_output_clips = 0;
  auto stage_energy = [frame_size](const int16_t *samples, size_t stride) -> uint64_t {
    uint64_t energy = 0;
    for (size_t n = 0; n < frame_size; ++n) {
      const int32_t sample = samples[n * stride];
      energy += static_cast<uint64_t>(sample * sample);
    }
    return energy;
  };
  uint64_t effect_samples = 0;
  uint64_t effect_ref_energy = 0;
  uint64_t effect_raw_energy[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  uint64_t effect_out_energy[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  int64_t effect_raw_ref_cross[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  int64_t effect_out_ref_cross[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
#endif
  while (true) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(this->rx_handle_, raw, raw_bytes, &bytes_read, 100);
    if (err != ESP_OK || bytes_read != raw_bytes) {
      this->rx_errors_++;
      continue;
    }

    // Busy time starts after the blocking DMA read: everything from here to
    // the end of the iteration is work done on behalf of this frame.
    const int64_t process_start = esp_timer_get_time();
    const bool log_slots = millis() - last_slot_log >= DIAGNOSTIC_LOG_INTERVAL_MS;
    if (log_slots)
      last_slot_log = millis();

#ifdef USE_AEC_SPEEXDSP_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_raw(raw, frame_size, this->tdm_slots_);
#endif

#ifdef USE_AEC_SPEEXDSP_SLOT_LOGS
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

    // Extract straight into the AEC's input layout: interleaved for shared
    // multichannel AEC, planar for independent mono states.
    for (size_t frame = 0; frame < frame_size; frame++) {
      for (uint8_t c = 0; c < channels; c++)
        mic[this->beamforming_enabled_ ? frame * channels + c : c * frame_size + frame] =
            raw[frame * this->tdm_slots_ + this->processed_slots_[c]];
      if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT)
        ref[frame] = raw[frame * this->tdm_slots_ + this->reference_slot_];
    }
    if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_PLAYBACK) {
      const size_t reference_bytes = frame_size * sizeof(int16_t);
      const size_t reference_read = this->reference_buffer_->read(ref, reference_bytes, pdMS_TO_TICKS(100));
      if (reference_read != reference_bytes) {
        std::memset(reinterpret_cast<uint8_t *>(ref) + reference_read, 0, reference_bytes - reference_read);
        this->reference_underruns_++;
      }
    }

    if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT)
      this->apply_reference_delay_(ref, frame_size);
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

#ifdef USE_AEC_SPEEXDSP_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_reference(ref, frame_size);
#endif

    bool speech_detected = false;
    if (this->agc_enabled_ && this->agc_gate_config_.enabled) {
      float reference_rms = this->reference_rms_.load();
      for (uint8_t c = 0; c < (this->beamforming_enabled_ ? 1 : channels); ++c)
        if (this->preprocess_state_[c] != nullptr)
          speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_REFERENCE_RMS, &reference_rms);
    }
    const int8_t diagnostic_raw_slot = this->diagnostic_raw_slot_.load();
    if (diagnostic_raw_slot >= 0) {
      // Deliberate DSP bypass: publish the raw slot as mono for slot mapping.
      for (size_t frame = 0; frame < frame_size; frame++)
        out[frame] = raw[frame * this->tdm_slots_ + diagnostic_raw_slot];
    } else if (this->echo_state_[0] == nullptr || this->preprocess_state_[0] == nullptr) {
      for (size_t frame = 0; frame < frame_size; frame++)
        out[frame] = mic[this->beamforming_enabled_ ? frame * channels : frame];
      this->dropped_frames_++;
    } else {
      if (this->beamforming_enabled_) {
        // Both AEC and beamformer consume the interleaved layout directly.
#ifdef USE_AEC_SPEEXDSP_PROFILE
        uint32_t profile_start = esp_cpu_get_cycle_count();
#endif
        const size_t beam_stride = channels;
        speex_echo_cancellation(this->echo_state_[0], mic, ref, processed);
#ifdef USE_AEC_SPEEXDSP_PROFILE
        profile_echo_cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - profile_start);
#endif
        const int16_t *beam_inputs[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
        for (uint8_t c = 0; c < channels; c++)
          beam_inputs[c] = processed + c;
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
        for (uint8_t c = 0; c < channels; ++c)
          stage_aec[c] += stage_energy(beam_inputs[c], beam_stride);
#endif
        this->beamformer_.process(beam_inputs, frame_size, out, beam_stride);
        // Apply exactly the same steering to raw microphones. Reuse processed
        // scratch only after the cleaned beamformer has saved its history.
        // The difference is the removed echo in the published beam's domain.
        for (uint8_t c = 0; c < channels; ++c)
          beam_inputs[c] = mic + c;
        this->raw_beamformer_.process_with_steering(this->beamformer_, beam_inputs, frame_size, processed, beam_stride);
        speex_echo_set_beamformed_estimate(this->echo_state_[0], processed, out);
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
        stage_pre[0] += stage_energy(out, 1);
#endif
#ifdef USE_AEC_SPEEXDSP_PROFILE
        profile_start = esp_cpu_get_cycle_count();
#endif
        speech_detected = speex_preprocess_run(this->preprocess_state_[0], out) != 0;
#ifdef USE_AEC_SPEEXDSP_PROFILE
        profile_preprocess_cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - profile_start);
#endif
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
        stage_post[0] += stage_energy(out, 1);
#endif
      } else {
        for (uint8_t c = 0; c < channels; c++) {
          int16_t *channel_out = processed + c * frame_size;
          const int16_t *channel_mic = mic + c * frame_size;
#ifdef USE_AEC_SPEEXDSP_PROFILE
          uint32_t profile_start = esp_cpu_get_cycle_count();
#endif
          speex_echo_cancellation(this->echo_state_[c], channel_mic, ref, channel_out);
#ifdef USE_AEC_SPEEXDSP_PROFILE
          profile_echo_cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - profile_start);
#endif
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
          const uint64_t energy = stage_energy(channel_out, 1);
          stage_aec[c] += energy;
          stage_pre[c] += energy;
#endif
#ifdef USE_AEC_SPEEXDSP_PROFILE
          profile_start = esp_cpu_get_cycle_count();
#endif
          const int detected = speex_preprocess_run(this->preprocess_state_[c], channel_out);
          if (c == 0)
            speech_detected = detected != 0;
#ifdef USE_AEC_SPEEXDSP_PROFILE
          profile_preprocess_cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - profile_start);
#endif
#ifdef USE_AEC_SPEEXDSP_TELEMETRY
          stage_post[c] += stage_energy(channel_out, 1);
#endif
        }
        if (channels == 2) {
          for (size_t frame = 0; frame < frame_size; frame++)
            out[frame] = static_cast<int16_t>((processed[frame] + processed[frame_size + frame]) / 2);
        } else {
          std::memcpy(out, processed, frame_size * sizeof(int16_t));
        }
      }
    }

    // VAD state and probability come from the primary preprocessor.
    if (this->preprocess_state_[0] != nullptr && diagnostic_raw_slot < 0) {
      // GET_VAD reports enablement; the run() return value is the decision.
      this->vad_state_.store(this->vad_enabled_ && speech_detected);
      spx_int32_t prob = 0;
      speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_GET_PROB, &prob);
      this->vad_probability_.store(static_cast<float>(prob) / 100.0f);
    }

#ifdef USE_AEC_SPEEXDSP_TELEMETRY
    if (diagnostic_raw_slot < 0 && this->echo_state_[0] != nullptr && this->preprocess_state_[0] != nullptr) {
      ++stage_frames;
      stage_reference_frames += this->reference_rms_.load() > 500.0f;
      stage_reference += reference_sum_sq;
      stage_output += stage_energy(out, 1);
      for (uint8_t c = 0; c < channels; ++c)
        stage_raw[c] += stage_energy(raw + this->processed_slots_[c], this->tdm_slots_);
      for (size_t n = 0; n < frame_size; ++n) {
        const uint32_t magnitude = std::abs(static_cast<int32_t>(out[n]));
        stage_output_peak = std::max(stage_output_peak, magnitude);
        stage_output_clips += magnitude >= 32767;
      }
    }
    const uint32_t stage_now = millis();
    if (stage_now - stage_last_log >= 1000 && stage_frames > 0) {
      const double samples = static_cast<double>(stage_frames) * frame_size;
      auto rms = [samples](uint64_t energy) { return std::sqrt(static_cast<double>(energy) / samples); };
      ESP_LOGI(TAG, "AEC_STAGES window_ms=%" PRIu32 " frames=%" PRIu32
                   " ref_active=%" PRIu32 " ref_rms=%.1f final_rms=%.1f peak=%" PRIu32
                   " clips=%" PRIu32 " beam=%s",
               stage_now - stage_last_log, stage_frames, stage_reference_frames, rms(stage_reference),
               rms(stage_output), stage_output_peak, stage_output_clips, YESNO(this->beamforming_enabled_));
      for (uint8_t c = 0; c < channels; ++c) {
        ESP_LOGI(TAG, "AEC_STAGE_MIC channel=%u slot=%u raw_rms=%.1f post_aec_rms=%.1f",
                 c, this->processed_slots_[c], rms(stage_raw[c]), rms(stage_aec[c]));
      }
      const uint8_t pre_channels = this->beamforming_enabled_ ? 1 : channels;
      for (uint8_t c = 0; c < pre_channels; ++c) {
        spx_int32_t gain_db = 0, probability = 0;
        if (this->agc_enabled_ && this->agc_gate_config_.enabled) {
          SpeexAgcGateState gate{};
          speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_GET_AGC_GATE_STATE, &gate);
          ESP_LOGI(TAG, "AGC_GATE last_frame channel=%u reference_active=%s eligible=%s startup_guard=%s pre_agc_rms=%.1f",
                   c, YESNO(gate.reference_active), YESNO(gate.open), YESNO(gate.startup_guard), gate.rms);
        }
        if (this->preprocess_state_[c] != nullptr) {
          if (this->agc_enabled_)
            speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_GET_AGC_GAIN, &gain_db);
          speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_GET_PROB, &probability);
        }
        ESP_LOGI(TAG, "AEC_STAGE_PRE channel=%u input_rms=%.1f output_rms=%.1f agc=%s"
                     " agc_gain_db_last=%ld speech_probability_last=%ld%%",
                 c, rms(stage_pre[c]), rms(stage_post[c]), YESNO(this->agc_enabled_),
                 static_cast<long>(gain_db), static_cast<long>(probability));
        SpeexEchoResidualDiagnostics echo_diag{};
        SpeexPreprocessResidualDiagnostics pre_diag{};
        speex_echo_ctl(this->echo_state_[c], SPEEX_ECHO_GET_RESIDUAL_DIAGNOSTICS, &echo_diag);
        speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_GET_RESIDUAL_DIAGNOSTICS, &pre_diag);
        ESP_LOGI(TAG, "AEC_RESIDUAL last_frame channel=%u adapted=%s leak=%.4f removed_rms=%.1f"
                     " input_ps=%.3g residual_ps=%.3g echo_ps=%.3g noise_ps=%.3g suppressed_ps=%.3g"
                     " Pframe=%.3f echo_floor_db=%.1f",
                 c, YESNO(echo_diag.adapted), echo_diag.leak, echo_diag.removed_rms,
                 pre_diag.input_power, pre_diag.residual_power, pre_diag.echo_power, pre_diag.noise_power,
                 pre_diag.suppressed_power, pre_diag.frame_probability, pre_diag.effective_echo_suppress_db);
      }
      stage_last_log = stage_now;
      stage_frames = stage_reference_frames = 0;
      stage_reference = stage_output = 0;
      stage_output_peak = stage_output_clips = 0;
      std::fill(std::begin(stage_raw), std::end(stage_raw), 0);
      std::fill(std::begin(stage_aec), std::end(stage_aec), 0);
      std::fill(std::begin(stage_pre), std::end(stage_pre), 0);
      std::fill(std::begin(stage_post), std::end(stage_post), 0);
    }
    if (this->reference_rms_.load() > 500.0f && diagnostic_raw_slot < 0) {
      for (size_t frame = 0; frame < frame_size; frame++) {
        const int32_t ref_sample = ref[frame];
        effect_ref_energy += static_cast<uint64_t>(ref_sample * ref_sample);
        for (uint8_t c = 0; c < channels; c++) {
          const int32_t raw_sample = raw[frame * this->tdm_slots_ + this->processed_slots_[c]];
          const int32_t out_sample = out[frame];
          effect_raw_energy[c] += static_cast<uint64_t>(raw_sample * raw_sample);
          effect_out_energy[c] += static_cast<uint64_t>(out_sample * out_sample);
          effect_raw_ref_cross[c] += static_cast<int64_t>(raw_sample) * ref_sample;
          effect_out_ref_cross[c] += static_cast<int64_t>(out_sample) * ref_sample;
        }
      }
      effect_samples += frame_size;
    }

    bool print_stats = false;
    if (this->reference_rms_.load() > 500.0f && diagnostic_raw_slot < 0) {
      if (log_slots && effect_samples > 0) {
        print_stats = true;
      }
    } else if (effect_samples > 0) {
      print_stats = true;  // playback just ended, dump stats immediately
    }

    if (print_stats) {
      for (uint8_t c = 0; c < channels; c++) {
        const double raw_rms = std::sqrt(static_cast<double>(effect_raw_energy[c]) / effect_samples);
        const double out_rms = std::sqrt(static_cast<double>(effect_out_energy[c]) / effect_samples);
        const double attenuation_db =
            raw_rms > 0.0 && out_rms > 0.0 ? 20.0 * std::log10(out_rms / raw_rms) : 0.0;
        const double raw_correlation =
            effect_raw_energy[c] > 0 && effect_ref_energy > 0
                ? static_cast<double>(effect_raw_ref_cross[c]) /
                      std::sqrt(static_cast<double>(effect_raw_energy[c]) * effect_ref_energy)
                : 0.0;
        const double out_correlation =
            effect_out_energy[c] > 0 && effect_ref_energy > 0
                ? static_cast<double>(effect_out_ref_cross[c]) /
                      std::sqrt(static_cast<double>(effect_out_energy[c]) * effect_ref_energy)
                : 0.0;
        ESP_LOGI(TAG,
                 "AEC_EFFECT channel=%u slot=%u samples=%u raw_rms=%.1f cleaned_rms=%.1f attenuation_db=%+.2f "
                 "raw_ref_correlation=%+.4f cleaned_ref_correlation=%+.4f",
                 c, this->processed_slots_[c], static_cast<unsigned>(effect_samples), raw_rms, out_rms, attenuation_db,
                 raw_correlation, out_correlation);
      }
      effect_samples = 0;
      effect_ref_energy = 0;
      std::fill(std::begin(effect_raw_energy), std::end(effect_raw_energy), 0);
      std::fill(std::begin(effect_out_energy), std::end(effect_out_energy), 0);
      std::fill(std::begin(effect_raw_ref_cross), std::end(effect_raw_ref_cross), 0);
      std::fill(std::begin(effect_out_ref_cross), std::end(effect_out_ref_cross), 0);
    }
#endif

#ifdef VOICE_ASSISTANT_BARGE_IN
    // This is only an inactivity meter. It must not drive turn detection.
    constexpr uint32_t MICROPHONE_ACTIVITY_PEAK = 600;
    for (size_t frame = 0; frame < frame_size; frame++) {
      const int32_t sample = out[frame];
      const uint32_t magnitude = sample == INT16_MIN ? 32768U : static_cast<uint32_t>(std::abs(sample));
      if (magnitude >= MICROPHONE_ACTIVITY_PEAK) {
        this->last_microphone_activity_ms_.store(millis());
        break;
      }
    }
#endif

#ifdef USE_AEC_SPEEXDSP_SLOT_LOGS
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
      ESP_LOGD(TAG, "DSP output rms=%.1f peak=%" PRIu32 " clipped=%.1f%% bypass=%s vad=%.2f listeners=%u",
               output_rms, output_peak, 100.0f * output_clipped / frame_size,
               YESNO(diagnostic_raw_slot >= 0), this->vad_probability_.load(),
               this->microphone_ == nullptr ? 0 : this->microphone_->get_listener_count());
    }
#endif

#ifdef USE_AEC_SPEEXDSP_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_output(out, frame_size);
#endif
    this->publish_frame_(out, frame_size);
    this->capture_frame_(out, frame_size);

    // Total busy time for this frame: DSP, reference prep, meters, publish.
    const uint32_t processing_us = static_cast<uint32_t>(esp_timer_get_time() - process_start);
    uint32_t previous_max = this->max_processing_us_.load();
    while (processing_us > previous_max && !this->max_processing_us_.compare_exchange_weak(previous_max, processing_us)) {
    }

    // CPU load vs real time. The task is audio-driven, so busy/budget is this
    // component's share of one core; a value >= 100% means the canceller
    // cannot keep up with 16 kHz and a transport overrun becomes likely.
    load_frames++;
    load_processing_us += processing_us;
    if (processing_us > load_peak_us)
      load_peak_us = processing_us;
    if (millis() - last_load_log >= LOAD_LOG_INTERVAL_MS) {
      const uint32_t elapsed_ms = millis() - last_load_log;
      if (load_frames > 0) {
        const uint32_t avg_us = static_cast<uint32_t>(load_processing_us / load_frames);
        const float avg_pct = 100.0f * avg_us / frame_budget_us;
        const float peak_pct = 100.0f * load_peak_us / frame_budget_us;
#ifdef USE_AEC_SPEEXDSP_PROFILE
        ESP_LOGI(TAG,
                 "DSP load: %" PRIu32 " frames in %" PRIu32 " ms: processing avg %" PRIu32
                 " us/frame (%.1f%% of real time), peak %" PRIu32 " (%.1f%%), frame budget %" PRIu32 " us",
                 load_frames, elapsed_ms, avg_us, avg_pct, load_peak_us, peak_pct, frame_budget_us);

        spx_fft_profile_t fft_now{};
        spx_fft_profile_get(&fft_now);
        const uint64_t forward_cycles = fft_now.forward_cycles - profile_fft_previous.forward_cycles;
        const uint64_t inverse_cycles = fft_now.inverse_cycles - profile_fft_previous.inverse_cycles;
        const uint32_t forward_calls = fft_now.forward_calls - profile_fft_previous.forward_calls;
        const uint32_t inverse_calls = fft_now.inverse_calls - profile_fft_previous.inverse_calls;
        const uint64_t fft_cycles = forward_cycles + inverse_cycles;
        const uint64_t speex_cycles = profile_echo_cycles + profile_preprocess_cycles;
        ESP_LOGI(TAG,
                 "DSP profile: cycles/frame echo=%llu preprocess=%llu; FFT=%llu (%.1f%% of echo+pre), "
                 "forward=%" PRIu32 " calls %.0f cyc/call, inverse=%" PRIu32 " calls %.0f cyc/call",
                 profile_echo_cycles / load_frames, profile_preprocess_cycles / load_frames,
                 fft_cycles / load_frames,
                 speex_cycles > 0 ? 100.0 * static_cast<double>(fft_cycles) / speex_cycles : 0.0,
                 forward_calls, forward_calls > 0 ? static_cast<double>(forward_cycles) / forward_calls : 0.0,
                 inverse_calls, inverse_calls > 0 ? static_cast<double>(inverse_cycles) / inverse_calls : 0.0);
        unsigned long long preprocess_stage_now[6]{};
        unsigned int preprocess_calls_now = 0;
        speex_preprocess_profile_get(preprocess_stage_now, &preprocess_calls_now);
        const uint32_t preprocess_calls = preprocess_calls_now - profile_preprocess_calls_previous;
        uint64_t preprocess_stage_per_call[6]{};
        for (size_t i = 0; i < 6; i++) {
          const uint64_t stage_cycles = preprocess_stage_now[i] - profile_preprocess_stage_previous[i];
          preprocess_stage_per_call[i] = preprocess_calls > 0 ? stage_cycles / preprocess_calls : 0;
          profile_preprocess_stage_previous[i] = preprocess_stage_now[i];
        }
        ESP_LOGI(TAG,
                 "Preprocess profile: %" PRIu32 " calls; cycles/call residual=%" PRIu64
                 " analysis=%" PRIu64 " noise_snr=%" PRIu64 " band_gain=%" PRIu64
                 " linear_gain=%" PRIu64 " synthesis=%" PRIu64,
                 preprocess_calls, preprocess_stage_per_call[0], preprocess_stage_per_call[1],
                 preprocess_stage_per_call[2], preprocess_stage_per_call[3], preprocess_stage_per_call[4],
                 preprocess_stage_per_call[5]);
        profile_preprocess_calls_previous = preprocess_calls_now;
        if (this->beamforming_enabled_) {
          const uint64_t localization_now = this->beamformer_.get_localization_cycles();
          const uint64_t beamforming_now = this->beamformer_.get_beamforming_cycles();
          const uint32_t localization_calls_now = this->beamformer_.get_localization_calls();
          const uint32_t beamforming_calls_now = this->beamformer_.get_beamforming_calls();
          const uint32_t localization_calls = localization_calls_now - profile_localization_calls_previous;
          const uint32_t beamforming_calls = beamforming_calls_now - profile_beamforming_calls_previous;
          ESP_LOGI(TAG,
                   "Beamformer profile: localization=%" PRIu64 " cyc/call (%" PRIu32
                   " calls), delay-sum=%" PRIu64 " cyc/frame; TDOA1=%+.2f samples confidence=%u%%",
                   localization_calls > 0 ? (localization_now - profile_localization_previous) / localization_calls : 0,
                   localization_calls,
                   beamforming_calls > 0 ? (beamforming_now - profile_beamforming_previous) / beamforming_calls : 0,
                   static_cast<double>(this->beamformer_.get_tdoa_q15(1)) / 32768.0,
                   static_cast<unsigned>(this->beamformer_.get_confidence_q15(1) * 100U / 32768U));
          profile_localization_previous = localization_now;
          profile_beamforming_previous = beamforming_now;
          profile_localization_calls_previous = localization_calls_now;
          profile_beamforming_calls_previous = beamforming_calls_now;
        }
        profile_echo_cycles = 0;
        profile_preprocess_cycles = 0;
        profile_fft_previous = fft_now;
#endif
      }
      load_frames = 0;
      load_processing_us = 0;
      load_peak_us = 0;
      last_load_log = millis();
    }

    // Over-budget frame: yield briefly so system work can run instead of
    // waiting for the watchdog. Diagnostics report the processing peak; a
    // subsequent transport overrun may also appear as an RX error.
    if (processing_us > frame_budget_us)
      vTaskDelay(1);
  }
}

void AECSpeexDspComponent::run_playback_task_() {
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
      const uint32_t now = millis();
      bool buffering = this->buffering_.load();
      if (buffering && this->buffering_since_ms_.load() == 0)
        this->buffering_since_ms_.store(now);

      const bool rebuffer_ready = available >= PLAYBACK_REBUFFER_BYTES ||
                                  (this->buffering_since_ms_.load() != 0 &&
                                   now - this->buffering_since_ms_.load() >= PLAYBACK_REBUFFER_MAX_MS);
      if (available > 0 && (!buffering || rebuffer_ready)) {
        this->buffering_.store(false);
        this->buffering_since_ms_.store(0);
        playback_bytes = this->playback_buffer_->read(playback, std::min(available, requested_playback_bytes), 0);
      }
      if (playback_bytes > 0) {
        this->playback_drained_bytes_.fetch_add(playback_bytes);
      }
      if (playback_bytes == 0 && available == 0 && !buffering) {
        this->buffering_.store(true);
        this->buffering_since_ms_.store(now);
        this->playback_underruns_++;
        ESP_LOGW(TAG, "Playback underrun #%" PRIu32 ": ring empty, prebuffering %u KB before resuming",
                 this->playback_underruns_.load(), static_cast<unsigned>(PLAYBACK_REBUFFER_BYTES / 1024));
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
        initialise_resampler();
#endif
      }
    }

    // Attenuate media before the reference tap so TX output and AEC reference
    // stay identical.
    if (this->playback_gain_ != 1.0f && playback_bytes > 0) {
      const float gain = this->playback_gain_;
      const size_t samples = playback_bytes / sizeof(int16_t);
      for (size_t i = 0; i < samples; ++i) {
        int32_t scaled = static_cast<int32_t>(std::lround(static_cast<float>(playback[i]) * gain));
        if (scaled > 32767)
          scaled = 32767;
        else if (scaled < -32768)
          scaled = -32768;
        playback[i] = static_cast<int16_t>(scaled);
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

#ifdef VOICE_ASSISTANT_BARGE_IN
    constexpr uint32_t PLAYBACK_ACTIVITY_PEAK = 128;
    for (size_t frame = 0; frame < available_frames; frame++) {
      const int32_t sample = reference[frame];
      const uint32_t magnitude = sample == INT16_MIN ? 32768U : static_cast<uint32_t>(std::abs(sample));
      if (magnitude >= PLAYBACK_ACTIVITY_PEAK) {
        this->last_playback_activity_ms_.store(millis());
        break;
      }
    }
#endif

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
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
      if (available_frames == frame_size)
        this->update_playback_rate_(write_us, frame_size);
#endif
      if (available_frames > 0 && this->speaker_ != nullptr)
        this->speaker_->notify_output(available_frames, esp_timer_get_time());
    }
  }
}

void AECSpeexDspComponent::apply_reference_delay_(int16_t *ref, size_t frames) {
  // Delays only the analogue reference, never the microphone or playback.
  // The history keeps running even when the delay is 0; zero delay is a
  // pass-through.
  const int d = this->reference_delay_.load();
  if (d <= 0)
    return;
  constexpr int size = REFERENCE_DELAY_MAX_SAMPLES + 1;
  for (size_t i = 0; i < frames; ++i) {
    this->reference_history_[this->reference_history_pos_] = ref[i];
    ref[i] = this->reference_history_[(this->reference_history_pos_ + size - d) % size];
    this->reference_history_pos_ = (this->reference_history_pos_ + 1) % size;
  }
}

void AECSpeexDspComponent::publish_frame_(const int16_t *mono, size_t frames) {
  if (this->microphone_ == nullptr)
    return;
  this->microphone_->publish(reinterpret_cast<const uint8_t *>(mono), frames * sizeof(int16_t));
}

void AECSpeexDspComponent::capture_frame_(const int16_t *mono, size_t frames) {
  if (this->capture_buffer_ == nullptr)
    return;
  const uint8_t state = this->capture_state_.load();
  if (state != AEC_SPEEXDSP_CAPTURE_CAPTURING)
    return;

  size_t written = this->capture_samples_written_.load();
  const size_t target_frames = this->capture_target_frames_.load();
  const size_t remaining = target_frames - written;
  const size_t to_copy = std::min(frames, remaining);

  std::memcpy(this->capture_buffer_ + written, mono, to_copy * sizeof(int16_t));
  written += to_copy;
  this->capture_samples_written_.store(written);

  if (written >= target_frames) {
    this->capture_state_.store(AEC_SPEEXDSP_CAPTURE_READY);
    ESP_LOGI(TAG, "Capture complete: %u frames (%.1fs)", static_cast<unsigned>(written),
             static_cast<float>(written) / 16000.0f);
  }
}

size_t AECSpeexDspComponent::play_capture() {
  if (this->capture_buffer_ == nullptr || this->playback_buffer_ == nullptr)
    return 0;
  if (this->capture_state_.load() != AEC_SPEEXDSP_CAPTURE_READY)
    return 0;

  const size_t captured_frames = this->capture_samples_written_.load();
  if (captured_frames == 0)
    return 0;

  const size_t mono_bytes = captured_frames * sizeof(int16_t);
  const size_t chunk_bytes = 512 * sizeof(int16_t);
  size_t offset = 0;
  while (offset < mono_bytes) {
    const size_t to_write = std::min(mono_bytes - offset, chunk_bytes);
    const size_t written = this->play(reinterpret_cast<const uint8_t *>(this->capture_buffer_) + offset, to_write,
                                      pdMS_TO_TICKS(200));
    offset += written;
    if (written == 0)
      break;  // ring buffer full — caller can retry
  }

  ESP_LOGI(TAG, "Queued %u capture frames for playback", static_cast<unsigned>(captured_frames));
  return captured_frames;
}

size_t AECSpeexDspComponent::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->playback_buffer_ == nullptr)
    return 0;

  this->play_calls_.fetch_add(1);
  this->play_requested_bytes_.fetch_add(length);

#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
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

#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
void AECSpeexDspComponent::initialise_resampler() {
  const uint32_t playback_rate = this->playback_rate_.load();
  this->phase_increment_.store((static_cast<uint64_t>(SAMPLE_RATE) << 16) / playback_rate);
  phase_accumulator_ = 0;
  last_samples_[0] = 0;
  last_samples_[1] = 0;
}

void AECSpeexDspComponent::update_playback_rate_(uint32_t write_us, size_t frames) {
  const uint32_t expected_us = static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000ULL) / SAMPLE_RATE);
  if (write_us < PLAYBACK_RATE_MEASURE_MIN_US)
    return;

  uint32_t measured_rate =
      static_cast<uint32_t>(((static_cast<uint64_t>(frames) * 1000000ULL) + (write_us / 2)) / write_us);
  measured_rate = std::clamp<uint32_t>(measured_rate, PLAYBACK_RATE_MIN, PLAYBACK_RATE_MAX);
  // Correct only the measured I2S playback rate. Network delivery is bursty:
  // its throughput is not the PCM sample rate. The ring buffer/backpressure
  // handles delivery variation independently, without changing voice pitch.
  const uint32_t target_rate = measured_rate;
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
    ESP_LOGI(TAG, "Playback rate adjust: write_us=%" PRIu32 " expected_us=%" PRIu32 " measured=%" PRIu32
                  " Hz target=measured rate=%" PRIu32 " Hz",
             write_us, expected_us, measured_rate, playback_rate);
  }
}

// Continuous Fractional Phase Accumulator
// Pass in the network buffer, returns a slightly smaller/larger buffer for I2S
std::vector<int16_t> AECSpeexDspComponent::resample(const int16_t *input, size_t frames, uint8_t channels) {
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

bool AECSpeexDspComponent::has_buffered_data() const {
  return this->playback_buffer_ != nullptr && this->playback_buffer_->available() > 0;
}

void AECSpeexDspComponent::clear_playback() {
  if (this->playback_buffer_ != nullptr)
    this->playback_buffer_->reset();

  this->buffering_since_ms_.store(millis());

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
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  // The phase accumulator and previous sample are stream-local. Retaining
  // them can interpolate the first sample of a new stream with the tail of
  // the previous stream.
  this->initialise_resampler();
#endif
  this->buffering_ = true;
}

AECSpeexDspMicrophone::~AECSpeexDspMicrophone() {
  heap_caps_free(this->buffer_);
  if (this->buffer_mutex_ != nullptr)
    vSemaphoreDelete(this->buffer_mutex_);
}

void AECSpeexDspMicrophone::setup() {
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

void AECSpeexDspMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "SpeexDSP microphone: cleaned 16-bit mono at 16000 Hz");
  ESP_LOGCONFIG(TAG, "  Rolling history: 1000 ms; utterance queue: 4000 ms in PSRAM");
}

void AECSpeexDspMicrophone::loop() {
  if (this->is_failed())
    return;
  const uint32_t now = millis();
  if (now - this->last_overflow_log_ms_ >= 5000) {
    this->last_overflow_log_ms_ = now;
    const uint32_t dropped = this->dropped_bytes_.exchange(0);
    if (dropped != 0)
      ESP_LOGW(TAG, "Microphone handoff capacity exceeded: dropped %" PRIu32 " bytes", dropped);
  }
  // A 16 ms minimum gives normal operation enough capacity for 16 kHz mono.
  // After a short UI/main-loop stall, drain a bounded number of queued blocks
  // so voice upload catches up rather than permanently losing wall-clock rate.
  if (now - this->last_delivery_ms_ < 16)
    return;
  // The 1024 UI can make a pass several hundred milliseconds long. Sixteen
  // blocks cover 512 ms, enough to regain real-time upload at the observed
  // hall loop rate while the four-second PSRAM FIFO bounds total backlog.
  constexpr size_t MAX_DELIVERIES_PER_LOOP = 16;
  for (size_t delivery = 0; delivery < MAX_DELIVERIES_PER_LOOP; ++delivery) {
    xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
    if (this->listeners_ == 0 || this->pre_roll_requested_) {
      xSemaphoreGive(this->buffer_mutex_);
      break;
    }
    const size_t count = std::min(this->buffered_bytes_, DELIVERY_BYTES);
    if (count == 0) {
      xSemaphoreGive(this->buffer_mutex_);
      break;
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
    this->last_delivery_ms_ = now;
    // Callbacks may start/stop listeners; never hold the queue lock here.
    this->data_callbacks_.call(this->vec);
  }
}

void AECSpeexDspMicrophone::start() {
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

void AECSpeexDspMicrophone::request_pre_roll() {
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

void AECSpeexDspMicrophone::begin_pre_roll_replay() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->pre_roll_requested_ = false;
  this->utterance_active_ = true;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECSpeexDspMicrophone::discard_pending_audio() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->pre_roll_requested_ = false;
  this->utterance_active_ = false;
  this->read_offset_ = 0;
  this->buffered_bytes_ = 0;
  xSemaphoreGive(this->buffer_mutex_);
}

void AECSpeexDspMicrophone::set_response_playing(bool playing) {
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

void AECSpeexDspMicrophone::stop() {
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

void AECSpeexDspMicrophone::publish(const uint8_t *data, const size_t data_size) {
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

void AECSpeexDspSpeaker::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, SAMPLE_RATE);
  this->state_ = speaker::STATE_STOPPED;
}

void AECSpeexDspSpeaker::dump_config() { ESP_LOGCONFIG(TAG, "SpeexDSP speaker: 16-bit mono/stereo at 16000 Hz"); }

void AECSpeexDspSpeaker::loop() {
  if ((this->state_ == speaker::STATE_RUNNING || this->state_ == speaker::STATE_STOPPING) &&
      !this->has_buffered_data())
    this->state_ = speaker::STATE_STOPPED;
}

size_t AECSpeexDspSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  this->start();
  return this->parent_ == nullptr ? 0 : this->parent_->play(data, length, ticks_to_wait);
}

size_t AECSpeexDspSpeaker::play(const uint8_t *data, size_t length) { return this->play(data, length, 0); }

void AECSpeexDspSpeaker::start() {
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

void AECSpeexDspSpeaker::stop() {
  if (this->parent_ != nullptr)
    this->parent_->clear_playback();
  this->state_ = speaker::STATE_STOPPED;
}

void AECSpeexDspSpeaker::finish() { this->state_ = this->has_buffered_data() ? speaker::STATE_STOPPING : speaker::STATE_STOPPED; }

bool AECSpeexDspSpeaker::has_buffered_data() const {
  return this->parent_ != nullptr && this->parent_->has_buffered_data();
}

}  // namespace aec_speexdsp
}  // namespace esphome

#endif
