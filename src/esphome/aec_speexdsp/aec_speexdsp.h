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

#include "speex_echo.h"
#include "speex_preprocess.h"
#include "beamformer.h"
#include "esphome/components/audio_adc/audio_adc.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

namespace esphome {
namespace aec_speexdsp {

// The SpeexDSP pipeline is fixed at 16 kHz / 16-bit, matching the rates the
// microphone and speaker endpoints publish.
static constexpr uint32_t SAMPLE_RATE = 16000;

class AECSpeexDspComponent;
extern AECSpeexDspComponent *global_aec_speexdsp;

enum AECSpeexDspReferenceSource : uint8_t {
  AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT = 0,
  AEC_SPEEXDSP_REFERENCE_PLAYBACK = 1,
};

// Which processed microphone channel is published. SpeexDSP has no dual-
// microphone enhancement stage, so each selected channel runs through its own
// echo canceller and preprocessor and the output is selected (or mixed) here.
enum AECSpeexDspOutputChannel : uint8_t {
  AEC_SPEEXDSP_OUTPUT_FIRST = 0,
  AEC_SPEEXDSP_OUTPUT_SECOND = 1,
  AEC_SPEEXDSP_OUTPUT_MIXED = 2,
};

enum AECSpeexDspCaptureState : uint8_t {
  AEC_SPEEXDSP_CAPTURE_IDLE = 0,
  AEC_SPEEXDSP_CAPTURE_CAPTURING = 1,
  AEC_SPEEXDSP_CAPTURE_READY = 2,
};

class AECSpeexDspMicrophone;
class AECSpeexDspSpeaker;

#ifdef USE_AEC_SPEEXDSP_METERS
class AECSpeexDspMetersCallback {
 public:
  virtual void process_raw(const int16_t *raw, size_t frames, uint8_t slots) = 0;
  virtual void process_reference(const int16_t *ref, size_t frames) = 0;
  virtual void process_output(const int16_t *mono, size_t frames) = 0;
};

class AECSpeexDspMetersComponent : public Component, public AECSpeexDspMetersCallback {
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
  void process_output(const int16_t *mono, size_t frames) override;

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

class AECSpeexDspComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void setup() override;
  void dump_config() override;
  void loop() override;

  void set_audio_adc(audio_adc::AudioAdc *adc) { this->audio_adc_ = adc; }
  void set_pins(int mclk, int bclk, int lrclk, int din, int dout);
  void set_i2s_port(uint8_t port) { this->i2s_port_ = port; }
  void set_tdm_slots(uint8_t slots) { this->tdm_slots_ = slots; }
  void set_microphone_slots(const std::vector<uint8_t> &slots) { this->microphone_slots_ = slots; }
  void set_reference_slot(uint8_t slot) { this->reference_slot_ = slot; }
  void set_reference_source(AECSpeexDspReferenceSource source) { this->reference_source_ = source; }
  void set_tx_slots(uint8_t first, uint8_t second) {
    this->tx_slots_[0] = first;
    this->tx_slots_[1] = second;
  }
  void set_diagnostic_raw_slot(int8_t slot) { this->diagnostic_raw_slot_.store(slot); }
  void set_frame_size(uint16_t frame_size) { this->frame_size_ = frame_size; }
  void set_filter_length(uint16_t length) { this->filter_length_ = length; }
  void set_output_channel(AECSpeexDspOutputChannel channel) { this->output_channel_ = channel; }
  void set_agc_enabled(bool enabled) { this->agc_enabled_ = enabled; }
  void set_agc_target_level(float level) { this->agc_target_level_ = level; }
  void set_noise_suppression_enabled(bool enabled) { this->noise_suppression_enabled_ = enabled; }
  void set_noise_suppression_level_db(uint8_t db) { this->noise_suppression_level_db_ = db; }
  void set_vad_enabled(bool enabled) { this->vad_enabled_ = enabled; }
  void set_vad_threshold(uint8_t threshold) { this->vad_threshold_ = threshold; }
  void set_echo_suppress_db(uint8_t db) { this->echo_suppress_db_ = db; }
  void set_echo_suppress_active_db(uint8_t db) { this->echo_suppress_active_db_ = db; }
  void set_microphone(AECSpeexDspMicrophone *microphone) { this->microphone_ = microphone; }
  void set_speaker(AECSpeexDspSpeaker *speaker) { this->speaker_ = speaker; }
  void set_reference_delay_samples(uint16_t samples) { this->reference_delay_samples_ = samples; }
  // Digital attenuation applied to media playback before the I2S TX and the
  // reference tap, keeping the analog chain (amp, loopback) out of clipping.
  void set_playback_gain_db(float db) { this->playback_gain_db_ = db; }
  void configure_beamforming(bool enabled, uint8_t max_lag, uint8_t update_frames, uint16_t min_rms,
                             uint8_t min_correlation_percent, uint8_t min_peak_dominance_percent) {
    this->beamforming_enabled_ = enabled;
    this->beamformer_.configure(this->microphone_slots_.size(), max_lag, update_frames, min_rms,
                                min_correlation_percent, min_peak_dominance_percent);
  }

#ifdef USE_AEC_SPEEXDSP_METERS
  void register_meters_callback(AECSpeexDspMetersCallback *callback) { this->meters_callback_ = callback; }
#endif

