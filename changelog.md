# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

### Removed

- Removed the `aec_audio` (ESP-SR-based) component. `aec_speexdsp` is now the
  only component in this repository. Configurations must switch to the
  `aec_speexdsp` hub and child platforms; ESP-SR-only options (`afe_input_format`,
  `aec_mode`, `nlp_level`, `speech_enhancement`, `wakenet`, and the unit-style
  `filter_length`) have no equivalent and must be removed.

### Changed

- Converted the Waveshare 7B audio-test and voice-assistant examples to
  `aec_speexdsp`. The audio-test example no longer includes automatic
  reference-delay calibration (not available on this engine); the tested
  `reference_delay_samples: 7` is set directly, and the DSP-output selection
  replaces the AFE-output selection in the touch UI.
- Revisited all documentation for the single-component repository.

### Added

- Added a Linux PC test suite for `aec_speexdsp` (`tests/aec_speexdsp`): the
  DSP sources are compiled on the host with small ESP-IDF shims and exercised
  for functional correctness (FFT, AEC, noise suppression, AGC, VAD,
  beamforming, the full pipeline on generated scenario corpora, robustness
  against hostile input, and allocation/leak checks) in both release and
  ASan+UBSan builds. The suite runs on every push and pull request via GitHub
  Actions.
- Added the `aec_speexdsp` component: an open-source SpeexDSP-based sibling of
  `aec_audio` providing AEC, noise suppression, AGC, and VAD, with optional
  post-AEC delay-and-sum beamforming, per-slot meters, and the same
  capture/playback and rate-matching tooling. It is now the recommended
  component: its adaptive echo filter runs with significantly longer tails
  (`256`–`16384` samples, up to about one second of echo path, versus ESP-SR's
  short unit-based filter), providing significantly better echo suppression,
  and it builds for every ESP32 variant with an FPU instead of requiring the
  ESP32-S3/P4-only ESP-SR binaries. SpeexDSP is vendored inside the component
  (BSD-licensed); the FFT is accelerated through `espressif/esp-dsp`.
- Added `playback_gain_db`, supporting digital playback attenuation from
  `-60` to `0` dB before both I2S transmission and the playback-reference tap.
- Added optional on-device analogue-reference delay calibration, enabled with
  `calibration: true`.
- Added calibration start, cancel, status, and measured-delay controls to the
  Waveshare 7B audio-test example.
- Added support for applying up to 256 samples (16 ms at 16 kHz) of delay to an
  analogue reference. Software playback references continue to support up to
  4000 samples (250 ms).

### Changed

- Moved the playback task to CPU 1 at priority 20 and lowered the CPU 0 AFE
  wrapper to priority 4, below ESP-SR's internal priority-5 worker.
- Added an explicit yield when AFE processing exceeds one frame period, reducing
  the risk of system-task starvation and watchdog resets.
- Added bounded microphone delivery catch-up after main-loop stalls so streamed
  audio can recover its real-time upload rate.
- Set the Waveshare voice-assistant filter length to the measured production
  ceiling of 8 and the calibration example to 4 for additional CPU headroom.
- Updated the Waveshare 7B audio-test and voice-assistant configurations to use
  `playback_gain_db: -12`, preventing the hardware reference from clipping when
  playback approaches full scale.
- Disabled playback resampling in the Waveshare audio-test configuration so the
  calibration probe remains at exactly 16 kHz.
- Expanded the configuration, architecture, setup, tuning, and example
  documentation for playback headroom and reference-delay calibration.

### Notes

- Calibration is intended for diagnostic firmware, never starts automatically,
  and retains its result only in RAM. Copy a verified result into
  `reference_delay_samples` for production firmware.
- Digital attenuation is necessary on the Waveshare 7B because its analogue
  reference path can clip near full-scale DAC playback even with the reference
  ADC channel at its minimum gain. AEC and AGC cannot recover clipped samples.
