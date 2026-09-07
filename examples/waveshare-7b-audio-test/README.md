# Waveshare 7B Audio Test

This diagnostic firmware is derived from the known-working
`esp_1024_audio_test.yaml` configuration used to develop `aec_audio`. It is the
recommended first image for validating the Waveshare 7B audio path.

The 1024×600 touch UI can:

- switch between the enhanced AFE output and all four raw TDM receive slots;
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
   AFE output substantially reduces it.
5. Speak during playback and confirm that near-end speech remains intelligible.

The configuration loads ES7210 TDM support from ESPHome pull request 18954
until that change is merged. If it has since landed in your ESPHome release,
remove that `external_components` entry.

The included `test-audio.wav` is used only as a repeatable signal for playback
and cancellation tests.

This example enables `resampler: true`, so playback also uses the component's
automatic host-to-satellite rate matching. It measures small sustained
differences between incoming nominal 16 kHz PCM and the board's physical TDM
clock, then gradually interpolates the stream to prevent long playback from
draining or filling the buffer. See the
[rate-matching documentation](https://matt123p.github.io/esphome-aec/architecture/#playback-automatic-rate-matching-and-reference)
for details.
