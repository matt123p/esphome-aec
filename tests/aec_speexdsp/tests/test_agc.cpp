/* Automatic gain control tests: quiet speech lifted toward the target,
 * loud speech held at the ceiling, silence left silent, disabled AGC
 * transparent. */
#include <cmath>
#include <random>
#include <cstring>
#include <vector>

#include "dsp_pipeline.h"
#include "speex_preprocess.h"
#include "test_framework.h"

namespace {

using namespace dsp;

struct AgcRig {
    SpeexPreprocessState *pre = nullptr;
    explicit AgcRig(float target_level = 0.25f, bool agc = true) {
        pre = speex_preprocess_state_init(256, kSampleRate);
        int denoise = 0;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
        int a = agc ? 1 : 0;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC, &a);
        if (agc) {
            float level = target_level * 32768.0f;
            speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
        }
        // The component always runs VAD alongside AGC; the loudness tracker
        // gates its adaptation on speech activity.
        int vad = 1;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_VAD, &vad);
        int prob_start = 35;
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_PROB_START, &prob_start);
    }
    ~AgcRig() { speex_preprocess_state_destroy(pre); }
    std::vector<int16_t> run(const std::vector<int16_t> &x, std::vector<int32_t> *gain_pct = nullptr) {
        std::vector<int16_t> out(x.size() + 256, 0);
        for (size_t off = 0; off + 256 <= x.size(); off += 256) {
            std::memcpy(out.data() + off, x.data() + off, 256 * sizeof(int16_t));
            speex_preprocess_run(pre, out.data() + off);
            if (gain_pct != nullptr) {
                int32_t g = 0;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_GET_AGC_GAIN, &g);
                gain_pct->push_back(g);
            }
        }
        speex_preprocess_run(pre, out.data() + x.size());
        return std::vector<int16_t>(out.begin() + 256, out.end());
    }
};

double rms_dbfs(const std::vector<int16_t> &x, size_t start, size_t count) {
    double sum = 0.0;
    for (size_t i = start; i < start + count && i < x.size(); i++)
        sum += static_cast<double>(x[i]) * x[i];
    const double rms = std::sqrt(sum / count);
    return 20.0 * std::log10(std::max(rms, 1e-9) / 32768.0);
}

}  // namespace

void run_agc_suite(testfw::Suite &s, const std::vector<Scenario> &scenarios) {
    const Scenario *quiet = nullptr;
    const Scenario *loud = nullptr;
    for (const auto &sc : scenarios) {
        if (sc.name == "near_quiet")
            quiet = &sc;
        if (sc.name == "near_loud")
            loud = &sc;
    }

    // Quiet speech pulled up toward the -12 dBFS (0.25 FS) target.
    SUITE_BEGIN("quiet_speech_gain");
    if (quiet == nullptr) {
        s.skip("scenario missing");
    } else {
        AgcRig rig(0.25f);
        // Target is frequency-weighted perceptual loudness, not waveform RMS.
        // Compare two targets on identical speech after a longer settling run.
        std::vector<int16_t> input;
        for (int repeat=0; repeat<3; ++repeat)
            input.insert(input.end(), quiet->mic0.begin(), quiet->mic0.end());
        std::vector<int16_t> out = rig.run(input);
        const double in_db = rms_dbfs(input, input.size()/2, input.size()/2);
        const double out_db = rms_dbfs(out, out.size()/2, out.size()/2);
        CHECK_MSG(out_db > in_db, "quiet speech amplified toward the perceptual target");
        CHECK_MSG(out_db < -2.0, "output stays below clipping range");
        AgcRig half_target(0.125f);
        const auto half_out = half_target.run(input);
        const double target_delta = out_db - rms_dbfs(half_out, half_out.size()/2, half_out.size()/2);
        CHECK_NEAR(target_delta, 20.0*std::log10(2.0), 1.0);
    }
    SUITE_END();

    // Loud speech must not be pushed into clipping.
    SUITE_BEGIN("loud_speech_ceiling");
    if (loud == nullptr) {
        s.skip("scenario missing");
    } else {
        AgcRig rig(0.25f);
        std::vector<int16_t> out = rig.run(loud->mic0);
        size_t clipped = 0;
        for (int16_t v : out)
            if (v >= 32700 || v <= -32700)
                clipped++;
        const double clipped_pct = 100.0 * clipped / out.size();
        const double in_db = rms_dbfs(loud->mic0, kSampleRate, loud->mic0.size() - kSampleRate);
        const double out_db = rms_dbfs(out, kSampleRate, out.size() - kSampleRate);
        CHECK_MSG(clipped_pct < 0.1, "loud speech not clipped by AGC");
        CHECK_MSG(out_db < in_db + 3.0, "loud speech not amplified");
    }
    SUITE_END();

    // Silence stays silent.
    SUITE_BEGIN("silence_stays_silent");
    {
        std::vector<int16_t> silence(kSampleRate * 3, 0);
        // tiny dither so the preprocessor has *something* to chew on
        std::mt19937 rng(131);
        std::uniform_int_distribution<int> dither(-2, 2);
        for (auto &v : silence)
            v = static_cast<int16_t>(dither(rng));
        AgcRig rig(0.25f);
        std::vector<int16_t> out = rig.run(silence);
        const double out_db = rms_dbfs(out, 0, out.size());
        CHECK_MSG(out_db < -50.0, "silence not amplified into noise");
    }
    SUITE_END();

    // AGC disabled: transparent path.
    SUITE_BEGIN("agc_disabled");
    if (quiet == nullptr) {
        s.skip("scenario missing");
    } else {
        AgcRig rig(0.25f, /*agc=*/false);
        std::vector<int16_t> out = rig.run(quiet->mic0);
        const double max_err = max_abs_error(quiet->mic0.data(), out.data(), out.size());
        CHECK_MSG(max_err <= 1.0, "bypassed path is bit-transparent (max 1 LSB)");
    }
    SUITE_END();

    // AGC gain telemetry: reported gain percent moves in the right direction.
    SUITE_BEGIN("gain_telemetry");
    if (quiet == nullptr) {
        s.skip("scenario missing");
    } else {
        AgcRig rig(0.25f);
        std::vector<int32_t> gain;
        rig.run(quiet->mic0, &gain);
        CHECK_MSG(!gain.empty(), "gain samples collected");
        // SPEEX_PREPROCESS_GET_AGC_GAIN reports rounded decibels.
        const double first = gain.size() > 8 ? gain[8] : gain.front();
        const double last = gain.back();
        CHECK_MSG(last > first && last > 0.0, "gain increased toward target for quiet input");
    }
    SUITE_END();
}
