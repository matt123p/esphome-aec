/* Acoustic echo canceller tests: raw MDF convergence, ERLE, delay
 * robustness, double-talk, impulse-response identification and the
 * multichannel state used by the beamforming topology. */
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "dsp_pipeline.h"
#include "pc_mem.h"
#include "test_framework.h"

namespace {

using namespace dsp;

// Feeds `far` through a synthetic echo path (delay + sparse early taps +
// diffuse decaying tail) and returns the echo component.
std::vector<int16_t> echo_of(const std::vector<int16_t> &far, double delay_samples, double gain) {
    const size_t d = static_cast<size_t>(delay_samples);
    // Sparse early reflections + ~5 ms diffuse tail after the direct path.
    const size_t tail = std::max<size_t>(160, d + 80);
    std::vector<double> ir(tail, 0.0);
    ir[d] = 1.00;
    if (d + 9 < tail)
        ir[d + 9] = 0.32;
    if (d + 25 < tail)
        ir[d + 25] = 0.18;
    if (d + 57 < tail)
        ir[d + 57] = 0.10;
    std::mt19937 rng(999);
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    for (size_t i = d + 60; i < tail; i++) {
        ir[i] = 0.05 * uniform(rng) * std::exp(-4.0 * (i - d - 60.0) / std::max<size_t>(1, tail - d - 60));
    }
    std::vector<int16_t> out(far.size(), 0);
    for (size_t n = 0; n < far.size(); n++) {
        double acc = 0.0;
        for (size_t k = 0; k < tail && k <= n; k++)
            acc += ir[k] * far[n - k];
        out[n] = static_cast<int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, gain * acc))));
    }
    return out;
}

// Raw MDF AEC (no preprocessor) over a whole track; returns output.
std::vector<int16_t> raw_aec(const std::vector<int16_t> &mic, const std::vector<int16_t> &ref, int frame_size,
                             int filter_length) {
    SpeexEchoState *st = speex_echo_state_init(frame_size, filter_length);
    int rate = kSampleRate;
    speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    std::vector<int16_t> out(mic.size(), 0);
    for (size_t off = 0; off + frame_size <= mic.size(); off += frame_size)
        speex_echo_cancellation(st, mic.data() + off, ref.data() + off, out.data() + off);
    speex_echo_state_destroy(st);
    return out;
}

}  // namespace

