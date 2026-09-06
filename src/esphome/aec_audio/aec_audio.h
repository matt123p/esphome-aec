#pragma once

#ifdef USE_ESP32

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/ringbuf.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esphome/components/audio_adc/audio_adc.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

namespace esphome {

namespace micro_wake_word {
class MicroWakeWord;
}

namespace aec_audio {

class AECAudioComponent;
extern AECAudioComponent *global_aec_audio;

enum AECAudioMode : uint8_t {
  AEC_AUDIO_MODE_FD_LOW_COST = 0,
  AEC_AUDIO_MODE_FD_HIGH_PERF = 1,
};

enum AECAudioReferenceSource : uint8_t {
  AEC_AUDIO_REFERENCE_ANALOG_SLOT = 0,
  AEC_AUDIO_REFERENCE_PLAYBACK = 1,
};

enum AECAudioNlpLevel : uint8_t {
  AEC_AUDIO_NLP_NORMAL = 0,
  AEC_AUDIO_NLP_AGGRESSIVE = 1,
  AEC_AUDIO_NLP_VERY_AGGRESSIVE = 2,
};

enum AECCaptureState : uint8_t {
  AEC_CAPTURE_IDLE = 0,
  AEC_CAPTURE_CAPTURING = 1,
  AEC_CAPTURE_READY = 2,
};

class AECAudioMicrophone;
class AECAudioSpeaker;

#ifdef USE_AEC_AUDIO_METERS
class AECAudioMetersCallback {
 public:
  virtual void process_raw(const int16_t *raw, size_t frames, uint8_t slots) = 0;
  virtual void process_reference(const int16_t *ref, size_t frames) = 0;
  virtual void process_output(const int16_t *planar, size_t frames) = 0;
};

class AECAudioMetersComponent : public Component, public AECAudioMetersCallback {
 public:
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void setup() override;
  void dump_config() override;

  static const uint8_t SPECTRUM_BINS = 32;

  float get_slot_rms(uint8_t slot) const { return slot < 4 ? this->slot_rms_[slot].load() : 0.0f; }
  uint32_t get_slot_peak(uint8_t slot) const { return slot < 4 ? this->slot_peak_[slot].load() : 0; }
  float get_output_dbfs() const { return this->output_dbfs_.load(); }
  uint32_t get_output_peak() const { return this->output_peak_.load(); }
  uint32_t get_output_peak_3s() const { return this->output_peak_3s_.load(); }
  float get_output_clipped_percent() const { return this->output_clipped_percent_.load(); }
  float get_output_alternating_percent() const { return this->output_alternating_percent_.load(); }
  float get_output_min_dbfs() const { return this->output_min_dbfs_.load(); }
  float get_output_max_dbfs() const { return this->output_max_dbfs_.load(); }
  float get_reference_rms() const { return this->reference_rms_.load(); }
  uint32_t get_reference_peak() const { return this->reference_peak_.load(); }
  float get_spectrum_db(uint8_t bin) const { return bin < SPECTRUM_BINS ? this->spectrum_db_[bin] : -80.0f; }
  uint16_t get_spectrum_bin_hz(uint8_t bin) const {
    if (bin >= SPECTRUM_BINS)
      return 0;
    const uint16_t lo = this->spectrum_lo_bin_[bin];
    const uint16_t hi = this->spectrum_hi_bin_[bin];
    return static_cast<uint16_t>((lo + hi) / 2 * 16000 / SPECTRUM_FFT_SIZE);
  }
  void set_spectrum_enabled(bool enabled) { this->spectrum_enabled_.store(enabled); }

  void process_raw(const int16_t *raw, size_t frames, uint8_t slots) override;
  void process_reference(const int16_t *ref, size_t frames) override;
  void process_output(const int16_t *planar, size_t frames) override;

 protected:
  static const uint16_t SPECTRUM_FFT_SIZE = 512;
  static const size_t OUTPUT_METER_HISTORY_SIZE = 160;
  static const size_t OUTPUT_PEAK_HISTORY_SIZE = 94;
  void init_spectrum_();
  void compute_spectrum_(const int16_t *mono, size_t frames);
  static void fft_r2_(float *re, float *im, int n);

