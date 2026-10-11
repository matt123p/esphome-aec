---
title: Wake-Word Confirmation
---

# Wake-word confirmation

The voice-assistant example acknowledges a detected wake word with a 100 ms,
1 kHz beep. It retains microphone pre-roll, requests the tone, and starts the
voice pipeline without waiting for playback to finish. Users can begin their
question while the beep sounds: microphone capture and AEC continue, and speech
captured during handoff stays queued until the listening stream is ready.

```yaml
micro_wake_word:
  on_wake_word_detected:
    - lambda: id(voice_microphone).request_pre_roll();
    - lambda: id(voice_audio).play_wake_beep();
    - voice_assistant.start:
        wake_word: !lambda return wake_word;
```

Merge these actions into the existing wake-word handler; keep its UI and session
setup actions. The tone request does not invoke TTS callbacks or response
microphone muting and does not arm the barge-in gate.

## Playback and reference

The playback task generates the tone with 5 ms fades, mixes it before playback
gain and the reference tap, then sends it through normal speaker TX. Hardware
references capture the actual output; software references include the rendered
PCM. The cue bypasses TTS rebuffering and allocates no memory or waits in the
wake callback. Clearing playback cancels it.

Cue level follows `playback_gain_db` and the DAC volume. The standard example
keeps the DAC at the configured assistant volume while listening. Integrations
that mute the DAC during listening can use
`id(voice_audio).is_wake_beep_active()` to temporarily allow the configured
volume, then restore their volume policy in a separate script without delaying
pipeline start or microphone handoff.

Host tests verify duration, fades, mono/stereo output, saturation, cancellation
and reference-tap placement. On hardware, check audibility and speech recognition
when the user speaks during the beep. Host tests cannot establish acoustic
cancellation quality.
