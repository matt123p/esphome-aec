#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <functional>
#include <string>
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
#include "near_end_detector.h"
#include "cancellation_quality.h"
#include "soft_double_talk.h"
#include "near_end_correlation.h"
#include "wake_beep.h"
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
#include "neural_barge_in.h"
#endif
#include "esphome/components/audio_adc/audio_adc.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#ifdef USE_AEC_SPEEXDSP_PROBABILITY_SENSORS
#include "esphome/components/sensor/sensor.h"
#endif

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

// Which microphone runs through the mono canceller and preprocessor.
// Beamforming supersedes this selection and processes all configured microphones.
enum AECSpeexDspOutputChannel : uint8_t {
  AEC_SPEEXDSP_OUTPUT_FIRST = 0,
  AEC_SPEEXDSP_OUTPUT_SECOND = 1,
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
  void configure_agc_gate(bool enabled, float ref_open, float ref_close, float open, float close,
                          int open_ms, int hold_ms, int tail_ms, int release_ms, int startup_guard_ms) {
    agc_gate_config_ = {enabled, ref_open, ref_close, open, close, open_ms, hold_ms, tail_ms, release_ms, startup_guard_ms};
  }
  void set_agc_target_level(float level) { this->agc_target_level_ = level; }
  void set_agc_max_gain_db(int db) { this->agc_max_gain_db_ = db; }
  void set_noise_suppression_enabled(bool enabled) { this->noise_suppression_enabled_ = enabled; }
  void set_noise_suppression_level_db(uint8_t db) { this->noise_suppression_level_db_ = db; }
  void set_vad_enabled(bool enabled) { this->vad_enabled_ = enabled; }
  void set_vad_threshold(uint8_t threshold) { this->vad_threshold_ = threshold; }
  void set_echo_suppress_db(uint8_t db) { this->echo_suppress_db_ = db; }
  void set_echo_suppress_active_db(uint8_t db) { this->echo_suppress_active_db_ = db; }
  void set_soft_double_talk_enabled(bool enabled) { this->soft_double_talk_enabled_ = enabled; }
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  void configure_neural_barge_in(float threshold, unsigned consecutive, bool protect, uint32_t log_ms) {
    neural_threshold_ = threshold > 0 ? threshold : barge_model::threshold;
    neural_confirmation_ = consecutive; neural_protection_ = protect; neural_log_ms_ = log_ms;
  }
  uint32_t get_neural_barge_events() const { return neural_events_.load(); }
  void reset_neural_barge_in() { neural_reset_requested_.store(true); }
  // The audio task applies changes at frame boundaries and resets causal state.
  void set_neural_barge_in_enabled(bool enabled) { neural_enabled_requested_.store(enabled); }
  bool is_neural_barge_in_enabled() const { return neural_enabled_requested_.load(); }
  // Evaluated on the main loop; the audio task only reads the atomic snapshot.
  void set_voice_assistant_active_condition(std::function<bool()> condition) {
    voice_assistant_active_condition_ = std::move(condition);
  }
#endif
  void set_microphone(AECSpeexDspMicrophone *microphone) { this->microphone_ = microphone; }
  void set_speaker(AECSpeexDspSpeaker *speaker) { this->speaker_ = speaker; }
  void play_wake_beep() { this->wake_beep_.start(); }
  bool is_wake_beep_active() const { return this->wake_beep_.active(); }
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

