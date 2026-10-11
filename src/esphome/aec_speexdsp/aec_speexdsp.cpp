#include "aec_speexdsp.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_cpu.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <soc/soc_caps.h>
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include <driver/usb_serial_jtag.h>
#endif

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include "esphome/core/application.h"
#include "esphome/components/logger/logger.h"

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
static constexpr uint32_t AUDIO_TASK_STACK_BYTES = 8192;
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
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  this->neural_barge_ = std::make_unique<NeuralBargeIn>();
  if (this->frame_size_ != 256 || this->microphone_slots_.size() != 2 || !this->neural_barge_->init()) {
    ESP_LOGE(TAG, "Neural barge-in initialization failed"); this->mark_failed(); return;
  }
#endif

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
  if (xTaskCreatePinnedToCore(AECSpeexDspComponent::audio_task, "speexdsp_audio", AUDIO_TASK_STACK_BYTES, this, 4,
                              &this->audio_task_handle_, 0) != pdPASS) {
    ESP_LOGE(TAG, "Could not start audio task");
    this->mark_failed();
  }

  // Host control channel over the USB Serial/JTAG console. The logger already
  // installed the driver; this task only reads RX and writes framed responses.
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  if (this->serial_control_enabled_) {
    this->diag_mutex_ = xSemaphoreCreateMutex();
    if (this->diag_mutex_ == nullptr ||
        xTaskCreatePinnedToCore(AECSpeexDspComponent::serial_task, "aec_serial_ctl", 6144, this, 2,
                                &this->serial_task_handle_, playback_core) != pdPASS) {
      ESP_LOGE(TAG, "Could not start serial control task");
    }
  }
#else
  if (this->serial_control_enabled_)
    ESP_LOGE(TAG, "serial_control requires a SoC with USB Serial/JTAG");
#endif
}

void AECSpeexDspComponent::dump_config() {
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  ESP_LOGCONFIG(TAG, "Neural barge-in: threshold=%.6f confirmation=%u protect=%s model=%s",
                neural_threshold_, neural_confirmation_, YESNO(neural_protection_), barge_model::checksum);
#endif
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
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  this->voice_assistant_active_.store(this->voice_assistant_active_condition_ &&
                                      this->voice_assistant_active_condition_());
#endif
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
  this->soft_double_talk_.configure(this->echo_suppress_db_, this->echo_suppress_active_db_);
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
    int meter = this->near_end_enabled_ ? 1 : 0;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_PRE_AGC_METER, &meter);
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
    int near_end_protection = this->soft_double_talk_enabled_ && this->legacy_near_end_enabled_() ? 1 : 0;
    speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_NEAR_END_PROTECTION, &near_end_protection);

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
  uint32_t quality_last_log = 0;
  // Edge tracking for diagnostic capture event markers.
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  uint64_t neural_time_us = 0; uint32_t neural_frames = 0, neural_peak_us = 0, neural_last_log = millis();
  float neural_prob_min = INFINITY, neural_prob_max = 0.f, neural_prob_sum = 0.f; uint32_t neural_prob_frames = 0;
  uint32_t neural_candidate_frames = 0, neural_confirmed_frames = 0;
