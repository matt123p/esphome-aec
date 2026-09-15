---
title: Overview
---

# AEC Audio for ESPHome

This repository provides an ESPHome component for voice devices that need to
keep listening while they play audio. It combines full-duplex TDM transport
with acoustic echo cancellation and exposes standard ESPHome microphone and
speaker endpoints.

- It allows the device to **play audio and listen at the same time**,
  so it can continue detecting speech or a wake word while music, an alarm, or a
  voice-assistant response is playing.

- It improves the audio sent to speech recognition by cancelling the device's
  own loudspeaker echo, reducing background noise, and applying automatic gain
  control (AGC) so quiet and loud speech arrive at a more useful level.

- Its optional rate matching compensates for small, sustained playback-rate
  differences, helping prevent under-runs or over-runs during long responses.

- Its rolling pre-buffer bridges wake-word detection and voice-assistant
  capture, preserving the beginning of the user's request when the integration
  invokes the hand-off API.

## Why SpeexDSP

`aec_speexdsp` is built on the open-source
[Xiph SpeexDSP](https://github.com/xiph/speexdsp) library, vendored inside the
component:

- **Long echo filters.** The adaptive filter runs from `256` to `16,384`
  samples — up to about one second of echo tail. The filter must span the
  speaker-plus-room decay, so long tails provide significantly better echo
  suppression in reflective rooms than short-tail engines.
- **Open source.** Plain C compiled from source: no closed binaries, and it
  builds for every ESP32 variant with an FPU (ESP32, S2, S3, P4).
- **Full preprocessing chain.** SpeexDSP noise suppression, AGC, residual-echo
  suppression with separate double-talk handling, and VAD, plus an optional
  post-AEC delay-and-sum beamformer for two-microphone boards.

The component is an ESPHome wrapper and audio transport layer. It takes care
of the full-duplex I2S/TDM hardware, prepares the captured channels for the
processing engine, configures the processing stages, and exposes the result
through standard ESPHome microphone and speaker interfaces. The microphone
output is enhanced, signed 16-bit mono PCM at 16 kHz. The speaker accepts
signed 16-bit mono or stereo PCM at 16 kHz.

> **Important**
> This is a hardware-specific external component, not a general replacement
> for ESPHome's I2S audio components. It requires synchronized microphone
> channels, a usable playback reference, and ESP-IDF.

> **Warning**
> **Begin with the known-good configuration shown in
> [Installation & Setup]({{ '/getting-started/' | relative_url }}) and change one
> setting at a time.**

For the shortest path to a working system, use the Waveshare reference board.
AEC quality depends heavily on the codec routing, reference signal, amplifier,
speaker, enclosure, and microphone placement; software cannot compensate for a
clipped or badly distorted physical audio path.

## Try it on the Waveshare 7B

Two complete Waveshare 7B configurations are ready to use:

1. Start with the **[audio-test example]({{ '/examples/' | relative_url }}#audio-test)**
   to verify every TDM channel, the hardware reference, playback, and AEC output.
2. Once the audio path passes, flash the **[voice-assistant example]({{ '/examples/' | relative_url }}#voice-assistant)**
   for a fully integrated Home Assistant satellite with a minimal touch UI.

The examples remove the guesswork from board-specific pins and codec routing
while keeping credentials and installation-specific Home Assistant behavior out
of the repository.

Ports to other suitable boards are welcome. Please contribute the exact board
revision, codec details, verified slot map, and tested configuration so other
users can reproduce the result.

## Documentation

- **[Waveshare 7B Examples]({{ '/examples/' | relative_url }})** — ready-to-run audio diagnostic and voice-assistant configurations.

- **[Installation & Setup]({{ '/getting-started/' | relative_url }})** — add the component to ESPHome and flash the known-good starting configuration.
- **[First-Time Board Setup]({{ '/first-time-setup/' | relative_url }})** — a staged hardware bring-up guide for a new board.
- **[Hardware & Audio Design]({{ '/hardware/' | relative_url }})** — supported processors, required audio hardware, references, and microphones.
- **[How It Works]({{ '/architecture/' | relative_url }})** — the processing pipeline and the capture/playback data flow.
- **[Configuration Reference]({{ '/configuration/' | relative_url }})** — every option.
- **[Testing & Tuning]({{ '/tuning/' | relative_url }})** — verify startup, map TDM slots, align references, and measure cancellation.
- **[Troubleshooting & Limitations]({{ '/troubleshooting/' | relative_url }})** — common symptoms and the current constraints.

## References

- [SpeexDSP (Xiph)](https://github.com/xiph/speexdsp) — the BSD-licensed DSP library vendored by `aec_speexdsp`.
- [ESP32-SpeexDSP (rjsachse)](https://github.com/rjsachse/ESP32-SpeexDSP) — the ESP32 port `aec_speexdsp` vendors its build configuration from.
