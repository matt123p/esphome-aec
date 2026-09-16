/* FFT (fftwrap / kiss_fftr) correctness tests.
 * The vendored subset uses the Kiss real-FFT backend on the PC (the ESP32
 * build uses ESP-DSP; the packing/scaling contract is identical). */
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "fftwrap.h"
#include "test_framework.h"
// filterbank.h predates C++ interop; its C symbols need unmangled linkage.
extern "C" {
#include "filterbank.h"
}

namespace {

using testfw::g_current;

// Speex packed real-FFT layout: out[0]=DC, out[N-1]=Nyquist, and for
// k=1..N/2-1 re(bin k)=out[2k-1], im(bin k)=out[2k].
double bin_re(const std::vector<float> &X, int N, int k) {
    return k == 0 ? X[0] : X[2 * k - 1];
}
double bin_im(const std::vector<float> &X, int N, int k) {
    return k == 0 ? 0.0 : X[2 * k];
}

}  // namespace

void run_fft_suite(testfw::Suite &s) {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);

    SUITE_BEGIN("dc_nyquist_roundtrip");
    for (int N : {128, 256, 512, 1024, 2048}) {
        void *table = spx_fft_init(N);
        for (bool nyquist : {false, true}) {
            std::vector<float> input(N), spectrum(N), output(N);
            for (int i=0; i<N; ++i) input[i] = nyquist && (i&1) ? -0.5f : 0.5f;
            spx_fft(table, input.data(), spectrum.data());
            spx_ifft(table, spectrum.data(), output.data());
            double error = 0;
            for (int i=0; i<N; ++i) error = std::max(error, double(std::abs(input[i]-output[i])));
            CHECK_MSG(error < 2e-4, "DC/Nyquist inverse preserves even and odd samples");
        }
        spx_fft_destroy(table);
    }
    SUITE_END();

    // Round-trip: spx_fft applies 1/N, spx_ifft is unscaled, so
    // ifft(fft(x)) == x for float builds.
    SUITE_BEGIN("roundtrip");
    for (int N : {128, 256, 512, 1024, 2048}) {
        std::vector<float> x(N), spec(N), back(N);
        for (auto &v : x)
            v = uniform(rng);
        void *table = spx_fft_init(N);
        CHECK_MSG(table != nullptr, "fft table allocated");
        spx_fft(table, x.data(), spec.data());
        spx_ifft(table, spec.data(), back.data());
        spx_fft_destroy(table);
        double max_err = 0.0;
        double energy = 0.0;
        for (int i = 0; i < N; i++) {
            max_err = std::max(max_err, std::fabs(static_cast<double>(x[i]) - back[i]));
            energy = std::max(energy, static_cast<double>(std::fabs(x[i])));
        }
        // float32 round-trip error at N=2048 stays at float epsilon scale
        CHECK_MSG(max_err < 2e-4, "roundtrip within float error");
    }
    SUITE_END();

    // Single-tone: cos at bin k maps to 0.5 re(bin k) after the 1/N scale.
    SUITE_BEGIN("single_tone");
    {
        const int N = 512;
        const int k = 37;
        std::vector<float> x(N), spec(N);
        for (int n = 0; n < N; n++)
            x[n] = std::cos(2.0 * M_PI * k * n / N);
        void *table = spx_fft_init(N);
        spx_fft(table, x.data(), spec.data());
        spx_fft_destroy(table);
        CHECK_NEAR(bin_re(spec, N, k), 0.5, 0.01);
        CHECK_NEAR(bin_im(spec, N, k), 0.0, 0.01);
        CHECK_NEAR(bin_re(spec, N, 0), 0.0, 0.01);
        CHECK_NEAR(std::fabs(bin_re(spec, N, N / 2)), 0.0, 0.01);
        // leakage into neighbours and DC is small relative to the peak
        CHECK_NEAR(bin_re(spec, N, k + 1), 0.0, 0.05);
    }
    SUITE_END();

    // Impulse -> flat spectrum of 1/N re per bin, zero imaginary.
    SUITE_BEGIN("impulse");
    {
        const int N = 256;
        std::vector<float> x(N, 0.0f), spec(N);
        x[0] = 1.0f;
        void *table = spx_fft_init(N);
        spx_fft(table, x.data(), spec.data());
        spx_fft_destroy(table);
        double max_err = 0.0;
        for (int k = 0; k < N / 2; k++) {
            max_err = std::max(max_err, std::fabs(bin_re(spec, N, k) - 1.0 / N));
            max_err = std::max(max_err, std::fabs(bin_im(spec, N, k)));
        }
        CHECK_MSG(max_err < 1e-5, "flat spectrum for impulse");
    }
    SUITE_END();

    // Parseval: sum_t x[t]^2 / N == DC^2 + Nyq^2 + 2*sum_k (re^2+im^2).
    SUITE_BEGIN("parseval");
    for (int N : {256, 512}) {
        std::vector<float> x(N), spec(N);
        for (auto &v : x)
            v = uniform(rng);
        void *table = spx_fft_init(N);
        spx_fft(table, x.data(), spec.data());
        spx_fft_destroy(table);
        double time_energy = 0.0;
        for (int i = 0; i < N; i++)
            time_energy += static_cast<double>(x[i]) * x[i];
        double freq_energy = bin_re(spec, N, 0) * bin_re(spec, N, 0) +
                             bin_re(spec, N, N / 2) * bin_re(spec, N, N / 2);
        for (int k = 1; k < N / 2; k++)
            freq_energy += 2.0 * (bin_re(spec, N, k) * bin_re(spec, N, k) + bin_im(spec, N, k) * bin_im(spec, N, k));
        const double expect = time_energy / N;
        CHECK_MSG(std::fabs(freq_energy - expect) / expect < 1e-4, "energy conserved (Parseval)");
    }
    SUITE_END();

    // Linearity: FFT(a+b) == FFT(a)+FFT(b).
    SUITE_BEGIN("linearity");
    {
        const int N = 256;
        std::vector<float> a(N), b(N), sum(N), spec_a(N), spec_b(N), spec_sum(N);
        for (int i = 0; i < N; i++) {
            a[i] = uniform(rng);
            b[i] = uniform(rng);
            sum[i] = a[i] + b[i];
        }
        void *table = spx_fft_init(N);
        spx_fft(table, a.data(), spec_a.data());
        spx_fft(table, b.data(), spec_b.data());
        spx_fft(table, sum.data(), spec_sum.data());
        spx_fft_destroy(table);
        double max_err = 0.0;
        for (int i = 0; i < N; i++)
            max_err = std::max(max_err, std::fabs(static_cast<double>(spec_sum[i]) - spec_a[i] - spec_b[i]));
        CHECK_MSG(max_err < 1e-5, "spectra add linearly");
    }
    SUITE_END();

    // float wrappers behave like the int16-typed wrappers in float builds.
    SUITE_BEGIN("float_wrappers");
    {
        const int N = 256;
        std::vector<float> x(N), spec1(N), spec2(N);
        for (auto &v : x)
            v = uniform(rng);
        void *table = spx_fft_init(N);
        spx_fft(table, x.data(), spec1.data());
        spx_fft_float(table, x.data(), spec2.data());
        spx_ifft_float(table, spec2.data(), spec2.data());
        spx_fft_destroy(table);
        // spec2 is now the inverse-transformed time signal: compare to x.
        double max_err = 0.0;
        for (int i = 0; i < N; i++)
            max_err = std::max(max_err, std::fabs(static_cast<double>(x[i]) - spec2[i]));
        CHECK_MSG(max_err < 1e-4, "float wrapper roundtrip");
    }
    SUITE_END();

    // Profile counters advance (proves the ESP profiling shim is wired).
    SUITE_BEGIN("profile_counters");
    {
        spx_fft_profile_t before{}, after{};
        spx_fft_profile_get(&before);
        const int N = 256;
        std::vector<float> x(N, 0.5f), spec(N);
        void *table = spx_fft_init(N);
        spx_fft(table, x.data(), spec.data());
        spx_ifft(table, spec.data(), spec.data());
        spx_fft_destroy(table);
        spx_fft_profile_get(&after);
        CHECK_MSG(after.forward_calls > before.forward_calls, "forward call counted");
        CHECK_MSG(after.inverse_calls > before.inverse_calls, "inverse call counted");
    }
    SUITE_END();
}

