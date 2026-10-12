---
title: Configuration Reference
---

# Configuration Reference

Start from the [known-good configuration]({{ '/getting-started/' | relative_url }})
and change one setting at a time.

## Hardware reference

With `reference_source: analog_slot`, reference samples are used directly.
See [reference timing]({{ '/reference-timing/' | relative_url }}) for hardware
reference capture and software playback-reference delay.

## Optional GRU barge-in

The latest component can select neural VAD only during playback with an active
voice assistant. It is disabled by default. See the
[GRU barge-in guide]({{ '/neural-barge-in/' | relative_url }}) for requirements,
options, integration and independent probability sensors. The bundled voice
assistant example does not enable this feature.

## `aec_speexdsp` hub

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `id` | yes | — | Hub ID referenced by the child platforms. |
| `audio_adc` | yes | — | Configured ESPHome `audio_adc` ID. It only initializes the codec; this component creates and owns the paired I2S RX/TX channels. |
| `mclk_pin` / `bclk_pin` / `lrclk_pin` / `din_pin` / `dout_pin` | yes | — | I2S pins; this component is the bus master. MCLK/BCLK/LRCLK/DOUT are outputs, DIN is an input. RX and TX share the clocks so capture, reference, and playback stay synchronized. |
| `i2s_port` | no | `0` | ESP-IDF I2S port (`0`–`2`; chip-dependent). |
| `tdm_slots` | no | `4` | Number of TDM slots; currently exactly `4`. |
| `microphone_slots` | no | `[0, 1]` | One to four distinct RX slots (`0`–`3`). Physical slot numbers are mapped to dense internal channel storage. |
| `reference_source` | no | `analog_slot` | `analog_slot` reads the loopback from an ADC slot (preferred); `playback` uses a mono copy of samples prepared for I2S TX, after resampling, cue mixing and digital gain. |
| `reference_slot` | no | `2` | RX slot for the analog reference; must differ from the microphone slots. |
| `reference_delay_samples` | no | `0` | Software `playback` reference only: `0`–`4000` samples. Rejected with `analog_slot`, even at zero. 16 samples = 1 ms. |
| `tx_slots` | no | `[0, 1]` | Two TDM TX slots consumed by the DAC (mono is duplicated; stereo maps L/R). |
| `diagnostic_raw_slot` | no | disabled | Publish raw RX slot `0`–`3` instead of DSP output; for slot mapping. Also settable at runtime via `set_diagnostic_raw_slot(n)` (`-1` restores DSP output). |
| `frame_size` | no | `256` | Processing frame in samples; power of two, `128`–`1024`. 256 = 16 ms. Larger frames amortize FFT overhead but increase latency. |
| `filter_length` | no | `2048` | AEC tail length in samples (`256`–`16384`, ≥ `frame_size`). This is the main quality knob: see [choosing an echo filter length](#choosing-an-echo-filter-length). |
| `output_channel` | no | `first` | `first` or `second` processes only the selected microphone (`second` requires two slots). Superseded by `beamforming`, which combines all configured microphones. |
| `beamforming` | no | disabled | Post-AEC adaptive delay-and-sum configuration below. Requires at least two microphone slots. |
| `noise_suppression` | no | `true` | Speex preprocessor denoise (stationary noise: hiss, fan, hum). |
| `noise_suppression_level_db` | no | `15` | Maximum attenuation in dB (`5`–`60`). Higher absorbs more noise — and more speech. The residual-echo suppressor shares this gain machinery. |
| `agc.enabled` | no | `true` | Speex automatic gain control on the cleaned output. |
| `agc.max_gain` | no | `12` | Maximum AGC boost in dB (`0`–`60`). A cap, not a fixed gain; `0` prevents boost but not attenuation. |
| `agc.target_level` | no | `0.25` | Target level as a fraction of full scale (`0.01`–`1.0`). `0.25` ≈ −12 dBFS. Values near `1.0` clip on loud syllables. |
| `agc.gate` | no | disabled | Reference-aware boost protection during playback; see [Reference-aware AGC gate](#reference-aware-agc-gate). |
| `vad` | no | `true` | Speex voice activity detection on the processed channel. |
| `vad_threshold` | no | `35` | Speech-start probability in percent (`20`–`90`). The speech-continue threshold stays at Speex's 20 %. |
| `reference_activity` | no | see below | Shared reference activity thresholds, tail and startup guard used by playback-aware processing. |
| `neural_barge_in` | no | disabled | Optional learned VAD during reference playback with an active assistant; see the [GRU guide]({{ '/neural-barge-in/' | relative_url }}). |
| `echo_suppress_db` | no | `40` | Residual-echo suppression (dB) applied by the preprocessor during far-end-only audio (`5`–`60`). |
| `echo_suppress_active_db` | no | `15` | Residual-echo suppression (dB) during double-talk. Lower preserves near-end speech. |
| `playback_gain_db` | no | `0` | Digital attenuation (`-60`–`0` dB) before the I2S TX and the reference tap. Use it to prevent DAC/amplifier/reference-loopback clipping. |
| `resampler` | no | `false` | Enable the playback drift-compensating resampler around 16 kHz. |
| `meters` | no | disabled | Compile an `AECSpeexDspMetersComponent`; accepts a nested component `id` and `enabled` (default `true`). Exposes per-slot RMS/peak, reference and cleaned-output levels, clipping/alternation stats, and a UI-gated 32-bin spectrum. |
| `telemetry` | no | `false` | Compile periodic `AEC_EFFECT` attenuation/correlation logging. |
| `profiling` | no | `false` | Compile per-stage frame profiling counters. |
| `slot_logs` | no | `false` | Compile five-second raw-slot and output level logging. |
| `diagnostics` | no | `false` | Compile five-second error, timing, buffer, and VAD statistics. |
| `serial_control` | no | `false` | Enable the diagnostic serial command interface. |
| `diag_capture_max_seconds` | no | `30` | Maximum diagnostic multi-channel capture duration in seconds (`1`–`120`); separate from the three-second `start_capture()` buffer. |
| `capture_only` | no | `false` | Capture diagnostic raw channels/reference while skipping DSP and microphone publication. Incompatible with enabled neural barge-in. |

### Shared reference activity

`reference_activity` defines when the captured reference counts as playback:

| Option | Default | Meaning |
| --- | --- | --- |
| `open_rms` | `200` | Linear int16 RMS threshold to activate playback state. |
| `close_rms` | `100` | Below this threshold for `tail_ms`, playback state closes. Must be below `open_rms`. |
| `tail_ms` | `250` | Quiet-reference interval before playback state closes. |
| `startup_ms` | `200` | Startup guard after reference activation. |

The corresponding `agc.gate.reference_open_rms`, `reference_close_rms`,
`tail_ms` and `startup_guard_ms` settings are legacy aliases for these shared
values. `near_end.startup_ms` is also a startup alias. Explicit conflicting
values are rejected; these are not independent controls for each feature.
RMS values accept `0.01`–`32768`, and durations accept `0`–`60000` ms.
If `near_end` is configured, startup plus onset plus two processing frames
must fit within its 512 ms pre-roll budget.

### `beamforming`

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable post-AEC adaptive delay-and-sum beamforming. Each microphone retains its own stable echo path; localization runs on echo-cancelled signals and one shared preprocessor runs after the aligned sum. |
| `max_lag` | `3` | Maximum TDOA in samples (`1`–`8`). Set from microphone spacing: at 16 kHz, sound travels about one sample per 21 mm (50 mm ≈ 2.3 samples, 135 mm ≈ 7 samples). |
| `update_frames` | `8` | Re-localization interval in processing frames. Eight 256-sample frames is 128 ms. |
| `min_rms` | `120` | Minimum input RMS accepted by the localizer. |
| `min_correlation_percent` | `50` | Minimum normalized positive correlation for a valid delay. |
| `min_peak_dominance_percent` | `5` | Required margin over the best non-adjacent correlation peak. Invalid, weak, or ambiguous peaks retain the previous stable direction rather than steering on noise. |

### Reference-aware AGC gate

`agc.gate` limits AGC boost during playback without ever muting or deleting
audio. With no active reference, AGC runs normally — even for quiet speech.
While the reference is active, only audio that sustains pre-AGC RMS above
`open_rms` keeps normal boost; ineligible audio loses boost and stops updating
the AGC loudness estimate. The gate controls boost only: residual echo can
still pass at unity gain, and the AEC keeps adapting throughout.

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable the gate. Requires `agc.enabled`. |
| `reference_open_rms` | `200` | Reference RMS at/above this marks playback active immediately. Keep above the measured idle/comfort-noise floor of the reference slot. |
| `reference_close_rms` | `100` | While active, the reference must stay below this for `tail_ms` to end protection. Between close and open the previous state persists. Must stay below `reference_open_rms`. |
| `open_rms` | `64` | During playback, pre-AGC RMS at/above this for `open_delay_ms` permits boost. This is not speech recognition. |
| `close_rms` | `32` | Once eligible, RMS below this for `hold_ms` withdraws eligibility. Between close and open, eligibility persists. Must stay below `open_rms`. |
| `open_delay_ms` | `32` | Continuous above-`open_rms` confirmation time before boost is permitted. `0` allows the first qualifying frame; no audio is discarded while waiting. |
| `hold_ms` | `250` | Eligibility survives below-`close_rms` gaps this long. `0` closes eligibility on the first below-close frame. |
| `tail_ms` | `250` | Continuous below-`reference_close_rms` time before returning to normal AGC. Covers echo tails, word gaps, and brief underruns. Expiry also re-arms the startup guard. `0` releases on the first quiet reference frame. |
| `release_ms` | `150` | After eligibility closes during playback, existing boost fades toward unity over this time; existing attenuation is preserved. `0` removes boost immediately. |
| `startup_guard_ms` | `200` | On reference activation, immediately cap gain at unity and block eligibility for this interval while the AEC keeps adapting; a fresh `open_delay_ms` confirmation is then required. Re-arms only after `tail_ms` of quiet reference — not on brief gaps or underruns. `0` disables. |

All RMS thresholds are linear int16 PCM counts, **not** dB: speech levels are
windowed RMS of the suppressed, pre-AGC signal (what `AGC_GATE` reports as
`pre_agc_rms`), and reference levels are time-domain RMS of the AEC reference.
Validation requires `0 < close < open <= 32768` for both threshold pairs, and
the gate cannot be enabled while `agc.enabled` is false. Durations are
milliseconds (`0`–`60000`), rounded up to audio-frame boundaries (16 ms at the
default `frame_size`).

With `telemetry: true`, the once-second `AGC_GATE` log line reports reference
activity, boost eligibility, `startup_guard` state, and pre-AGC RMS. Its
snapshots can miss a short guard interval.

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
| `bits_per_sample` / `num_channels` / `sample_rate` | no | `16` / `1` (or `2`) / `16000` | Fixed values. |

Configure the board's top-level `audio_dac:` separately so ESPHome initializes
the codec; the speaker child does not take an `audio_dac` option.

### Choosing an echo filter length

`filter_length` is a plain sample count of echo tail (16 samples = 1 ms at
16 kHz), not an abstract unit. It can run up to 16,384 samples — about one
second of echo path. A longer tail can help when a shorter filter does not cover
the path; it does not guarantee better suppression. The tail must cover
speaker-plus-room decay plus the reference offset, or residual echo remains
after the canceller converges.

| Room / echo path | Recommended value | Tail time |
| --- | ---: | ---: |
| Very close, well-damped speaker setup | `512` | ~32 ms |
| Small rooms, direct speaker-to-microphone path | `1024` | ~64 ms |
| Typical indoor echo tail (default) | `2048` | ~128 ms |
| Large or reflective rooms, long playback-path latency | `4096` | ~256 ms |

The complete Waveshare examples deliberately use `1024`, below the component
default, to retain headroom for beamforming, display updates, wake-word
processing, networking, meters, and profiling.

Longer filters cost CPU and memory; the canceller state prefers internal RAM
and falls back to PSRAM automatically. Validate any increase against the DSP
load log and sustained microphone throughput as described in
[Testing & Tuning]({{ '/tuning/' | relative_url }}).

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
4. **The FFT runs on ESP-DSP.** The vendored `fftwrap.c` uses the Espressif
   `espressif/esp-dsp` FFT (added automatically as a managed component): on
   ESP32-S3 uses optimized AES3 kernels and ESP32-P4 uses ARP4 kernels.
   P4 floating-point arithmetic uses a scalar FPU and hardware loops, rather
   than packed floating-point SIMD. Other targets use ANSI C kernels.
   Real transforms use a half-size complex FFT; `spx_ifft` uses inverse real
   post-processing followed by the forward complex FFT with conjugation.
5. Only then reach for smaller `filter_length` values.

### C++ / lambda API

```yaml
esphome:
  on_boot:
    then:
      - lambda: |-
          id(voice_audio).start_capture();      // record 3 s of cleaned mono
```

- `start_capture(frames)`, `get_capture_state()`, `get_capture_frames()`,
  `play_capture()` — record the cleaned stream to PSRAM and queue it back
  through the speaker (delay/reference tuning aid).
- `get_vad_state()` / `get_vad_probability()` — current speech activity of the
  primary processed channel.
- Microphone `set_response_playing(bool)` — suppress response audio and a
  300 ms tail from delivery/history in an ordinary assistant session.
- Microphone `set_barge_in_active(bool)` / `is_near_end_speech()` — arm the
  capture gate and read its speech decision for a compatible full-duplex
  integration; see the [GRU guide]({{ '/neural-barge-in/' | relative_url }}).

The optional meters component is configured on the hub:

```yaml
aec_speexdsp:
  # ...
  meters:
    id: audio_meters
```

It exposes per-slot RMS/peak, reference and cleaned-output levels,
clipping/alternation statistics, and a 32-bin spectrum. The spectrum costs CPU
in the real-time audio task and is off until explicitly enabled:

```yaml
esphome:
  on_boot:
    then:
      - lambda: id(audio_meters).set_spectrum_enabled(true);
```

## Automatic playback rate matching

Enable rate matching to compensate for a small difference between nominally
16 kHz PCM and the actual I2S playback rate. Clock-divider rounding and source
clock tolerance can make the hardware rate differ from the requested 16,000 Hz:

```yaml
aec_speexdsp:
  # ...
  resampler: true
```

The component estimates the hardware playback rate from successful blocking
I2S write durations and gently resamples the nominally 16 kHz PCM to match it.
Write timing is a proxy for the hardware rate and can also be affected by DMA
buffering and task scheduling. Correction is automatic: there is no rate or
ratio to configure. The measured rate is clamped to 15–17 kHz, and the active
correction moves toward it one hertz at a time to avoid abrupt pitch or timing
changes. Its last learned rate is reused at the beginning of the next playback
stream. Network delivery is bursty, and its throughput is deliberately not
treated as a sample rate: short-term delivery variation is absorbed by the
playback buffer rather than by changing voice pitch.

This option is for small hardware sample-rate errors, not source-format
conversion. The speaker input must remain signed 16-bit, nominally 16 kHz PCM.
See [How It Works]({{ '/architecture/' | relative_url }}#playback-automatic-rate-matching-and-reference)
for the control flow and [Testing & Tuning]({{ '/tuning/' | relative_url }}#automatic-rate-matching)
for validation.
