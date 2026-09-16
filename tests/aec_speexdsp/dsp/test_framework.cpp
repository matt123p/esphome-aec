#include "test_framework.h"

#include <cstdio>

namespace testfw {

Suite *g_current = nullptr;

Suite::Suite(std::string name, std::function<void(Suite &)> body) : name_(std::move(name)) {
    g_current = this;
    body(*this);
    g_current = nullptr;
}

void Suite::begin(const std::string &name) {
    cases_.push_back({});
    cases_.back().name = name;
}

void Suite::end() {}

void Suite::check(bool condition, const std::string &text, const std::string &detail) {
    if (cases_.empty())
        cases_.push_back({});
    cases_.back().checks.push_back({text, condition, detail});
    if (!condition && cases_.back().status != "fail")
        cases_.back().status = "fail";
    if (cases_.back().status.empty())
        cases_.back().status = "pass";
}

void Suite::check_near(const std::string &text, double actual, double expected, double tolerance) {
    char detail[256];
    std::snprintf(detail, sizeof(detail), "actual=%.8g expected=%.8g tol=%.3g", actual, expected, tolerance);
    const bool ok = std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
    check(ok, text, detail);
}

void Suite::skip(const std::string &reason) {
    if (cases_.empty())
        cases_.push_back({});
    cases_.back().status = "skip";
    cases_.back().error = reason;
}

void Suite::fail(const std::string &text) {
    if (cases_.empty())
        cases_.push_back({});
    cases_.back().status = "fail";
    cases_.back().error = text;
}

int Runner::parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--label")
            label = next("--label");
        else if (arg == "--data-dir")
            data_dir = next("--data-dir");
        else if (arg == "--out-dir")
            out_dir = next("--out-dir");
        else if (arg == "--no-outputs")
            write_outputs = false;
        else {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    return 0;
}

int Runner::run_all(int argc, char **argv) {
    (void)argc;
    (void)argv;
    size_t pass = 0, fail = 0, skip = 0, asserts = 0;
    for (const Suite &s : suites)
        for (const TestCase &c : s.cases()) {
            asserts += c.checks.size();
            if (c.status == "pass")
                pass++;
            else if (c.status == "fail")
                fail++;
            else
                skip++;
        }
    std::printf("\n=== %s: %zu passed, %zu failed, %zu skipped (%zu checks) ===\n", label.c_str(), pass,
                fail, skip, asserts);
    if (fail > 0)
        for (const Suite &s : suites)
            for (const TestCase &c : s.cases())
                if (c.status == "fail") {
                    std::printf("FAIL %s/%s", s.name().c_str(), c.name.c_str());
                    if (!c.error.empty())
                        std::printf(" (%s)", c.error.c_str());
                    std::printf("\n");
                    for (const Check &ck : c.checks)
                        if (!ck.passed)
                            std::printf("   ! %s [%s]\n", ck.text.c_str(), ck.detail.c_str());
                }

    return fail > 0 ? 1 : 0;
}

}  // namespace testfw
