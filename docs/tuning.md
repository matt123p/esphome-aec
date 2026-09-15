---
title: Testing & Tuning
---

# Testing & Tuning

A staged process for verifying a working installation and tuning the AFE
pipeline. Start from the [known-good configuration]({{ '/getting-started/' | relative_url }}) and change one setting at a
time.

## 1. Verify startup

Compile and flash over USB for the first bring-up, then inspect logs. A healthy
startup reports paired TDM initialization, the final enabled AFE stages, an AFE
feed shape matching `MMR` or `MMNR`, one fetch channel, and creation of both
audio tasks. Treat allocation failures, `ESP-SR rejected the AFE configuration`,
or `Unsupported ESP-SR AFE shape` as setup failures rather than tuning issues.

## 2. Map TDM slots

Temporarily enable:

```yaml
aec_audio:
  # ...
  diagnostic_raw_slot: 0
  slot_logs: true
```

Speak close to each microphone and play a tone. Repeat slots `0` through `3`.
Use the logs/listening tests to identify both microphone slots and, if present,
the analog reference. Update `microphone_slots` and `reference_slot`, then
remove `diagnostic_raw_slot` so the published stream comes from the AFE.

## 3. Verify full-duplex routing

Play a known 16-bit/16 kHz WAV through the `aec_audio` speaker while listening
to or recording the `aec_audio` microphone. Check that:

- playback is clean and reaches the intended DAC channels;
- microphone capture continues throughout playback;
- the reference meter is active during playback;
- `rx_errors`, `tx_errors`, underruns, and dropped frames do not continually
  increase when `diagnostics: true` is enabled.

If long-form audio slowly underruns or overruns while short clips work, follow
the automatic rate-matching test below.

### Automatic rate matching

The host and satellite have independent clocks. To test whether their small
sustained difference is causing playback drift:

1. Enable `resampler: true` and `diagnostics: true`.
2. Play continuous, nominally 16 kHz PCM for at least 30 seconds. Several
   minutes is better for exposing a small mismatch.
3. Confirm the log initially reports `Playback resampler enabled` at 16000 Hz.
4. Watch `Playback input rate` for the offered and accepted host rates.
5. Watch `Playback rate adjust` for the measured TDM rate, estimated host input,
   target, and gradually adjusted playback rate.
6. Confirm playback remains continuous and that underrun, TX-error, and dropped
   frame counters do not continually rise.

The host estimate needs at least three seconds before it becomes active. A
settled value slightly above or below 16 kHz is expected; it indicates that the
component is adding or removing a small number of interpolated samples to keep
the one-second playback buffer stable. The correction is retained for the next
response and refined when new audio arrives.

Rate matching is not a cure for the host sending the wrong format, large network
gaps, an overloaded device, or persistent I2S errors. Convert all input to
signed 16-bit, nominally 16 kHz PCM before it reaches the speaker. If the
estimated rate repeatedly reaches the 15 kHz or 17 kHz limit, investigate the
source and transport rather than treating the limit as normal clock drift.

## 4. Prevent reference clipping

Clipping must be fixed before delay or AFE tuning. A reference channel that
reaches digital full scale has lost information, even if its ADC gain is already
at minimum. Use `playback_gain_db` to attenuate media before it reaches both the
DAC and reference tap. The Waveshare 7B examples use `-12` dB because its
hardware loopback clips when the DAC is driven close to full scale. Re-test the
speaker and raw reference at the loudest intended setting after changing it.

## 5. Align the reference

With `reference_source: playback`, the reference must line up with the echo at
the microphones. `reference_delay_samples` pre-fills the reference ring buffer
with silence, delaying the reference relative to playback:

```text
delay_ms = reference_delay_samples / 16
samples  = delay_ms * 16
```

Useful starting points are 512 samples (32 ms), 1024 (64 ms), 1600 (100 ms),
and 2048 (128 ms). Play a deterministic, non-repeating calibration signal,
record a raw microphone slot, and cross-correlate the recording with the source
WAV. Use the correlation peak as the initial delay, then sweep nearby values
while measuring and listening.

For an analogue reference, delay may be adjusted from 0 to 256 samples (16 ms).
The Waveshare audio-test example can determine a useful value automatically:

1. Flash the audio-test example and leave the board in its normal enclosure and
   position.
2. In Home Assistant press **Auto-tune AEC (keep silent)** and remain silent for
   roughly 30–60 seconds while its bounded broadband probe plays.
3. Watch **AEC calibration status** and **AEC calibrated reference delay**. The
   test rejects clipped/quiet inputs, weak correlation, processing failures,
   boundary results, and timeouts.
4. Confirm a retained result with playback-only and double-talk tests, then copy
   the reported sample value into `reference_delay_samples` in production YAML.

The result is RAM-only and calibration never starts automatically. The routine
tests zero plus candidate delays derived from both microphones, then retains a
delay only when a repeat trial reduces correlated leakage without materially
increasing total residual output. Press **Cancel AEC auto-tune** to stop the
probe and restore the pre-test delay.

## 6. Measure cancellation and double-talk

Enable `telemetry: true`, play a repeatable speech/noise clip, and compare its
`AEC_EFFECT` lines. Effective cancellation normally reduces cleaned/reference
correlation and reduces cleaned RMS during playback-only sections. Also speak
while playback continues: a setting that removes echo but destroys near-end
speech is not acceptable full-duplex performance.

Tune in this order:

1. correct slot mapping and unclipped ADC/DAC levels;
2. correct reference source and delay;
3. `nlp_level`, starting with `normal` or `aggressive`;
4. `filter_length`, using `8` for the Waveshare voice assistant and `4` while
   running its calibration build; longer filters must be validated against
   watchdog stability and sustained microphone-upload throughput;
5. `fd_high_perf` only if the low-cost pipeline is stable and insufficient;
6. optional noise suppression, speech enhancement, AGC, meters, and resampling.

Test at several playback volumes and distances. Include playback-only, speech-
only, and simultaneous speech/playback cases, then test the real wake-word and
voice-assistant hand-off. Objective telemetry is useful, but listening and
recognition success during double-talk are the final quality tests.

### Validate filter length against real-time throughput

Increase `filter_length` only after routing, reference level/delay, and the
lower-cost AFE settings work. For each candidate value:

1. Cold boot several times and confirm there is no startup watchdog reset.
2. Stream microphone audio for at least a minute while the display, network,
   wake-word engine, and playback path are active.
3. Confirm that five seconds of mono 16-bit/16 kHz audio produces 160,000 bytes
   of microphone data. A materially lower sustained rate means the system is
   falling behind even if speech still sounds plausible.
4. Watch `max_processing_us`, RX/TX errors, dropped frames, and microphone queue
   overflow logs with diagnostics enabled.
5. Repeat playback-only and double-talk tests. Retain the longer filter only if
   cancellation improves without harming voice delivery or stability.

The Waveshare reference firmware uses `4` during calibration and `8` for the
voice assistant. Treat those as workload-specific tested settings. If a longer
filter is required, first reduce other CPU costs—for example spectrum metering,
calibration analysis, display work, or speech enhancement—then repeat the full
throughput test.
