#include "dsp_pipeline.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include "audio_io.h"
#include "json.h"

namespace dsp {

using esphome::aec_speexdsp::AdaptiveDelayAndSumBeamformer;

Pipeline::Pipeline(const PipelineConfig &cfg) : config(cfg) {
    init_states_();
}

uint8_t Pipeline::processed_channels_() const {
    if (config.beam.enabled)
        return config.beam.microphones;
    return config.output_mixed ? 2 : 1;
}

void Pipeline::init_states_() {
    channels_ = processed_channels_();
    const int frame = config.frame_size;
    const int filter = config.filter_length;

    // Slot mapping: a single state per selected channel; with beamforming the
    // primary state is the shared multichannel canceller (start_dsp_).
    const uint8_t echo_states = config.beam.enabled ? 1 : channels_;
    for (uint8_t c = 0; c < echo_states; c++) {
        echo_state_[c] = config.beam.enabled ? speex_echo_state_init_mc(frame, filter, channels_, 1)
                                            : speex_echo_state_init(frame, filter);
        assert(echo_state_[c] != nullptr);
        int rate = kSampleRate;
        speex_echo_ctl(echo_state_[c], SPEEX_ECHO_SET_SAMPLING_RATE, &rate);

        preprocess_state_[c] = speex_preprocess_state_init(frame, kSampleRate);
        assert(preprocess_state_[c] != nullptr);

        int denoise = config.noise_suppression ? 1 : 0;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        int noise_db = config.noise_suppression_db;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise_db);

        int agc = config.agc ? 1 : 0;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC, &agc);
        if (config.agc) {
            float level = config.agc_target_level * 32768.0f;
            speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
        }

        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_STATE, echo_state_[c]);
        int echo_db = config.echo_suppress_db;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_db);
        int echo_active_db = config.echo_suppress_active_db;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echo_active_db);

        int vad = config.vad ? 1 : 0;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_VAD, &vad);
        int prob_start = config.vad_threshold;
        speex_preprocess_ctl(preprocess_state_[c], SPEEX_PREPROCESS_SET_PROB_START, &prob_start);
    }

    if (config.beam.enabled) {
        beamformer_.configure(config.beam.microphones, config.beam.max_lag, config.beam.update_frames,
                              config.beam.min_rms, config.beam.min_correlation_percent,
                              config.beam.min_peak_dominance_percent);
    }

    if (config.reference_delay_samples > 0) {
        const size_t delay = std::min<size_t>(config.reference_delay_samples,
                                              AdaptiveDelayAndSumBeamformer::MAX_LAG * 32);
        ref_delay_history_.assign(delay + 1, 0);
        ref_delay_pos_ = 0;
    }
}

Pipeline::~Pipeline() {
    for (int c = 0; c < kMaxStates; c++) {
        if (echo_state_[c] != nullptr) {
            speex_echo_state_destroy(echo_state_[c]);
            echo_state_[c] = nullptr;
        }
        if (preprocess_state_[c] != nullptr) {
            speex_preprocess_state_destroy(preprocess_state_[c]);
            preprocess_state_[c] = nullptr;
        }
    }
}

void Pipeline::apply_reference_delay_(int16_t *ref, size_t frames) {
    if (ref_delay_history_.empty())
        return;
    const size_t delay = ref_delay_history_.size() - 1;
    for (size_t n = 0; n < frames; n++) {
        ref_delay_history_[ref_delay_pos_] = ref[n];
        ref_delay_pos_ = (ref_delay_pos_ + 1) % ref_delay_history_.size();
        // Sample written `delay` iterations ago.
        const size_t oldest = (ref_delay_pos_ + ref_delay_history_.size() - 1 - delay) % ref_delay_history_.size();
        ref[n] = ref_delay_history_[oldest];
    }
}

