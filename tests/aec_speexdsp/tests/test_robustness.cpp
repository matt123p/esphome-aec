/* Robustness: hostile inputs and unusual configurations through the full
 * pipeline. Nothing here may crash, hang, or produce non-finite output. */
#include <cmath>
#include <random>
#include <vector>

#include "dsp_pipeline.h"
#include "pc_mem.h"
#include "test_framework.h"

namespace {

using namespace dsp;

PipelineConfig component_defaults() {
    PipelineConfig cfg;
    return cfg;
}

double peak_abs(const std::vector<int16_t> &x) {
    double m = 0;
    for (int16_t v : x)
        m = std::max(m, static_cast<double>(std::abs(v)));
    return m;
}

}  // namespace

void run_robustness_suite(testfw::Suite &s) {
    // Worst-case waveforms through the complete component-default pipeline.
    SUITE_BEGIN("hostile_inputs");
    {
        const size_t n = kSampleRate * 2;
        std::mt19937 rng(901);
        std::uniform_int_distribution<int> coin(0, 1);

        std::vector<std::pair<std::string, std::vector<int16_t>>> inputs;
        inputs.emplace_back("silence", std::vector<int16_t>(n, 0));
        {
            std::vector<int16_t> sq(n);
            for (size_t i = 0; i < n; i++)
                sq[i] = (i / 32) % 2 == 0 ? 32767 : -32768;
            inputs.emplace_back("square_fullscale", std::move(sq));
        }
        {
            std::vector<int16_t> sn(n);
            for (size_t i = 0; i < n; i++)
                sn[i] = static_cast<int16_t>(32767.0 * std::sin(2 * M_PI * 7000.0 * i / kSampleRate));
            inputs.emplace_back("sine_7khz_fullscale", std::move(sn));
        }
        {
            std::vector<int16_t> dc(n, 32000);
            inputs.emplace_back("dc_positive", std::move(dc));
            std::vector<int16_t> dc2(n, -32768);
            inputs.emplace_back("dc_negative", std::move(dc2));
        }
        {
            std::vector<int16_t> click(n, 0);
            for (size_t i = 0; i < n; i += 1000)
                click[i] = i % 2000 == 0 ? 32767 : -32768;
            inputs.emplace_back("impulse_train", std::move(click));
        }
        {
            std::vector<int16_t> rz(n);
            for (auto &v : rz)
                v = static_cast<int16_t>(coin(rng) ? 32767 : -32768);
            inputs.emplace_back("random_square_fullscale", std::move(rz));
        }

        PipelineConfig cfg = component_defaults();
        for (const auto &entry : inputs) {
            // far-end drives the reference; mic = echo + the hostile input.
            std::vector<int16_t> ref(n);
            for (size_t i = 0; i < n; i++)
                ref[i] = static_cast<int16_t>(18000.0 * std::sin(2 * M_PI * 997.0 * i / kSampleRate));
            std::vector<int16_t> mic(n);
            for (size_t i = 0; i < n; i++)
                mic[i] = static_cast<int16_t>(
                    std::max(-32768.0, std::min(32767.0, 0.5 * ref[i] + entry.second[i])));
            Pipeline pipeline(cfg);
            std::vector<int16_t> out = pipeline.process_all(mic, ref);
            CHECK_MSG(out.size() == (n / 256) * 256, "full-length output produced");
            bool finite = true;
            for (int16_t v : out)
                if (!std::isfinite(v))
                    finite = false;
            CHECK_MSG(finite, "output finite for " + entry.first);
        }
        CHECK_MSG(true, "no crashes across hostile inputs");
    }
    SUITE_END();

    // Configuration sweep: every combination must init, process and destroy
    // cleanly with zero leaked blocks.
    SUITE_BEGIN("configuration_sweep");
    {
        const int frames_cfgs[][2] = {{128, 256},  {128, 1024}, {256, 256},  {256, 512},
                                      {256, 2048}, {256, 4096}, {512, 512},  {512, 2048},
                                      {1024, 1024}, {1024, 8192}};
        for (const auto &fc : frames_cfgs) {
            PipelineConfig cfg = component_defaults();
            cfg.frame_size = fc[0];
            cfg.filter_length = fc[1];
            const size_t n = 16000;  // 1 s
            std::vector<int16_t> mic(n), ref(n);
            std::mt19937 rng(1000u + fc[0] + fc[1]);
            std::normal_distribution<double> gauss(0.0, 1.0);
            for (size_t i = 0; i < n; i++) {
                ref[i] = static_cast<int16_t>(gauss(rng) * 5000);
                mic[i] = static_cast<int16_t>(gauss(rng) * 8000);
            }
            pc_mem::Stats before = pc_mem::stats();
            {
                Pipeline pipeline(cfg);
                std::vector<int16_t> out = pipeline.process_all(mic, ref);
                CHECK_MSG(out.size() > 0, "output produced");
            }
            pc_mem::Stats after = pc_mem::stats();
            if (after.live_blocks != before.live_blocks)
                s.fail("leaked blocks for frame=" + std::to_string(fc[0]) + " filter=" + std::to_string(fc[1]) +
                       "\n" + pc_mem::leak_report());
        }
        CHECK_MSG(true, "all frame/filter combinations leak-free");
    }
    SUITE_END();

    // Multichannel state sizes: nb_mic 1..4, nb_speakers 1..2.
    // Contract note: with nb_speakers=K the MDF consumes the reference as
    // K-interleaved frames (mdf.c reads far_end[i*K+speak]); the component
    // always uses nb_speakers=1 with a mono reference.
    SUITE_BEGIN("multichannel_sizes");
    {
        for (int nb_mic = 1; nb_mic <= 4; nb_mic++) {
            for (int nb_spk = 1; nb_spk <= 2; nb_spk++) {
                SpeexEchoState *st = speex_echo_state_init_mc(256, 2048, nb_mic, nb_spk);
                CHECK_MSG(st != nullptr, "mc state allocated");
                if (st == nullptr)
                    continue;
                int rate = 16000;
                speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
                std::vector<int16_t> interleaved(256 * nb_mic, 100), out(256 * nb_mic, 0);
                std::vector<int16_t> ref(256 * nb_spk, 50);  // K-interleaved
                for (int f = 0; f < 20; f++)
                    speex_echo_cancellation(st, interleaved.data(), ref.data(), out.data());
                speex_echo_state_destroy(st);
            }
        }
        CHECK_MSG(true, "all mc configurations survive 20 frames");
    }
    SUITE_END();

    // Unknown ctl requests report failure rather than misbehaving.
    SUITE_BEGIN("unknown_ctl_requests");
    {
        SpeexEchoState *echo = speex_echo_state_init(256, 2048);
        SpeexPreprocessState *pre = speex_preprocess_state_init(256, 16000);
        int dummy = 0;
        CHECK_MSG(speex_echo_ctl(echo, 9999, &dummy) == -1, "echo ctl rejects unknown request");
        CHECK_MSG(speex_preprocess_ctl(pre, 9999, &dummy) == -1, "preprocess ctl rejects unknown request");
        speex_echo_state_destroy(echo);
        speex_preprocess_state_destroy(pre);
    }
    SUITE_END();

    // Preprocessor ctl ranges accepted by the component's YAML schema.
    SUITE_BEGIN("ctl_ranges");
    {
        for (int ns_db : {5, 15, 30, 60}) {
            for (int vad_thr : {10, 35, 90}) {
                SpeexPreprocessState *pre = speex_preprocess_state_init(256, 16000);
                int denoise = 1;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
                int ndb = ns_db;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ndb);
                int vad = 1;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_VAD, &vad);
                int thr = vad_thr;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_PROB_START, &thr);
                std::vector<int16_t> frame(256, 5000);
                for (int f = 0; f < 10; f++)
                    speex_preprocess_run(pre, frame.data());
                speex_preprocess_state_destroy(pre);
            }
        }
        CHECK_MSG(true, "all NS/VAD ranges survive processing");
    }
    SUITE_END();

    // Rapid reconfiguration churn: the ESP component re-creates states when
    // settings change (start_dsp_/destroy_dsp_); stress that cycle.
    SUITE_BEGIN("state_churn");
    {
        pc_mem::Stats before = pc_mem::stats();
        std::mt19937 rng(909);
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::vector<int16_t> mic(256 * 40), ref(256 * 40);
        for (size_t i = 0; i < mic.size(); i++) {
            mic[i] = static_cast<int16_t>(gauss(rng) * 6000);
            ref[i] = static_cast<int16_t>(gauss(rng) * 4000);
        }
        for (int cycle = 0; cycle < 60; cycle++) {
            PipelineConfig cfg = component_defaults();
            cfg.frame_size = 256;
            cfg.filter_length = 2048;
            cfg.beam.enabled = cycle % 2 == 1;
            cfg.beam.microphones = 2;
            Pipeline pipeline(cfg);
            // Planar multi-channel input when beamforming is enabled.
            std::vector<int16_t> planar;
            const uint8_t chans = pipeline.channels();
            if (chans > 1) {
                planar.resize(mic.size() * chans);
                for (size_t i = 0; i < mic.size(); i++)
                    for (uint8_t c = 0; c < chans; c++)
                        planar[i * chans + c] = mic[i];
                pipeline.process_all(planar, ref);
            } else {
                pipeline.process_all(mic, ref);
            }
        }
        pc_mem::Stats after = pc_mem::stats();
        CHECK_MSG(after.live_blocks == before.live_blocks, "60 reconfiguration cycles leak-free");
    }
    SUITE_END();
}
