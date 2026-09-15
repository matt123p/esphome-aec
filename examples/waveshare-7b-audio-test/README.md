# Waveshare 7B Audio Test

This diagnostic firmware validates the complete Waveshare 7B audio path using
the `aec_speexdsp` component. It is the recommended first image for the board.

The 1024×600 touch UI can:

- switch between the enhanced DSP output and all four raw TDM receive slots;
- show RMS, current peak, three-second peak, and a 32-bin spectrum;
- capture two seconds of the selected source into PSRAM and play it back; and
- play the bundled 16-bit, 16 kHz reference clip through the speaker.

## Use it

1. Copy `secrets.yaml.example` to `secrets.yaml` and enter your Wi-Fi details.
2. Compile and flash `audio-test.yaml` with a current ESPHome development build
   that supports the ESP32-P4 and this display.
3. Select each raw channel and verify the expected Waveshare mapping:

   | UI selection | TDM slot | Expected signal |
   | --- | ---: | --- |
   | Raw Mic 1 | 0 | Microphone 1 |
   | Raw Mic 2 | 2 | Microphone 2 |
   | Raw Mic 3 | 1 | Hardware AEC reference |
   | Raw Mic 4 | 3 | Unused |

4. Play the test clip. The reference channel should follow playback while the
   DSP output substantially reduces it.
5. Speak during playback and confirm that near-end speech remains intelligible.

The example uses `playback_gain_db: -12` because the board's hardware reference
can clip near full-scale DAC playback even at its minimum ADC gain. It also uses
an AEC filter length of `1024` samples to retain CPU headroom for the meters,
spectrum, and display.

There is no automatic reference-delay tuning on this engine. The configuration
sets `reference_delay_samples: 7`, the value measured for this board's
amp/loopback path; SpeexDSP adapts over a range of delays itself, so the value
only needs to be roughly right. To measure it on another board, record a raw
microphone slot while playing a repeatable signal and cross-correlate the two
(see [Testing & Tuning](https://matt123p.github.io/esphome-aec/tuning/)).

The configuration loads ES7210 TDM support from ESPHome pull request 18954
until that change is merged. If it has since landed in your ESPHome release,
remove that `external_components` entry.

The included `test-audio.wav` is used only as a repeatable signal for playback
and cancellation tests.

The diagnostic example disables the resampler so its generated signals remain
at a known 16 kHz rate. The voice-assistant example enables rate matching for
long streamed responses.
