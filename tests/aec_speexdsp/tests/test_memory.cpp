/* Memory analysis: exact allocation accounting via the speex allocator
 * hooks, footprint measurement, and reset stability. */
#include <vector>

#include "dsp_pipeline.h"
#include "pc_mem.h"
#include "test_framework.h"

namespace {

using namespace dsp;

}  // namespace

void run_memory_suite(testfw::Suite &s) {
    // Footprint of the component-default pipeline states (what start_dsp_
    // allocates on the ESP32).
    SUITE_BEGIN("footprint");
    {
        pc_mem::reset();
        {
            PipelineConfig cfg;  // component defaults, single channel
            Pipeline pipeline(cfg);
            const pc_mem::Stats st = pc_mem::stats();
            CHECK_MSG(st.live_bytes > 100 * 1024, "single-channel state allocated (>100 KB expected)");
            CHECK_MSG(st.live_bytes < 4096 * 1024, "single-channel state fits comfortably in RAM");
        }
        pc_mem::reset();
        {
            PipelineConfig cfg;
            cfg.beam.enabled = true;
            cfg.beam.microphones = 2;
            Pipeline pipeline(cfg);
            const pc_mem::Stats st = pc_mem::stats();
        }
        pc_mem::reset();
        {
            PipelineConfig cfg;
            cfg.beam.enabled = true;
            cfg.beam.microphones = 4;
            Pipeline pipeline(cfg);
            const pc_mem::Stats st = pc_mem::stats();
            CHECK_MSG(st.live_bytes < 8192 * 1024, "4-mic state under 8 MB (PSRAM budget)");
        }
        pc_mem::reset();
    }
    SUITE_END();

    // Init/destroy symmetry across the raw library API.
    SUITE_BEGIN("init_destroy_symmetry");
    {
        const int configs[][2] = {{256, 2048}, {128, 512}, {512, 4096}, {1024, 8192}, {256, 256}};
        for (const auto &c : configs) {
            pc_mem::Stats before = pc_mem::stats();
            SpeexEchoState *echo = speex_echo_state_init(c[0], c[1]);
            SpeexPreprocessState *pre = speex_preprocess_state_init(c[0], 16000);
            SpeexEchoState *mc = speex_echo_state_init_mc(c[0], c[1], 4, 2);
            pc_mem::Stats live = pc_mem::stats();
            CHECK_MSG(live.live_bytes > before.live_bytes, "allocations accounted while states alive");
            speex_echo_state_destroy(echo);
            speex_preprocess_state_destroy(pre);
            speex_echo_state_destroy(mc);
            pc_mem::Stats after = pc_mem::stats();
            CHECK_MSG(after.live_bytes == before.live_bytes && after.live_blocks == before.live_blocks,
                      "init/destroy balanced");
        }
    }
    SUITE_END();

    // Reset keeps the footprint constant (no hidden growth per call).
    SUITE_BEGIN("reset_stability");
    {
        SpeexEchoState *echo = speex_echo_state_init(256, 2048);
        SpeexPreprocessState *pre = speex_preprocess_state_init(256, 16000);
        int rate = 16000;
        speex_echo_ctl(echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
        pc_mem::Stats before = pc_mem::stats();
        std::vector<int16_t> frame(256, 1000), ref(256, 500), out(256, 0);
        for (int i = 0; i < 500; i++) {
            speex_echo_state_reset(echo);
            speex_preprocess_run(pre, frame.data());
            speex_echo_cancellation(echo, frame.data(), ref.data(), out.data());
        }
        pc_mem::Stats after = pc_mem::stats();
        speex_echo_state_destroy(echo);
        speex_preprocess_state_destroy(pre);
        CHECK_MSG(after.live_blocks == before.live_blocks, "500 reset cycles allocate nothing");
    }
    SUITE_END();

    // Pipeline-level leak check across all supported topologies.
    SUITE_BEGIN("pipeline_leak_check");
    {
        pc_mem::Stats before = pc_mem::stats();
        std::vector<int16_t> mic(16000 * 2, 0), ref(16000 * 2, 0);
        for (size_t i = 0; i < mic.size(); i++) {
            mic[i] = static_cast<int16_t>(1000 * std::sin(0.05 * i));
            ref[i] = static_cast<int16_t>(800 * std::sin(0.03 * i));
        }
        for (int variant = 0; variant < 4; variant++) {
            PipelineConfig cfg;
            cfg.beam.enabled = variant >= 2;
            cfg.beam.microphones = variant == 3 ? 4 : 2;
            cfg.output_mixed = variant == 1;
            Pipeline pipeline(cfg);
            std::vector<int16_t> planar;
            const uint8_t chans = pipeline.channels();
            planar.assign(mic.size() * chans, 0);
            for (size_t i = 0; i < mic.size(); i++)
                for (uint8_t c = 0; c < chans; c++)
                    planar[i * chans + c] = mic[i];
            pipeline.process_all(planar, ref);
        }
        pc_mem::Stats after = pc_mem::stats();
        CHECK_MSG(after.live_blocks == before.live_blocks, "all pipeline variants leak-free");
    }
    SUITE_END();
}
