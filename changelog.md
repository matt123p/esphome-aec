# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

### Added

- Added a reference-aware AGC gate under `agc.gate`. During playback, AGC boost
  is permitted only after the suppressed pre-AGC signal satisfies the configured
  level and timing thresholds. The gate never mutes audio or resets the AEC.
- Added `agc.max_gain`, a configurable cap on AGC boost.
- Added `AGC_GATE` telemetry for reference activity, eligibility, startup-guard
  state, pre-AGC RMS, and applied gain.

### Changed

- **Breaking:** `agc` is now a mapping containing `enabled`, `max_gain`,
  `target_level`, and the optional `gate`. Replace `agc: true` with
  `agc: {enabled: true}` and move the former top-level `agc_target_level` value
  to `agc.target_level`.
- Playback rate matching now uses only measured I2S timing as its correction
  target. Bursty host delivery throughput remains diagnostic information and no
  longer changes playback pitch.
- Made playback task affinity safe on single-core ESP32-S2 targets; dual-core
  targets continue to place playback on CPU 1 and DSP processing on CPU 0.
- Aligned the Waveshare examples and documentation on a 1,024-sample filter,
  retaining CPU and internal-RAM headroom for their complete workloads.

## 2026-09-18

### Added

- Added `aec_speexdsp`, an open-source SpeexDSP-based AEC component with noise
  suppression, AGC, VAD, optional post-AEC beamforming, diagnostics, meters,
  profiling, capture/playback tools, and playback clock-drift compensation.
- Added a Linux host test suite covering FFT, AEC, preprocessing, beamforming,
  generated pipeline scenarios, hostile input, and allocation/leak checks in
  release and sanitizer builds.
- Added digital playback attenuation with `playback_gain_db` and analogue or
  software-reference delay via `reference_delay_samples`.

### Changed

- Converted the Waveshare 7B audio-test and voice-assistant examples to
  `aec_speexdsp`.
- Made `aec_speexdsp` the only AEC component in this repository and updated the
  documentation for its sample-based echo-filter length, performance settings,
  beamforming, and Speex preprocessor controls.

### Removed

- Removed the ESP-SR-based `aec_audio` component. Configurations must migrate
  the hub and child platforms to `aec_speexdsp` and remove ESP-SR-only options
  such as `afe_input_format`, `aec_mode`, `nlp_level`, `speech_enhancement`, and
  `wakenet`.
- Removed the old automatic reference-delay calibration. SpeexDSP reference
  delay is measured manually and configured with `reference_delay_samples`.

## 2026-09-14

### Added

- Added `playback_gain_db` to the former `aec_audio` component to prevent the
  Waveshare hardware reference from clipping near full-scale playback.
- Added the former diagnostic analogue-reference delay calibration workflow.

### Changed

- Split playback and AEC work across CPU cores and added bounded microphone
  delivery catch-up after main-loop stalls.