#endif
  bool diag_prev_reference_active = false;
  while (true) {
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(this->rx_handle_, raw, raw_bytes, &bytes_read, 100);
    if (err != ESP_OK || bytes_read != raw_bytes) {
      this->rx_errors_++;
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
      this->neural_barge_->reset(); this->neural_probability_.store(0);
#endif
      this->cancellation_quality_.invalidate();  // Do not correlate across missing capture samples.
      this->near_end_detector_.discard_pending_observation();
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
    bool reference_complete = true;
    if (this->reference_source_ == AEC_SPEEXDSP_REFERENCE_PLAYBACK) {
      const size_t reference_bytes = frame_size * sizeof(int16_t);
      const size_t reference_read = this->reference_buffer_->read(ref, reference_bytes, pdMS_TO_TICKS(100));
      if (reference_read != reference_bytes) {
        reference_complete = false;
        std::memset(reinterpret_cast<uint8_t *>(ref) + reference_read, 0, reference_bytes - reference_read);
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

#ifdef USE_AEC_SPEEXDSP_METERS
    if (this->meters_callback_ != nullptr)
      this->meters_callback_->process_reference(ref, frame_size);
#endif

    // Diagnostic capture taps the microphone interface directly after the DMA
    // read and reference alignment: these channels stay complete even if the
    // canceller later cannot keep up (capture-only mode never can drop them).
    const auto reference_frame = this->reference_tracker_.process(this->reference_rms_.load(), frame_size);
    if (reference_frame.active != diag_prev_reference_active) {
      this->diag_record_event_(reference_frame.active ? "REF_START" : "REF_STOP");
      diag_prev_reference_active = reference_frame.active;
    }
    this->capture_diag_frame_(raw, ref, frame_size);
    // Advance the master sample clock once per delivered DMA frame.
    this->frame_samples_.fetch_add(frame_size, std::memory_order_relaxed);
    if (this->capture_only_) {
      // Microphones + reference are recorded; skip the canceller entirely.
      // The host recreates the DSP outputs offline from the captured audio.
      continue;
    }

#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    this->neural_reference_active_ = reference_frame.active;
    this->neural_barge_active_ = this->voice_assistant_active_.load() && reference_frame.active;
    const bool requested_neural_enabled = this->neural_enabled_requested_.load();
    if (requested_neural_enabled != this->neural_enabled_) {
      this->neural_enabled_ = requested_neural_enabled;
      this->neural_barge_->set_enabled(requested_neural_enabled);
      neural_time_us = 0; neural_frames = neural_peak_us = 0; neural_last_log = millis();
      neural_prob_min = INFINITY; neural_prob_max = neural_prob_sum = 0.f; neural_prob_frames = 0;
      neural_candidate_frames = neural_confirmed_frames = 0;
    }
    const int64_t neural_start = esp_timer_get_time();
    if (this->neural_reset_requested_.exchange(false)) this->neural_barge_->reset();
    const bool neural_valid = reference_complete && this->diagnostic_raw_slot_.load() < 0 &&
                              this->echo_state_[0] != nullptr && this->preprocess_state_[0] != nullptr;
    const auto neural = this->neural_enabled_ ? this->neural_barge_->process(raw + this->microphone_slots_[0],
        raw + this->microphone_slots_[1], this->tdm_slots_, ref, reference_frame.active,
        neural_valid, this->neural_threshold_, this->neural_confirmation_,
        this->neural_barge_active_) : NeuralBargeIn::Result{};
    this->neural_probability_.store(neural.probability);
    // Only frames where the GRU actually inferred (playback + VA active + context
    // full) carry a meaningful probability; idle frames report 0 and would skew
    // the min/avg used for threshold calibration.
    if (neural.ready && this->neural_barge_active_) {
      neural_prob_min = std::min(neural_prob_min, neural.probability);
      neural_prob_max = std::max(neural_prob_max, neural.probability);
      neural_prob_sum += neural.probability; ++neural_prob_frames;
      neural_candidate_frames += neural.candidate;
      neural_confirmed_frames += neural.confirmed;
    }
    if (this->neural_controls_audio_()) this->near_end_probability_.store(neural.probability);
    else if (!this->legacy_near_end_enabled_()) this->near_end_probability_.store(0.f);
    const uint32_t neural_us = esp_timer_get_time() - neural_start;
    neural_time_us += neural_us; ++neural_frames; neural_peak_us = std::max(neural_peak_us, neural_us);
    if (neural.event) { this->neural_events_.fetch_add(1); this->diag_record_event_("NEURAL_BARGE"); }
    if (millis() - neural_last_log >= this->neural_log_ms_) {
      ESP_LOGI(TAG, "NEURAL_BARGE enabled=%s active=%s p=%.5f min=%.5f avg=%.5f max=%.5f infer_frames=%" PRIu32
                    " ready=%s candidate=%s confirmed=%s events=%" PRIu32
                    " candidate_frames=%" PRIu32 " confirmed_frames=%" PRIu32
                    " avg_us=%" PRIu64 " peak_us=%" PRIu32 " core_budget=%.2f%%",
          YESNO(this->neural_enabled_), YESNO(this->neural_barge_active_), neural.probability,
          neural_prob_frames ? neural_prob_min : 0.f,
          neural_prob_frames ? neural_prob_sum / neural_prob_frames : 0.f, neural_prob_max, neural_prob_frames,
          YESNO(neural.ready), YESNO(neural.candidate),
          YESNO(neural.confirmed), this->neural_events_.load(), neural_candidate_frames, neural_confirmed_frames,
          neural_time_us/neural_frames, neural_peak_us,
          100.f*neural_time_us/neural_frames/frame_budget_us);
      const float *features = this->neural_barge_->features();
      float bark_mean[3]{}, spatial_abs_mean = 0, variance_min = INFINITY, variance_max = 0;
      for (unsigned b = 0; b < 24; ++b)
        for (unsigned c = 0; c < 3; ++c) bark_mean[c] += features[3*b+c]/24.f;
      for (unsigned i = 72; i < 84; ++i) spatial_abs_mean += std::abs(features[i])/12.f;
      for (unsigned i = 0; i < 84; ++i) {
        variance_min = std::min(variance_min, this->neural_barge_->normalizer_variance()[i]);
        variance_max = std::max(variance_max, this->neural_barge_->normalizer_variance()[i]);
      }
      ESP_LOGI(TAG, "NEURAL_FEATURES sample=%" PRIu64 " playback=%s threshold=%.6f consecutive=%u context=%u resets=%" PRIu32
                    " bark_logpower_mean=[%.3f,%.3f,%.3f] spatial_delta_abs_mean=%.3f variance=[%.4f,%.4f]",
          this->frame_samples_.load(), YESNO(reference_frame.active), this->neural_threshold_,
          this->neural_barge_->consecutive_frames(), this->neural_barge_->context_frames(), this->neural_barge_->resets(),
          bark_mean[0], bark_mean[1], bark_mean[2], spatial_abs_mean, variance_min, variance_max);
      neural_time_us=0; neural_frames=neural_peak_us=0; neural_last_log=millis();
      neural_prob_min = INFINITY; neural_prob_max = neural_prob_sum = 0.f; neural_prob_frames = 0;
      neural_candidate_frames = neural_confirmed_frames = 0;
    }
    float neural_protect = this->neural_protection_ && this->neural_controls_audio_() ? neural.probability : 0.f;
    for (auto *pre : this->preprocess_state_)
      if (pre) speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_NEURAL_PROTECT_PROB, &neural_protect);
#endif
    bool speech_detected = false;
    float clean_rms = 0.0f;
    auto measure_clean = [this, frame_size, &clean_rms](const int16_t *pcm) {
      if (!this->legacy_near_end_enabled_()) return;
      double energy = 0;
      for (size_t i = 0; i < frame_size; ++i) energy += static_cast<double>(pcm[i]) * pcm[i];
      clean_rms = std::sqrt(energy / frame_size);
    };
    SpeexReferenceState reference_state{reference_frame.active, reference_frame.startup};
    NearEndCorrelation::Result near_correlation{};
    if (this->agc_enabled_ && this->agc_gate_config_.enabled) {
      for (uint8_t c = 0; c < (this->beamforming_enabled_ ? 1 : channels); ++c)
        if (this->preprocess_state_[c] != nullptr)
          speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_REFERENCE_STATE, &reference_state);
    }
    const int8_t diagnostic_raw_slot = this->diagnostic_raw_slot_.load();
    auto measure_quality = [&](const int16_t *input, const int16_t *clean) {
      if (this->legacy_near_end_enabled_())
        this->cancellation_quality_.process(input, clean, ref, frame_size, reference_frame.active,
                                            reference_frame.excited, reference_complete);
    };
    auto update_soft_suppression = [&]() {
      if (!this->soft_double_talk_enabled_ || !this->legacy_near_end_enabled_())
        return;
      const auto control = this->soft_double_talk_.process(
          reference_frame, this->cancellation_quality_.snapshot(), this->vad_probability_.load(), frame_size);
      int attenuation = control.suppression_db;
      for (uint8_t c = 0; c < (this->beamforming_enabled_ ? 1 : channels); ++c)
        if (this->preprocess_state_[c] != nullptr)
          speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &attenuation);
    };
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
        if (this->legacy_near_end_enabled_()) {
          for (uint8_t c = 0; c < channels; ++c)
            this->near_end_correlation_.process_channel(c, mic + c, channels, processed + c, channels, frame_size);
          near_correlation = this->near_end_correlation_.finish_frame(channels);
          SpeexNearEndHint hint{near_correlation.mic_error_correlation,
                                near_correlation.echo_error_correlation,
                                this->reference_rms_.load() * this->reference_rms_.load(),
                                reference_frame.active ? 1 : 0};
          speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_SET_NEAR_END_HINT, &hint);
        }
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
        measure_clean(out);
        measure_quality(processed, out);
        update_soft_suppression();
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
          if (this->legacy_near_end_enabled_()) {
            this->near_end_correlation_.process_channel(c, channel_mic, 1, channel_out, 1, frame_size);
            const auto &channel = this->near_end_correlation_.result().channels[c];
            SpeexNearEndHint hint{channel.mic_error_correlation, channel.echo_error_correlation,
                                  this->reference_rms_.load() * this->reference_rms_.load(),
                                  reference_frame.active ? 1 : 0};
            speex_preprocess_ctl(this->preprocess_state_[c], SPEEX_PREPROCESS_SET_NEAR_END_HINT, &hint);
          }
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
          if (c == 0) { measure_clean(channel_out); measure_quality(channel_mic, channel_out); update_soft_suppression(); }
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
        if (this->legacy_near_end_enabled_())
          near_correlation = this->near_end_correlation_.finish_frame(channels);
        if (channels == 2) {
          for (size_t frame = 0; frame < frame_size; frame++)
            out[frame] = static_cast<int16_t>((processed[frame] + processed[frame_size + frame]) / 2);
        } else {
          std::memcpy(out, processed, frame_size * sizeof(int16_t));
        }
      }
    }

    const bool previous_near_end = this->near_end_speech_;
    this->near_end_speech_ = false;
    if (this->legacy_near_end_enabled_() && diagnostic_raw_slot < 0 &&
        this->echo_state_[0] != nullptr && this->preprocess_state_[0] != nullptr) {
      SpeexEchoResidualDiagnostics echo{};
      SpeexPreprocessResidualDiagnostics pre{};
      float rms = 0;
      speex_echo_ctl(this->echo_state_[0], SPEEX_ECHO_GET_RESIDUAL_DIAGNOSTICS, &echo);
      speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_GET_RESIDUAL_DIAGNOSTICS, &pre);
      speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_GET_PRE_AGC_RMS, &rms);
      SpeexNearEndDiagnostics near{};
      speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_GET_NEAR_END_DIAGNOSTICS, &near);
      this->near_end_probability_.store(near.probability);
      bool observation_valid = reference_complete;
      for (size_t n = 0; n < frame_size && observation_valid; ++n) {
        for (uint8_t c = 0; c < channels; ++c)
          observation_valid &= std::abs(static_cast<int32_t>(raw[n * this->tdm_slots_ + this->processed_slots_[c]])) < 32767;
        observation_valid &= std::abs(static_cast<int32_t>(ref[n])) < 32767;
      }
      const bool detected_near_end = this->near_end_detector_.process(
          this->vad_enabled_ && speech_detected, echo.adapted, this->reference_rms_.load(),
          rms, clean_rms, echo.removed_rms, pre.input_power, pre.echo_power, pre.noise_power, frame_size,
          observation_valid, &reference_frame, &this->cancellation_quality_.snapshot(), millis());
      this->near_end_speech_ = detected_near_end;
      if (!observation_valid) this->cancellation_quality_.invalidate();
      if (this->near_end_detector_.observation_enabled() &&
          ((reference_frame.active && millis() - quality_last_log >= 250) || previous_near_end != this->near_end_speech_)) {
        quality_last_log = millis();
        const auto &q = this->cancellation_quality_.snapshot();
        const auto &baseline = this->near_end_detector_.echo_baseline();
        ESP_LOGI(TAG, "ECHO_CONTROL adaptive=%s linear_envelope_rms=%.1f threshold_pre_agc_rms=%.1f pre_agc_rms=%.1f clean_rms=%.1f speech=%s",
                 YESNO(this->near_end_detector_.adaptive_used()), this->near_end_detector_.adaptive_threshold_rms(),
                 this->near_end_detector_.adaptive_pre_agc_threshold_rms(), rms,
                 clean_rms, YESNO(this->near_end_speech_));
        const auto &learning = this->near_end_detector_.echo_learning_counters();
        ESP_LOGI(TAG, "ECHO_LEARN model raw_corr=%.3f qualified_frames=%" PRIu32 " commits=%" PRIu32
                     " speech_resets=%" PRIu32 " integrity_resets=%" PRIu32 " guard_resets=%" PRIu32
                     " evidence_skips=%" PRIu32,
                 q.input_correlation, learning.qualified, learning.committed, learning.speech_resets,
                 learning.integrity_resets, learning.startup_resets, learning.evidence_skips);
        ESP_LOGI(TAG, "ECHO_BASELINE model band=%d qualified=%s ready=%s accepted_ms=%" PRIu32
                     " age_ms=%" PRIu32 " coverage=%.2f median_db=%.1f upper_db=%.1f excess_db=%.1f",
                 baseline.band, YESNO(baseline.qualified), YESNO(baseline.ready), baseline.accepted_ms,
                 baseline.age_ms, baseline.coverage, baseline.median_db, baseline.upper_db, baseline.excess_db);
        ESP_LOGI(TAG, "AEC_QUALITY shadow valid=%s ref=%s guard=%s adapted=%s age_ms=%" PRIu32
                     " reduction_db=%.1f residual_ref_db=%.1f corr=%.3f lag=%d candidate=%s speech=%s echo_hypothesis=%s",
                 YESNO(q.valid), YESNO(reference_frame.active), YESNO(reference_frame.startup), YESNO(echo.adapted),
                 q.active_ms, q.reduction_db, q.residual_to_reference_db, q.residual_correlation, q.lag_samples,
                 YESNO(this->near_end_detector_.candidate()), YESNO(this->near_end_speech_),
                 YESNO(this->near_end_detector_.echo_dominated()));
        ESP_LOGI(TAG, "AEC_NEAR p=%.3f instant=%.3f excess=%.3f corr=%.3f path=%.3f "
                      "cde0=%.3f cde1=%.3f cye0=%.3f cye1=%.3f pframe=%.3f leak=%.4f "
                      "ref=%.1f",
                 near.probability, near.instantaneous_probability, near.excess_probability,
                 near.correlation_probability, near.path_change_probability,
                 near_correlation.channels[0].mic_error_correlation,
                 near_correlation.channels[1].mic_error_correlation,
                 near_correlation.channels[0].echo_error_correlation,
                 near_correlation.channels[1].echo_error_correlation,
                 pre.frame_probability, echo.leak, this->reference_rms_.load());
      }
      if (previous_near_end != this->near_end_speech_) {
        this->diag_record_event_(this->near_end_speech_ ? "NEAR_START" : "NEAR_STOP");
        ESP_LOGD(TAG, "Near-end edge: speech=%s reference=%s candidate=%s pre_agc=%.1f clean=%.1f removed=%.1f "
                      "input_ps=%.3g echo_ps=%.3g noise_ps=%.3g",
                  YESNO(this->near_end_speech_), YESNO(this->near_end_detector_.reference_active()),
                  YESNO(this->near_end_detector_.candidate()), rms, clean_rms, echo.removed_rms,
                  pre.input_power, pre.echo_power, pre.noise_power);
      }
    } else {
      this->near_end_detector_.discard_pending_observation();
      this->cancellation_quality_.invalidate();
    }