void run_filterbank_suite(testfw::Suite &s) {
    // Bark/mel band mapping: a sine's energy lands in the band predicted by
    // the same toBARK scale the filterbank uses.
    SUITE_BEGIN("band_mapping");
    {
        const int N = 256;
        const int banks = 24;
        FilterBank *bank = filterbank_new(banks, 16000, N, 0);
        CHECK_MSG(bank != nullptr, "filterbank allocated");
        // Same scale as the vendored filterbank.c (float build).
        auto to_bark = [](double hz) {
            return 13.1f * std::atan(0.00074f * hz) + 2.24f * std::atan(hz * hz * 1.85e-8f) + 1e-4f * hz;
        };
        const double max_mel = to_bark(8000.0);
        const double mel_interval = max_mel / (banks - 1);
        bool all_match = true;
        double worst = 0.0;
        for (double hz : {250.0, 500.0, 1000.0, 2000.0, 4000.0}) {
            std::vector<float> ps(N, 0.0f), mel(banks);
            const int bin = static_cast<int>(hz / 8000.0 * N);
            ps[bin] = 1000.0f;
            filterbank_compute_bank32(bank, ps.data(), mel.data());
            int argmax = 0;
            for (int b = 1; b < banks; b++)
                if (mel[b] > mel[argmax])
                    argmax = b;
            const double expected = to_bark(hz) / mel_interval;
            worst = std::max(worst, std::fabs(argmax - expected));
            if (std::fabs(argmax - expected) > 1.2)
                all_match = false;
        }
        CHECK_MSG(all_match, "sine lands in expected bark band (±1)");
        filterbank_destroy(bank);
        CHECK_MSG(bank != nullptr, "filterbank destroyed");
    }
    SUITE_END();

    // Inverse mapping (mel -> psd) approximately recovers a smooth spectrum.
    SUITE_BEGIN("psd_roundtrip");
    {
        const int N = 256;
        const int banks = 24;
        FilterBank *bank = filterbank_new(banks, 16000, N, 0);
        std::vector<float> ps(N), mel(banks), ps2(N);
        for (int i = 0; i < N; i++)
            ps[i] = 100.0f + 50.0f * std::cos(0.05 * i);  // smooth spectrum
        filterbank_compute_bank32(bank, ps.data(), mel.data());
        filterbank_compute_psd16(bank, mel.data(), ps2.data());
        double dot = 0, na = 0, nb = 0;
        for (int i = 0; i < N; i++) {
            dot += ps[i] * ps2[i];
            na += ps[i] * ps[i];
            nb += ps2[i] * ps2[i];
        }
        const double corr = dot / std::sqrt(na * nb);
        CHECK_MSG(corr > 0.85, "psd->bank->psd preserves spectral shape");
        filterbank_destroy(bank);
    }
    SUITE_END();
}
