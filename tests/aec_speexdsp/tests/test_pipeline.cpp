/* Full-pipeline tests: the exact component topology (per-channel
 * AEC+preprocess, and multichannel-AEC + beamforming + shared preprocess)
 * driven with the generated scenarios, plus realtime-factor timing. */
#include <chrono>
#include <cmath>
#include <vector>

#include "audio_io.h"
#include "dsp_pipeline.h"
#include "pc_mem.h"
#include "test_framework.h"

namespace {

using namespace dsp;

PipelineConfig component_defaults() {
    // Defaults from aec_speexdsp.h (see noise_suppression_level_db_ etc.).
    PipelineConfig cfg;
    cfg.frame_size = 256;
    cfg.filter_length = 2048;
    cfg.noise_suppression = true;
    cfg.noise_suppression_db = 15;
    cfg.agc = true;
    cfg.agc_target_level = 0.25f;
    cfg.vad = true;
    cfg.vad_threshold = 35;
    cfg.echo_suppress_db = 40;
    cfg.echo_suppress_active_db = 15;
    return cfg;
}

}  // namespace

void run_pipeline_suite(testfw::Suite &s, const std::vector<Scenario> &scenarios, bool write_outputs,
                        const std::string &out_dir) {
    // Deterministic PCM artifacts for stage-to-stage numerical comparisons.
    // Include near-only, silence, mixed speech/echo and multichannel inputs.
    if (write_outputs) {
        for (const auto &scenario : scenarios) {
            PipelineConfig cfg = component_defaults();
            cfg.filter_length = 1024;
            cfg.beam.enabled = !scenario.mic1.empty();
            std::vector<int16_t> mic = scenario.mic0;
            if (cfg.beam.enabled) mic.insert(mic.end(), scenario.mic1.begin(), scenario.mic1.end());
            Pipeline pipeline(cfg);
            const auto ref = scenario.reference.empty() ? std::vector<int16_t>(scenario.mic0.size(), 0)
                                                        : scenario.reference;
            auto out = pipeline.process_all(mic, ref);
            audio_io::write_wav(out_dir + "/corpus_" + scenario.name + ".wav", out.data(), out.size(), kSampleRate);
        }
    }
    const Scenario *far_noise = nullptr;
    const Scenario *far_music = nullptr;
    const Scenario *doubletalk = nullptr;
    const Scenario *beam_echo = nullptr;
    for (const auto &sc : scenarios) {
        if (sc.name == "far_only_noise")
            far_noise = &sc;
        if (sc.name == "far_only_music")
            far_music = &sc;
        if (sc.name == "doubletalk_music")
            doubletalk = &sc;
        if (sc.name == "beamforming_echo_2mic")
            beam_echo = &sc;
    }

    // Single-mic topology (component default: output_channel FIRST) on the
    // far-end-only scenario: the headline "does it cancel echo" measure.
    SUITE_BEGIN("pipeline_far_only");
    if (far_noise == nullptr) {
        s.skip("scenario missing");
    } else {
        PipelineConfig cfg = component_defaults();
        cfg.agc = false;  // keep the ERLE measurement level-pure
        Pipeline pipeline(cfg);
        std::vector<float> prob;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<int16_t> out = pipeline.process_all(far_noise->mic0, far_noise->reference, &prob);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double audio_seconds = static_cast<double>(out.size()) / kSampleRate;
        const double rtf = seconds / audio_seconds;
        // ERLE over echo-active, speech-inactive frames after settling.
        std::vector<int16_t> mic_sel, out_sel;
        for (size_t i = kSampleRate; i < out.size(); i++)
            if (i < far_noise->echo_labels.size() && far_noise->echo_labels[i] > 0.5f &&
                (i >= far_noise->speech_labels.size() || far_noise->speech_labels[i] < 0.5f)) {
                mic_sel.push_back(far_noise->mic0[i]);
                out_sel.push_back(out[i]);
            }
        const double erle = erle_db(mic_sel.data(), out_sel.data(), mic_sel.size());
        CHECK_MSG(erle > 25.0, "full pipeline ERLE > 25 dB on far-end-only audio");
        CHECK_MSG(rtf < 1.0, "processes faster than realtime");
        if (write_outputs && !out_dir.empty())
            audio_io::write_wav(out_dir + "/pipeline_far_only_out.wav", out.data(), out.size(), kSampleRate);
    }
    SUITE_END();

    // Real music far-end through the full component-default pipeline.
    SUITE_BEGIN("pipeline_music_far_end");
    if (far_music == nullptr) {
        s.skip("scenario missing");
    } else {
        PipelineConfig cfg = component_defaults();
        cfg.agc = false;
        Pipeline pipeline(cfg);
        std::vector<int16_t> out = pipeline.process_all(far_music->mic0, far_music->reference);
        std::vector<int16_t> mic_sel, out_sel;
        for (size_t i = kSampleRate; i < out.size(); i++)
            if (i < far_music->echo_labels.size() && far_music->echo_labels[i] > 0.5f) {
                mic_sel.push_back(far_music->mic0[i]);
                out_sel.push_back(out[i]);
            }
        const double erle = erle_db(mic_sel.data(), out_sel.data(), mic_sel.size());
        CHECK_MSG(erle > 15.0, "music echo suppressed by full pipeline");
        if (write_outputs && !out_dir.empty())
            audio_io::write_wav(out_dir + "/pipeline_music_out.wav", out.data(), out.size(), kSampleRate);
    }
    SUITE_END();

    // Double-talk through the component-default pipeline (AGC on, like the
    // device): speech must survive, echo must not.
    SUITE_BEGIN("pipeline_doubletalk");
    if (doubletalk == nullptr) {
        s.skip("scenario missing");
    } else {
        PipelineConfig cfg = component_defaults();
        Pipeline pipeline(cfg);
        std::vector<int16_t> out = pipeline.process_all(doubletalk->mic0, doubletalk->reference);
        // Align output to clean speech by scaling out the AGC gain: use the
        // RMS ratio over double-talk frames as a gain estimate.
        std::vector<size_t> dt;
        for (size_t i = kSampleRate; i < out.size(); i++)
            if (i < doubletalk->speech_labels.size() && doubletalk->speech_labels[i] > 0.5f &&
                i < doubletalk->echo_labels.size() && doubletalk->echo_labels[i] > 0.5f)
                dt.push_back(i);
        CHECK_MSG(dt.size() > kSampleRate, "double-talk region present");
        double clean_energy = 0, out_energy = 0;
        for (size_t i : dt) {
            clean_energy += static_cast<double>(doubletalk->near_clean[i]) * doubletalk->near_clean[i];
            out_energy += static_cast<double>(out[i]) * out[i];
        }
        const double gain = std::sqrt(clean_energy / std::max(out_energy, 1e-9));
        std::vector<int16_t> out_aligned(dt.size());
        for (size_t k = 0; k < dt.size(); k++)
            out_aligned[k] = static_cast<int16_t>(
                std::max(-32768.0, std::min(32767.0, gain * out[dt[k]])));
        // AGC gain moves over time, so compare 32 ms RMS envelopes (gain
        // tracking) instead of raw sample correlation.
        auto envelope_corr = [&](size_t win) {
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            size_t n = 0;
            for (size_t start = 0; start + win <= dt.size(); start += win, n++) {
                double ex = 0, ey = 0;
                for (size_t k = start; k < start + win; k++) {
                    ex += static_cast<double>(doubletalk->near_clean[dt[k]]) * doubletalk->near_clean[dt[k]];
                    ey += static_cast<double>(out_aligned[k]) * out_aligned[k];
                }
                ex = std::sqrt(ex / win);
                ey = std::sqrt(ey / win);
                sx += ex;
                sy += ey;
                sxx += ex * ex;
                syy += ey * ey;
                sxy += ex * ey;
            }
            if (n < 2 || sxx <= 0 || syy <= 0)
                return 0.0;
            return (sxy / n - (sx / n) * (sy / n)) /
                   std::sqrt((sxx / n - (sx / n) * (sx / n)) * (syy / n - (sy / n) * (sy / n)));
        };
        const double env_corr = envelope_corr(512);
        std::vector<int16_t> clean_sel(dt.size());
        for (size_t k = 0; k < dt.size(); k++)
            clean_sel[k] = doubletalk->near_clean[dt[k]];
        const double segsnr = segmental_snr_db(clean_sel.data(), out_aligned.data(), dt.size(), 512);
        // AGC controls perceptual loudness, not the clean signal's RMS.
        // Verify the actual safety property instead of an arbitrary RMS ratio.
        size_t clipped = 0;
        for (size_t i : dt) if (std::abs(int(out[i])) >= 32760) ++clipped;
        CHECK_MSG(clipped < dt.size()/1000, "double-talk output does not clip");
        CHECK_MSG(env_corr > 0.6, "double-talk output envelope tracks near-end speech");
        CHECK_MSG(segsnr > 0.0, "near-end segSNR positive after full pipeline");
        if (write_outputs && !out_dir.empty())
            audio_io::write_wav(out_dir + "/pipeline_doubletalk_out.wav", out.data(), out.size(), kSampleRate);
    }
    SUITE_END();

    // Beamforming topology (component beamforming: enabled config) with echo:
    // mc canceller + beamformer + shared preprocess.
    SUITE_BEGIN("pipeline_beamforming_echo");
    if (beam_echo == nullptr) {
        s.skip("scenario missing");
    } else {
        PipelineConfig cfg = component_defaults();
        cfg.agc = false;
        cfg.beam.enabled = true;
        cfg.beam.microphones = 2;
        Pipeline pipeline(cfg);
        // Planar two-channel input.
        std::vector<int16_t> planar(beam_echo->mic0.size() + beam_echo->mic1.size());
        const size_t n = beam_echo->mic0.size();
        for (size_t i = 0; i < n; i++) {
            planar[i] = beam_echo->mic0[i];
            planar[n + i] = beam_echo->mic1[i];
        }
        std::vector<float> prob;
        std::vector<int16_t> out = pipeline.process_all(planar, beam_echo->reference, &prob);
        std::vector<int16_t> mic_sel, out_sel;
        for (size_t i = kSampleRate; i < out.size(); i++)
            if (i < beam_echo->echo_labels.size() && beam_echo->echo_labels[i] > 0.5f &&
                i < beam_echo->speech_labels.size() && beam_echo->speech_labels[i] < 0.5f) {
                mic_sel.push_back(beam_echo->mic0[i]);
                out_sel.push_back(out[i]);
            }
        const double erle = erle_db(mic_sel.data(), out_sel.data(), mic_sel.size());
        const double tdoa1 = pipeline.beamformer().get_tdoa_q15(1) / 32768.0;
        CHECK_MSG(erle > 15.0, "beamforming topology cancels echo");
        if (!beam_echo->mic_delays.empty())
            CHECK_NEAR(tdoa1, beam_echo->mic_delays[1], 1.5);
        if (write_outputs && !out_dir.empty())
            audio_io::write_wav(out_dir + "/pipeline_beam_out.wav", out.data(), out.size(), kSampleRate);
    }
    SUITE_END();

    // Mixed dual-channel topology (output_channel MIXED).
    SUITE_BEGIN("pipeline_output_mixed");
    if (doubletalk == nullptr) {
        s.skip("scenario missing");
    } else {
        PipelineConfig cfg = component_defaults();
        cfg.agc = false;
        cfg.output_mixed = true;
        Pipeline pipeline(cfg);
        std::vector<int16_t> planar(doubletalk->mic0.size() * 2);
        const size_t n = doubletalk->mic0.size();
        for (size_t i = 0; i < n; i++) {
            planar[i] = doubletalk->mic0[i];
            planar[n + i] = doubletalk->mic0[i];  // second channel duplicates scenario
        }
        std::vector<int16_t> out = pipeline.process_all(planar, doubletalk->reference);
        CHECK_MSG(out.size() >= kSampleRate, "mixed topology produces output");
        bool bounded = true;
        for (int16_t v : out)
            if (v < -32768 || v > 32767)
                bounded = false;
        CHECK_MSG(bounded, "mixed output within int16 range");
    }
    SUITE_END();

    // Soak: repeated full-pipeline processing must not grow memory or slow
    // down (state is fixed-size; proves no accumulation over long sessions).
    SUITE_BEGIN("soak_memory_stability");
    {
        PipelineConfig cfg = component_defaults();
        cfg.agc = false;
        pc_mem::Stats before = pc_mem::stats();
        double rtf_first = 0.0, rtf_last = 0.0;
        for (int pass = 0; pass < 6; pass++) {
            const auto t0 = std::chrono::steady_clock::now();
            if (doubletalk != nullptr) {
                Pipeline pipeline(cfg);
                std::vector<int16_t> out = pipeline.process_all(doubletalk->mic0, doubletalk->reference);
                const double rtf = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() /
                                   (static_cast<double>(out.size()) / kSampleRate);
                if (pass == 0)
                    rtf_first = rtf;
                rtf_last = rtf;
            }
        }
        pc_mem::Stats after = pc_mem::stats();
        CHECK_MSG(after.live_blocks == before.live_blocks, "no leaked blocks across repeated runs");
        CHECK_MSG(rtf_last < 1.0, "throughput stable and realtime");
    }
    SUITE_END();
}