  std::atomic<float> slot_rms_[4]{};
  std::atomic<uint32_t> slot_peak_[4]{};
  std::atomic<float> reference_rms_{0.0f};
  std::atomic<uint32_t> reference_peak_{0};
  float output_meter_history_[OUTPUT_METER_HISTORY_SIZE]{};
  size_t output_meter_history_index_{0};
  size_t output_meter_history_count_{0};
  std::atomic<float> output_dbfs_{-96.0f};
  std::atomic<uint32_t> output_peak_{0};
  uint32_t output_peak_history_[OUTPUT_PEAK_HISTORY_SIZE]{};
  size_t output_peak_history_index_{0};
  size_t output_peak_history_count_{0};
  std::atomic<uint32_t> output_peak_3s_{0};
  std::atomic<float> output_clipped_percent_{0.0f};
  std::atomic<float> output_alternating_percent_{0.0f};
  std::atomic<float> output_min_dbfs_{-96.0f};
  std::atomic<float> output_max_dbfs_{-96.0f};
  float *fft_re_{nullptr};
  float *fft_im_{nullptr};
  float *hann_win_{nullptr};
  uint16_t spectrum_lo_bin_[SPECTRUM_BINS]{};
  uint16_t spectrum_hi_bin_[SPECTRUM_BINS]{};
  volatile float spectrum_db_[SPECTRUM_BINS]{};
  std::atomic<bool> spectrum_enabled_{false};
};
#endif

class AECAudioComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_audio_adc(audio_adc::AudioAdc *adc) { this->audio_adc_ = adc; }
  void set_pins(int mclk, int bclk, int lrclk, int din, int dout);
  void set_i2s_port(uint8_t port) { this->i2s_port_ = port; }
  void set_tdm_slots(uint8_t slots) { this->tdm_slots_ = slots; }
  void set_microphone_slots(uint8_t first, uint8_t second) {
    this->microphone_slots_[0] = first;
    this->microphone_slots_[1] = second;
  }
  void set_reference_slot(uint8_t slot) { this->reference_slot_ = slot; }
  void set_reference_source(AECAudioReferenceSource source) { this->reference_source_ = source; }
  void set_tx_slots(uint8_t first, uint8_t second) {
    this->tx_slots_[0] = first;
    this->tx_slots_[1] = second;
  }
  void set_diagnostic_raw_slot(int8_t slot) { this->diagnostic_raw_slot_.store(slot); }
  void set_aec_mode(AECAudioMode mode) { this->aec_mode_ = mode; }
  void set_nlp_level(AECAudioNlpLevel level) { this->nlp_level_ = level; }
  void set_filter_length(uint8_t length) { this->filter_length_ = length; }
  void set_agc_enabled(bool enabled) { this->agc_enabled_ = enabled; }
  void set_afe_include_unused_channel(bool enabled) { this->afe_include_unused_channel_ = enabled; }
  void set_microphone(AECAudioMicrophone *microphone) { this->microphone_ = microphone; }
  void set_speaker(AECAudioSpeaker *speaker) { this->speaker_ = speaker; }
  void set_reference_delay_samples(uint16_t samples) { this->reference_delay_samples_ = samples; }
  void set_noise_suppression_enabled(bool enabled) { this->noise_suppression_enabled_ = enabled; }
  void set_speech_enhancement_enabled(bool enabled) { this->speech_enhancement_enabled_ = enabled; }
  void set_wakenet_enabled(bool enabled) { this->wakenet_enabled_ = enabled; }
  void set_wakenet_running(bool running) { this->wakenet_running_ = running; }
  bool get_vad_state() const { return this->vad_state_.load(); }
  void register_micro_wake_word(micro_wake_word::MicroWakeWord *mww) { this->micro_wake_word_ = mww; }

#ifdef USE_AEC_AUDIO_METERS
  void register_meters_callback(AECAudioMetersCallback *callback) { this->meters_callback_ = callback; }
#endif

