# AEC Audio for ESPHome

**Latest release: v1.01** — see the [changelog](changelog.md#v101---2026-10-11) for release notes.

Talk to your ESPHome voice assistant while it is talking back.

This repository provides an ESPHome component that brings full-duplex
audio and acoustic echo cancellation (AEC) to ESPHome. It continuously
captures microphones and a playback reference, removes the device's own
loudspeaker echo, cleans up the remaining speech, and publishes the result through
standard ESPHome `microphone` and `speaker` interfaces.

**`aec_speexdsp`** is built on the open-source
[Xiph SpeexDSP](https://github.com/xiph/speexdsp) library. Its adaptive echo
filter can run with significantly long tails (up to 16,384 samples, about
one second of echo path), which can improve echo suppression in reflective
rooms when shorter filters do not cover the path. The DSP is plain C,
with no closed-source binaries. The complete component requires ESP-IDF TDM
support and PSRAM: ESP32-P4 is tested, while ESP32-S3 needs board validation.
Original ESP32 and ESP32-S2 lack the required TDM mode.

Release `v1.01` includes optional
[GRU barge-in detection](docs/neural-barge-in.md) and independent SpeexDSP/GRU
probability sensors. Use `ref: v1.01` as shown in the installation snippet below.
The bundled voice-assistant example intentionally leaves barge-in disabled.

Hardware-reference samples reach AEC directly from the synchronized TDM capture. The optional bundled `voice_assistant` component
includes full-duplex capture and API recovery fixes; see the
[GRU integration guide](docs/neural-barge-in.md). These changes are included in `v1.01`.

## Why use it?

- **True full-duplex audio** — keep listening for speech while a response,
  alarm, or other audio is playing.
- **Acoustic echo cancellation** — reduce the device's own playback in the
  microphone stream, with filter lengths up to a full second of echo path on
  `aec_speexdsp`.
- **Cleaner recognition audio** — noise suppression, optional automatic gain
  control (with an optional reference-aware gate that avoids amplifying
  playback), and optional two-microphone beamforming.
- **Reliable wake-word hand-off** — a rolling pre-buffer preserves the start of
  the utterance while control passes from wake-word detection to the voice
  assistant.
- **Long-playback stability** — optional rate matching compensates for small,
  differences between nominal 16 kHz PCM and the actual I2S sample clock.
- **Bring-up tools** — raw-slot selection, meters, spectrum analysis,
  diagnostics, and a short capture/playback API help validate new hardware.

The component supports 16-bit, 16 kHz, four-slot TDM designs using ESP-IDF.
The reference platform is the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B).

> [!IMPORTANT]
> This is a hardware-specific component, not a drop-in replacement for every
> ESPHome I2S setup. You need synchronized microphone channels, a usable
> playback reference, and compatible TDM ADC/DAC hardware. A clean hardware
> reference gives substantially better results than a copied software playback
> stream.

## Start here

The documentation covers the complete installation and bring-up process:

- **[Overview and documentation](https://matt123p.github.io/esphome-aec/)**
- **[Waveshare 7B examples](https://matt123p.github.io/esphome-aec/examples/)** — start with the audio diagnostic, then flash the complete voice assistant.
- **[Installation and known-good configuration](https://matt123p.github.io/esphome-aec/getting-started/)**
- **[First-time board setup](https://matt123p.github.io/esphome-aec/first-time-setup/)**
- **[Hardware requirements](https://matt123p.github.io/esphome-aec/hardware/)**
- **[Architecture and rolling pre-buffer](https://matt123p.github.io/esphome-aec/architecture/)**
- **[Configuration reference](https://matt123p.github.io/esphome-aec/configuration/)**
- **[Testing and tuning](https://matt123p.github.io/esphome-aec/tuning/)**
- **[Troubleshooting and limitations](https://matt123p.github.io/esphome-aec/troubleshooting/)**

The example source files are also available directly in the repository:

- **[Audio test](examples/waveshare-7b-audio-test/)** — validate microphones, raw TDM slots, the hardware reference, playback, levels, and AEC output.
- **[Voice assistant](examples/waveshare-7b-voice-assistant/)** — a focused, fully integrated Home Assistant satellite with local wake word and a minimal touch UI.

Add the component to an ESPHome configuration with:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: v1.01
      path: src/esphome
    components: [aec_speexdsp]
```

Then follow the [installation guide](https://matt123p.github.io/esphome-aec/getting-started/)
before copying any board-specific pin or slot mapping. Start from the tested
Waveshare configuration, change one setting at a time, and verify every raw TDM
channel before tuning AEC.

Ports to other suitable ESP32 boards are welcome. Please open a pull request
with the board revision, codec details, verified slot map, and a tested
configuration so other users can reproduce it.

## License

This project is released under the [MIT License](LICENSE).
