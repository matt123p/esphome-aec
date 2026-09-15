---
title: Configuration Reference
---

# Configuration Reference

Start from the [known-good configuration]({{ '/getting-started/' | relative_url }})
and change one setting at a time.

## `aec_speexdsp` hub

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `id` | yes | — | Hub ID referenced by the child platforms. |
| `audio_adc` | yes | — | Configured ESPHome `audio_adc` ID. It only initializes the codec; this component creates and owns the paired I2S RX/TX channels. |
| `mclk_pin` / `bclk_pin` / `lrclk_pin` / `din_pin` / `dout_pin` | yes | — | I2S pins; this component is the bus master. MCLK/BCLK/LRCLK/DOUT are outputs, DIN is an input. RX and TX share the clocks so capture, reference, and playback stay synchronized. |
| `i2s_port` | no | `0` | ESP-IDF I2S port (`0`–`2`; chip-dependent). |
| `tdm_slots` | no | `4` | Number of TDM slots; currently exactly `4`. |
| `microphone_slots` | no | `[0, 1]` | One to four distinct RX slots (`0`–`3`). Physical slot numbers are mapped to dense internal channel storage. |
| `reference_source` | no | `analog_slot` | `analog_slot` reads the loopback from an ADC slot (preferred); `playback` uses a mono copy of accepted speaker PCM. |
| `reference_slot` | no | `2` | RX slot for the analog reference; must differ from the microphone slots. |
| `reference_delay_samples` | no | `0` | `0`–`4000` with `playback`; `0`–`256` with `analog_slot`. 16 samples = 1 ms at 16 kHz. SpeexDSP also adapts over a range of delays itself, so this only needs to be roughly right. |
| `tx_slots` | no | `[0, 1]` | Two TDM TX slots consumed by the DAC (mono is duplicated; stereo maps L/R). |
| `diagnostic_raw_slot` | no | disabled | Publish raw RX slot `0`–`3` instead of DSP output; for slot mapping. Also settable at runtime via `set_diagnostic_raw_slot(n)` (`-1` restores DSP output). |
| `frame_size` | no | `256` | Processing frame in samples; power of two, `128`–`1024`. 256 = 16 ms. Larger frames amortize FFT overhead but increase latency. |
| `filter_length` | no | `2048` | AEC tail length in samples (`256`–`16384`, ≥ `frame_size`). This is the main quality knob: see [choosing an echo filter length](#choosing-an-echo-filter-length). |
| `output_channel` | no | `first` | `first`, `second`, or `mixed` (average; requires two microphone slots and runs two cancellers). Superseded by `beamforming`. |
| `beamforming` | no | disabled | Post-AEC adaptive delay-and-sum configuration below. Requires at least two microphone slots. |
| `noise_suppression` | no | `true` | Speex preprocessor denoise (stationary noise: hiss, fan, hum). |
| `noise_suppression_level_db` | no | `15` | Maximum attenuation in dB (`5`–`60`). Higher absorbs more noise — and more speech. The residual-echo suppressor shares this gain machinery. |
| `agc` | no | `true` | Speex automatic gain control on the cleaned output. |
| `agc_target_level` | no | `0.25` | Target level as a fraction of full scale (`0.01`–`1.0`). `0.25` ≈ −12 dBFS. Values near `1.0` clip on loud syllables. |
| `vad` | no | `true` | Speex voice activity detection on the processed channel. |
| `vad_threshold` | no | `35` | Speech-start probability in percent (`20`–`90`). The speech-continue threshold stays at Speex's 20 %. |
| `echo_suppress_db` | no | `40` | Residual-echo suppression (dB) applied by the preprocessor during far-end-only audio (`5`–`60`). |
| `echo_suppress_active_db` | no | `15` | Residual-echo suppression (dB) during double-talk. Lower preserves near-end speech. |
| `playback_gain_db` | no | `0` | Digital attenuation (`-60`–`0` dB) before the I2S TX and the reference tap. Use it to prevent DAC/amplifier/reference-loopback clipping. |
| `resampler` | no | `false` | Enable the playback drift-compensating resampler around 16 kHz. |
| `meters` | no | disabled | Compile an `AECSpeexDspMetersComponent`; accepts a nested component `id` and `enabled` (default `true`). Exposes per-slot RMS/peak, reference and cleaned-output levels, clipping/alternation stats, and a UI-gated 32-bin spectrum. |
| `telemetry` | no | `false` | Compile periodic `AEC_EFFECT` attenuation/correlation logging. |
| `profiling` | no | `false` | Compile per-stage frame profiling counters. |
| `slot_logs` | no | `false` | Compile five-second raw-slot and output level logging. |
| `diagnostics` | no | `false` | Compile five-second error, timing, buffer, and VAD statistics. |

### `beamforming`

| Option | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable post-AEC adaptive delay-and-sum beamforming. Each microphone retains its own stable echo path; localization runs on echo-cancelled signals and one shared preprocessor runs after the aligned sum. |
| `max_lag` | `3` | Maximum TDOA in samples (`1`–`8`). Set from microphone spacing: at 16 kHz, sound travels about one sample per 21 mm (50 mm ≈ 2.3 samples, 135 mm ≈ 7 samples). |
| `update_frames` | `8` | Re-localization interval in processing frames. Eight 256-sample frames is 128 ms. |
| `min_rms` | `120` | Minimum input RMS accepted by the localizer. |
| `min_correlation_percent` | `50` | Minimum normalized positive correlation for a valid delay. |
| `min_peak_dominance_percent` | `5` | Required margin over the best non-adjacent correlation peak. Invalid, weak, or ambiguous peaks retain the previous stable direction rather than steering on noise. |

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
second of echo path — providing significantly better echo suppression in
reflective rooms. The tail must cover
speaker-plus-room decay plus the reference offset, or residual echo remains
after the canceller converges.

| Room / echo path | Recommended value | Tail time |
| --- | ---: | ---: |
| Very close, well-damped speaker setup | `512` | ~32 ms |
| Small rooms, direct speaker-to-microphone path | `1024` | ~64 ms |
| Typical indoor echo tail (default) | `2048` | ~128 ms |
| Large or reflective rooms, long playback-path latency | `4096` | ~256 ms |

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
   ESP32-S3 and ESP32-P4 its float FFT uses the chip's SIMD instructions;
   other targets fall back to ANSI C. `spx_ifft` is built from the forward
   transform via the re/im-swap identity.
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
- `reset_audio_activity()` / `audio_silent_for(ms)` — inactivity meters for
  barge-in policies (`VOICE_ASSISTANT_BARGE_IN` is honoured).

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

Enable rate matching when Home Assistant playback is nominally 16 kHz but long
responses slowly underrun or overrun:

```yaml
aec_speexdsp:
  # ...
  resampler: true
```

The component measures the sustained rate at which the host's PCM is accepted
and gently resamples it to the satellite's fixed 16 kHz TDM clock. Correction is
automatic: there is no rate or ratio to configure. The estimator uses
multi-second windows, limits its target to 15–17 kHz, and changes the active
rate one hertz at a time to avoid abrupt pitch or timing changes. Its last
learned rate is reused at the beginning of the next playback stream.

This option is for small clock and delivery-rate differences, not source-format
conversion. The speaker input must remain signed 16-bit, nominally 16 kHz PCM.
See [How It Works]({{ '/architecture/' | relative_url }}#playback-automatic-rate-matching-and-reference)
for the control flow and [Testing & Tuning]({{ '/tuning/' | relative_url }}#automatic-rate-matching)
for validation.