  // Capture API
  void start_capture(size_t frames = CAPTURE_FRAMES) {
    this->capture_target_frames_.store(std::min(frames, CAPTURE_FRAMES));
    this->capture_samples_written_.store(0);
    this->capture_state_.store(AEC_CAPTURE_CAPTURING);
  }
  AECCaptureState get_capture_state() const { return static_cast<AECCaptureState>(this->capture_state_.load()); }
  size_t get_capture_frames() const { return this->capture_samples_written_.load(); }
  size_t play_capture();
  static const size_t CAPTURE_SECONDS = 3;
  static const size_t CAPTURE_FRAMES = CAPTURE_SECONDS * 16000;  // 16 kHz mono

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait);
  bool has_buffered_data() const;
  void clear_playback();

 protected:
  static void audio_task(void *params);
  static void playback_task(void *params);
  bool start_i2s_();
  bool start_afe_();
  void run_audio_task_();
  void run_playback_task_();
  void publish_frame_(const int16_t *raw, size_t frames);
  void capture_frame_(const int16_t *mono, size_t frames);
#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  std::vector<int16_t> resample(const int16_t *input, size_t frames, uint8_t channels);
  void initialise_resampler();
  void record_playback_input_rate_(size_t accepted_bytes, uint8_t channels);
  void update_playback_rate_(uint32_t write_us, size_t frames);
#endif

  audio_adc::AudioAdc *audio_adc_{nullptr};
  AECAudioMicrophone *microphone_{nullptr};
  AECAudioSpeaker *speaker_{nullptr};
  std::shared_ptr<ring_buffer::RingBuffer> playback_buffer_;
  std::shared_ptr<ring_buffer::RingBuffer> reference_buffer_;
  TaskHandle_t audio_task_handle_{nullptr};
  TaskHandle_t playback_task_handle_{nullptr};
  i2s_chan_handle_t rx_handle_{nullptr};
  i2s_chan_handle_t tx_handle_{nullptr};
  const esp_afe_sr_iface_t *afe_iface_{nullptr};
  esp_afe_sr_data_t *afe_data_{nullptr};
  size_t afe_feed_chunksize_{0};
  size_t afe_fetch_chunksize_{0};

  gpio_num_t mclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t bclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t lrclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t din_pin_{I2S_GPIO_UNUSED};
  gpio_num_t dout_pin_{I2S_GPIO_UNUSED};
  uint8_t i2s_port_{0};
  uint8_t tdm_slots_{4};
  uint8_t microphone_slots_[2]{0, 1};
  uint8_t reference_slot_{2};
  AECAudioReferenceSource reference_source_{AEC_AUDIO_REFERENCE_ANALOG_SLOT};
  uint8_t tx_slots_[2]{0, 1};
  std::atomic<int8_t> diagnostic_raw_slot_{-1};
  AECAudioMode aec_mode_{AEC_AUDIO_MODE_FD_LOW_COST};
  AECAudioNlpLevel nlp_level_{AEC_AUDIO_NLP_AGGRESSIVE};
  uint8_t filter_length_{4};
  bool agc_enabled_{true};
  bool afe_include_unused_channel_{true};
  uint16_t reference_delay_samples_{0};
  bool noise_suppression_enabled_{true};
  bool speech_enhancement_enabled_{true};
  bool wakenet_enabled_{false};
  std::atomic<bool> wakenet_running_{false};
  std::atomic<bool> vad_state_{false};
  QueueHandle_t wakenet_queue_{nullptr};
  micro_wake_word::MicroWakeWord *micro_wake_word_{nullptr};

  std::atomic<uint32_t> rx_errors_{0};
  std::atomic<uint32_t> tx_errors_{0};
  std::atomic<uint32_t> playback_underruns_{0};
  std::atomic<uint32_t> reference_underruns_{0};
  std::atomic<uint32_t> reference_overflows_{0};
  std::atomic<uint32_t> dropped_frames_{0};
  std::atomic<uint32_t> max_processing_us_{0};
  std::atomic<bool> buffering_{true};

