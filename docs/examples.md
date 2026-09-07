---
title: Examples
---

# Waveshare 7B Examples

The repository includes two complete examples for the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B).
They contain the board's tested codec, pin, TDM-slot, display, touch, and
ESP32-C6 Wi-Fi configuration, so they are the quickest route from a new board
to a working `aec_audio` voice satellite.

> **Start with the audio test.** Confirm the microphones, hardware playback
> reference, speaker, and AFE output before trying the voice assistant. A voice
> pipeline cannot compensate for an incorrect slot map or distorted audio.

## Audio test

The
[Waveshare 7B audio-test example](https://github.com/matt123p/esphome-aec/tree/main/examples/waveshare-7b-audio-test)
is the recommended first firmware for this board. Its touch interface lets you:

- listen to the enhanced AFE output or any of the four raw TDM slots;
- inspect RMS, peak levels, and a 32-bin spectrum;
- record and replay two seconds of captured audio; and
- play a bundled, repeatable test signal through the speaker.

Use it to prove that slots `0` and `2` contain the microphones, slot `1`
contains the hardware playback reference, and playback continues cleanly while
the microphones are active.

**[Open the audio-test example and setup instructions](https://github.com/matt123p/esphome-aec/tree/main/examples/waveshare-7b-audio-test)**

## Voice assistant

After the audio test passes, use the
[Waveshare 7B voice-assistant example](https://github.com/matt123p/esphome-aec/tree/main/examples/waveshare-7b-voice-assistant).
It is a focused but complete Home Assistant voice satellite with:

- local “Alexa” wake-word detection;
- full-duplex AEC and rolling pre-buffer hand-off;
- continuous conversation support;
- listening, thinking, replying, ready, and error states;
- transcript and response text;
- manual start/stop and persistent volume control; and
- a deliberately minimal, voice-assistant-only touch interface.

It contains no dashboard, alarm, weather, relay, credential, or other
installation-specific behavior.

**[Open the voice-assistant example and setup instructions](https://github.com/matt123p/esphome-aec/tree/main/examples/waveshare-7b-voice-assistant)**

## Using an example

Each example folder includes its YAML configuration, UI include, a README, and
`secrets.yaml.example`. Copy the example folder locally, rename
`secrets.yaml.example` to `secrets.yaml`, enter your Wi-Fi details, then compile
the main YAML file with ESPHome.

These configurations are specific to the 7B model. For another board or board
revision, follow [First-Time Board Setup]({{ '/first-time-setup/' | relative_url }})
and verify the schematic, pins, codecs, and TDM routing instead of copying the
7B values unchanged.
