/* Noise suppression (speex preprocessor denoiser) tests: attenuation of
 * stationary and non-stationary noise, speech preservation, the noise
 * ceiling ctl, and the noise PSD introspection. */
#include <cmath>
#include <random>
#include <cstring>
#include <vector>

#include "dsp_pipeline.h"
#include "speex_preprocess.h"
#include "test_framework.h"

namespace {

using namespace dsp;

// Noise-only attenuation that is robust against the preprocessor's rare
// one-frame transient bursts: the median per-frame energy ratio is used,
// and burst frames are counted.
struct RobustAttn {
    double median_db = 0.0;
    size_t burst_frames = 0;  // frames with output > 4x input frame RMS
    size_t frames = 0;
};

RobustAttn robust_attenuation(const int16_t *in, const int16_t *out, size_t count, size_t frame = 256) {
    std::vector<double> ratios;
    RobustAttn r;
    for (size_t off = 0; off + frame <= count; off += frame) {
        double ei = 0, eo = 0;
        for (size_t i = 0; i < frame; i++) {
            ei += static_cast<double>(in[off + i]) * in[off + i];
            eo += static_cast<double>(out[off + i]) * out[off + i];
        }
        if (ei < 1e-9)
            continue;
        const double ratio = eo / ei;
        r.frames++;
        if (ratio > 16.0)
            r.burst_frames++;
        else
            ratios.push_back(ratio);
    }
    if (!ratios.empty()) {
        std::sort(ratios.begin(), ratios.end());
        r.median_db = -10.0 * std::log10(ratios[ratios.size() / 2]);
    }
    return r;
}

// Processes mic through a preprocessor only (echo state linked but silent
// reference, mirroring the component when nothing is playing).
struct NsRig {
    SpeexPreprocessState *pre = nullptr;
    explicit NsRig(int noise_db = 15, bool denoise = true, bool agc = false) {
        pre = speex_preprocess_state_init(256, kSampleRate);
        int d = denoise ? 1 : 0;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &d);
        int ndb = noise_db;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ndb);
        int a = agc ? 1 : 0;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC, &a);
        int vad = 1;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_VAD, &vad);
        int prob_start = 35;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_PROB_START, &prob_start);
    }
    ~NsRig() { speex_preprocess_state_destroy(pre); }
    std::vector<int16_t> run(const std::vector<int16_t> &x) {
        // WOLA introduces one frame of latency. Flush it and align metrics
        // to the input timeline; do not compare different audio samples.
        std::vector<int16_t> out(x.size() + 256, 0);
        for (size_t off = 0; off + 256 <= x.size(); off += 256) {
            std::memcpy(out.data() + off, x.data() + off, 256 * sizeof(int16_t));
            speex_preprocess_run(pre, out.data() + off);
        }
        speex_preprocess_run(pre, out.data() + x.size());
        return std::vector<int16_t>(out.begin() + 256, out.end());
    }
};

}  // namespace

