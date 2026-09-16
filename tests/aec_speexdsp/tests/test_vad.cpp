/* Voice activity detection tests: probability tracking, accuracy against
 * generated ground-truth labels, threshold sensitivity and silence
 * behaviour. */
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "dsp_pipeline.h"
#include "test_framework.h"

namespace {

using namespace dsp;

// VAD probability per frame via the full pipeline with NS on and a silent
// reference — exactly what the component publishes as vad_probability.
std::vector<float> vad_probabilities(const std::vector<int16_t> &mic) {
    PipelineConfig cfg;
    cfg.agc = false;
    Pipeline pipeline(cfg);
    std::vector<int16_t> silence_ref(mic.size(), 0);
    std::vector<float> prob;
    pipeline.process_all(mic, silence_ref, &prob);
    return prob;
}

}  // namespace

void run_vad_suite(testfw::Suite &s, const std::vector<Scenario> &scenarios) {
    SUITE_BEGIN("disabled_vad_does_not_publish_speech");
    {
        PipelineConfig cfg;
        cfg.vad = false;
        Pipeline pipeline(cfg);
        std::vector<int16_t> silence(256, 0);
        CHECK_MSG(!pipeline.process(silence.data(), silence.data()).vad_state,
                  "disabled VAD must not treat run()'s passthrough return as speech");
    }
    SUITE_END();
    const Scenario *near_babble = nullptr;
    const Scenario *speech_fan = nullptr;
    const Scenario *near_quiet = nullptr;
    const Scenario *doubletalk = nullptr;
    for (const auto &sc : scenarios) {
        if (sc.name == "near_only_babble")
            near_babble = &sc;
        if (sc.name == "speech_fan")
            speech_fan = &sc;
        if (sc.name == "near_quiet")
            near_quiet = &sc;
        if (sc.name == "doubletalk_music")
            doubletalk = &sc;
    }

    // Babble is speech-like: measure sensitivity on it (hard positives).
    SUITE_BEGIN("vad_sensitivity_babble");
    if (near_babble == nullptr) {
        s.skip("scenario missing");
    } else {
        std::vector<float> prob = vad_probabilities(near_babble->mic0);
        const size_t frame = 256;
        const size_t settled = kSampleRate / frame;  // skip first second
        size_t tp = 0, fn = 0;
        for (size_t f = settled; f < prob.size(); f++) {
            size_t speech_samples = 0;
            for (size_t n = 0; n < frame; n++)
                if (f * frame + n < near_babble->speech_labels.size() &&
                    near_babble->speech_labels[f * frame + n] > 0.5f)
                    speech_samples++;
            const bool truth = speech_samples > frame / 2;
            const bool detected = prob[f] >= 0.5f;
            if (truth && detected)
                tp++;
            else if (truth && !detected)
                fn++;
        }
        const double sensitivity = static_cast<double>(tp) / std::max<size_t>(1, tp + fn);
        CHECK_MSG(sensitivity > 0.65, "speech frames detected in babble (sensitivity > 0.65)");
    }
    SUITE_END();

    // Stationary fan noise is the classic negative case: VAD must reject it.
    SUITE_BEGIN("vad_specificity_fan");
    if (speech_fan == nullptr) {
        s.skip("scenario missing");
    } else {
        std::vector<float> prob = vad_probabilities(speech_fan->mic0);
        const size_t frame = 256;
        const size_t settled = kSampleRate / frame;
        size_t tn = 0, fp = 0;
        for (size_t f = settled; f < prob.size(); f++) {
            size_t speech_samples = 0;
            for (size_t n = 0; n < frame; n++)
                if (f * frame + n < speech_fan->speech_labels.size() &&
                    speech_fan->speech_labels[f * frame + n] > 0.5f)
                    speech_samples++;
            const bool truth = speech_samples > frame / 2;
            const bool detected = prob[f] >= 0.5f;
            if (!truth && !detected)
                tn++;
            else if (!truth && detected)
                fp++;
        }
        const double specificity = static_cast<double>(tn) / std::max<size_t>(1, tn + fp);
        CHECK_MSG(specificity > 0.6, "fan-noise frames rejected (specificity > 0.6)");
    }
    SUITE_END();

    // Pure silence: probability must stay low after settling.
    SUITE_BEGIN("vad_silence");
    {
        std::vector<int16_t> silence(kSampleRate * 3, 0);
        std::mt19937 rng(137);
        std::uniform_int_distribution<int> dither(-3, 3);
        for (auto &v : silence)
            v = static_cast<int16_t>(dither(rng));
        std::vector<float> prob = vad_probabilities(silence);
        double max_after_settle = 0.0;
        for (size_t f = kSampleRate / 256; f < prob.size(); f++)
            max_after_settle = std::max(max_after_settle, static_cast<double>(prob[f]));
        CHECK_MSG(max_after_settle < 0.5, "silence not detected as speech");
    }
    SUITE_END();

    // Speech alone: probability must rise during speech.
    SUITE_BEGIN("vad_speech_detection");
    if (doubletalk == nullptr) {
        s.skip("scenario missing");
    } else {
        std::vector<float> prob = vad_probabilities(doubletalk->near_clean);
        double max_prob = 0.0;
        double mean_active = 0.0;
        size_t active_frames = 0;
        for (size_t f = 0; f < prob.size(); f++) {
            max_prob = std::max(max_prob, static_cast<double>(prob[f]));
            // frames overlapping speech in the ground truth
            size_t speech_samples = 0;
            for (size_t n = 0; n < 256; n++)
                if (f * 256 + n < doubletalk->speech_labels.size() &&
                    doubletalk->speech_labels[f * 256 + n] > 0.5f)
                    speech_samples++;
            if (speech_samples > 128) {
                mean_active += prob[f];
                active_frames++;
            }
        }
        mean_active /= std::max<size_t>(1, active_frames);
        CHECK_MSG(max_prob > 0.7, "VAD fires on clean speech");
        CHECK_MSG(mean_active > 0.3, "mean probability high during speech");
    }
    SUITE_END();

    // Threshold ctl: a stricter start threshold reduces detected frames.
    SUITE_BEGIN("vad_threshold_effect");
    if (near_quiet == nullptr) {
        s.skip("scenario missing");
    } else {
        size_t previous_count = near_quiet->mic0.size(), first_count = 0;
        std::vector<int32_t> reference_prob;
        for (int threshold : {0, 20, 90, 100}) {
            auto *pre = speex_preprocess_state_init(256, kSampleRate);
            int enabled = 1;
            speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_VAD, &enabled);
            speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_PROB_START, &threshold);
            // Isolate threshold sensitivity from the default 20% latch.
            speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &threshold);
            size_t detections = 0;
            bool same_probability = true;
            for (size_t off = 0; off + 256 <= near_quiet->mic0.size(); off += 256) {
                std::vector<int16_t> block(near_quiet->mic0.begin()+off, near_quiet->mic0.begin()+off+256);
                const int detected = speex_preprocess_run(pre, block.data());
                int32_t probability = 0;
                speex_preprocess_ctl(pre, SPEEX_PREPROCESS_GET_PROB, &probability);
                if (threshold == 0) reference_prob.push_back(probability);
                else same_probability &= probability == reference_prob[off/256];
                if (off >= kSampleRate && detected)
                    detections++;
            }
            CHECK_MSG(same_probability, "threshold changes decisions, not probability estimates");
            CHECK_MSG(detections <= previous_count, "raising threshold cannot add detections");
            previous_count = detections;
            if (threshold == 0) first_count = detections;
            speex_preprocess_state_destroy(pre);
        }
        CHECK_MSG(first_count > 0 && previous_count == 0, "0% and 100% thresholds give opposite decisions");
    }
    SUITE_END();
}