#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    if (this->neural_controls_audio_())
      this->near_end_speech_ = neural.confirmed;
#endif
    if (!this->neural_controls_audio_() && !this->legacy_near_end_enabled_())
      this->near_end_speech_ = speech_detected;
    // One decision for callers and PCM gating. Without near_end configured,
    // retain legacy Speex VAD semantics for existing non-barge-in installations.
    this->vad_state_.store(diagnostic_raw_slot < 0 && this->vad_enabled_ &&
                          ((this->neural_controls_audio_() || this->legacy_near_end_enabled_()) ? this->near_end_speech_ : speech_detected));
    this->speex_vad_probability_.store(0.f);
    if (this->preprocess_state_[0] != nullptr && diagnostic_raw_slot < 0) {
      // GET_VAD reports enablement; the run() return value is the decision.
      spx_int32_t prob = 0;
      speex_preprocess_ctl(this->preprocess_state_[0], SPEEX_PREPROCESS_GET_PROB, &prob);
      this->speex_vad_probability_.store(static_cast<float>(prob) / 100.0f);
    }
    this->vad_probability_.store(this->speex_vad_probability_.load());
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    if (this->neural_controls_audio_()) this->vad_probability_.store(neural.probability);
#endif

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
      if (this->legacy_near_end_enabled_()) {
        using Scene = AcousticObserver::Scene;
        const auto &observer = this->near_end_detector_.observer();
        for (auto scene : {Scene::NOISE, Scene::NEAR_ONLY, Scene::FAR_ONLY}) {
          const auto &stats = observer.distribution(scene);
          if (stats.accepted_samples)
            ESP_LOGI(TAG, "ACOUSTIC_OBSERVE scene=%u accepted_s=%.1f linear_rms_p20=%.1f p50=%.1f p80=%.1f",
                     static_cast<unsigned>(scene), stats.accepted_samples / 16000.0,
                     stats.percentile(0.2f), stats.percentile(0.5f), stats.percentile(0.8f));
        }
      }
      if (this->legacy_near_end_enabled_())
        ESP_LOGI(TAG, "NEAR_END last_frame reference=%s candidate=%s speech=%s clean_rms=%.1f",
                 YESNO(this->near_end_detector_.reference_active()), YESNO(this->near_end_detector_.candidate()),
                 YESNO(this->near_end_speech_), clean_rms);
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
        ESP_LOGI(TAG, "DSP stack: minimum free=%u bytes of %u",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                 static_cast<unsigned>(AUDIO_TASK_STACK_BYTES));

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

    // Host-driven pause: leave buffered media untouched and feed silence to
    // the TX (and therefore the reference) until resumed.
    const bool playback_paused = this->playback_paused_.load();
    size_t playback_bytes = 0;
    if (this->playback_buffer_ != nullptr && !playback_paused) {
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

    // The wake cue bypasses rebuffering, but shares gain, TX and reference.
    const size_t beep_frames = this->wake_beep_.render(playback, frame_size, channels);
    playback_bytes = std::max(playback_bytes, beep_frames * channels * sizeof(int16_t));

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

void AECSpeexDspComponent::publish_frame_(const int16_t *mono, size_t frames) {
  if (this->microphone_ == nullptr)
    return;
  // Neural masking is restricted to playback during an active assistant.
  // Outside that scope the processed PCM reaches WakeNet unchanged.
  bool echo_rejected = this->legacy_near_end_enabled_() && this->near_end_detector_.reference_active() &&
                       !this->near_end_detector_.candidate();
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  // Preserve onset candidates in history before confirmation opens the FIFO.
  // Confirmation controls delivery; it must not erase its own pre-roll.
  if (this->neural_controls_audio_())
    echo_rejected = this->neural_reference_active_ && !this->neural_barge_->result().candidate;
#endif
  this->microphone_->publish(reinterpret_cast<const uint8_t *>(mono), frames * sizeof(int16_t),
                            this->vad_state_.load(), echo_rejected,
                            this->neural_reference_active_ ||
                                (this->legacy_near_end_enabled_() && this->near_end_detector_.reference_active()));
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

void AECSpeexDspComponent::capture_diag_frame_(const int16_t *raw, const int16_t *ref, size_t frames) {
  if (this->diag_state_.load(std::memory_order_relaxed) != AEC_SPEEXDSP_CAPTURE_CAPTURING)
    return;
  if (this->diag_buffer_ == nullptr)
    return;

  size_t written = this->diag_written_frames_.load();
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  // This method runs on the audio task: align model initialization to the
  // first recorded frame without mutating inference state from another task.
  if (written == 0) this->neural_barge_->reset();
#endif
  const uint32_t target = this->diag_target_frames_.load();
  const size_t remaining = target > written ? target - written : 0;
  const size_t to_copy = std::min(frames, remaining);

  for (size_t n = 0; n < to_copy; ++n) {
    int16_t *dst = this->diag_buffer_ + (written + n) * DIAG_CHANNELS;
    dst[0] = raw[n * this->tdm_slots_ + this->processed_slots_[0]];
    dst[1] = raw[n * this->tdm_slots_ + this->processed_slots_[1]];
    dst[2] = ref[n];
  }

  written += to_copy;
  this->diag_written_frames_.store(written);
  if (written >= target) {
    this->diag_state_.store(AEC_SPEEXDSP_CAPTURE_READY);
    ESP_LOGI(TAG, "Diag capture complete: %u frames x %u channels (%.1fs)", static_cast<unsigned>(written),
             static_cast<unsigned>(DIAG_CHANNELS), static_cast<float>(written) / SAMPLE_RATE);
  }
}

void AECSpeexDspComponent::diag_record_event_(const char *id) {
  const uint64_t sample = this->frame_samples_.load();
  const uint32_t now_ms = millis();
  // Live marker for the host: also visible in the console log stream.
  ESP_LOGI(TAG, "EVT %s sample=%llu millis=%u", id, static_cast<unsigned long long>(sample),
           static_cast<unsigned>(now_ms));
  if (this->diag_state_.load(std::memory_order_relaxed) != AEC_SPEEXDSP_CAPTURE_CAPTURING)
    return;
  if (this->diag_mutex_ == nullptr)
    return;
  DiagEvent event{};
  event.sample = sample;
  event.millis = now_ms;
  snprintf(event.id, sizeof(event.id), "%s", id);
  if (xSemaphoreTake(this->diag_mutex_, 0) == pdTRUE) {
    if (this->diag_events_.size() < 256)
      this->diag_events_.push_back(event);
    xSemaphoreGive(this->diag_mutex_);
  }
}

bool AECSpeexDspComponent::diag_capture_start(uint32_t seconds) {
  if (this->diag_state_.load() == AEC_SPEEXDSP_CAPTURE_CAPTURING)
    return false;
  if (this->diag_max_seconds_ == 0)
    return false;
  const uint32_t capped = std::clamp<uint32_t>(seconds, 1, this->diag_max_seconds_);
  const size_t target = static_cast<size_t>(capped) * SAMPLE_RATE;
  if (this->diag_buffer_ == nullptr || this->diag_capacity_frames_ < target) {
    if (this->diag_buffer_ != nullptr) {
      heap_caps_free(this->diag_buffer_);
      this->diag_buffer_ = nullptr;
      this->diag_capacity_frames_ = 0;
    }
    const size_t bytes = target * DIAG_CHANNELS * sizeof(int16_t);
    // Plain malloc path: the pointer must stay valid for heap_caps_free().
    // (heap_caps_aligned_alloc pointers may only be freed via the aligned
    // allocator; mixing them aborts the heap at CAP FREE time.)
    this->diag_buffer_ = static_cast<int16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (this->diag_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Could not allocate %u KiB diag capture buffer", static_cast<unsigned>(bytes / 1024));
      return false;
    }
    this->diag_capacity_frames_ = target;
  }
  if (this->diag_mutex_ != nullptr && xSemaphoreTake(this->diag_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    this->diag_events_.clear();
    xSemaphoreGive(this->diag_mutex_);
  }
  this->diag_channels_.store(this->processed_channels_());
  this->diag_written_frames_.store(0);
  this->diag_target_frames_.store(target);
  this->diag_start_sample_.store(this->frame_samples_.load());
  this->diag_state_.store(AEC_SPEEXDSP_CAPTURE_CAPTURING);
  return true;
}

bool AECSpeexDspComponent::diag_capture_stop() {
  uint8_t expected = AEC_SPEEXDSP_CAPTURE_CAPTURING;
  if (this->diag_state_.compare_exchange_strong(expected, AEC_SPEEXDSP_CAPTURE_READY))
    return true;
  return expected == AEC_SPEEXDSP_CAPTURE_READY;  // already stopped
}

bool AECSpeexDspComponent::diag_capture_free() {
  if (this->diag_state_.load() == AEC_SPEEXDSP_CAPTURE_CAPTURING)
    return false;
  if (this->diag_buffer_ != nullptr) {
    heap_caps_free(this->diag_buffer_);
    this->diag_buffer_ = nullptr;
    this->diag_capacity_frames_ = 0;
  }
  this->diag_written_frames_.store(0);
  this->diag_target_frames_.store(0);
  this->diag_state_.store(AEC_SPEEXDSP_CAPTURE_IDLE);
  return true;
}

size_t AECSpeexDspComponent::diag_copy_frames(uint8_t *dst, size_t dst_bytes, size_t src_frame) const {
  if (this->diag_buffer_ == nullptr || dst == nullptr)
    return 0;
  const size_t written = this->diag_written_frames_.load();
  if (src_frame >= written)
    return 0;
  const size_t frame_bytes = DIAG_CHANNELS * sizeof(int16_t);
  const size_t max_frames = std::min({dst_bytes / frame_bytes, written - src_frame, this->diag_capacity_frames_ - src_frame});
  std::memcpy(dst, reinterpret_cast<const uint8_t *>(this->diag_buffer_) + src_frame * frame_bytes,
              max_frames * frame_bytes);
  return max_frames;
}

std::vector<AECSpeexDspComponent::DiagEvent> AECSpeexDspComponent::diag_copy_events() const {
  std::vector<DiagEvent> events;
  if (this->diag_mutex_ != nullptr && xSemaphoreTake(this->diag_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
    events = this->diag_events_;
    xSemaphoreGive(this->diag_mutex_);
  }
  return events;
}

// ---------------------------------------------------------------------------
// USB Serial/JTAG host control channel.
//
// Text commands are newline terminated: PING, INFO, PLAY <track>, STOP, PAUSE,
// RESUME, MARK <label>, CAP START [seconds], CAP STOP, CAP STATUS,
// CAP DUMP [from_frame], CAP FREE. Every command is answered with one
// "OK ..." or "ERR ..." line. Asynchronous events are "EVT <id> ..." lines.
// Binary diagnostic dumps are CRC framed packets interleaved with (suppressed)
// console logs; see serial_dump_capture_.
// ---------------------------------------------------------------------------

static constexpr uint8_t DIAG_PACKET_SYNC0 = 0xA5;
static constexpr uint8_t DIAG_PACKET_SYNC1 = 0x5A;
static constexpr uint8_t DIAG_PACKET_TYPE = 0x01;
static constexpr uint32_t DIAG_FRAMES_PER_PACKET = 256;
static constexpr size_t DIAG_PACKET_HEADER = 9;   // sync(2) type(1) len(2) seq(4)
static constexpr size_t DIAG_PACKET_CRC = 2;

static uint16_t diag_crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

void AECSpeexDspComponent::set_serial_control_actions(SerialControlActions actions) {
  this->serial_actions_ = std::move(actions);
  // Tell the host the on_boot wiring completed, so it can distinguish a
  // fully booted device from one that is still coming up after a reset.
  this->serial_write_string_("AEC_ACTIONS_WIRED\n");
}

void AECSpeexDspComponent::serial_task(void *params) {
  static_cast<AECSpeexDspComponent *>(params)->run_serial_task_();
}

bool AECSpeexDspComponent::serial_write_(const uint8_t *data, size_t length) {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  // The logger installed the USB Serial/JTAG driver with a 512-byte TX ring;
  // usb_serial_jtag_write_bytes uses a no-split ring send, so any single call
  // larger than the ring can never complete. Write in small chunks.
  constexpr size_t MAX_CHUNK = 256;
  size_t offset = 0;
  uint32_t stalled_ms = 0;
  while (offset < length) {
    const size_t chunk = std::min(length - offset, MAX_CHUNK);
    const size_t written =
        usb_serial_jtag_write_bytes(data + offset, chunk, pdMS_TO_TICKS(500));
    if (written == 0) {
      // Host not reading (or detached): give up after a while so the control
      // task stays responsive to new commands. Generous, because a dump takes
      // tens of seconds and transient host hiccups should not abort it.
      stalled_ms += 500;
      if (stalled_ms >= 10000)
        return false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    stalled_ms = 0;
    offset += written;
  }
#endif
  return true;
}

const char *diag_state_name(uint8_t state) {
  switch (state) {
    case AEC_SPEEXDSP_CAPTURE_CAPTURING:
      return "capturing";
    case AEC_SPEEXDSP_CAPTURE_READY:
      return "ready";
    default:
      return "idle";
  }
}

void AECSpeexDspComponent::run_serial_task_() {
#if SOC_USB_SERIAL_JTAG_SUPPORTED
  ESP_LOGI(TAG, "Serial control ready: PING/INFO/PLAY/STOP/PAUSE/RESUME/MARK/CAP");
  this->serial_write_string_("AEC_READY frame_samples=" + std::to_string(this->frame_samples_.load()) + "\n");
  std::string line;
  uint8_t buffer[256];
  while (true) {
    const int received = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), pdMS_TO_TICKS(5));
    for (int i = 0; i < received; i++) {
      const char c = static_cast<char>(buffer[i]);
      if (c == '\n' || c == '\r') {
        if (!line.empty()) {
          this->handle_serial_line_(line);
          line.clear();
        }
      } else if (line.size() < 160) {
        line.push_back(c);
      }
    }
  }
#endif
}

void AECSpeexDspComponent::handle_serial_line_(const std::string &line) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t space = line.find(' ', start);
    if (space == std::string::npos) {
      if (start < line.size())
        parts.emplace_back(line.substr(start));
      break;
    }
    if (space > start)
      parts.emplace_back(line.substr(start, space - start));
    start = space + 1;
  }
  if (parts.empty())
    return;
  std::string verb = parts[0];
  std::transform(verb.begin(), verb.end(), verb.begin(), ::toupper);

  if (verb == "PING") {
    this->serial_write_string_("PONG uptime_ms=" + std::to_string(millis()) +
                               " frame_samples=" + std::to_string(this->frame_samples_.load()) + "\n");
    return;
  }
  if (verb == "INFO") {
    std::string mics;
    for (const uint8_t slot : this->microphone_slots_)
      mics += (mics.empty() ? "" : ",") + std::to_string(slot);
    this->serial_write_string_("INFO name=" + std::string(esphome::App.get_name()) + " version=" + ESPHOME_VERSION +
                               " uptime_ms=" + std::to_string(millis()) + "\n");
    this->serial_write_string_("INFO audio rate=16000 frame=" + std::to_string(this->frame_size_) +
                               " filter=" + std::to_string(this->filter_length_) +
                               " beam=" + (this->beamforming_enabled_ ? "1" : "0") + " mics=" + mics +
                               " ref_slot=" + std::to_string(this->reference_slot_) + "\n");
    this->serial_write_string_("INFO diag state=" + std::string(diag_state_name(this->diag_state_.load())) +
                               " frames=" + std::to_string(this->diag_written_frames_.load()) +
                               " capacity=" + std::to_string(this->diag_capacity_frames_) +
                               " max_seconds=" + std::to_string(this->diag_max_seconds_) +
                               " paused=" + (this->playback_paused_.load() ? "1" : "0") +
                               " frame_samples=" + std::to_string(this->frame_samples_.load()) + "\n");
    this->serial_write_string_("OK INFO\n");
    return;
  }
  if (verb == "PLAY") {
    if (parts.size() < 2) {
      this->serial_write_string_("ERR PLAY missing_track\n");
      return;
    }
    std::string track = parts[1];
    std::transform(track.begin(), track.end(), track.begin(), ::tolower);
    const auto action = this->serial_actions_.play.find(track);
    if (action == this->serial_actions_.play.end()) {
      this->serial_write_string_("ERR PLAY unknown_track " + track + "\n");
      return;
    }
    this->diag_record_event_(("PLAY_" + track).c_str());
    ESP_LOGI(TAG, "PLAY %s requested over serial at frame %llu", track.c_str(),
             static_cast<unsigned long long>(this->frame_samples_.load()));
    action->second();
    this->serial_write_string_("OK PLAY " + track + "\n");
    return;
  }
  if (verb == "STOP" || verb == "PAUSE" || verb == "RESUME") {
    this->diag_record_event_(verb.c_str());
    switch (verb[0]) {
      case 'S':
        if (this->serial_actions_.stop)
          this->serial_actions_.stop();
        this->set_playback_paused(false);
        break;
      case 'P':
        if (this->serial_actions_.pause)
          this->serial_actions_.pause();
        else
          this->set_playback_paused(true);
        break;
      default:
        if (this->serial_actions_.resume)
          this->serial_actions_.resume();
        else
          this->set_playback_paused(false);
        break;
    }
    this->serial_write_string_("OK " + verb + "\n");
    return;
  }
  if (verb == "MARK") {
    std::string label = parts.size() > 1 ? parts[1] : "mark";
    std::transform(label.begin(), label.end(), label.begin(), ::toupper);
    std::string event_id = "MARK_" + label;
    this->diag_record_event_(event_id.c_str());
    this->serial_write_string_("OK MARK " + label + " sample=" +
                               std::to_string(this->frame_samples_.load()) + "\n");
    return;
  }
  if (verb == "VOLUME") {
    if (parts.size() < 2) {
      this->serial_write_string_("ERR VOLUME missing_level\n");
      return;
    }
    const float level = strtof(parts[1].c_str(), nullptr);
    if (level < 0.0f || level > 1.0f) {
      this->serial_write_string_("ERR VOLUME out_of_range\n");
      return;
    }
    if (!this->serial_actions_.set_volume) {
      this->serial_write_string_("ERR VOLUME not_wired\n");
      return;
    }
    char volume_event[20];
    snprintf(volume_event, sizeof(volume_event), "VOLUME_%d", static_cast<int>(level * 100.0f + 0.5f));
    this->diag_record_event_(volume_event);
    this->serial_actions_.set_volume(level);
    this->serial_write_string_("OK VOLUME " + std::to_string(level).substr(0, 5) + "\n");
    return;
  }
  if (verb == "CAP") {
    if (parts.size() < 2) {
      this->serial_write_string_("ERR CAP missing_subcommand\n");
      return;
    }
    std::string sub = parts[1];
    std::transform(sub.begin(), sub.end(), sub.begin(), ::toupper);
    if (sub == "START") {
      uint32_t seconds = parts.size() > 2 ? strtoul(parts[2].c_str(), nullptr, 10) : 0;
      if (seconds == 0)
        seconds = this->diag_max_seconds_;
      if (!this->diag_capture_start(seconds)) {
        this->serial_write_string_("ERR CAP START busy\n");
        return;
      }
      this->serial_write_string_("OK CAP START frames=" + std::to_string(this->diag_target_frames_.load()) +
                                 " channels=" + std::to_string(static_cast<unsigned>(DIAG_CHANNELS)) +
                                 " start_sample=" + std::to_string(this->diag_start_sample_.load()) + "\n");
      return;
    }
    if (sub == "STOP") {
      if (!this->diag_capture_stop()) {
        this->serial_write_string_("ERR CAP STOP not_capturing\n");
        return;
      }
      this->serial_write_string_("OK CAP STOP frames=" + std::to_string(this->diag_written_frames_.load()) + "\n");
      return;
    }
    if (sub == "STATUS") {
      this->serial_write_string_("OK CAP STATUS state=" + std::string(diag_state_name(this->diag_state_.load())) +
                                 " frames=" + std::to_string(this->diag_written_frames_.load()) +
                                 " capacity=" + std::to_string(this->diag_capacity_frames_) +
                                 " start_sample=" + std::to_string(this->diag_start_sample_.load()) +
                                 " mdf_channels=" + std::to_string(this->diag_channels_.load()) +
                                 " frame_samples=" + std::to_string(this->frame_samples_.load()) + "\n");
      return;
    }
    if (sub == "DUMP") {
      const uint32_t from_frame = parts.size() > 2 ? strtoul(parts[2].c_str(), nullptr, 10) : 0;
      this->serial_dump_capture_(from_frame);
      return;
    }
    if (sub == "FREE") {
      if (!this->diag_capture_free()) {
        this->serial_write_string_("ERR CAP FREE busy\n");
        return;
      }
      this->serial_write_string_("OK CAP FREE\n");
      return;
    }
    this->serial_write_string_("ERR CAP unknown_subcommand " + sub + "\n");
    return;
  }
  this->serial_write_string_("ERR unknown_command " + verb + "\n");
}

void AECSpeexDspComponent::serial_dump_capture_(uint32_t from_frame) {
  if (this->get_diag_capture_state() != AEC_SPEEXDSP_CAPTURE_READY) {
    this->serial_write_string_("ERR CAP DUMP not_ready\n");
    return;
  }
  const size_t frames = this->diag_written_frames_.load();
  if (from_frame >= frames) {
    this->serial_write_string_("ERR CAP DUMP from_out_of_range " + std::to_string(from_frame) + "\n");
    return;
  }
  this->serial_write_string_("OK CAP DUMP from_frame=" + std::to_string(from_frame) + "\n");

  char meta[192];
  snprintf(meta, sizeof(meta),
           "DIAG_META frames=%u channels=%u rate=%u bytes=%u start_sample=%llu mdf_channels=%u beam=%d\n",
           static_cast<unsigned>(frames), static_cast<unsigned>(DIAG_CHANNELS), static_cast<unsigned>(SAMPLE_RATE),
           static_cast<unsigned>(frames * DIAG_CHANNELS * sizeof(int16_t)),
           static_cast<unsigned long long>(this->diag_start_sample_.load()),
           static_cast<unsigned>(this->diag_channels_.load()), this->beamforming_enabled_ ? 1 : 0);
  // Console logs interleave at USB write granularity, so silence both log
  // paths for the binary transfer. The host CRC-checks packets and resumes
  // with "CAP DUMP <frame>" if a packet is ever corrupted.
  esp_log_level_t previous_esp_level = esp_log_level_get("*");
  esp_log_level_set("*", ESP_LOG_NONE);
  const uint8_t previous_esphome_level = esphome::logger::global_logger->get_log_level();
  esphome::logger::global_logger->set_log_level(ESPHOME_LOG_LEVEL_NONE);

  this->serial_write_string_(meta);
  for (const auto &event : this->diag_copy_events()) {
    char event_line[48];
    snprintf(event_line, sizeof(event_line), "DIAG_EVT %llu %u %s\n",
             static_cast<unsigned long long>(event.sample), static_cast<unsigned>(event.millis), event.id);
    this->serial_write_string_(event_line);
  }

  const size_t frame_bytes = DIAG_CHANNELS * sizeof(int16_t);
  std::vector<uint8_t> packet(DIAG_PACKET_HEADER + DIAG_FRAMES_PER_PACKET * frame_bytes + DIAG_PACKET_CRC);
  uint32_t frame = from_frame;
  while (frame < frames) {
    const uint32_t chunk =
        std::min<uint32_t>(DIAG_FRAMES_PER_PACKET, static_cast<uint32_t>(frames - frame));
    const size_t payload_bytes = chunk * frame_bytes;
    packet[0] = DIAG_PACKET_SYNC0;
    packet[1] = DIAG_PACKET_SYNC1;
    packet[2] = DIAG_PACKET_TYPE;
    packet[3] = payload_bytes & 0xFF;
    packet[4] = (payload_bytes >> 8) & 0xFF;
    for (uint8_t i = 0; i < 4; i++)
      packet[5 + i] = (frame >> (8 * i)) & 0xFF;
    const size_t copied = this->diag_copy_frames(packet.data() + DIAG_PACKET_HEADER, payload_bytes, frame);
    if (copied != chunk) {
      esp_log_level_set("*", previous_esp_level);
      esphome::logger::global_logger->set_log_level(previous_esphome_level);
      this->serial_write_string_("ERR CAP DUMP read_failed at_frame=" + std::to_string(frame) + "\n");
      return;
    }    const uint16_t crc =
        diag_crc16(packet.data() + 2, DIAG_PACKET_HEADER - 2 + payload_bytes);
    packet[DIAG_PACKET_HEADER + payload_bytes] = crc & 0xFF;
    packet[DIAG_PACKET_HEADER + payload_bytes + 1] = (crc >> 8) & 0xFF;
    if (!this->serial_write_(packet.data(), DIAG_PACKET_HEADER + payload_bytes + DIAG_PACKET_CRC)) {
      // Host vanished mid-transfer: restore logging and wait for a fresh CAP DUMP.
      esp_log_level_set("*", previous_esp_level);
      esphome::logger::global_logger->set_log_level(previous_esphome_level);
      ESP_LOGW(TAG, "Diag dump aborted: host stopped reading at frame %u", static_cast<unsigned>(frame));
      return;
    }
    frame += chunk;
  }

  char tail[48];
  snprintf(tail, sizeof(tail), "DIAG_DUMP_END frames=%u\n", static_cast<unsigned>(frame));
  this->serial_write_string_(tail);
  esp_log_level_set("*", previous_esp_level);
  esphome::logger::global_logger->set_log_level(previous_esphome_level);
  ESP_LOGI(TAG, "Diag dump complete: frames=%u from=%u", static_cast<unsigned>(frame),
           static_cast<unsigned>(from_frame));
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
  this->wake_beep_.stop();
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
    xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
    const bool gated = this->barge_in_active_;
    const bool open = this->captured_bytes_ < this->speech_until_byte_;
    const size_t queued = this->buffered_bytes_;
    xSemaphoreGive(this->buffer_mutex_);
    if (gated)
      ESP_LOGD(TAG, "Capture gate: open=%s queued=%u bytes", YESNO(open), static_cast<unsigned>(queued));
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
    // Keep retained speech in the capture FIFO until every downstream block
    // fits. This callback is installed on the main task before capture starts.
    if (this->delivery_condition_ && !this->delivery_condition_(count)) {
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

size_t AECSpeexDspMicrophone::begin_pre_roll_replay() {
  if (this->is_failed())
    return 0;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  const size_t replay_bytes = this->buffered_bytes_;
  this->pre_roll_requested_ = false;
  this->utterance_active_ = true;
  xSemaphoreGive(this->buffer_mutex_);
  return replay_bytes;
}

void AECSpeexDspMicrophone::discard_pending_audio() {
  if (this->is_failed())
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->pre_roll_requested_ = false;
  this->utterance_active_ = this->barge_in_active_;
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

bool AECSpeexDspMicrophone::set_barge_in_active(bool active) {
  if (this->is_failed()) return false;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  const bool changed = active != this->barge_in_active_;
  if (changed) {
    this->barge_in_active_ = active;
    this->speech_until_byte_ = 0;
    this->playback_active_ = false;
    this->playback_silence_bytes_ = 0;
    if (active) {
      this->utterance_active_ = true;
      this->buffered_bytes_ = 0;
      this->read_offset_ = 0;
      this->history_bytes_ = 0;
      this->history_write_offset_ = 0;
      this->last_enqueued_byte_ = this->captured_bytes_;
    }
  }
  xSemaphoreGive(this->buffer_mutex_);
  return changed;
}

void AECSpeexDspMicrophone::publish(const uint8_t *data, const size_t data_size, bool near_end, bool echo_rejected,
                                 bool playback_active) {
  this->near_end_speech_.store(near_end);
  if (this->buffer_ == nullptr || this->buffer_mutex_ == nullptr)
    return;
  xSemaphoreTake(this->buffer_mutex_, portMAX_DELAY);
  this->captured_bytes_ += data_size;
  if (this->barge_in_active_) {
    constexpr size_t RESTART_SILENCE_BYTES = SAMPLE_RATE * sizeof(int16_t) * 300 / 1000;
    if (playback_active) {
      if (!this->playback_active_ && this->playback_silence_bytes_ >= RESTART_SILENCE_BYTES) {
        // A replacement reply must not inherit the preceding utterance's tail.
        // Keep queued speech; fresh confirmation below can reopen capture.
        this->speech_until_byte_ = 0;
        ESP_LOGD(TAG, "Capture hangover cancelled: playback resumed after silence");
      }
      this->playback_silence_bytes_ = 0;
    } else {
      this->playback_silence_bytes_ = std::min(RESTART_SILENCE_BYTES,
                                             this->playback_silence_bytes_ + data_size);
    }
    this->playback_active_ = playback_active;
  }
  const bool was_open = this->barge_in_active_ &&
                        this->captured_bytes_ - data_size < this->speech_until_byte_;
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
  // Mask rejected pre-roll before the gate opens. Once speech is confirmed,
  // retain continuous cleaned PCM through the hangover, including weak words.
  if (this->barge_in_active_ && !was_open && echo_rejected && !near_end) {
    memset(this->buffer_ + BUFFER_BYTES + this->history_write_offset_, 0, history_first);
    memset(this->buffer_ + BUFFER_BYTES, 0, history_count - history_first);
  } else {
    memcpy(this->buffer_ + BUFFER_BYTES + this->history_write_offset_, history_data, history_first);
    memcpy(this->buffer_ + BUFFER_BYTES, history_data + history_first, history_count - history_first);
  }
  this->history_write_offset_ = (this->history_write_offset_ + history_count) % HISTORY_BYTES;
  this->history_bytes_ = std::min(HISTORY_BYTES, this->history_bytes_ + history_count);
  if (this->listeners_ == 0 && !this->pre_roll_requested_) {
    xSemaphoreGive(this->buffer_mutex_);
    return;
  }
  if (this->barge_in_active_) {
    // Decisions and hangover use capture positions, not main-loop wall time.
    // Keep 1 s of trailing audio for remote endpointing (plus detector release).
    if (near_end) this->speech_until_byte_ = this->captured_bytes_ + SAMPLE_RATE * sizeof(int16_t);
    if (!near_end && !was_open) {
      xSemaphoreGive(this->buffer_mutex_);
      return;
    }
    if (!was_open) {
      const uint64_t history_start = this->captured_bytes_ - this->history_bytes_;
      constexpr size_t PRE_ROLL = 512 * SAMPLE_RATE / 1000 * sizeof(int16_t);
      const uint64_t start = std::max(this->last_enqueued_byte_, std::max(history_start,
          this->captured_bytes_ > PRE_ROLL ? this->captured_bytes_ - PRE_ROLL : uint64_t{0}));
      const size_t count = this->captured_bytes_ - start;
      if (count <= BUFFER_BYTES - this->buffered_bytes_) {
        const size_t source = (this->history_write_offset_ + HISTORY_BYTES - count) % HISTORY_BYTES;
        // Both rings can wrap independently; copying at most 512 ms happens
        // only on an opening edge. The current frame is included exactly once.
        if (this->buffered_bytes_ == 0) this->queue_start_byte_ = start;
        for (size_t i = 0; i < count; ++i)
          this->buffer_[(this->read_offset_ + this->buffered_bytes_ + i) % BUFFER_BYTES] =
              this->buffer_[BUFFER_BYTES + (source + i) % HISTORY_BYTES];
        this->buffered_bytes_ += count;
      } else {
        this->dropped_bytes_.fetch_add(count);
      }
      this->last_enqueued_byte_ = this->captured_bytes_;
      xSemaphoreGive(this->buffer_mutex_);
      return;
    }
    this->last_enqueued_byte_ = this->captured_bytes_;
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
    // The gate above bounds delivery; frame rejection must not chop an utterance.
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
