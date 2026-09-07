# Examples

For the guided documentation version of this page, see
[Waveshare 7B Examples](https://matt123p.github.io/esphome-aec/examples/).

These examples target the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B).
They use the board's two microphones, ES7210 hardware-reference channel,
ES8311 DAC, speaker amplifier, 1024×600 display, touch controller, and ESP32-C6
Wi-Fi companion.

## Available examples

- [Audio test](waveshare-7b-audio-test/) — map and inspect every raw TDM slot,
  compare the ESP-SR output, view levels and spectrum, make a short recording,
  and play a known test clip.
- [Voice assistant](waveshare-7b-voice-assistant/) — a complete local-wake-word
  Home Assistant voice satellite with full-duplex AEC, rolling pre-buffer
  hand-off, continuous conversation, recovery logic, volume control, and a
  deliberately minimal touch UI.

Each folder is self-contained apart from Wi-Fi secrets and external components
downloaded during compilation. Start with the audio test. Only move to the voice
assistant after both microphones, playback, and the hardware reference work
cleanly.

These configurations contain the pin and codec routing for the 7B model. Do not
flash them onto another Waveshare model or board revision without checking its
schematic.