FrameResult Pipeline::process(const int16_t *mic, const int16_t *ref_in) {
    const size_t frame = static_cast<size_t>(config.frame_size);
    FrameResult result;
    result.output.assign(frame, 0);

    std::vector<int16_t> ref(ref_in, ref_in + frame);
    apply_reference_delay_(ref.data(), frame);

    if (config.beam.enabled) {
        // Interleave for the multichannel canceller, cancel, transpose the
        // residuals back to planar, beamform, then one shared preprocessor.
        std::vector<int16_t> interleaved(frame * channels_);
        std::vector<int16_t> residual_interleaved(frame * channels_);
        for (size_t n = 0; n < frame; n++)
            for (uint8_t c = 0; c < channels_; c++)
                interleaved[n * channels_ + c] = mic[c * frame + n];
        speex_echo_cancellation(echo_state_[0], interleaved.data(), ref.data(), residual_interleaved.data());
        const int16_t *beam_inputs[AdaptiveDelayAndSumBeamformer::MAX_MICROPHONES]{};
        for (uint8_t c = 0; c < channels_; c++)
            beam_inputs[c] = residual_interleaved.data() + c;
        beamformer_.process(beam_inputs, frame, result.output.data(), channels_);
        result.vad_state = speex_preprocess_run(preprocess_state_[0], result.output.data()) != 0;
    } else {
        std::vector<int16_t> processed(frame * channels_);
        for (uint8_t c = 0; c < channels_; c++) {
            int16_t *channel_out = processed.data() + c * frame;
            speex_echo_cancellation(echo_state_[c], mic + c * frame, ref.data(), channel_out);
            const int detected = speex_preprocess_run(preprocess_state_[c], channel_out);
            if (c == 0) result.vad_state = detected != 0;
        }
        if (channels_ == 2) {
            for (size_t n = 0; n < frame; n++)
                result.output[n] = static_cast<int16_t>((processed[n] + processed[frame + n]) / 2);
        } else {
            std::memcpy(result.output.data(), processed.data(), frame * sizeof(int16_t));
        }
    }

    // Component: VAD state and probability from the primary preprocessor.
    // GET_VAD reports whether detection is enabled, not its decision.
    result.vad_state = config.vad && result.vad_state;
    spx_int32_t prob = 0;
    speex_preprocess_ctl(preprocess_state_[0], SPEEX_PREPROCESS_GET_PROB, &prob);
    result.vad_probability = static_cast<float>(prob) / 100.0f;
    return result;
}

std::vector<int16_t> Pipeline::process_all(const std::vector<int16_t> &mic, const std::vector<int16_t> &ref,
                                           std::vector<float> *vad_prob) {
    const size_t frame = static_cast<size_t>(config.frame_size);
    assert(mic.size() >= channels_ * frame);
    assert(ref.size() >= frame);
    std::vector<int16_t> out;
    out.reserve(ref.size());
    if (vad_prob != nullptr)
        vad_prob->clear();
    std::vector<int16_t> block(frame * channels_);
    for (size_t offset = 0; offset + frame <= ref.size(); offset += frame) {
        for (uint8_t c=0; c<channels_; ++c)
            std::copy_n(mic.data() + c * ref.size() + offset, frame, block.data() + c * frame);
        FrameResult r = process(block.data(), ref.data() + offset);
        out.insert(out.end(), r.output.begin(), r.output.end());
        if (vad_prob != nullptr)
            vad_prob->push_back(r.vad_probability);
    }
    // Remove the preprocessor's one-frame WOLA latency, flushing the tail.
    std::fill(block.begin(), block.end(), 0);
    std::vector<int16_t> zero_ref(frame, 0);
    FrameResult tail = process(block.data(), zero_ref.data());
    out.insert(out.end(), tail.output.begin(), tail.output.end());
    out.erase(out.begin(), out.begin() + frame);
    return out;
}

// ---- Scenario loading ----