  // Authoritative speech decision and selected detector probability.
  bool get_vad_state() const { return this->vad_state_.load(); }
  void configure_reference(float open, float close, uint32_t tail_ms, uint32_t startup_ms) {
    this->reference_tracker_.configure(open, close, tail_ms, startup_ms);
  }
  void set_observation_enabled(bool enabled) { this->near_end_detector_.set_observation_enabled(enabled); }
  void set_adaptive_enabled(bool enabled) { this->near_end_detector_.set_adaptive_enabled(enabled); }
  float get_vad_probability() const { return this->vad_probability_.load(); }
  // Independent diagnostic probabilities, each in [0, 1].
  float get_speex_vad_probability() const { return this->speex_vad_probability_.load(); }
  float get_neural_barge_probability() const {
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    return this->neural_probability_.load();
#else
    return 0.0f;
#endif
  }
  float get_near_end_probability() const { return this->near_end_probability_.load(); }
  void configure_near_end(float min_rms, float margin_db, float echo_floor_db,
                          uint32_t onset_ms, uint32_t release_ms, uint32_t startup_ms) {
    this->near_end_enabled_ = true;
    this->near_end_detector_.configure(min_rms, margin_db, echo_floor_db, onset_ms, release_ms, startup_ms);
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

  // Diagnostic capture: interleaved multi-channel taps of the microphone
  // interface, buffered in PSRAM and dumped to the host over the USB
  // Serial/JTAG port. Channel layout (per sample frame):
  //   0: raw microphone slot A   (processed_slots_[0])
  //   1: raw microphone slot B   (processed_slots_[1])
  //   2: AEC reference (post delay alignment)
  // The DSP outputs are intentionally not captured: the host recreates them
  // offline from these three channels with the same code.
  static constexpr size_t DIAG_CHANNELS = 3;
  static constexpr uint32_t DIAG_MAX_SECONDS = 120;

  // One labelled marker recorded alongside the diagnostic stream. Sample index
  // is in the shared audio frame clock (get_frame_samples()).
  struct DiagEvent {
    uint64_t sample;
    uint32_t millis;
    char id[20];
  };

  // Host-driven playback/capture control wired up from YAML on_boot so this
  // component stays decoupled from media_player/button instances.
  struct SerialControlActions {
    std::map<std::string, std::function<void()>> play;
    std::function<void()> stop;
    std::function<void()> pause;
    std::function<void()> resume;
    std::function<void(float)> set_volume;
  };
  void set_serial_control_enabled(bool enabled) { this->serial_control_enabled_ = enabled; }
  void set_diag_capture_max_seconds(uint32_t seconds) { this->diag_max_seconds_ = std::min(seconds, DIAG_MAX_SECONDS); }
  void set_serial_control_actions(SerialControlActions actions);

  // True pause: the playback task stops draining media into the I2S TX and
  // feeds silence instead, so buffered audio resumes at the same sample.
  void set_playback_paused(bool paused) { this->playback_paused_.store(paused); }
  bool is_playback_paused() const { return this->playback_paused_.load(); }

  // Capture-only mode: read the microphones and reference, buffer them, and
  // skip the canceller/preprocess entirely. The host recreates the DSP outputs
  // offline. Guaranteed real-time capture with zero dropped frames.
  void set_capture_only(bool enabled) { this->capture_only_ = enabled; }
  bool is_capture_only() const { return this->capture_only_; }

  // Master sample clock: DSP frames processed since boot (16 kHz domain).
  uint64_t get_frame_samples() const { return this->frame_samples_.load(); }

  // Diagnostic capture control (host/serial context, not the audio task).
  bool diag_capture_start(uint32_t seconds);
  bool diag_capture_stop();
  bool diag_capture_free();
  AECSpeexDspCaptureState get_diag_capture_state() const {
    return static_cast<AECSpeexDspCaptureState>(this->diag_state_.load());
  }
  size_t get_diag_capture_frames() const { return this->diag_written_frames_.load(); }
  uint64_t get_diag_capture_start_sample() const { return this->diag_start_sample_.load(); }
  uint8_t get_diag_channels() const { return this->diag_channels_.load(); }
  uint32_t get_diag_max_seconds() const { return this->diag_max_seconds_; }
  // Copies up to dst_frames of interleaved frames starting at src_frame into
  // dst. Returns frames copied. Only valid in READY state.
  size_t diag_copy_frames(uint8_t *dst, size_t dst_bytes, size_t src_frame) const;
  std::vector<DiagEvent> diag_copy_events() const;

  static const size_t CAPTURE_SECONDS = 3;
  static const size_t CAPTURE_FRAMES = CAPTURE_SECONDS * 16000;  // 16 kHz mono
  // Cap of the runtime analog-reference delay history.

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
  void capture_diag_frame_(const int16_t *raw, const int16_t *ref, size_t frames);
  void diag_record_event_(const char *id);
#ifdef USE_AEC_SPEEXDSP_PLAYBACK_RESAMPLER
  std::vector<int16_t> resample(const int16_t *input, size_t frames, uint8_t channels);
  void initialise_resampler();
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
  SpeexAgcGateConfig agc_gate_config_{0, 200.f, 100.f, 64.f, 32.f, 32, 250, 250, 150, 200};
  float agc_target_level_{0.25f};
  int agc_max_gain_db_{12};
  bool noise_suppression_enabled_{true};
  uint8_t noise_suppression_level_db_{15};
  bool vad_enabled_{true};
  uint8_t vad_threshold_{35};
  uint8_t echo_suppress_db_{40};
  uint8_t echo_suppress_active_db_{15};
  bool soft_double_talk_enabled_{false};
  uint16_t reference_delay_samples_{0};
  float playback_gain_db_{0.0f};
  float playback_gain_{1.0f};
  WakeBeep wake_beep_;
  bool beamforming_enabled_{false};
  AdaptiveDelayAndSumBeamformer beamformer_;
  AdaptiveDelayAndSumBeamformer raw_beamformer_;
  std::atomic<bool> vad_state_{false};
  std::atomic<float> vad_probability_{0.0f};
  std::atomic<float> speex_vad_probability_{0.0f};
  std::atomic<float> near_end_probability_{0.0f};
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
  std::unique_ptr<NeuralBargeIn> neural_barge_;
  bool neural_protection_{false};
  float neural_threshold_{barge_model::threshold};
  unsigned neural_confirmation_{3}; uint32_t neural_log_ms_{5000};
  std::atomic<float> neural_probability_{0}; std::atomic<uint32_t> neural_events_{0};
  std::atomic<bool> neural_reset_requested_{false};
  std::atomic<bool> neural_enabled_requested_{true};
  bool neural_enabled_{true};  // Applied by the audio task, never written by the main loop.
  bool neural_barge_active_{false};  // Playback AND assistant gate, audio task only.
  std::function<bool()> voice_assistant_active_condition_{};
  std::atomic<bool> voice_assistant_active_{false};
#endif
  bool neural_reference_active_{false};  // Audio task only.
  bool neural_controls_audio_() const {
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    return neural_enabled_ && neural_barge_active_;
#else
    return false;
#endif
  }
  bool legacy_near_end_enabled_() const {
#ifdef USE_AEC_SPEEXDSP_NEURAL_BARGE_IN
    // Neural installations use Speex VAD outside the barge-in scope.
    return false;
#else
    return near_end_enabled_;
#endif
  }
  bool near_end_enabled_{false};
  NearEndDetector near_end_detector_;
  SoftDoubleTalkController soft_double_talk_;
  NearEndCorrelation near_end_correlation_;
  ReferenceTracker reference_tracker_;
  CancellationQuality cancellation_quality_;
  bool near_end_speech_{false};  // DSP task only; passed alongside its PCM frame
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

  // Diagnostic multi-channel capture (PSRAM, allocated on demand).
  int16_t *diag_buffer_{nullptr};
  size_t diag_capacity_frames_{0};
  std::atomic<uint8_t> diag_state_{AEC_SPEEXDSP_CAPTURE_IDLE};
  std::atomic<size_t> diag_written_frames_{0};
  std::atomic<uint32_t> diag_target_frames_{0};
  std::atomic<uint64_t> diag_start_sample_{0};
  std::atomic<uint8_t> diag_channels_{DIAG_CHANNELS};
  uint32_t diag_max_seconds_{30};
  mutable SemaphoreHandle_t diag_mutex_{nullptr};
  std::vector<DiagEvent> diag_events_;

  // USB Serial/JTAG host control channel.
  bool serial_control_enabled_{false};
  SerialControlActions serial_actions_;
  TaskHandle_t serial_task_handle_{nullptr};
  static void serial_task(void *params);
  void run_serial_task_();
  void handle_serial_line_(const std::string &line);
  bool serial_write_(const uint8_t *data, size_t length);
  bool serial_write_string_(const std::string &text) {
    return this->serial_write_(reinterpret_cast<const uint8_t *>(text.data()), text.size());
  }
  void serial_dump_capture_(uint32_t from_frame);

  // Master sample clock of the DSP pipeline.
  std::atomic<uint64_t> frame_samples_{0};
  // True pause gate for the playback task.
  std::atomic<bool> playback_paused_{false};
  // Skip the canceller; record microphones + reference only.
  bool capture_only_{false};

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
  uint32_t phase_accumulator_{0};
  int16_t last_samples_[2]{0, 0};
  uint32_t last_playback_rate_log_{0};
#endif

#ifdef USE_AEC_SPEEXDSP_METERS
  AECSpeexDspMetersCallback *meters_callback_{nullptr};
#endif
};

#ifdef USE_AEC_SPEEXDSP_PROBABILITY_SENSORS
class AECSpeexDspProbabilitySensors : public PollingComponent {
 public:
  void set_parent(AECSpeexDspComponent *parent) { parent_ = parent; }
  void set_speex_vad_probability_sensor(sensor::Sensor *value) { speex_ = value; }
  void set_gru_probability_sensor(sensor::Sensor *value) { gru_ = value; }
  void update() override {
    if (parent_ == nullptr) return;
    if (speex_) speex_->publish_state(100.0f * parent_->get_speex_vad_probability());
    if (gru_) gru_->publish_state(100.0f * parent_->get_neural_barge_probability());
  }

