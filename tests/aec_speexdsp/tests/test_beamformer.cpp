/* Beamformer tests: TDOA cross-correlation accuracy (integer and
 * fractional), validity gating, coherent array gain, interference
 * rejection, steering smoothness and frame-boundary continuity. */
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "beamformer.h"
#include "dsp_pipeline.h"
#include "test_framework.h"

namespace {

using namespace dsp;
using esphome::aec_speexdsp::AdaptiveDelayAndSumBeamformer;
using esphome::aec_speexdsp::TdoaResult;

// Speech-band noise (150-1500 Hz, like voiced speech energy) via FFT
// band shaping: the low-frequency dominance mirrors real speech, whose
// autocorrelation stays high over the beamformer's ±7 sample search range.
std::vector<int16_t> bandlimited_noise(size_t n, uint32_t seed, double lo_hz = 150.0, double hi_hz = 1500.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    std::vector<double> x(n);
    for (auto &v : x)
        v = gauss(rng);
    std::vector<double> spec(n / 2 + 1);
    // naive DFT-free approach: FFT via kiss is overkill here; use a
    // cascade of one-pole filters (strong lowpass minus weaker lowpass)
    // iterated a few times for steeper edges.
    double l1 = 0, l2 = 0;
    const double a_hi = 1.0 - std::exp(-2.0 * M_PI * hi_hz / kSampleRate);
    const double a_lo = 1.0 - std::exp(-2.0 * M_PI * lo_hz / kSampleRate);
    double lo1 = 0, lo2 = 0;
    for (size_t i = 0; i < n; i++) {
        l1 += a_hi * (x[i] - l1);
        l1 += a_hi * (x[i] - l1);  // twice for a steeper top edge
        l2 += a_hi * (l1 - l2);
        lo1 += a_lo * (l1 - lo1);
        lo2 += a_lo * (lo1 - lo2);
        x[i] = l2 - lo2;
    }
    std::vector<int16_t> out(n);
    double rms = 0.0;
    for (double v : x)
        rms += v * v;
    rms = std::sqrt(rms / n);
    const double gain = 9000.0 / std::max(rms, 1e-9);
    for (size_t i = 0; i < n; i++)
        out[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, x[i] * gain)));
    return out;
}

}  // namespace