namespace {

std::string parent_dir(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

void read_track(const std::string &dir, const json::Value &entry, const char *key, std::vector<int16_t> &out) {
    if (!entry.has(key) || entry.at(key).is_null())
        return;
    uint32_t rate = 0;
    uint16_t channels = 0;
    audio_io::read_wav(dir + "/" + entry.at(key).str(), out, rate, channels);
}

}  // namespace

std::vector<Scenario> load_scenarios(const std::string &data_dir) {
    auto root = json::load(data_dir + "/manifest.json");
    std::vector<Scenario> scenarios;
    if (root == nullptr || !root->has("scenarios"))
        return scenarios;
    for (const auto &entry_ptr : root->at("scenarios").array) {
        const json::Value &e = *entry_ptr;
        Scenario s;
        s.name = e.at("name").str();
        s.description = e.at("description").str();
        s.sample_rate = static_cast<uint32_t>(e.at("sample_rate").num(kSampleRate));
        s.frames = static_cast<size_t>(e.at("samples").num());
        const std::string dir = parent_dir(data_dir + "/manifest.json");
        read_track(dir, e, "reference", s.reference);
        read_track(dir, e, "mic0", s.mic0);
        read_track(dir, e, "mic1", s.mic1);
        read_track(dir, e, "near_clean", s.near_clean);
        read_track(dir, e, "echo_copy", s.echo_only_copy);
        auto read_labels = [&](const char *key, std::vector<float> &out) {
            if (!e.has(key) || e.at(key).is_null())
                return;
            audio_io::read_f32(dir + "/" + e.at(key).str(), out);
        };
        read_labels("speech_labels", s.speech_labels);
        read_labels("echo_labels", s.echo_labels);
        if (e.has("mic_delays"))
            for (const auto &d : e.at("mic_delays").array)
                s.mic_delays.push_back(d->num());
        if (e.has("interference_delay"))
            s.interference_delay = e.at("interference_delay").num();
        scenarios.push_back(std::move(s));
    }
    return scenarios;
}

// ---- Metrics ----

double rms_db(const int16_t *x, size_t n) {
    if (n == 0)
        return -120.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += static_cast<double>(x[i]) * x[i];
    const double rms = std::sqrt(sum / n);
    return 20.0 * std::log10(std::max(rms, 1e-9) / 32768.0);
}

double erle_db(const int16_t *mic, const int16_t *out, size_t n) {
    double mic_energy = 0.0, out_energy = 0.0;
    for (size_t i = 0; i < n; i++) {
        mic_energy += static_cast<double>(mic[i]) * mic[i];
        out_energy += static_cast<double>(out[i]) * out[i];
    }
    if (mic_energy < 1e-9 || out_energy < 1e-9)
        return 0.0;
    return 10.0 * std::log10(mic_energy / out_energy);
}

double attenuation_db(const int16_t *a, const int16_t *b, size_t n) {
    double ea = 0.0, eb = 0.0;
    for (size_t i = 0; i < n; i++) {
        ea += static_cast<double>(a[i]) * a[i];
        eb += static_cast<double>(b[i]) * b[i];
    }
    if (ea < 1e-9 || eb < 1e-9)
        return 0.0;
    return 10.0 * std::log10(eb / ea);
}

double correlation(const int16_t *a, const int16_t *b, size_t n) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (size_t i = 0; i < n; i++) {
        sa += a[i];
        sb += b[i];
    }
    const double ma = sa / n, mb = sb / n;
    for (size_t i = 0; i < n; i++) {
        const double da = a[i] - ma, db = b[i] - mb;
        saa += da * da;
        sbb += db * db;
        sab += da * db;
    }
    if (saa < 1e-9 || sbb < 1e-9)
        return 0.0;
    return sab / std::sqrt(saa * sbb);
}

double segmental_snr_db(const int16_t *clean, const int16_t *test, size_t n, size_t segment) {
    if (segment == 0 || n < segment)
        return -120.0;
    double sum_snr = 0.0;
    size_t count = 0;
    for (size_t start = 0; start + segment <= n; start += segment) {
        double noise = 0.0;
        double peak = 0.0;
        for (size_t i = 0; i < segment; i++) {
            const double d = static_cast<double>(clean[start + i]) - test[start + i];
            noise += d * d;
            peak = std::max(peak, std::abs(static_cast<double>(clean[start + i])));
        }
        // Only score active segments (per ITU P.561 convention of -15 dBFS active)
        if (peak < 32768.0 * std::pow(10.0, -40.0 / 20.0))
            continue;
        double sig = 0.0;
        for (size_t i = 0; i < segment; i++)
            sig += static_cast<double>(clean[start + i]) * clean[start + i];
        if (sig < 1e-9)
            continue;
        sum_snr += 10.0 * std::log10(sig / std::max(noise, 1e-9));
        count++;
    }
    return count > 0 ? sum_snr / count : -120.0;
}

double max_abs_error(const int16_t *a, const int16_t *b, size_t n) {
    double m = 0.0;
    for (size_t i = 0; i < n; i++)
        m = std::max(m, std::abs(static_cast<double>(a[i]) - b[i]));
    return m;
}

std::vector<int16_t> delay_signal(const std::vector<int16_t> &x, double delay) {
    // Fractional delay with linear interpolation. Negative delays shift the
    // signal earlier (leading zeros are dropped, trailing zero-padded).
    std::vector<int16_t> y(x.size(), 0);
    if (std::fabs(delay) < 1e-9)
        return x;
    const double magnitude = std::fabs(delay);
    const size_t integer = static_cast<size_t>(std::floor(magnitude));
    const double fraction = magnitude - integer;
    for (size_t n = 0; n < x.size(); n++) {
        double v = 0.0;
        if (delay > 0) {
            if (n < integer)
                continue;
            const double older = x[n - integer];
            const double newer = (n >= integer + 1 && fraction > 0.0) ? x[n - integer - 1] : older;
            v = fraction > 0.0 ? newer * fraction + older * (1.0 - fraction) : older;
        } else {
            const size_t src = n + integer;
            if (src >= x.size())
                continue;
            const double older = x[src];
            const double newer = (src + 1 < x.size() && fraction > 0.0) ? x[src + 1] : older;
            v = fraction > 0.0 ? older * (1.0 - fraction) + newer * fraction : older;
        }
        y[n] = static_cast<int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, v))));
    }
    return y;
}

}  // namespace dsp
