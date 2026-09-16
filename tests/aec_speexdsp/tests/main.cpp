/* Entry point: registers all suites and dispatches CLI arguments. */
#include "dsp_pipeline.h"
#include "test_framework.h"

void run_fft_suite(testfw::Suite &s);
void run_filterbank_suite(testfw::Suite &s);
void run_aec_suite(testfw::Suite &s, const std::vector<dsp::Scenario> &scenarios);
void run_noise_suppression_suite(testfw::Suite &s, const std::vector<dsp::Scenario> &scenarios);
void run_agc_suite(testfw::Suite &s, const std::vector<dsp::Scenario> &scenarios);
void run_vad_suite(testfw::Suite &s, const std::vector<dsp::Scenario> &scenarios);
void run_beamformer_suite(testfw::Suite &s);
void run_pipeline_suite(testfw::Suite &s, const std::vector<dsp::Scenario> &scenarios, bool write_outputs,
                        const std::string &out_dir);
void run_robustness_suite(testfw::Suite &s);
void run_memory_suite(testfw::Suite &s);

int main(int argc, char **argv) {
    using namespace testfw;
    Runner runner;
    // Suites execute during construction; parse paths/options before them.
    if (int error = runner.parse_args(argc, argv))
        return error;

    runner.suites.emplace_back("fft", [](Suite &s) { run_fft_suite(s); });
    runner.suites.emplace_back("filterbank", [](Suite &s) { run_filterbank_suite(s); });
    runner.suites.emplace_back("beamformer", [](Suite &s) { run_beamformer_suite(s); });

    // Scenario-driven suites need the generated test data.
    std::vector<dsp::Scenario> scenarios = dsp::load_scenarios(runner.data_dir);
    if (scenarios.empty()) {
        std::fprintf(stderr,
                     "warning: no scenarios loaded from %s/manifest.json; scenario suites will be skipped\n",
                     runner.data_dir.c_str());
    }

    runner.suites.emplace_back("aec", [&scenarios](Suite &s) { run_aec_suite(s, scenarios); });
    runner.suites.emplace_back("noise_suppression", [&scenarios](Suite &s) {
        run_noise_suppression_suite(s, scenarios);
    });
    runner.suites.emplace_back("agc", [&scenarios](Suite &s) { run_agc_suite(s, scenarios); });
    runner.suites.emplace_back("vad", [&scenarios](Suite &s) { run_vad_suite(s, scenarios); });
    runner.suites.emplace_back("pipeline", [&scenarios, &runner](Suite &s) {
        run_pipeline_suite(s, scenarios, runner.write_outputs, runner.out_dir);
    });
    runner.suites.emplace_back("robustness", [](Suite &s) { run_robustness_suite(s); });
    runner.suites.emplace_back("memory", [](Suite &s) { run_memory_suite(s); });

    return runner.run_all(argc, argv);
}
