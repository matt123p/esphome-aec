# AEC Audio for ESPHome

Talk to your ESPHome voice assistant while it is talking back.

`aec_audio` brings full-duplex audio and Espressif's ESP-SR Audio Front-End to
ESPHome. It continuously captures two microphones and a playback reference,
removes the device's own loudspeaker echo, cleans up the remaining speech, and
publishes the result through standard ESPHome `microphone` and `speaker`
interfaces.

## Why use it?

- **True full-duplex audio** — keep listening for speech while a response,
  alarm, or other audio is playing.
- **Acoustic echo cancellation** — reduce the device's own playback in the
  microphone stream.
- **Cleaner recognition audio** — optional noise suppression, dual-microphone
  speech enhancement, and automatic gain control.
- **Reliable wake-word hand-off** — a rolling pre-buffer preserves the start of
  the utterance while control passes from wake-word detection to the voice
  assistant.
- **Long-playback stability** — optional rate matching compensates for small,
  sustained source/clock differences.
- **Bring-up tools** — raw-slot selection, meters, spectrum analysis,
  diagnostics, and a short capture/playback API help validate new hardware.

Built on [Espressif ESP-SR](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html),
the component supports 16-bit, 16 kHz, four-slot TDM designs on ESP32-S3 and
ESP32-P4 using ESP-IDF. The reference platform is the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B).

> [!IMPORTANT]
> This is a hardware-specific component, not a drop-in replacement for every
> ESPHome I2S setup. You need two synchronized microphone channels, a usable
> playback reference, PSRAM, and compatible TDM ADC/DAC hardware. A clean
> hardware reference gives substantially better results than a copied software
> playback stream.

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

- **[Audio test](examples/waveshare-7b-audio-test/)** — validate microphones, raw TDM slots, the hardware reference, playback, levels, and AFE output.
- **[Voice assistant](examples/waveshare-7b-voice-assistant/)** — a focused, fully integrated Home Assistant satellite with local wake word and a minimal touch UI.

Add the component to an ESPHome configuration with:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: main
      path: src/esphome
    components: [aec_audio]
```

Then follow the [installation guide](https://matt123p.github.io/esphome-aec/getting-started/)
before copying any board-specific pin or slot mapping. Start from the tested
Waveshare configuration, change one setting at a time, and verify every raw TDM
channel before tuning AEC.

Ports to other suitable ESP32-S3 and ESP32-P4 boards are welcome. Please open a
pull request with the board revision, codec details, verified slot map, and a
tested configuration so other users can reproduce it.

## License

This project is released under the [MIT License](LICENSE).