void run_beamformer_suite(testfw::Suite &s) {
    SUITE_BEGIN("interleaved_matches_planar");
    for (uint8_t channels : {1, 2, 3, 4}) {
        AdaptiveDelayAndSumBeamformer planar_bf, strided_bf;
        planar_bf.configure(channels, 7, 1, 120, 50, 5);
        strided_bf.configure(channels, 7, 1, 120, 50, 5);
        std::mt19937 rng(701);
        bool equal = true;
        for (int repeat=0; repeat<40; ++repeat) {
            // Include blocks shorter than the delay history and odd lengths.
            const size_t samples = repeat%3 == 0 ? 5 : 257;
            std::vector<int16_t> planar(channels*samples), interleaved(planar.size()), a(samples), b(samples);
            const int16_t *pa[4]{}, *pb[4]{};
            for (uint8_t c=0; c<channels; ++c) {
                pa[c] = planar.data()+c*samples;
                pb[c] = interleaved.data()+c;
                for (size_t n=0; n<samples; ++n) {
                    const int16_t v = int(rng()%65536)-32768;
                    planar[c*samples+n] = interleaved[n*channels+c] = v;
                }
            }
            planar_bf.process(pa, samples, a.data());
            strided_bf.process(pb, samples, b.data(), channels);
            equal &= a == b;
            for (uint8_t c=0; c<channels; ++c)
                equal &= planar_bf.get_tdoa_q15(c) == strided_bf.get_tdoa_q15(c) &&
                         planar_bf.get_confidence_q15(c) == strided_bf.get_confidence_q15(c);
        }
        CHECK_MSG(equal, "strided and planar PCM, localization and short-block history match exactly");
    }
    SUITE_END();
    // --- estimate_tdoa_xcorr: integer lags, fractional lags, gating ---
    SUITE_BEGIN("tdoa_integer_lags");
    for (int lag = -6; lag <= 6; lag += 2) {
        const size_t n = 512;
        std::vector<int16_t> ref = bandlimited_noise(n, 200u + lag, 300.0, 4000.0);
        std::vector<int16_t> mic = delay_signal(ref, static_cast<double>(lag));
        // Inject mic onto slot B while slot A is the reference signal.
        const TdoaResult r = AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(
            ref.data(), mic.data(), n, /*max_lag=*/7, /*min_rms=*/120, /*min_corr_q15=*/16384,
            /*min_dom_q15=*/1638);
        CHECK_MSG(r.valid, "TDOA valid for integer lag");
        CHECK_MSG(r.peak_lag == lag, "peak lag matches injected delay");
        CHECK_MSG(r.correlation_q15 > 16384, "correlation above 0.5 for clean copy");
    }
    SUITE_END();

    SUITE_BEGIN("tdoa_fractional_lags");
    for (double delay : {2.5, -1.25, 4.75}) {
        const size_t n = 512;
        std::vector<int16_t> ref = bandlimited_noise(n, 300u, 300.0, 4000.0);
        std::vector<int16_t> mic = delay_signal(ref, delay);
        const TdoaResult r = AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(
            ref.data(), mic.data(), n, 7, 120, 16384, 1638);
        CHECK_MSG(r.valid, "TDOA valid for fractional delay");
        const double estimated = r.delay_q15 / 32768.0;
        CHECK_NEAR(estimated, delay, 0.35);
    }
    SUITE_END();

    SUITE_BEGIN("tdoa_gating");
    {
        const size_t n = 512;
        // Below min_rms: silence must not localize.
        std::vector<int16_t> quiet(n, 3);
        std::vector<int16_t> quiet2(n, 4);
        TdoaResult r = AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(quiet.data(), quiet2.data(), n, 7, 120,
                                                                          16384, 1638);
        CHECK_MSG(!r.valid, "low-level input rejected by min_rms gate");
        // Uncorrelated signals: no trustworthy peak.
        std::vector<int16_t> a = bandlimited_noise(n, 401, 300.0, 4000.0);
        std::vector<int16_t> b = bandlimited_noise(n, 402, 300.0, 4000.0);
        r = AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(a.data(), b.data(), n, 7, 120, 16384, 1638);
        if (r.valid)
            CHECK_MSG(r.confidence_q15 < 16384, "uncorrelated input yields low confidence");
        // Peak pinned at the search edge is rejected (can't trust a one-sided max).
        std::vector<int16_t> far_delayed = delay_signal(a, 7.0);
        r = AdaptiveDelayAndSumBeamformer::estimate_tdoa_xcorr(a.data(), far_delayed.data(), n, 7, 120, 16384, 1638);
        CHECK_MSG(!r.valid, "lag pinned at search edge rejected");
    }
    SUITE_END();

    // --- AdaptiveDelayAndSumBeamformer: coherent gain and alignment ---
    SUITE_BEGIN("array_gain_two_mics");
    {
        const size_t frame = 256;
        const size_t frames = 512;  // allow the 1/8 smoother to settle at update_frames=8
        const size_t n = frame * frames;
        const double true_delay = 3.0;  // integer steering isolates array noise gain from interpolation loss
        std::vector<int16_t> target = bandlimited_noise(n, 501, 300.0, 3500.0);
        target = delay_signal(target, 200.0);  // start after the initial transient
        std::mt19937 rng(502);
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::vector<int16_t> mic0(n), mic1(n), noise0(n), noise1(n);
        for (size_t i = 0; i < n; i++) {
            noise0[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, gauss(rng) * 900.0)));
            noise1[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, gauss(rng) * 900.0)));
            mic0[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, static_cast<double>(target[i] + noise0[i]))));
            mic1[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, static_cast<double>(target[i] + noise1[i]))));
        }
        // mic1 target arrives true_delay late.
        std::vector<int16_t> target_delayed = delay_signal(target, true_delay);
        for (size_t i = 0; i < n; i++)
            mic1[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, static_cast<double>(target_delayed[i] + noise1[i]))));

        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(2, 7, 8, 120, 50, 5);
        std::vector<int16_t> out(n, 0);
        const int16_t *inputs[4] = {mic0.data(), mic1.data()};
        for (size_t f = 0; f < frames; f++) {
            inputs[0] = mic0.data() + f*frame;
            inputs[1] = mic1.data() + f*frame;
            bf.process(inputs, frame, out.data() + f * frame);
        }

        // After convergence, the beam steers to the target: TDOA close to truth.
        const double tdoa_estimated = bf.get_tdoa_q15(1) / 32768.0;
        const double confidence = bf.get_confidence_q15(1) / 32768.0;
        CHECK_NEAR(tdoa_estimated, true_delay, 0.6);

        // Array SNR gain over mic0 during the last half (converged).
        // The beam aligns every mic to the latest-arriving signal (mic1),
        // so the coherent reference for the beam output is the target as
        // mic1 receives it (target delayed by true_delay).
        auto snr_vs = [&](const std::vector<int16_t> &sig, const std::vector<int16_t> &ref) {
            double s = 0, nn = 0;
            for (size_t i = n / 2; i < n; i++) {
                s += static_cast<double>(ref[i]) * ref[i];
                const double d = sig[i] - ref[i];
                nn += d * d;
            }
            return 10.0 * std::log10(s / std::max(nn, 1e-9));
        };
        std::vector<int16_t> target_at_mic1 = delay_signal(target, true_delay);
        const double snr_mic0 = snr_vs(mic0, target);
        const double snr_beam = snr_vs(out, target_at_mic1);
        CHECK_MSG(snr_beam > snr_mic0 + 1.5, "2-mic array gain > 1.5 dB (theory ~3 dB)");
        CHECK_MSG(bf.get_beamforming_calls() == frames, "beamforming called per frame");
        CHECK_MSG(bf.get_localization_calls() == frames / 8, "localization on update cadence");
    }
    SUITE_END();

    SUITE_BEGIN("array_gain_four_mics");
    {
        const size_t frame = 256;
        const size_t frames = 512;
        const size_t n = frame * frames;
        const std::vector<double> delays = {0.0, 2.0, 4.0, 6.0};
        std::vector<int16_t> target = bandlimited_noise(n, 601, 300.0, 3500.0);
        std::vector<std::vector<int16_t>> mics(4), delayed(4);
        for (uint8_t m = 0; m < 4; m++)
            delayed[m] = delay_signal(target, delays[m]);
        std::mt19937 rng(602);
        std::normal_distribution<double> gauss(0.0, 1.0);
        std::vector<std::vector<int16_t>> noise(4);
        for (uint8_t m = 0; m < 4; m++) {
            noise[m].resize(n);
            for (size_t i = 0; i < n; i++)
                noise[m][i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, gauss(rng) * 900.0)));
            mics[m].resize(n);
            for (size_t i = 0; i < n; i++)
                mics[m][i] = static_cast<int16_t>(
                    std::max(-32768.0, std::min(32767.0, static_cast<double>(delayed[m][i] + noise[m][i]))));
        }
        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(4, 8, 8, 120, 50, 5);
        std::vector<int16_t> out(n, 0);
        const int16_t *inputs[4] = {mics[0].data(), mics[1].data(), mics[2].data(), mics[3].data()};
        for (size_t f = 0; f < frames; f++) {
            for (uint8_t m=0; m<4; ++m) inputs[m] = mics[m].data() + f*frame;
            bf.process(inputs, frame, out.data() + f * frame);
        }

        double delay_err = 0.0;
        for (uint8_t m = 1; m < 4; m++)
            delay_err = std::max(delay_err, std::fabs(bf.get_tdoa_q15(m) / 32768.0 - delays[m]));
        CHECK_MSG(delay_err < 0.6, "all four TDOAs within 0.6 samples");

        auto snr_vs = [&](const std::vector<int16_t> &sig, const std::vector<int16_t> &ref) {
            double s = 0, nn = 0;
            for (size_t i = n / 2; i < n; i++) {
                s += static_cast<double>(ref[i]) * ref[i];
                nn += static_cast<double>(sig[i] - ref[i]) * (sig[i] - ref[i]);
            }
            return 10.0 * std::log10(s / std::max(nn, 1e-9));
        };
        const double snr_mic0 = snr_vs(mics[0], target);
        // Beam output lives on mic3's (latest) timeline.
        std::vector<int16_t> target_at_mic3 = delay_signal(target, delays[3]);
        const double snr_beam = snr_vs(out, target_at_mic3);
        CHECK_MSG(snr_beam > snr_mic0 + 3.0, "4-mic array gain > 3 dB (theory ~6 dB)");
    }
    SUITE_END();

    // Interference from a different direction is attenuated once steered
    // toward the target.
    SUITE_BEGIN("interference_rejection");
    {
        const size_t frame = 256;
        const size_t frames = 512;
        const size_t n = frame * frames;
        const double target_delay = 3.0;
        const double interference_delay = -5.0;  // other side of the array
        std::vector<int16_t> target = bandlimited_noise(n, 701, 300.0, 3000.0);
        std::vector<int16_t> interference = bandlimited_noise(n, 702, 300.0, 3000.0);
        std::vector<int16_t> t0 = target, t1 = delay_signal(target, target_delay);
        std::vector<int16_t> i0 = interference, i1 = delay_signal(interference, interference_delay);
        std::vector<int16_t> mic0(n), mic1(n);
        for (size_t i = 0; i < n; i++) {
            mic0[i] = t0[i];  // target at mic0 defines the clean reference
            mic1[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, static_cast<double>(t1[i] + i1[i]))));
        }
        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(2, 7, 8, 120, 50, 5);
        std::vector<int16_t> out(n, 0);
        const int16_t *inputs[4] = {mic0.data(), mic1.data()};
        for (size_t f = 0; f < frames; f++) {
            inputs[0] = mic0.data() + f*frame;
            inputs[1] = mic1.data() + f*frame;
            bf.process(inputs, frame, out.data() + f * frame);
        }

        // Compare target-to-interference ratio: beam vs single mic1.
        // Interference-only measure: correlation of each signal with the
        // interference component during the second half.
        auto interference_leak = [&](const std::vector<int16_t> &sig) {
            double dot = 0, na = 0, nb = 0;
            for (size_t i = n / 2; i < n; i++) {
                dot += static_cast<double>(sig[i]) * i1[i];
                na += static_cast<double>(sig[i]) * sig[i];
                nb += static_cast<double>(i1[i]) * i1[i];
            }
            return std::fabs(dot) / std::sqrt(std::max(na * nb, 1e-9));
        };
        const double leak_mic1 = interference_leak(mic1);
        const double leak_beam = interference_leak(out);
        CHECK_MSG(leak_beam < leak_mic1, "beam output contains less interference than the raw second mic");
        // Target preserved: correlation with the target on the beam's
        // timeline (mic1, delayed by target_delay) stays high.
        const std::vector<int16_t> &target_ref = t1;
        double dot = 0, na = 0, nb = 0;
        for (size_t i = n / 2; i < n; i++) {
            dot += static_cast<double>(out[i]) * target_ref[i];
            na += static_cast<double>(out[i]) * out[i];
            nb += static_cast<double>(target_ref[i]) * target_ref[i];
        }
        const double target_corr = dot / std::sqrt(std::max(na * nb, 1e-9));
        CHECK_MSG(target_corr > 0.85, "beam preserves the target");
    }
    SUITE_END();

    // Frame-boundary continuity: fractional delays must not click between
    // frames (the beamformer keeps HISTORY_SAMPLES of context).
    SUITE_BEGIN("frame_boundary_continuity");
    {
        const size_t frame = 256;
        const size_t frames = 40;
        const size_t n = frame * frames;
        // 1 kHz tone at a healthy level.
        std::vector<int16_t> tone(n);
        for (size_t i = 0; i < n; i++)
            tone[i] = static_cast<int16_t>(12000.0 * std::sin(2 * M_PI * 1000.0 * i / kSampleRate));
        std::vector<int16_t> mic1 = delay_signal(tone, 2.5);
        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(2, 7, 8, 120, 50, 5);
        std::vector<int16_t> out(n, 0);
        const int16_t *inputs[4] = {tone.data(), mic1.data()};
        std::vector<int16_t> prev_tail(8, 0);
        bool smooth = true;
        double worst_jump = 0.0;
        for (size_t f = 0; f < frames; f++) {
            inputs[0] = tone.data() + f*frame;
            inputs[1] = mic1.data() + f*frame;
            bf.process(inputs, frame, out.data() + f * frame);
            if (f > 0) {
                const double jump = std::abs(static_cast<double>(out[f * frame]) - prev_tail.back());
                // Tone slope at 1 kHz is at most 2*pi*1000/16000*12000 ~ 4714/sample;
                // allow a small multiple to flag gross discontinuities only.
                worst_jump = std::max(worst_jump, jump);
                if (jump > 2.0 * 4714.0)
                    smooth = false;
            }
            for (int k = 0; k < 8; k++)
                prev_tail[k] = out[f * frame + frame - 8 + k];
        }
        CHECK_MSG(smooth, "no inter-frame discontinuities from the fractional delay");
    }
    SUITE_END();

    // Saturation: full-scale inputs must clamp, never wrap.
    SUITE_BEGIN("saturation_clamps");
    {
        const size_t frame = 256;
        const size_t n = frame * 8;
        std::vector<int16_t> a(n, 32767), b(n, 32767);
        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(2, 7, 8, 120, 50, 5);
        std::vector<int16_t> out(n, 0);
        const int16_t *inputs[4] = {a.data(), b.data()};
        for (size_t f = 0; f < n / frame; f++)
            bf.process(inputs, frame, out.data() + f * frame);
        bool bounded = true;
        for (int16_t v : out)
            if (v < 0) {
                bounded = false;
                break;
            }
        CHECK_MSG(bounded, "positive full-scale sum clamps without wraparound");
        // mixed signs
        for (size_t i = 0; i < n; i++)
            b[i] = i % 2 == 0 ? -32768 : 32767;
        for (size_t f = 0; f < n / frame; f++)
            bf.process(inputs, frame, out.data() + f * frame);
        bounded = true;
        for (int16_t v : out)
            if (v < -32768 || v > 32767)
                bounded = false;
        CHECK_MSG(bounded, "output remains within int16 after mixed full-scale input");
    }
    SUITE_END();

    // configure() clamps out-of-range arguments.
    SUITE_BEGIN("configure_clamping");
    {
        AdaptiveDelayAndSumBeamformer bf;
        bf.configure(9, 99, 0, 0, 200, 200);
        // microphones clamped to MAX_MICROPHONES; max_lag to MAX_LAG; percent
        // conversions saturate. Nothing should crash and calls should work.
        std::vector<int16_t> m0(256, 1000), m1(256, -1000), out(256, 0);
        const int16_t *inputs[4] = {m0.data(), m1.data(), m1.data(), m1.data()};
        bf.process(inputs, 256, out.data());
        CHECK_MSG(bf.get_beamforming_calls() == 1, "process works after clamped configure");
    }
    SUITE_END();
}
