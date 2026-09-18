---
title: Troubleshooting & Limitations
---

# Troubleshooting & Limitations

## Troubleshooting

- **Silence or the wrong microphone:** map every slot with
  `diagnostic_raw_slot`; do not assume the codec's channel order.
- **No cancellation:** verify that the reference contains playback, confirm
  reference polarity/gain, then measure delay. A silent or badly shifted
  reference cannot cancel echo.
- **Speech sounds chopped or metallic:** reduce `echo_suppress_db` /
  `echo_suppress_active_db`, shorten the filter, test with noise suppression
  disabled, and check that the ADC is not clipping.
- **Crackles or gaps:** enable `diagnostics`, reduce processing cost, disable
  spectrum metering, and look for I2S errors, underruns, dropped frames, or
  excessive processing time.
- **Failure to allocate:** confirm PSRAM is enabled and working; remove meters
  and reduce other memory-heavy features.
- **Long playback slowly underruns or overruns:** enable `resampler` and play a
  continuous test for at least 30 seconds. Check `Playback rate adjust` for the
  measured I2S rate and the gradually adjusted playback rate; the offered and
  accepted throughput in `Playback input rate` is informational only — bursty
  delivery is normal. If the playback rate settles at the 15–17 kHz clamp, or
  gaps remain bursty rather than gradual, fix the source or network path.
  Non-16 kHz audio must be converted before it reaches this component.
- **Speech too quiet during playback:** with the AGC gate enabled, boost is
  withheld until speech proves itself. Check `AGC_GATE` telemetry and lower
  `agc.gate.open_rms` / `open_delay_ms` / `startup_guard_ms` if genuine speech
  cannot acquire boost. The gate controls boost only; it never mutes audio.
- **Echo becomes louder during playback:** AGC is amplifying residual echo.
  Raise `agc.gate.open_rms`, `open_delay_ms`, or `startup_guard_ms`, or disable
  `agc.gate.enabled` and improve cancellation first.
- **Wake word fails only during playback:** first validate AEC independently,
  then run ESPHome Micro Wake Word from `cleaned_microphone`.

- **The `DSP load` log approaches 100%:** the canceller can no longer keep up
  with 16 kHz and frames are dropped. Compile with
  `CONFIG_COMPILER_OPTIMIZATION_PERF` (see the
  [performance checklist]({{ '/configuration/' | relative_url }}#performance-checklist)),
  run the chip at its maximum `cpu_frequency`, disable spectrum metering, and
  only then reduce `filter_length`.
- **The startup memory log shows PSRAM in use:** the canceller state did not
  fit in internal RAM, which measurably slows the frame loop. Reduce
  `filter_length` until the state fits.
- **Slow or unstable DSP despite modest settings:** confirm both
  `CONFIG_COMPILER_OPTIMIZATION_SIZE: "n"` and
  `CONFIG_COMPILER_OPTIMIZATION_PERF: "y"` are set — ESPHome's size-optimization
  default makes the floating-point loops several times slower. RISC-V targets
  (C3/C5/C6) also run this floating-point code in slow soft-float.
- **Residual echo remains at the end of loud playback:** the echo tail is
  longer than the filter. Increase `filter_length` (this is the main advantage
  of this component), or reduce `reference_delay_samples` error so the tail is
  spent on the room rather than the fixed offset.

For hardware bring-up problems (blank display, undetected codecs, silent
slots, a reference that behaves like a microphone), see the troubleshooting
section of [First-Time Board Setup]({{ '/first-time-setup/' | relative_url }}).

## Current limitations

- Floating-point build: best on ESP32/S2/S3/P4 (hardware FPU). RISC-V variants
  (C3/C5/C6) run it in slow software float.
- Load scales with `filter_length`; very long filters must be validated
  against the `DSP load` log and sustained microphone throughput. Boards
  without PSRAM should keep `filter_length` modest.
- No dual-microphone speech enhancement: channels are processed independently
  (or combined by the optional post-AEC beamformer, which improves pickup but
  is not source separation).
- No wake-net (feed the cleaned microphone to ESPHome Micro Wake Word instead).
- Manual reference delay/gain tuning; no automatic calibration (measure the
  delay with the capture API or the audio-test page).
- Four-slot Philips TDM, 16-bit PCM, and 16 kHz are fixed.
- AEC cannot fully model loudspeaker nonlinearities or rescue clipped signals.