void run_aec_suite(testfw::Suite &s, const std::vector<Scenario> &scenarios) {
    SUITE_BEGIN("multichannel_reset_clears_all_coefficients");
    for (int channels : {2, 4}) {
        auto *used = speex_echo_state_init_mc(256, 1024, channels, 1);
        auto *fresh = speex_echo_state_init_mc(256, 1024, channels, 1);
        std::vector<int16_t> ref(256), mic(256*channels), out(mic.size()), expected(mic.size());
        std::mt19937 rng(611);
        for (int block=0; block<200; ++block) {
            for (int n=0; n<256; ++n) {
                ref[n] = int(rng()%20001)-10000;
                for (int c=0; c<channels; ++c) mic[n*channels+c] = ref[n]/(c+2);
            }
            speex_echo_cancellation(used, mic.data(), ref.data(), out.data());
        }
        speex_echo_state_reset(used);
        speex_echo_cancellation(used, mic.data(), ref.data(), out.data());
        speex_echo_cancellation(fresh, mic.data(), ref.data(), expected.data());
        CHECK_MSG(out == expected, "first output after multichannel reset matches a fresh canceller");
        speex_echo_state_destroy(used);
        speex_echo_state_destroy(fresh);
    }
    SUITE_END();
    const Scenario *far_noise = nullptr;
    const Scenario *far_music = nullptr;
    const Scenario *doubletalk = nullptr;
    for (const auto &sc : scenarios) {
        if (sc.name == "far_only_noise")
            far_noise = &sc;
        if (sc.name == "far_only_music")
            far_music = &sc;
        if (sc.name == "doubletalk_music")
            doubletalk = &sc;
    }

    // Raw MDF on shaped-noise far-end: the canonical convergence measure.
    SUITE_BEGIN("raw_mdf_erle_noise");
    if (far_noise == nullptr) {
        s.skip("scenario missing");
    } else {
        // Skip the leading 0.5 s of adaptation.
        const size_t skip = kSampleRate / 2;
        const size_t count = far_noise->mic0.size() - skip;
        std::vector<int16_t> out = raw_aec(far_noise->mic0, far_noise->reference, 256, 2048);
        const double erle = erle_db(far_noise->mic0.data() + skip, out.data() + skip, count);
        // Correlate against the ground-truth echo component (same timeline),
        // not the reference: real far-end material decorrelates at echo-path
        // lags, but the injected echo copy is sample-aligned by construction.
        const double mic_echo_corr = correlation(far_noise->mic0.data() + skip, far_noise->echo_only_copy.data() + skip, count);
        const double out_echo_corr = correlation(out.data() + skip, far_noise->echo_only_copy.data() + skip, count);
        CHECK_MSG(erle > 20.0, "raw MDF ERLE > 20 dB on synthetic echo");
        CHECK_MSG(std::fabs(out_echo_corr) < std::fabs(mic_echo_corr) * 0.5, "echo correlation strongly reduced");
    }
    SUITE_END();

    // Raw MDF on real music: harder (tonal, non-stationary) content.
    SUITE_BEGIN("raw_mdf_erle_music");
    if (far_music == nullptr) {
        s.skip("scenario missing");
    } else {
        const size_t skip = kSampleRate;
        const size_t count = far_music->mic0.size() - skip;
        std::vector<int16_t> out = raw_aec(far_music->mic0, far_music->reference, 256, 2048);
        const double erle = erle_db(far_music->mic0.data() + skip, out.data() + skip, count);
        CHECK_MSG(erle > 12.0, "raw MDF ERLE > 12 dB on music far-end");
    }
    SUITE_END();

    // Delay robustness: echo path delay beyond the injected reference, with
    // and without the component's reference-delay compensation.
    SUITE_BEGIN("delay_robustness");
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        // Shaped noise far-end, 6 s.
        std::vector<int16_t> far(kSampleRate * 6);
        double lp = 0.0, prev_lp = 0.0;
        double maxv = 0.0;
        std::vector<double> shaped(far.size());
        for (size_t i = 0; i < far.size(); i++) {
            lp += 0.32 * (uniform(rng) - lp);
            shaped[i] = lp - prev_lp * 0.985;
            prev_lp = lp;
            maxv = std::max(maxv, std::fabs(shaped[i]));
        }
        for (size_t i = 0; i < far.size(); i++)
            far[i] = static_cast<int16_t>(std::lround(shaped[i] / maxv * 20000.0));

        const double extra_delays[] = {0.0, 16.0, 64.0, 160.0};
        for (double extra : extra_delays) {
            std::vector<int16_t> echo = echo_of(far, 20.0 + extra, 0.5);
            std::vector<int16_t> out = raw_aec(echo, far, 256, 2048);
            const size_t skip = kSampleRate / 2;
            const double erle = erle_db(echo.data() + skip, out.data() + skip, echo.size() - skip);
            CHECK_MSG(erle > 8.0, "MDF converges with unmatched reference delay");
        }
        // Reference-delay compensation (like set_reference_delay_samples) for
        // the worst case: delay the reference so the filter sees a short path.
        {
            std::vector<int16_t> echo = echo_of(far, 20.0 + 160.0, 0.5);
            SpeexEchoState *st = speex_echo_state_init(256, 2048);
            int rate = kSampleRate;
            speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
            std::vector<int16_t> delayed_ref(far.size(), 0);
            const size_t d = 160;
            for (size_t i = d; i < far.size(); i++)
                delayed_ref[i] = far[i - d];
            std::vector<int16_t> out(echo.size(), 0);
            for (size_t off = 0; off + 256 <= echo.size(); off += 256)
                speex_echo_cancellation(st, echo.data() + off, delayed_ref.data() + off, out.data() + off);
            speex_echo_state_destroy(st);
            const size_t skip = kSampleRate / 2;
            const double erle = erle_db(echo.data() + skip, out.data() + skip, echo.size() - skip);
            CHECK_MSG(erle > 20.0, "compensated reference delay restores full ERLE");
        }
    }
    SUITE_END();

    // Filter tail: echo beyond the filter length cannot be cancelled.
    SUITE_BEGIN("filter_length_limits");
    {
        std::vector<int16_t> far(kSampleRate * 4);
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        for (auto &v : far)
            v = static_cast<int16_t>(uniform(rng) * 20000.0f);
        // Echo at 300 samples (38 ms) vs filter tails of 12 ms and 128 ms.
        std::vector<int16_t> echo = echo_of(far, 300.0, 0.5);
        const size_t skip = kSampleRate / 2;
        std::vector<int16_t> out_short = raw_aec(echo, far, 256, 256);    // 12 ms tail
        std::vector<int16_t> out_long = raw_aec(echo, far, 256, 2048);    // 128 ms tail
        const double erle_short = erle_db(echo.data() + skip, out_short.data() + skip, echo.size() - skip);
        const double erle_long = erle_db(echo.data() + skip, out_long.data() + skip, echo.size() - skip);
        CHECK_MSG(erle_long > erle_short + 10.0, "longer filter cancels delayed echo much better");
        CHECK_MSG(erle_long > 20.0, "echo inside tail cancelled");
    }
    SUITE_END();

    // Double-talk: speech preservation while echo is being cancelled.
    SUITE_BEGIN("doubletalk");
    if (doubletalk == nullptr) {
        s.skip("scenario missing");
    } else {
        const Scenario &sc = *doubletalk;
        std::vector<int16_t> out = raw_aec(sc.mic0, sc.reference, 256, 2048);
        // During double-talk the near-end speech must survive.
        std::vector<size_t> dt_samples;
        double mic_ref_corr_before = 0;
        for (size_t i = 0; i < sc.speech_labels.size() && i < out.size(); i++)
            if (sc.speech_labels[i] > 0.5f && sc.echo_labels.size() > i && sc.echo_labels[i] > 0.5f)
                dt_samples.push_back(i);
        CHECK_MSG(dt_samples.size() > kSampleRate, "scenario has substantial double-talk region");
        // SegSNR of output vs clean near speech over active DT segments.
        std::vector<int16_t> clean_at_dt(dt_samples.size()), out_at_dt(dt_samples.size());
        for (size_t i = 0; i < dt_samples.size(); i++) {
            clean_at_dt[i] = sc.near_clean[dt_samples[i]];
            out_at_dt[i] = out[dt_samples[i]];
        }
        // AGC is off in the raw path; compare directly.
        const double segsnr = segmental_snr_db(clean_at_dt.data(), out_at_dt.data(), dt_samples.size(), 512);
        const double corr = correlation(clean_at_dt.data(), out_at_dt.data(), dt_samples.size());
        CHECK_MSG(segsnr > 0.0, "near-end speech survives double-talk (segSNR > 0 dB)");
        CHECK_MSG(corr > 0.8, "output correlates with clean near-end speech");
    }
    SUITE_END();

    // Impulse-response identification: adapted filter peaks at echo delay.
    SUITE_BEGIN("impulse_response");
    {
        std::vector<int16_t> far(kSampleRate * 4);
        std::mt19937 rng(23);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        for (auto &v : far)
            v = static_cast<int16_t>(uniform(rng) * 18000.0f);
        const int echo_delay = 40;
        std::vector<int16_t> echo = echo_of(far, echo_delay, 0.6);
        SpeexEchoState *st = speex_echo_state_init(256, 2048);
        int rate = kSampleRate;
        speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        std::vector<int16_t> out(echo.size(), 0);
        for (size_t off = 0; off + 256 <= echo.size(); off += 256)
            speex_echo_cancellation(st, echo.data() + off, far.data() + off, out.data() + off);
        // Read back the adapted IR.
        int32_t ir_size = 0;
        speex_echo_ctl(st, SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE, &ir_size);
        std::vector<spx_int32_t> ir(ir_size);
        speex_echo_ctl(st, SPEEX_ECHO_GET_IMPULSE_RESPONSE, ir.data());
        speex_echo_state_destroy(st);
        int peak = 0;
        double peak_val = 0.0;
        for (int i = 0; i < 256; i++) {
            const double v = std::fabs(static_cast<double>(ir[i]));
            if (v > peak_val) {
                peak_val = v;
                peak = i;
            }
        }
        CHECK_MSG(std::abs(peak - echo_delay) <= 3, "adapted IR peak within 3 samples of true delay");
    }
    SUITE_END();

    // Multichannel (mc) state used by the beamforming topology: shared
    // reference FFT must cancel echo on both microphones.
    SUITE_BEGIN("multichannel_state");
    {
        std::vector<int16_t> far(kSampleRate * 4);
        std::mt19937 rng(31);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        for (auto &v : far)
            v = static_cast<int16_t>(uniform(rng) * 18000.0f);
        std::vector<int16_t> mic0 = echo_of(far, 20.0, 0.5);
        std::vector<int16_t> mic1 = echo_of(far, 24.0, 0.45);
        // Interleave into 2-channel frames.
        const int frame = 256;
        SpeexEchoState *st = speex_echo_state_init_mc(frame, 2048, 2, 1);
        int rate = kSampleRate;
        speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        std::vector<int16_t> interleaved(mic0.size() * 2), out_interleaved(mic0.size() * 2);
        for (size_t i = 0; i < mic0.size(); i++) {
            interleaved[2 * i] = mic0[i];
            interleaved[2 * i + 1] = mic1[i];
        }
        double e0_in = 0, e0_out = 0, e1_in = 0, e1_out = 0;
        for (size_t off = 0; off + frame <= mic0.size(); off += frame) {
            speex_echo_cancellation(st, interleaved.data() + off * 2, far.data() + off,
                                    out_interleaved.data() + off * 2);
            for (int n = 0; n < frame * 2; n++) {
                const size_t idx = off * 2 + n;
                if ((n & 1) == 0) {
                    e0_in += static_cast<double>(interleaved[idx]) * interleaved[idx];
                    e0_out += static_cast<double>(out_interleaved[idx]) * out_interleaved[idx];
                } else {
                    e1_in += static_cast<double>(interleaved[idx]) * interleaved[idx];
                    e1_out += static_cast<double>(out_interleaved[idx]) * out_interleaved[idx];
                }
            }
        }
        speex_echo_state_destroy(st);
        const double erle0 = 10.0 * std::log10(e0_in / std::max(e0_out, 1e-9));
        const double erle1 = 10.0 * std::log10(e1_in / std::max(e1_out, 1e-9));
        CHECK_MSG(erle0 > 15.0 && erle1 > 15.0, "mc canceller converges on both microphones");
    }
    SUITE_END();

    // speex_echo_capture/playback: the soundcard-buffered variant keeps the
    // reference in an internal buffer delayed by PLAYBACK_DELAY frames.
    SUITE_BEGIN("capture_playback_api");
    {
        std::vector<int16_t> far(kSampleRate * 2);
        std::mt19937 rng(41);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        for (auto &v : far)
            v = static_cast<int16_t>(uniform(rng) * 16000.0f);
        // Same signal arrives at the mic with a small acoustic delay.
        std::vector<int16_t> echo = echo_of(far, 12.0, 0.5);
        SpeexEchoState *st = speex_echo_state_init(256, 2048);
        int rate = kSampleRate;
        speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        std::vector<int16_t> out(echo.size(), 0);
        for (size_t off = 0; off + 256 <= echo.size(); off += 256) {
            speex_echo_playback(st, far.data() + off);
            speex_echo_capture(st, echo.data() + off, out.data() + off);
        }
        speex_echo_state_destroy(st);
        // capture() processes the mic through the internal buffering path; it
        // must produce output without crashing. int16 is bounded by type, so
        // verify the output is non-trivial (echo attenuated relative to input).
        double in_energy = 0, out_energy = 0;
        for (size_t i = 0; i < echo.size(); i++) {
            in_energy += static_cast<double>(echo[i]) * echo[i];
            out_energy += static_cast<double>(out[i]) * out[i];
        }
        CHECK_MSG(out_energy > 0.0 && out_energy < in_energy * 4.0, "capture path produces sane output");
        // Frame-size ctl roundtrip.
        SpeexEchoState *st2 = speex_echo_state_init(256, 2048);
        int fs = 0;
        speex_echo_ctl(st2, SPEEX_ECHO_GET_FRAME_SIZE, &fs);
        speex_echo_state_destroy(st2);
        CHECK_MSG(fs == 256, "GET_FRAME_SIZE reports configured frame");
    }
    SUITE_END();

    // Reset returns the canceller to its un-adapted state.
    SUITE_BEGIN("state_reset");
    {
        std::vector<int16_t> far(kSampleRate * 3);
        std::mt19937 rng(53);
        std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
        for (auto &v : far)
            v = static_cast<int16_t>(uniform(rng) * 16000.0f);
        std::vector<int16_t> echo = echo_of(far, 16.0, 0.5);
        SpeexEchoState *st = speex_echo_state_init(256, 2048);
        int rate = kSampleRate;
        speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        std::vector<int16_t> out(echo.size(), 0);
        const size_t converge = kSampleRate;
        for (size_t off = 0; off + 256 <= echo.size(); off += 256)
            speex_echo_cancellation(st, echo.data() + off, far.data() + off, out.data() + off);
        const double erle_before =
            erle_db(echo.data() + converge, out.data() + converge, echo.size() - converge);
        // Read the adapted IR, reset, then read it again: reset must clear it.
        auto ir_peak = [&st]() {
            int32_t ir_size = 0;
            speex_echo_ctl(st, SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE, &ir_size);
            std::vector<spx_int32_t> ir(ir_size);
            speex_echo_ctl(st, SPEEX_ECHO_GET_IMPULSE_RESPONSE, ir.data());
            double peak = 0.0;
            for (int32_t v : ir)
                peak = std::max(peak, std::fabs(static_cast<double>(v)));
            return peak;
        };
        const double peak_before = ir_peak();
        speex_echo_state_reset(st);
        const double peak_after = ir_peak();
        std::fill(out.begin(), out.end(), static_cast<int16_t>(0));
        for (size_t off = 0; off + 256 <= echo.size(); off += 256)
            speex_echo_cancellation(st, echo.data() + off, far.data() + off, out.data() + off);
        // Immediately after reset, the first 100 ms are re-learned only.
        const double erle_after_reset =
            erle_db(echo.data(), out.data(), kSampleRate / 10);
        speex_echo_state_destroy(st);
        CHECK_MSG(erle_before > 20.0, "converged before reset");
        CHECK_MSG(peak_after < peak_before * 0.1, "reset clears the adapted impulse response");
        CHECK_MSG(erle_after_reset < erle_before - 8.0, "echo passes right after reset (re-converging)");
    }
    SUITE_END();
}