  // Capture buffer (PSRAM, allocated in setup)
  int16_t *capture_buffer_{nullptr};
  std::atomic<uint8_t> capture_state_{AEC_CAPTURE_IDLE};
  std::atomic<size_t> capture_samples_written_{0};
  std::atomic<size_t> capture_target_frames_{CAPTURE_FRAMES};
  std::atomic<float> reference_rms_{0.0f};
  std::atomic<uint32_t> reference_peak_{0};
  uint32_t last_diagnostic_log_{0};
  std::atomic<uint32_t> play_calls_{0};
  std::atomic<uint32_t> play_zero_writes_{0};
  std::atomic<size_t> play_requested_bytes_{0};
  std::atomic<size_t> play_source_bytes_{0};
  std::atomic<size_t> play_enqueued_bytes_{0};
  std::atomic<size_t> playback_drained_bytes_{0};
  uint32_t last_input_rate_log_{0};
  size_t last_logged_play_requested_bytes_{0};
  size_t last_logged_input_rate_accepted_bytes_{0};
  uint32_t last_handoff_log_{0};
  uint32_t last_logged_play_calls_{0};
  uint32_t last_logged_play_zero_writes_{0};
  size_t last_logged_play_source_bytes_{0};
  size_t last_logged_play_enqueued_bytes_{0};
  size_t last_logged_playback_drained_bytes_{0};

#ifdef USE_AEC_AUDIO_PLAYBACK_RESAMPLER
  std::atomic<uint32_t> playback_rate_{0};
  std::atomic<uint32_t> phase_increment_{0};
  std::atomic<uint32_t> input_source_rate_{0};
  std::atomic<uint32_t> input_playback_rate_{0};
  std::atomic<bool> input_rate_valid_{false};
  uint32_t phase_accumulator_{0};
  int16_t last_samples_[2]{0, 0};
  uint32_t last_playback_rate_log_{0};
  uint32_t input_rate_window_start_ms_{0};
  uint32_t input_rate_last_ms_{0};
  size_t input_rate_window_bytes_{0};
  uint8_t input_rate_channels_{0};
#endif

#ifdef USE_AEC_AUDIO_METERS
  AECAudioMetersCallback *meters_callback_{nullptr};
#endif
};

class AECAudioMicrophone : public microphone::Microphone, public Component {
 public:
  ~AECAudioMicrophone();
  void setup() override;
  void dump_config() override;
  void loop() override;
  void start() override;
  void stop() override;
  void set_parent(AECAudioComponent *parent) { this->parent_ = parent; }
  void publish(const uint8_t *data, const size_t data_size);
  void request_pre_roll();
  void begin_pre_roll_replay();
  void discard_pending_audio();
  void set_response_playing(bool playing);
  uint8_t get_listener_count() const { return this->listeners_.load(); }

 protected:
  // Fixed PSRAM storage: retain setup latency and main-loop stalls without
  // growing allocations or consuming scarce internal RAM.
  static constexpr size_t BUFFER_BYTES = 16000 * sizeof(int16_t) * 4;
  static constexpr size_t DELIVERY_BYTES = 1024;
  static constexpr size_t HISTORY_BYTES = 16000 * sizeof(int16_t);
  AECAudioComponent *parent_{nullptr};
  std::atomic<uint8_t> listeners_{0};
  SemaphoreHandle_t buffer_mutex_{nullptr};
  uint8_t *buffer_{nullptr};
  uint64_t captured_bytes_{0};
  uint64_t queue_start_byte_{0};
  uint64_t last_detector_byte_{0};
  bool detector_position_valid_{false};
  bool response_playing_{false};
  bool response_tail_{false};
  uint32_t response_end_ms_{0};
  size_t history_write_offset_{0};
  size_t history_bytes_{0};
  size_t read_offset_{0};
  size_t buffered_bytes_{0};
  bool pre_roll_requested_{false};
  bool utterance_active_{false};
  uint32_t last_delivery_ms_{0};
  std::atomic<uint32_t> dropped_bytes_{0};
  uint32_t last_overflow_log_ms_{0};
  uint32_t last_heap_log_ms_{0};
  std::vector<uint8_t> vec;

};

class AECAudioSpeaker : public speaker::Speaker, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  void set_parent(AECAudioComponent *parent) { this->parent_ = parent; }
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override;
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  void notify_output(uint32_t frames, int64_t timestamp) { this->audio_output_callback_(frames, timestamp); }

 protected:
  AECAudioComponent *parent_{nullptr};
};

}  // namespace aec_audio
}  // namespace esphome

#endif
