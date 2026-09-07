---
title: Overview
---

# AEC Audio for ESPHome

`aec_audio` is a custom ESPHome component for voice devices that need to keep
listening while they play audio. It combines full-duplex TDM transport with
Espressif's ESP-SR Audio Front-End and exposes standard ESPHome microphone and
speaker endpoints.

- It allows the device to **play audio and listen at the same time**,
  so it can continue detecting speech or a wake word while music, an alarm, or a
  voice-assistant response is playing.

- It also improves the audio sent to speech
  recognition by cancelling the device's own loudspeaker echo, reducing background
  noise, enhancing speech, and applying automatic gain control (AGC) so quiet and
  loud speech arrive at a more useful level.

- Its optional rate matching compensates for small, sustained playback-rate
  differences, helping prevent under-runs or over-runs during long responses.

- Its rolling pre-buffer bridges wake-word detection and voice-assistant
  capture, preserving the beginning of the user's request when the integration
  invokes the hand-off API.

This component is an ESPHome wrapper and audio transport layer around
Espressif's [ESP-SR Audio Front-End (AFE)](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html).
It takes care of the full-duplex I2S/TDM hardware, converts the captured channels
into the layout expected by the AFE, configures the processing stages, and
exposes the result through standard ESPHome microphone and speaker interfaces.
The AFE itself performs the signal processing. The result is a cleaned
microphone stream intended for wake-word detection and voice-assistant speech
recognition.

The component exposes normal ESPHome `microphone` and `speaker` endpoints. The
microphone output is enhanced, signed 16-bit mono PCM at 16 kHz. The speaker
accepts signed 16-bit mono or stereo PCM at 16 kHz.

> **Important**
> This is a hardware-specific external component, not a general replacement for
> ESPHome's I2S audio components. It requires two microphone channels, a usable
> playback reference, ESP-IDF, and an ESP32-S3 or ESP32-P4 with
> adequate memory and processing headroom.

> **Warning**
> **Begin with the known-good configuration shown in
> [Installation & Setup]({{ '/getting-started/' | relative_url }}) and change one
> setting at a time.** Although the schema exposes the ESP-SR AFE controls that
> are useful for development and tuning, not every combination of AFE type,
> input format, processing stage, mode, and target chip works. This is a
> limitation of the Espressif ESP-SR AFE library and its target-specific binary
> pipelines, not just YAML validation. A configuration can be syntactically
> valid yet be rejected by ESP-SR during startup or produce an unsupported feed
> or fetch shape.

For the shortest path to a working system, use the Waveshare reference board.
AEC quality depends heavily on the codec routing, reference signal, amplifier,
speaker, enclosure, and microphone placement; software cannot compensate for a
clipped or badly distorted physical audio path.

## Try it on the Waveshare 7B

Two complete Waveshare 7B configurations are ready to use:

1. Start with the **[audio-test example]({{ '/examples/' | relative_url }}#audio-test)**
   to verify every TDM channel, the hardware reference, playback, and AFE output.
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
- **[How It Works]({{ '/architecture/' | relative_url }})** — the AFE processing pipeline and the capture/playback data flow.
- **[Configuration Reference]({{ '/configuration/' | relative_url }})** — every option for the hub, microphone, speaker, and meters.
- **[Testing & Tuning]({{ '/tuning/' | relative_url }})** — verify startup, map TDM slots, align references, and measure cancellation.
- **[Troubleshooting & Limitations]({{ '/troubleshooting/' | relative_url }})** — common symptoms and the current constraints of the component.

## References

- [ESP-SR 2.4.6 component registry](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/readme?language=en)
- [ESP-SR changelog](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/changelog?language=en)
- [Espressif AFE framework and input formats](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html)
- [Espressif full-duplex AEC modes, NLP, and resource data](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html)
