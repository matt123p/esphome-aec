# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Unreleased

### Added

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