  bool get_vad_state() const { return this->vad_state_.load(); }
  float get_vad_probability() const { return this->vad_probability_.load(); }
  void reset_audio_activity() {
    const uint32_t now = millis();
    this->last_microphone_activity_ms_.store(now);
    this->last_playback_activity_ms_.store(now);
  }
  bool audio_silent_for(uint32_t duration_ms) const {
    const uint32_t now = millis();
    return now - this->last_microphone_activity_ms_.load() >= duration_ms &&
           now - this->last_playback_activity_ms_.load() >= duration_ms;
  }

  // Capture API
  void start_capture(size_t frames = CAPTURE_FRAMES) {
    this->capture_target_frames_.store(std::min(frames, CAPTURE_FRAMES));
    this->capture_samples_written_.store(0);
    this->capture_state_.store(AEC_SPEEXDSP_CAPTURE_CAPTURING);
  }
  AECSpeexDspCaptureState get_capture_state() const {
    return static_cast<AECSpeexDspCaptureState>(this->capture_state_.load());
  }
  size_t get_capture_frames() const { return this->capture_samples_written_.load(); }
  size_t play_capture();
  static const size_t CAPTURE_SECONDS = 3;
  static const size_t CAPTURE_FRAMES = CAPTURE_SECONDS * 16000;  // 16 kHz mono
  // Cap of the runtime analog-reference delay history.
  static constexpr int REFERENCE_DELAY_MAX_SAMPLES = 256;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait);
  bool has_buffered_data() const;
  void clear_playback();

 protected:
  static void audio_task(void *params);
  static void playback_task(void *params);
  bool start_i2s_();
  bool start_dsp_();
  void destroy_dsp_();
  void run_audio_task_();
  void run_playback_task_();
  uint8_t processed_channels_() const;
  void publish_frame_(const int16_t *mono, size_t frames);
  void capture_frame_(const int16_t *mono, size_t frames);
  void apply_reference_delay_(int16_t *ref, size_t frames);
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  std::vector<int16_t> resample(const int16_t *input, size_t frames, uint8_t channels);
  void initialise_resampler();
  void record_playback_input_rate_(size_t accepted_bytes, uint8_t channels);
  void update_playback_rate_(uint32_t write_us, size_t frames);
#endif

  audio_adc::AudioAdc *audio_adc_{nullptr};
  AECSpeexDspMicrophone *microphone_{nullptr};
  AECSpeexDspSpeaker *speaker_{nullptr};
  std::shared_ptr<ring_buffer::RingBuffer> playback_buffer_;
  std::shared_ptr<ring_buffer::RingBuffer> reference_buffer_;
  TaskHandle_t audio_task_handle_{nullptr};
  TaskHandle_t playback_task_handle_{nullptr};
  i2s_chan_handle_t rx_handle_{nullptr};
  i2s_chan_handle_t tx_handle_{nullptr};