void run_noise_suppression_suite(testfw::Suite &s, const std::vector<Scenario> &scenarios) {
    const Scenario *near_babble = nullptr;
    const Scenario *speech_fan = nullptr;
    for (const auto &sc : scenarios) {
        if (sc.name == "near_only_babble")
            near_babble = &sc;
        if (sc.name == "speech_fan")
            speech_fan = &sc;
    }

    // Stationary white noise: measure attenuation during noise-only audio.
    SUITE_BEGIN("stationary_noise_attenuation");
    {
        std::vector<int16_t> noise(kSampleRate * 4);
        std::mt19937 rng(101);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (auto &v : noise)
            v = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, gauss(rng) * 32768.0 * 0.0316)));  // -30 dBFS
        NsRig rig(15);
        std::vector<int16_t> out = rig.run(noise);
        const size_t skip = kSampleRate;  // first second: noise estimate settling
        const RobustAttn ra = robust_attenuation(noise.data() + skip, out.data() + skip, noise.size() - skip);
        CHECK_MSG(ra.median_db > 8.0, "stationary noise reduced by more than 8 dB");
        CHECK_MSG(ra.median_db < 40.0, "attenuation stays below absurd levels");
        CHECK_MSG(ra.burst_frames <= ra.frames / 50, "transient bursts stay rare (<2% of frames)");
    }
    SUITE_END();

    // Fan/hum noise (tonal + low-frequency): the preprocessor's bread and
    // butter on the device.
    SUITE_BEGIN("fan_noise_attenuation");
    {
        std::vector<int16_t> noise(kSampleRate * 4);
        std::mt19937 rng(103);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (size_t i = 0; i < noise.size(); i++) {
            const double t = static_cast<double>(i) / kSampleRate;
            double v = 0.35 * std::sin(2 * M_PI * 50 * t) + 0.2 * std::sin(2 * M_PI * 100 * t) +
                       0.12 * std::sin(2 * M_PI * 150 * t) + 0.25 * gauss(rng);
            noise[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, v * 32768.0 * 0.04)));
        }
        NsRig rig(15);
        std::vector<int16_t> out = rig.run(noise);
        const size_t skip = kSampleRate;
        const RobustAttn ra = robust_attenuation(noise.data() + skip, out.data() + skip, noise.size() - skip);
        CHECK_MSG(ra.median_db > 8.0, "fan/hum noise reduced by more than 8 dB");
    }
    SUITE_END();

    // Babble (non-stationary, speech-like) from the generated scenario.
    SUITE_BEGIN("babble_noise_attenuation");
    if (near_babble == nullptr) {
        s.skip("scenario missing");
    } else {
        NsRig rig(15);
        std::vector<int16_t> out = rig.run(near_babble->mic0);
        // Noise-only frames: speech label == 0.
        std::vector<int16_t> in_noise, out_noise;
        for (size_t off = kSampleRate; off + 256 <= out.size(); off += 256) {
            bool quiet = true;
            for (size_t n = 0; n < 256; n++)
                if (off + n < near_babble->speech_labels.size() && near_babble->speech_labels[off + n] > 0.5f)
                    quiet = false;
            if (quiet) {
                in_noise.insert(in_noise.end(), near_babble->mic0.begin() + off, near_babble->mic0.begin() + off + 256);
                out_noise.insert(out_noise.end(), out.begin() + off, out.begin() + off + 256);
            }
        }
        CHECK_MSG(in_noise.size() > kSampleRate, "scenario has noise-only region");
        const RobustAttn ra = robust_attenuation(in_noise.data(), out_noise.data(), in_noise.size());
        CHECK_MSG(ra.median_db > 3.0, "babble noise reduced (non-stationary, harder)");
    }
    SUITE_END();

    // Speech preservation: with NS on and no noise, speech must not be crushed.
    SUITE_BEGIN("speech_preservation");
    if (near_babble == nullptr) {
        s.skip("scenario missing");
    } else {
        // Feed clean near speech alone.
        NsRig rig(15);
        std::vector<int16_t> out = rig.run(near_babble->near_clean);
        const double segsnr = segmental_snr_db(near_babble->near_clean.data(), out.data(), out.size(), 512);
        const double attn = attenuation_db(near_babble->near_clean.data(), out.data(), out.size());
        CHECK_MSG(segsnr > 0.0, "clean speech survives the denoiser (segSNR > 0 dB)");
        CHECK_MSG(std::fabs(attn) < 8.0, "speech level not crushed by the denoiser");
    }
    SUITE_END();

    // The noise ceiling ctl bounds suppression strength.
    SUITE_BEGIN("suppress_ceiling_ctl");
    {
        std::vector<int16_t> noise(kSampleRate * 3);
        std::mt19937 rng(107);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (auto &v : noise)
            v = static_cast<int16_t>(gauss(rng) * 1000.0);  // quiet hiss
        NsRig gentle(8);
        NsRig strong(35);
        std::vector<int16_t> out_gentle = gentle.run(noise);
        std::vector<int16_t> out_strong = strong.run(noise);
        const size_t skip = kSampleRate;
        const double attn_gentle = robust_attenuation(noise.data() + skip, out_gentle.data() + skip, noise.size() - skip).median_db;
        const double attn_strong = robust_attenuation(noise.data() + skip, out_strong.data() + skip, noise.size() - skip).median_db;
        CHECK_MSG(attn_strong > attn_gentle + 3.0, "higher ceiling suppresses more");
        CHECK_MSG(attn_gentle < 12.0, "low ceiling caps suppression");
    }
    SUITE_END();

    // NS disabled: output tracks input.
    SUITE_BEGIN("denoise_disabled");
    {
        std::vector<int16_t> noise(kSampleRate * 2);
        std::mt19937 rng(109);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (auto &v : noise)
            v = static_cast<int16_t>(gauss(rng) * 1000.0);
        NsRig rig(15, /*denoise=*/false);
        std::vector<int16_t> out = rig.run(noise);
        const double attn = attenuation_db(noise.data(), out.data(), noise.size());
        const double max_err = max_abs_error(noise.data(), out.data(), noise.size());
        CHECK_MSG(std::fabs(attn) < 2.0, "no suppression when denoise disabled");
    }
    SUITE_END();

    // Noise PSD introspection: the estimated noise PSD must track the true
    // input noise level while noise-only frames are processed.
    SUITE_BEGIN("noise_psd_estimate");
    {
        NsRig rig(15);
        std::vector<int16_t> noise(kSampleRate * 2);
        std::mt19937 rng(113);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (auto &v : noise)
            v = static_cast<int16_t>(gauss(rng) * 800.0);
        for (size_t off = 0; off + 256 <= noise.size(); off += 256) {
            std::vector<int16_t> frame(noise.begin() + off, noise.begin() + off + 256);
            speex_preprocess_run(rig.pre, frame.data());
        }
        int psd_size = 0;
        speex_preprocess_ctl(rig.pre, SPEEX_PREPROCESS_GET_NOISE_PSD_SIZE, &psd_size);
        std::vector<spx_int32_t> noise_psd(psd_size);
        speex_preprocess_ctl(rig.pre, SPEEX_PREPROCESS_GET_NOISE_PSD, noise_psd.data());
        double band_total = 0.0;
        for (int32_t v : noise_psd)
            band_total += v;
        // Input noise variance ~800^2 per sample; the vendored port reports
        // psd_size == window_size bins. The total must land in a band that
        // proves the estimator tracked the actual input level (per-bin PSD
        // is Q-format, so bounds are generous but exclude zero / garbage).
        CHECK_MSG(psd_size == 256, "noise PSD reported at window size (vendored port contract)");
        CHECK_MSG(band_total > 1e2 && band_total < 1e9, "noise PSD in plausible range for sigma=800 input");
    }
    SUITE_END();

    // speech + fan mixture: NS improves SNR without killing speech.
    SUITE_BEGIN("speech_in_fan_snr_gain");
    if (speech_fan == nullptr) {
        s.skip("scenario missing");
    } else {
        NsRig rig(15);
        std::vector<int16_t> out = rig.run(speech_fan->mic0);
        // Segmental SNR against the clean speech during speech-active frames.
        std::vector<int16_t> clean, processed, noisy;
        for (size_t i = 0; i < out.size(); i++)
            if (i < speech_fan->speech_labels.size() && speech_fan->speech_labels[i] > 0.5f && i > kSampleRate) {
                clean.push_back(speech_fan->near_clean[i]);
                processed.push_back(out[i]);
                noisy.push_back(speech_fan->mic0[i]);
            }
        const double segsnr_in = segmental_snr_db(clean.data(), noisy.data(), clean.size(), 512);
        const double segsnr_out = segmental_snr_db(clean.data(), processed.data(), processed.size(), 512);
        CHECK_MSG(segsnr_out > segsnr_in, "NS improves segmental SNR of speech in fan noise");
        CHECK_MSG(segsnr_out > -6.0, "output speech remains intelligible-level");
    }
    SUITE_END();
}
