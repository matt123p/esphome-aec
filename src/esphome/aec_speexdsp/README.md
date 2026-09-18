# AEC SpeexDSP for ESPHome

`aec_speexdsp` is an ESPHome voice-assistant audio component that provides
acoustic echo cancellation (AEC), noise suppression (NS), automatic gain
control (AGC) and voice activity detection (VAD) using the open-source
[Xiph SpeexDSP](https://github.com/xiph/speexdsp) library, as packaged by
[ESP32-SpeexDSP](https://github.com/rjsachse/ESP32-SpeexDSP).

It is plain C built from the vendored SpeexDSP sources, so it compiles for
every ESP32 variant (best with a hardware FPU: ESP32, S2, S3, P4). Single- and
multi-microphone operation are supported: each microphone receives echo
cancellation, and enabling this component's optional post-AEC beamformer aligns
and combines multiple microphones into one enhanced output. Upstream SpeexDSP
does not provide that beamformer itself, and this component has no WakeNet
stage; use ESPHome Micro Wake Word with the cleaned microphone output instead.

> [!IMPORTANT]
> This is a hardware-specific external component. It
> requires ESP-IDF, a 16 kHz / 16-bit, four-slot I2S/TDM audio front end
> (capture ADC plus playback DAC sharing one bus), and a usable AEC reference
> (an analog loopback slot or the digital playback buffer).

## At a glance

| Feature | This component |
| --- | --- |
| AEC engine | SpeexDSP `mdf.c` (open source, vendored) |
| Noise suppression / AGC / VAD | SpeexDSP preprocessor |
| Echo filter length | `256`–`16384` samples (up to ~1 s of echo tail) |
| Dual-microphone | Independent per-channel processing; optional post-AEC delay-and-sum beamforming |
| Wake word | None; feed the cleaned microphone to ESPHome Micro Wake Word |
| Supported chips | Any ESP32 with enough CPU/RAM (best with an FPU: ESP32, S2, S3, P4) |
| Processing cost | Scales with `filter_length`; floating-point |

## Which SpeexDSP pieces are used — and which are deliberately not

ESP32-SpeexDSP packages seven facilities. This component uses them where they
fit the pipeline and keeps its existing machinery where that is
already better suited:

| ESP32-SpeexDSP facility | Used | Reason |
| --- | --- | --- |
| **AEC** (`speex_echo_*`, `mdf.c`) | Yes | The core replacement for the ESP-SR AEC. |
| **NS + AGC** (`speex_preprocess_*`) | Yes | Runs on the AEC output; also applies residual-echo suppression by consuming the echo state (`SPEEX_PREPROCESS_SET_ECHO_STATE`). |
| **VAD** (`speex_preprocess_*`) | Yes | Free with the preprocessor; replaces the ESP-SR/AFE VAD signal. Exposed via `get_vad_state()` / `get_vad_probability()`. |
| **Jitter buffer** (`jitter_buffer_*`) | No | It solves packet-arrival jitter for RTP/network audio. This component's timing problems are handled by the microphone's pre-roll/utterance FIFO and the playback prebuffering, which are matched to the I2S/HA-streaming failure modes. |
| **Resampler** (`speex_resampler_*`) | No | The `resampler: true` option uses the component's adaptive drift compensator, which continuously retunes its ratio from measured I2S write timing. `speex_resampler` is a (higher quality, heavier) fixed-ratio converter and cannot track drift without being reconfigured every adjustment. |
| **Ring buffer** (`speex_buffer_*`) | No | The ESPHome `ring_buffer` component is used instead — it wraps the same FreeRTOS ring buffer but is thread-safe across the three tasks involved and integrates with ESPHome tooling. |
| **G.711 / RTP** | No | Out of scope for an assistant satellite. |

## Audio pipeline

```text
TDM ADC (4 interleaved RX slots, 16-bit / 16 kHz)
  -> select microphone_slots and (optionally) the analog reference slot
     or read the mono reference from the playback ring buffer
  -> apply reference_delay (analog slot) / pre-filled silence (playback)
  -> per processed microphone channel:
       speex_echo_cancellation()          (linear echo removal)
       speex_preprocess_run()             (NS + residual echo + AGC + VAD)
  -> combine channels (first / second / mixed)
  -> cleaned 16 kHz mono PCM
  -> pre-roll and live microphone ring buffers
  -> ESPHome microphone consumers

ESPHome speaker (16-bit, 16 kHz, mono/stereo)
  -> optional drift-compensating resampler
  -> one-second+ playback ring buffer (rebuffering on underrun)
  -> place samples in tx_slots
  -> four-slot full-duplex TDM TX -> DAC -> amplifier -> speaker
```

Processing runs on a dedicated task pinned to core 0 at priority 4. Playback
runs at priority 20 on core 1 for dual-core targets and core 0 on single-core
ESP32-S2. An over-budget frame yields so system work can run; diagnostics show
the processing peak, and a resulting transport overrun may later appear as an
`rx_error`.

The audio task logs its own load every five seconds at INFO level:

```text
DSP load: 313 frames in 5002 ms: processing avg 2210 us/frame (13.8% of real time), peak 4980 us (31.1%), frame budget 16000 us
```

The percentage is the share of one core used to keep up with 16 kHz: the
measured busy time per frame divided by the frame's real-time duration
(`frame_size` / 16 kHz). Load scales with `filter_length`, `frame_size`,
channel count (`output_channel: mixed` roughly doubles it) and the meters;
values near 100% mean the canceller can no longer keep up and frames will be
dropped. At startup the component also logs where the canceller state was
allocated:

```text
SpeexDSP state memory: 118 KB internal, 0 KB PSRAM
```

### Performance checklist

The engine is plain C compiled from source, so build settings matter:

1. **Compile with `-O2`, not `-Os`.** ESPHome's default is size optimization
   (`CONFIG_COMPILER_OPTIMIZATION_SIZE`), which makes the floating-point DSP
   loops several times slower. Add to the `esp32:` framework
   `sdkconfig_options` (both keys are required — they form a Kconfig choice
   and the size default must be explicitly cleared):
   ```yaml
   sdkconfig_options:
     CONFIG_COMPILER_OPTIMIZATION_SIZE: "n"
     CONFIG_COMPILER_OPTIMIZATION_PERF: "y"
   ```
2. **Keep the canceller state in internal RAM.** The allocator prefers
   internal RAM and falls back to PSRAM automatically; the startup memory log
   confirms the split. PSRAM-resident state measurably slows the frame loop —
   if the log shows PSRAM usage, reduce `filter_length` until the state fits.
3. Run the chip at its maximum `cpu_frequency`.
4. **The FFT runs on ESP-DSP.** `fftwrap.c` uses the target-optimized
   `aes3`/`arp4` kernels. P4 float arithmetic uses scalar FPU instructions
   and hardware loops, not packed floating-point SIMD. An N-point real FFT
   uses an N/2-point complex FFT: radix-4 for power-of-four complex lengths,
   radix-2 otherwise. Both are ESP-DSP paths; unsupported window sizes are
   rejected rather than falling back to KISS. The inverse uses conjugation.
   Kernel tables are initialized for up to 1024 complex points, covering
   the maximum supported 2048-point real window.
5. Only then reach for smaller `filter_length` values.

Echo cancellation state is per microphone. Without beamforming,
`output_channel: first` (default) and `second` run one canceller; `mixed` runs
two and publishes their average. With `beamforming.enabled: true`, every
configured microphone has its own adaptive echo filter inside one Speex
multichannel MDF state. The state shares the far-end FFT, reference history,
power estimate, and adaptation bookkeeping. An adaptive delay-and-sum
beamformer then produces one mono stream for a single Speex preprocessor.
Keeping nonlinear suppression after summation preserves the inter-microphone
phase information used for localization and avoids running the relatively
expensive preprocessor once per microphone.

The localizer is allocation-free fixed-point code: normalized time-domain
cross-correlation, Q15 parabolic sub-sample peak interpolation, and 1/8 delay
smoothing. Localization runs only every `update_frames`; the per-frame delay
stage uses Q15 linear interpolation. Invalid, weak, or ambiguous peaks retain
the previous stable direction rather than steering on noise.

## Reference audio

Two reference sources are supported:

- `reference_source: analog_slot` — the board routes the DAC/amp signal into
  an ADC slot. Preferred when available; `reference_delay_samples` (0–256)
  trims the fixed slot-to-acoustics offset.
- `reference_source: playback` — a mono copy of accepted speaker PCM feeds the
  canceller from a ring buffer. Easier, but it cannot model DAC/amplifier/
  speaker delay, gain and distortion; expect weaker cancellation. Use
  `reference_delay_samples` (0–4000) to line the copy up with the echo.

Physical audio quality still matters: a distorting speaker or a buzzing
enclosure produces harmonics that no reference-based canceller can remove.

## Installation and setup

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: main
      path: src/esphome
    components: [aec_speexdsp]
```

The ES7210 support used by the Waveshare examples is a separate external
component; follow the repository's
[installation guide](../../../docs/getting-started.md) for its current source.

For local development:

```yaml
external_components:
  - source:
      type: local
      path: src/esphome
    components: [aec_speexdsp]
```

SpeexDSP itself is vendored inside the component (BSD-licensed; see the file
headers). The component automatically adds the `espressif/esp-dsp` IDF managed
component used by its accelerated FFT; no Arduino library is required.

The complete known-good P4 configurations are the Waveshare
[audio test](../../../examples/waveshare-7b-audio-test/audio-test.yaml) and
[voice assistant](../../../examples/waveshare-7b-voice-assistant/voice-assistant.yaml).
The following is a compact ESP32-S3 starting point; replace its pins and codec
configuration for the actual board:

```yaml
esp32:
  board: esp32-s3-devkitc-1
  variant: ESP32S3
  framework:
    type: esp-idf

psram:
  mode: octal
  speed: 80MHz

i2c:
  - id: bus_a
    sda: GPIO8
    scl: GPIO9
    frequency: 400kHz

audio_adc:
  - platform: es7210
    id: audio_adc_es7210
    i2c_id: bus_a
    address: 0x40
    bits_per_sample: 16bit
    sample_rate: 16000
    mic_gain: 30db
    tdm: true

audio_dac:
  - platform: es8311
    id: audio_dac_es8311
    i2c_id: bus_a
    address: 0x18
    bits_per_sample: 16bit
    sample_rate: 16000
    use_mclk: true

aec_speexdsp:
  id: speexdsp_main
  audio_adc: audio_adc_es7210
  mclk_pin: GPIO2
  bclk_pin: GPIO17
  lrclk_pin: GPIO45
  din_pin: GPIO16
  dout_pin: GPIO15
  i2s_port: 0
  tdm_slots: 4
  microphone_slots: [0]
  reference_source: playback
  reference_delay_samples: 1632
  tx_slots: [0, 1]
  frame_size: 256
  filter_length: 2048
  agc:
    enabled: true
  noise_suppression: true
  vad: true
  resampler: true

microphone:
  - platform: aec_speexdsp
    id: cleaned_microphone
    aec_speexdsp_id: speexdsp_main

speaker:
  - platform: aec_speexdsp
    id: full_duplex_speaker
    aec_speexdsp_id: speexdsp_main
    audio_dac: audio_dac_es8311
```

Do not configure another component to own the same I2S peripheral or pins. The
`audio_adc` object only configures the codec; this component creates and owns
the paired ESP-IDF TDM RX/TX channels.

## Configuration reference

### `aec_speexdsp` hub

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `id` | yes | — | Hub ID referenced by the child platforms. |
| `audio_adc` | yes | — | Configured ESPHome `audio_adc` ID. |
| `mclk_pin` / `bclk_pin` / `lrclk_pin` / `din_pin` / `dout_pin` | yes | — | I2S pins; this component is the bus master. |
| `i2s_port` | no | `0` | ESP-IDF I2S port (`0`–`2`; chip-dependent). |
| `tdm_slots` | no | `4` | Number of TDM slots; currently exactly `4`. |
| `microphone_slots` | no | `[0, 1]` | One to four distinct RX slots (`0`–`3`). Physical slot numbers are mapped to dense internal channel storage. |
| `reference_source` | no | `analog_slot` | `analog_slot` or `playback`. |
| `reference_slot` | no | `2` | RX slot for the analog reference; must differ from the microphone slots. |
| `reference_delay_samples` | no | `0` | `0`–`4000`; `0`–`256` with `analog_slot`. 16 samples = 1 ms at 16 kHz. |
| `tx_slots` | no | `[0, 1]` | Two TDM TX slots consumed by the DAC (mono is duplicated). |
| `diagnostic_raw_slot` | no | disabled | Publish raw RX slot `0`–`3` instead of DSP output; for slot mapping. |
| `frame_size` | no | `256` | Processing frame in samples; power of two: `128`, `256`, `512`, or `1024`. 256 = 16 ms. |
| `filter_length` | no | `2048` | AEC tail length in samples (`256`–`16384`, ≥ `frame_size`). 2048 ≈ 128 ms of echo path. |
| `output_channel` | no | `first` | `first`, `second`, or `mixed` (average; requires two microphone slots). |
| `beamforming` | no | disabled | Adaptive delay-and-sum configuration below. Requires at least two microphone slots and supersedes `output_channel`. |
| `noise_suppression` | no | `true` | Speex preprocessor denoise. |
| `noise_suppression_level_db` | no | `15` | Maximum attenuation in dB (`5`–`60`). Higher absorbs more noise — and more speech. |
| `agc.enabled` | no | `true` | Speex automatic gain control. |
| `agc.max_gain` | no | `12` | Maximum AGC boost in dB (`0`–`60`). A cap, not a fixed gain; `0` prevents boost but not attenuation. |
| `agc.target_level` | no | `0.25` | Target level as a fraction of full scale (`0.01`–`1.0`). `0.25` ≈ −12 dBFS. |
| `agc.gate` | no | disabled | Reference-aware boost protection; see the [AGC gate](#reference-aware-agc-gate) section. |
| `vad` | no | `true` | Speex voice activity detection on the processed channel. |
| `vad_threshold` | no | `35` | Speech-start probability in percent (`20`–`90`). The speech-continue threshold stays at Speex's 20 %. |
| `echo_suppress_db` | no | `40` | Residual-echo suppression (dB) applied by the preprocessor during far-end-only audio (`5`–`60`). |
| `echo_suppress_active_db` | no | `15` | Residual-echo suppression (dB) during double-talk. Lower preserves near-end speech. |
| `playback_gain_db` | no | `0` | Digital attenuation (`-60`–`0` dB) before the I2S TX and the reference tap. |
| `resampler` | no | `false` | Enable the playback drift-compensating resampler around 16 kHz. |
| `meters` | no | disabled | Compile an `AECSpeexDspMetersComponent`; accepts a nested component `id` and `enabled` (default `true`). Exposes per-slot RMS/peak, reference and cleaned-output levels, clipping/alternation stats, and a UI-gated 32-bin spectrum. |
| `telemetry` | no | `false` | Compile `AEC_EFFECT` logging and one-second signal-stage diagnostics. |
| `profiling` | no | `false` | Compile detailed per-stage CPU-cycle profiling counters. |
| `slot_logs` | no | `false` | Compile five-second raw-slot and output level logging. |
| `diagnostics` | no | `false` | Compile five-second error, timing, buffer, and VAD statistics. |

### `beamforming`

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable post-AEC adaptive delay-and-sum beamforming. |
| `max_lag` | `3` | Maximum TDOA in samples (`1`–`8`). Set from microphone spacing; at 16 kHz, 50 mm is about 2.3 samples. |
| `update_frames` | `8` | Re-localization interval in processing frames. Eight 256-sample frames is 128 ms. |
| `min_rms` | `120` | Minimum input RMS accepted by the localizer. |
| `min_correlation_percent` | `50` | Minimum normalized positive correlation for a valid delay. |
| `min_peak_dominance_percent` | `5` | Required margin over the best non-adjacent correlation peak. |

### Microphone child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_speexdsp`. |
| `id` | yes | — | ESPHome microphone ID. |
| `aec_speexdsp_id` | yes | — | Parent hub ID. |
| `bits_per_sample` / `num_channels` / `sample_rate` | no | `16` / `1` / `16000` | Fixed values. |

### Speaker child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_speexdsp`. |
| `id` | yes | — | ESPHome speaker ID. |
| `aec_speexdsp_id` | yes | — | Parent hub ID. |
| `audio_dac` | hardware-dependent | — | DAC used by ESPHome's speaker setup. |
| `bits_per_sample` / `num_channels` / `sample_rate` | no | `16` / `1` (or `2`) / `16000` | Fixed values. |

### C++ / lambda API

```yaml
esphome:
  on_boot:
    then:
      - lambda: |-
          id(speexdsp_main).start_capture();      // record 3 s of cleaned mono
```

- `start_capture(frames)`, `get_capture_state()`, `get_capture_frames()`,
  `play_capture()` — record the cleaned stream to PSRAM and queue it back
  through the speaker (delay/reference tuning aid).
- `get_vad_state()` / `get_vad_probability()` — current speech activity of the
  primary processed channel.
- `reset_audio_activity()` / `audio_silent_for(ms)` — inactivity
  meters for barge-in policies (`VOICE_ASSISTANT_BARGE_IN`
  is honoured).

## Tuning

### Reference-aware AGC gate

Normal AGC remains configurable as `agc: {enabled: true, max_gain: 12, target_level: 0.25}`.
`agc.gate` limits amplification during playback without deleting audio.
The former hard output gate has been removed; legacy `playback_gate` YAML
blocks must be removed from configurations.

The 1024 common configuration now selects reference-aware AGC gating:

```yaml
agc:
  enabled: true
  max_gain: 12
  target_level: 0.25
  gate:
    enabled: true
    reference_open_rms: 200
    reference_close_rms: 100
    open_rms: 24
    close_rms: 12
    open_delay_ms: 32
    hold_ms: 250
    tail_ms: 250
    release_ms: 150
    startup_guard_ms: 200
```

Reference activity uses actual AEC reference RMS, not queue occupancy. Below the
reference closing threshold for `tail_ms`, AGC operates normally, even for quiet
near-end speech. During reference activity, sustained suppressed pre-AGC RMS
above `open_rms` permits normal AGC. Below `close_rms` for `hold_ms`, boost fades
toward unity over `release_ms`; existing attenuation is preserved. The closed
gate freezes loudness adaptation, never replaces samples with zeros, and does
not reset the AEC. Quiet residual echo can therefore still reach the remote end.
On reference activation, `startup_guard_ms` (default 200, 0 disables) immediately
caps AGC gain at unity while preserving existing attenuation and blocks eligibility
for that duration. AEC continues adapting and audio is not muted. Confirmation
starts afresh afterwards. This prevents retained AGC boost and loud onset echo
from immediately reopening eligibility; it does not guarantee removal of unboosted
echo. Genuine speech at playback onset also passes without boost during the guard.
The guard re-arms only after reference silence has satisfied `tail_ms`, not on
brief gaps or underruns. Durations round up to audio-frame boundaries; output
overlap-add can retain samples from the preceding frame.
`AGC_GATE` telemetry reports reference activity, eligibility, `startup_guard`, and
pre-AGC RMS. Its once-second snapshots may miss a short guard interval.
The component default for this optional gate is disabled (speech thresholds
64/32); the 1024 experiment explicitly keeps the previously selected 24/12.

Gate levels are windowed RMS PCM counts from the suppressed spectrum before AGC;
reference levels are time-domain PCM RMS. Both threshold pairs require
0 < close < open <= 32768. Delays are integer milliseconds from 0 to 60000.

With beamforming enabled, the residual-echo estimate is formed from raw and
AEC-cleaned audio using identical steering and independent delay histories.
The raw companion path does not perform localization; it reuses the current
cleaned-audio delays and existing frame scratch storage. MDF also supplies its
evolving removed-echo estimate before the global adaptation flag is set.
This avoids an empty startup estimate, but does not guarantee immediate echo
removal before the adaptive filter has learned the acoustic path.

1. Map slots with `diagnostic_raw_slot` and `slot_logs: true`.
2. Fix the reference (source, gain via `playback_gain_db`, delay via
   `reference_delay_samples`). The capture/play-back buttons are useful for
   measuring the delay by cross-correlation.
3. Enable `telemetry: true`. During speaker-only playback, inspect
   `AEC_STAGE_MIC` (raw and immediate post-AEC RMS), then `AEC_STAGE_PRE`
   (preprocessor input/output RMS, last-frame AGC gain in dB and speech
   probability). With beamforming enabled its output is preprocessor input 0;
   otherwise each preprocessor is reported separately. `AEC_STAGES` reports
   final output RMS/peak, full-scale sample count and the number of frames
   with reference RMS above 500. One-second windows include pauses/tails.
   Levels are not latency-aligned ERLE; AGC/probability are snapshots, not
   window averages. Legacy `AEC_EFFECT` compares raw inputs with the same
   final mono output: positive dB means amplification, and its zero-lag
   correlation does not compensate processing delay. Verify double-talk
   survival by listening to the cleaned stream while a response plays.
4. Tune in this order: `noise_suppression_level_db`, `echo_suppress_db` /
   `echo_suppress_active_db`, `filter_length` (longer tails cancel longer
   rooms but cost CPU and memory), `agc.target_level`.
5. Without beamforming, try `output_channel: second` (or `mixed`) if the first
   microphone is weaker. With beamforming, inspect the periodic `Beamformer
   profile` TDOA/confidence log and tune `max_lag` from the physical spacing
   before relaxing the confidence thresholds.

## Residual echo investigation

For a local barge-in listening test, flash `esp_1024_speexdsp_test.yaml`, select
**DSP OUTPUT**, start **PLAY TEST AUDIO**, then press **RECORD 3 SECONDS** and
speak over playback. Wait for the recording-ready message, then press
**PLAY RECORDING** (this stops and clears test playback before replaying).
Repeat once while silent and once with normal-volume speech. Capture copies
the same final PCM buffer published to microphone listeners, after suppression
and AGC, but does not include transport or remote processing. Selecting a raw
source deliberately bypasses DSP; use DSP OUTPUT for this comparison.
The test configuration matches the hall DSP settings, including reference-aware
AGC gating and the 200 ms startup guard.

With `telemetry: true`, `AEC_RESIDUAL last_frame` reports a snapshot alongside
the one-second RMS summaries. `removed_rms` is the two-frame removed-echo history;
`leak` is MDF's estimated leakage, and `adapted` is its convergence flag, not a
speech detector. The residual power multiplier is `min(1, 2*leak)`.

`input_ps`, fresh `residual_ps`, smoothed `echo_ps`, `noise_ps`, and
`suppressed_ps` are sums of spectral power bins excluding Nyquist, not PCM RMS.
`suppressed_ps` is before AGC. `Pframe` selects between the configured inactive
and active echo suppression floors; `echo_floor_db` is that interpolated floor,
not the attenuation actually achieved. These snapshots can miss brief events.

For a silent-listener playback test, a small residual estimate despite substantial
post-AEC input suggests an echo-model/leakage problem. A high `Pframe` selecting a
weak floor suggests residual echo is being treated as near-end activity. Low
suppressed power followed by high final RMS points toward AGC amplification.
Repeat with actual near-end speech and beamforming off to distinguish these cases.
Do not classify echo using quietness or speech probability alone.

Beamforming uses matching raw and cleaned steering histories to construct the
removed-echo estimate. However, the leakage estimate still comes from the shared
multichannel MDF model, not a separately learned beam-output model. Its accuracy
after steering is a remaining hypothesis to evaluate, not a proven defect.
The echo and preprocessor analysis windows differ, so power ratios are diagnostic
indicators, not an exact echo-only probability. Diagnostic sums run only when
queried; they do not add an FFT or change AGC/gating policy.

## Current limitations

- Delay-and-sum beamforming improves coherent near-field speech pickup but is
  not source separation.
- No wake-net (feed the cleaned microphone to ESPHome Micro Wake Word instead).
- Floating-point build: best on ESP32/S2/S3/P4 (hardware FPU). RISC-V
  variants (C3/C5/C6) run it in slow software float.
- Large `filter_length` values allocate the canceller history from PSRAM when
  available (with an internal-RAM fallback); boards without PSRAM should keep
  `filter_length` modest.
- Four-slot Philips TDM, 16-bit PCM, and 16 kHz are fixed.
- Manual reference delay/gain tuning; no automatic calibration (measure the
  delay with the capture API or the audio-test page).

## References

- [SpeexDSP (Xiph)](https://github.com/xiph/speexdsp) — BSD license; the
  vendored `mdf.c`, `preprocess.c`, `fftwrap.c`, `filterbank.c`,
  `kiss_fft*.c` and headers are from this library.
- [ESP32-SpeexDSP (rjsachse)](https://github.com/rjsachse/ESP32-SpeexDSP) —
  the ESP32 port this component vendors: `config.h`, `os_support*.h` and the
  floating-point/Kiss-FFT build configuration.
