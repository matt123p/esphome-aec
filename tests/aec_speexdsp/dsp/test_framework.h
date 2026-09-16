/* Minimal test framework for the aec_speexdsp PC suite.
 * Collects per-test pass/fail and prints a summary; a non-zero exit code
 * signals a failure. */
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace testfw {

struct Check {
    std::string text;
    bool passed;
    std::string detail;
};

struct TestCase {
    std::string name;
    std::string status;  // "pass" | "fail" | "skip"
    std::vector<Check> checks;
    std::string error;
};

class Suite {
 public:
    Suite(std::string name, std::function<void(Suite &)> body);

    // Explicit test-case boundaries; checks attach to the open case.
    void begin(const std::string &name);
    void end();

    void check(bool condition, const std::string &text, const std::string &detail = "");
    void check_near(const std::string &text, double actual, double expected, double tolerance);
    void skip(const std::string &reason);
    void fail(const std::string &text);

    const std::vector<TestCase> &cases() const { return cases_; }
    const std::string &name() const { return name_; }

 private:
    std::string name_;
    std::vector<TestCase> cases_;
};

struct Runner {
    std::vector<Suite> suites;
    std::string label = "tests";
    std::string data_dir = "test_data";
    std::string out_dir = "out";       // directory for WAV artifacts
    bool write_outputs = true;         // write WAV artifacts

    int parse_args(int argc, char **argv);
    int run_all(int argc, char **argv);
};

// Global pointer used by the currently running suite (check macros).
extern Suite *g_current;

}  // namespace testfw

#define SUITE_BEGIN(name) testfw::g_current->begin(name)
#define SUITE_END() testfw::g_current->end()
#define CHECK(cond) testfw::g_current->check((cond), #cond)
#define CHECK_MSG(cond, msg) testfw::g_current->check((cond), msg)
#define CHECK_NEAR(actual, expected, tol) testfw::g_current->check_near(#actual, (actual), (expected), (tol))
