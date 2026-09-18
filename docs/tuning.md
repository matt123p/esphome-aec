---
title: Testing & Tuning
---

# Testing & Tuning

A staged process for verifying a working installation and tuning the echo
cancellation pipeline. Start from the
[known-good configuration]({{ '/getting-started/' | relative_url }}) and change
one setting at a time.

## 1. Verify startup

Compile and flash over USB for the first bring-up, then inspect logs.

With `aec_speexdsp`, a healthy startup reports paired TDM initialization,
creation of both audio tasks, and the canceller memory placement:

```text
SpeexDSP state memory: 118 KB internal, 0 KB PSRAM
```

If the state lands in PSRAM, reduce `filter_length` until it fits in internal
RAM — PSRAM-resident state measurably slows the frame loop. Once running, the
audio task logs its load every five seconds:

```text
DSP load: 313 frames in 5002 ms: processing avg 2210 us/frame (13.8% of real time), peak 4980 us (31.1%), frame budget 16000 us
```

## 2. Map TDM slots

Temporarily enable:

```yaml
aec_speexdsp:
  # ...
  diagnostic_raw_slot: 0
  slot_logs: true
```

Speak close to each microphone and play a tone. Repeat slots `0` through `3`.
Use the logs/listening tests to identify both microphone slots and, if present,
the analog reference. Update `microphone_slots` and `reference_slot`, then
remove `diagnostic_raw_slot` so the published stream comes from the DSP.

## 3. Verify full-duplex routing

Play a known 16-bit/16 kHz WAV through the component's speaker while listening
to or recording the component's microphone. Check that:

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
4. Watch `Playback rate adjust` for the measured I2S rate and the gradually
   adjusted playback rate. `Playback input rate` is informational only —
   bursty offered/accepted throughput is normal and does not move the target.
5. Confirm playback remains continuous and that underrun, TX-error, and dropped
   frame counters do not continually rise.

A settled playback rate slightly above or below 16 kHz is expected; it
indicates that the component is adding or removing a small number of
interpolated samples to keep the one-second playback buffer stable. The
correction is retained for the next response and refined as new measurements
arrive.

Rate matching is not a cure for the host sending the wrong format, large network
gaps, an overloaded device, or persistent I2S errors. Convert all input to
signed 16-bit, nominally 16 kHz PCM before it reaches the speaker. If the
playback rate repeatedly settles at the 15 kHz or 17 kHz clamp, measure the
board's actual audio clocks rather than treating the limit as normal drift.

## 4. Prevent reference clipping

Clipping must be fixed before delay or DSP tuning. A reference channel that
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
The analog reference arrives in the same TDM frame as the microphones, so the
delay only compensates the residual amplifier/loopback group delay — on the
reference Waveshare 7B this measured 7 samples. To measure it on another board,
play a repeatable signal, record a raw microphone slot (the capture API or the
audio-test example both work), and cross-correlate the recording with the
source signal. SpeexDSP also adapts over a range of delays itself, so the
configured value only needs to be roughly right.

## 6. Measure cancellation and double-talk

Enable `telemetry: true`, play a repeatable speech/noise clip, and compare its
`AEC_EFFECT` lines. Effective cancellation normally reduces cleaned/reference
correlation and reduces cleaned RMS during playback-only sections. Also speak
while playback continues: a setting that removes echo but destroys near-end
speech is not acceptable full-duplex performance.

Tune in this order:

1. correct slot mapping and unclipped ADC/DAC levels;
2. correct reference source, `playback_gain_db`, and
   `reference_delay_samples` (the capture/play-back API is useful for
   measuring the delay by cross-correlation);
3. `noise_suppression_level_db`;
4. `echo_suppress_db` / `echo_suppress_active_db` — higher values remove more
   residual echo; lower `echo_suppress_active_db` preserves double-talk;
5. `filter_length` — the main quality knob. The complete Waveshare examples
   start at `1024` for workload headroom; `2048` is the component default. Try
   longer tails only when measurements show they are needed and the DSP/memory
   budget allows. SpeexDSP's own adaptive delay
   search means the reference delay only needs to be roughly right;
6. `agc.target_level`, then optional meters and resampling.

With the AGC gate enabled, verify its decisions during playback: enable
`telemetry: true` and watch the once-second `AGC_GATE` line. Quiet speech that
cannot acquire boost during playback means `open_rms` is too high or
`open_delay_ms`/`startup_guard_ms` too long for the material; residual echo
that acquires boost means raise `open_rms`, `open_delay_ms`, or
`startup_guard_ms`. Set `reference_open_rms` above the measured idle floor of
the reference slot, and lengthen `tail_ms` if protection releases during
quiet playback tails. Remember the gate controls boost only: it never mutes,
and it cannot remove echo that passes at unity gain.

Test at several playback volumes and distances. Include playback-only, speech-
only, and simultaneous speech/playback cases, then test the real wake-word and
voice-assistant hand-off. Objective telemetry is useful, but listening and
recognition success during double-talk are the final quality tests.

With beamforming enabled, inspect the periodic `Beamformer profile`
TDOA/confidence log and set `max_lag` from the physical microphone spacing
(about one sample per 21 mm at 16 kHz) before relaxing the confidence
thresholds. Without beamforming, try `output_channel: second` (or `mixed`) if
the first microphone is weaker.

### Validate filter length against real-time throughput

Increase `filter_length` only after routing, reference level/delay, and the
lower-cost settings work. For each candidate value:

1. Cold boot several times and confirm there is no startup watchdog reset.
2. Stream microphone audio for at least a minute while the display, network,
   wake-word engine, and playback path are active.
3. Confirm that five seconds of mono 16-bit/16 kHz audio produces 160,000 bytes
   of microphone data. A materially lower sustained rate means the system is
   falling behind even if speech still sounds plausible.
4. Watch `max_processing_us`, RX/TX errors, dropped frames, and microphone queue
   overflow logs with diagnostics enabled. On `aec_speexdsp`, watch the
   five-second `DSP load` line: the average percentage is the share of one
   core needed to keep up, and values near 100% mean frames will be dropped.
   Also confirm the startup memory log still reports the canceller state in
   internal RAM.
5. Repeat playback-only and double-talk tests. Retain the longer filter only if
   cancellation improves without harming voice delivery or stability.

If a longer filter is required, first reduce other CPU costs — for example
spectrum metering, display work, or other optional processing — then repeat the
full throughput test.