 protected:
  AECSpeexDspComponent *parent_{nullptr};
  sensor::Sensor *speex_{nullptr}, *gru_{nullptr};
};
#endif

class AECSpeexDspMicrophone : public microphone::Microphone, public Component {
 public:
  ~AECSpeexDspMicrophone();
  void setup() override;
  void dump_config() override;
  void loop() override;
  void start() override;
  void stop() override;
  void set_parent(AECSpeexDspComponent *parent) { this->parent_ = parent; }
  void publish(const uint8_t *data, const size_t data_size, bool near_end = false, bool echo_rejected = false,
               bool playback_active = false);
  void request_pre_roll();
  size_t begin_pre_roll_replay();
  void discard_pending_audio();
  void set_response_playing(bool playing);
  // Enable only for the full-duplex response. Repeated flush/start notifications
  // are idempotent and must not truncate a user utterance already in progress.
  bool set_barge_in_active(bool active);
  bool is_near_end_speech() const { return this->near_end_speech_.load(); }
  void set_delivery_condition(std::function<bool(size_t)> condition) { this->delivery_condition_ = std::move(condition); }
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
  bool barge_in_active_{false};
  std::atomic<bool> near_end_speech_{false};
  uint64_t speech_until_byte_{0};
  bool playback_active_{false};
  size_t playback_silence_bytes_{0};
  uint64_t last_enqueued_byte_{0};
  std::function<bool(size_t)> delivery_condition_{};
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
