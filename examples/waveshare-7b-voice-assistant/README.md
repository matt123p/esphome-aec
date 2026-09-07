# Waveshare 7B Voice Assistant

This is a focused, fully integrated Home Assistant voice satellite for the
Waveshare ESP32-P4-WIFI6-Touch-LCD-7B. 

- local Micro Wake Word detection using “Alexa”;
- continuous Home Assistant voice-assistant conversations;
- rolling pre-buffer hand-off between wake-word and STT listeners;
- suppression of response audio and its acoustic tail from microphone history;
- listening, thinking, replying, idle, and error states;
- recognized-request and spoken-response text on screen;
- manual start/stop, a 15-second idle/error timeout, and recovery after API
  disconnects or pipeline failures; and
- persistent speaker volume control.

## Use it

1. Copy `secrets.yaml.example` to `secrets.yaml` and enter your Wi-Fi details.
2. Compile and flash `voice-assistant.yaml` with a current ESPHome development
   build that supports ESP32-P4, ESP32 Hosted, MIPI DSI, and the Waveshare 7B.
3. Add the device to Home Assistant and allow it to use a configured Assist
   pipeline.
4. Say “Alexa” or tap **Start**. The screen shows the state, transcript, and
   response. Tap **Stop** to end continuous mode and re-arm the wake word.

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
