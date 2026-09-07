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
- **Speech sounds chopped or metallic:** reduce NLP aggressiveness, shorten the
  filter, test with noise suppression/speech enhancement disabled, and check
  that the ADC is not clipping.
- **Crackles or gaps:** enable `diagnostics`, reduce AFE cost, disable spectrum
  metering, and look for I2S errors, underruns, dropped frames, or excessive
  processing time.
- **Failure to allocate:** confirm PSRAM is enabled and working; remove meters,
  use `fd_low_cost`, and reduce other memory-heavy features.
- **Long playback drifts:** enable `resampler`; if the input is not nominally
  16 kHz PCM, convert it before it reaches this component.
- **Wake word fails only during playback:** first validate AEC independently,
  then run ESPHome Micro Wake Word from `cleaned_microphone`. Leave the
  experimental ESP-SR `wakenet` option off unless its model path is intentionally
  integrated and tested.

For hardware bring-up problems (blank display, undetected codecs, silent
slots, a reference that behaves like a microphone), see the troubleshooting
section of [First-Time Board Setup]({{ '/first-time-setup/' | relative_url }}).

## Current limitations

- ESP-SR AFE exposes configuration choices that are not composable in every
  combination. Some syntactically valid configurations are rejected during AFE
  creation or return a feed/fetch layout this component cannot use. Start from
  the known-good configuration and tune incrementally.
- ESP32-S3 and ESP32-P4 only; ESP-IDF only.
- Four-slot Philips TDM, 16-bit PCM, and 16 kHz are fixed.
- Exactly two microphone inputs and one mono AFE output.
- A single component owns one paired I2S RX/TX peripheral.
- Manual reference delay/gain/acoustic tuning; no automatic calibration.
- No runtime selection of task core/priority or ESP-SR version.
- Capture controls and VAD state are C++ APIs rather than polished YAML actions
  or entities.
- No automatic recovery from persistent I2S or AFE failure.
- AEC cannot fully model loudspeaker nonlinearities or rescue clipped signals.

## References

- [ESP-SR 2.4.6 component registry](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/readme?language=en)
- [ESP-SR changelog](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/changelog?language=en)
- [Espressif AFE framework and input formats](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html)
- [Espressif full-duplex AEC modes, NLP, and resource data](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html)
