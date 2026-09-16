/* PC port of the aec_speexdsp component's processing chain.
 *
 * Mirrors AECSpeexDspComponent::start_dsp_ (aec_speexdsp.cpp:330) and the DSP
 * section of run_audio_task_ (aec_speexdsp.cpp:496): identical state
 * initialisation, ctl calls, buffer layouts and per-channel topology, minus
 * the I2S/FreeRTOS/ESPHome plumbing. Feeding it WAV data therefore exercises
 * exactly the code path the ESP32 runs. */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "beamformer.h"
#include "speex_echo.h"
#include "speex_preprocess.h"

namespace dsp {

constexpr uint32_t kSampleRate = 16000;

struct BeamConfig {
    bool enabled = false;
    uint8_t microphones = 2;
    uint8_t max_lag = 7;
    uint8_t update_frames = 8;
    uint16_t min_rms = 120;
    uint8_t min_correlation_percent = 50;
    uint8_t min_peak_dominance_percent = 5;
};

struct PipelineConfig {
    int frame_size = 256;
    int filter_length = 2048;
    bool noise_suppression = true;
    int noise_suppression_db = 15;
    bool agc = true;
    float agc_target_level = 0.25f;  // fraction of full scale, like the component
    bool vad = true;
    int vad_threshold = 35;
    int echo_suppress_db = 40;
    int echo_suppress_active_db = 15;
    int reference_delay_samples = 0;  // component's apply_reference_delay_
    bool output_mixed = false;        // AEC_SPEEXDSP_OUTPUT_MIXED (2 independent channels averaged)
    BeamConfig beam;
};

struct FrameResult {
    std::vector<int16_t> output;  // mono, frame_size samples
    float vad_probability = 0.0f;
    bool vad_state = false;
};

class Pipeline {
 public:
    explicit Pipeline(const PipelineConfig &config);
    ~Pipeline();
    Pipeline(const Pipeline &) = delete;
    Pipeline &operator=(const Pipeline &) = delete;

    // Number of microphones the pipeline consumes per frame (planar layout).
    uint8_t channels() const { return channels_; }

    // mic: planar channels*frame samples; ref: frame samples. Same per-frame
    // order of operations as run_audio_task_.
    FrameResult process(const int16_t *mic, const int16_t *ref);

    // Convenience: process a whole scenario. mic is planar channels x N; ref
    // is N. Returns latency-compensated mono output and per-frame VAD
    // probability. Flushes one frame: use process() for streaming instead.
    std::vector<int16_t> process_all(const std::vector<int16_t> &mic, const std::vector<int16_t> &ref,
                                     std::vector<float> *vad_prob = nullptr);

    const esphome::aec_speexdsp::AdaptiveDelayAndSumBeamformer &beamformer() const { return beamformer_; }

    PipelineConfig config;

 private:
    void init_states_();
    void apply_reference_delay_(int16_t *ref, size_t frames);
    uint8_t processed_channels_() const;

    uint8_t channels_ = 1;
    static constexpr int kMaxStates = esphome::aec_speexdsp::AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES;
    SpeexEchoState *echo_state_[kMaxStates]{};
    SpeexPreprocessState *preprocess_state_[kMaxStates]{};
    uint8_t processed_slots_[kMaxStates]{0, 1, 2, 3};
    esphome::aec_speexdsp::AdaptiveDelayAndSumBeamformer beamformer_;

    std::vector<int16_t> ref_delay_history_;
    size_t ref_delay_pos_ = 0;
};

// Scenario data loaded from the generated test_data directory.
struct Scenario {
    std::string name;
    std::string description;
    uint32_t sample_rate = kSampleRate;
    size_t frames = 0;                       // total samples per track
    std::vector<int16_t> reference;          // far-end / playback reference (mono)
    std::vector<int16_t> mic0;               // microphone 1
    std::vector<int16_t> mic1;               // microphone 2 (empty if single-mic scenario)
    std::vector<int16_t> near_clean;         // clean near-end speech (0 when absent)
    std::vector<int16_t> echo_only_copy;     // ground-truth echo component in mic0
    std::vector<float> speech_labels;        // 1.0 per sample where near speech active
    std::vector<float> echo_labels;          // 1.0 per sample where echo present (far-end active)
    // Ground truth for beamforming scenarios:
    std::vector<double> mic_delays;          // injected TDOA per mic, in samples (>=0)
    double interference_delay = 0.0;         // injected TDOA of interference vs mic0
};

// Loads scenarios/manifest.json from the given directory.
std::vector<Scenario> load_scenarios(const std::string &data_dir);

// ---- Metric helpers (shared by suites) ----
double rms_db(const int16_t *x, size_t n);                       // dBFS RMS
double erle_db(const int16_t *mic, const int16_t *out, size_t n);
double attenuation_db(const int16_t *a, const int16_t *b, size_t n);  // 20log10(rms_b / rms_a)
double correlation(const int16_t *a, const int16_t *b, size_t n);
double segmental_snr_db(const int16_t *clean, const int16_t *test, size_t n, size_t segment);
double max_abs_error(const int16_t *a, const int16_t *b, size_t n);

// Fractional delay of x by `delay` samples (>=0, can be fractional) with
// linear interpolation; used to synthesise multi-mic signals.
std::vector<int16_t> delay_signal(const std::vector<int16_t> &x, double delay);

}  // namespace dsp
