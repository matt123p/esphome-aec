# Waveshare 7B Voice Assistant

This is a focused, fully integrated Home Assistant voice satellite for the
Waveshare ESP32-P4-WIFI6-Touch-LCD-7B, built on the recommended
`aec_speexdsp` component.

- local Micro Wake Word detection using “Alexa”;
- one-request Home Assistant voice-assistant sessions (wake again for another request);
- rolling pre-buffer hand-off between wake-word and STT listeners;
- suppression of response audio and its acoustic tail from microphone history;
- listening, thinking, replying, idle, and error states;
- recognized-request and spoken-response text on screen;
- manual start/stop, a 15-second idle/error timeout, and recovery after API
  disconnects or pipeline failures; and
- persistent speaker volume control.

The example also enables automatic host-to-satellite playback rate matching.
This compensates for small sustained differences between Home Assistant's PCM
delivery rate and the satellite's fixed 16 kHz audio clock, improving stability
during long spoken responses. See
[How It Works](https://matt123p.github.io/esphome-aec/architecture/#playback-automatic-rate-matching-and-reference).

Playback is digitally attenuated by 12 dB before the DAC and AEC reference tap
because the board's hardware reference can otherwise clip near full scale. The
example uses `filter_length: 1024` (~64 ms) to leave CPU and internal-RAM
headroom for beamforming, the display, wake-word processing, and networking.
The component default is `2048`; increase toward it only while the `DSP load`
log remains comfortably below 100% and the state remains in internal RAM.

AGC is limited by a reference-aware gate (`agc.gate`): while a response is
playing, boost is only permitted for audio that sustains pre-AGC RMS above the
speech threshold, so residual echo is not amplified with the user's voice.
The gate never mutes audio, and the AEC keeps adapting throughout. See the
[configuration reference](https://matt123p.github.io/esphome-aec/configuration/#reference-aware-agc-gate)
for the thresholds and timings.

## Barge-in is disabled in this example

The example uses the standard `voice_assistant.start` lifecycle and suppresses
microphone delivery during TTS. It does not configure `neural_barge_in`, arm a
barge-in capture gate, or import a custom voice-assistant engine. AEC and ordinary
SpeexDSP VAD remain available for wake-word and request audio. See the
[optional GRU guide](../../docs/neural-barge-in.md) for a separate full-duplex
integration; it is not enabled by this example.

## Use it

1. Copy `secrets.yaml.example` to `secrets.yaml` and enter your Wi-Fi details.
2. Compile and flash `voice-assistant.yaml` with a current ESPHome development
   build that supports ESP32-P4, ESP32 Hosted, MIPI DSI, and the Waveshare 7B.
3. Add the device to Home Assistant and allow it to use a configured Assist
   pipeline.
4. Say “Alexa” or tap **Start**. The screen shows the state, transcript, and
   response. Tap **Stop** to end the request and re-arm the wake word.

Run the sibling [audio-test example](../waveshare-7b-audio-test/) first if this
is a new board. Voice-assistant behavior cannot compensate for incorrect TDM
slots, a clipped microphone, distorted playback, or a missing reference.

The configuration loads ES7210 TDM support from ESPHome pull request 18954
until that change is merged. If it has since landed in your ESPHome release,
remove that `external_components` entry.

## State engine

`assistant_state` drives the minimal UI:

| Value | State | Entered when |
| ---: | --- | --- |
| 0 | Ready | The conversation is stopped or completes |
| 1 | Listening | A wake word/manual start opens the microphone |
| 2 | Thinking | Speech-to-text returns the user's request |
| 3 | Replying | Home Assistant starts its TTS response |
| 4 | Error | The API is unavailable or the pipeline reports an error |

The `update_assistant_ui` script is the single rendering point for those
states. The other scripts own start, stop, timeout, and cleanup transitions.

A detected wake word plays a short confirmation beep. Capture and AEC continue,
and the voice pipeline starts without waiting for the cue. See
[wake-word confirmation](../../docs/wake-word-confirmation.md) for integration
and volume handling.