  // AEC remains per physical microphone. Beamforming uses one post-sum
  // preprocessor to preserve inter-microphone phase and avoid repeated NLP.
  SpeexEchoState *echo_state_[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  SpeexPreprocessState *preprocess_state_[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
  // Mapping of processing-state index to the microphone slot it consumes.
  uint8_t processed_slots_[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{0, 1, 2, 3};

  gpio_num_t mclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t bclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t lrclk_pin_{I2S_GPIO_UNUSED};
  gpio_num_t din_pin_{I2S_GPIO_UNUSED};
  gpio_num_t dout_pin_{I2S_GPIO_UNUSED};
  uint8_t i2s_port_{0};
  uint8_t tdm_slots_{4};
  std::vector<uint8_t> microphone_slots_{0, 1};
  uint8_t reference_slot_{2};
  AECSpeexDspReferenceSource reference_source_{AEC_SPEEXDSP_REFERENCE_ANALOG_SLOT};
  uint8_t tx_slots_[2]{0, 1};
  std::atomic<int8_t> diagnostic_raw_slot_{-1};
  uint16_t frame_size_{256};
  uint16_t filter_length_{2048};
  AECSpeexDspOutputChannel output_channel_{AEC_SPEEXDSP_OUTPUT_FIRST};
  bool agc_enabled_{true};
  float agc_target_level_{0.25f};
  bool noise_suppression_enabled_{true};
  uint8_t noise_suppression_level_db_{15};
  bool vad_enabled_{true};
  uint8_t vad_threshold_{35};
  uint8_t echo_suppress_db_{40};
  uint8_t echo_suppress_active_db_{15};
  uint16_t reference_delay_samples_{0};
  float playback_gain_db_{0.0f};
  float playback_gain_{1.0f};
  bool beamforming_enabled_{false};
  AdaptiveDelayAndSumBeamformer beamformer_;
  // Runtime analog-reference delay in RAM (no persistence). Seeded from
  // reference_delay_samples_ at setup.
  std::atomic<int> reference_delay_{0};
  int16_t reference_history_[REFERENCE_DELAY_MAX_SAMPLES + 1]{};
  size_t reference_history_pos_{0};
  std::atomic<bool> vad_state_{false};
  std::atomic<float> vad_probability_{0.0f};
  std::atomic<uint32_t> last_microphone_activity_ms_{0};
  std::atomic<uint32_t> last_playback_activity_ms_{0};

  std::atomic<uint32_t> rx_errors_{0};
  std::atomic<uint32_t> tx_errors_{0};
  std::atomic<uint32_t> playback_underruns_{0};
  std::atomic<uint32_t> reference_underruns_{0};
  std::atomic<uint32_t> reference_overflows_{0};
  std::atomic<uint32_t> dropped_frames_{0};
  std::atomic<uint32_t> max_processing_us_{0};
  std::atomic<bool> buffering_{true};
  std::atomic<uint32_t> buffering_since_ms_{0};

  // Capture buffer (PSRAM, allocated in setup)
  int16_t *capture_buffer_{nullptr};
  std::atomic<uint8_t> capture_state_{AEC_SPEEXDSP_CAPTURE_IDLE};
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

#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  static const uint32_t PLAYBACK_RATE = 16000;
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

#ifdef USE_AEC_SPEEXDSP_METERS
  AECSpeexDspMetersCallback *meters_callback_{nullptr};
#endif
};

class AECSpeexDspMicrophone : public microphone::Microphone, public Component {
 public:
  ~AECSpeexDspMicrophone();
  void setup() override;
  void dump_config() override;
  void loop() override;
  void start() override;
  void stop() override;
  void set_parent(AECSpeexDspComponent *parent) { this->parent_ = parent; }
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
  AECSpeexDspComponent *parent_{nullptr};
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
  std::vector<uint8_t> vec;
};

class AECSpeexDspSpeaker : public speaker::Speaker, public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  void set_parent(AECSpeexDspComponent *parent) { this->parent_ = parent; }
  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override;
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  void notify_output(uint32_t frames, int64_t timestamp) { this->audio_output_callback_(frames, timestamp); }

 protected:
  AECSpeexDspComponent *parent_{nullptr};
};

}  // namespace aec_speexdsp
}  // namespace esphome

#endif
