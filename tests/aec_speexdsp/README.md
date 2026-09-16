# aec_speexdsp PC test suite

Functional tests for the vendored SpeexDSP DSP code (`mdf.c`, `preprocess.c`,
the FFT wrappers, and `beamformer.cpp`). The suite runs on any Linux-like host
(WSL included); it checks that the DSP behaves correctly — echo cancellation,
noise suppression, AGC, VAD, beamforming, robustness against hostile input,
and allocation correctness — not how fast it runs.

## What it covers

| Suite | Checks |
| --- | --- |
| `fft` / `filterbank` | forward/inverse FFT round-trips and the filterbank analysis/synthesis |
| `aec` | ERLE on synthetic and real far-end material, double-talk survival, delay identification, canceller reset behaviour |
| `noise_suppression` | noise-only attenuation, speech preservation, transient-burst robustness |
| `agc` | gain toward target, clipping avoidance, level tracking |
| `vad` | speech/noise discrimination and state transitions |
| `beamformer` | TDOA estimation, array gain, interference rejection, stability at boundaries |
| `pipeline` | the full component pipeline on generated scenario corpora with ground-truth labels |
| `robustness` | silence, clipping, square waves, NaN-adjacent inputs, state reuse |
| `memory` | allocation accounting: no leaks, PSRAM/internal-RAM splits as expected |

## How it works

The component sources are written for ESP-IDF (ESP-DSP FFT, `heap_caps`
allocator, cycle counters). The CMake build copies them into `build/`, adds a
generated PC `config.h` that selects the vendored Kiss FFT backend, and
compiles them with small shims (`shim/`). `shim/pc_mem.cpp` hooks the Speex
allocator so `memory` tests can account every live allocation.

Test signals and ground-truth label tracks are generated deterministically
(fixed seeds) by `tools/generate_signals.py` into `test_data/`.

## Running

Requires `cmake`, a C/C++ toolchain, and Python 3 with `numpy`:

```sh
tests/aec_speexdsp/run_tests.sh
```

The script builds and runs two variants and exits non-zero on any failure:

- **Release (`-O2`)** — functional correctness;
- **ASan + UBSan** — memory and undefined-behaviour correctness (slower).

Set `PYTHON=/path/to/python` to choose the Python interpreter, `BUILD_DIR` to
choose the build location. Test logs land in `build/out/`.

`test_data/` and `build/` are generated and not tracked in git.
