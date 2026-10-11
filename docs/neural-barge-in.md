---
title: GRU Barge-In Detector
---

# Optional GRU barge-in detection

The latest component includes an optional learned detector for independent
near-end speech during playback. Use it when a full-duplex voice-assistant
integration can accept microphone audio during its spoken response and handle
an interruption. Ordinary wake-word recognition during music or alarms does
not require this detector. AEC continues running in every mode.

The supplied [voice-assistant example]({{ '/examples/' | relative_url }})
does **not** enable barge-in. Adding a detector cannot make a standard assistant
session interruptible by itself: the assistant/provider must implement that
response lifecycle. The optional bundled assistant engine requires separate
configuration, as described below.

This feature is included in release `v1.01`.

## How VAD is selected

| Device reference playback | Voice assistant active | VAD source |
| --- | --- | --- |
| No | Either | SpeexDSP |
| Yes | No | SpeexDSP; processed audio goes directly to the wake-word listener |
| Yes | Yes | GRU, after consecutive-frame confirmation |

The component exposes one speech decision through `get_vad_state()` and the
microphone's `is_near_end_speech()`. `get_vad_probability()` follows the selected
detector. Playback is determined on the capture timeline from the reference,
including its configured tail, rather than the speaker queue alone.
The assistant-active lambda is evaluated on the main loop and its atomic
snapshot is read by the audio task. Keep that lambda inexpensive.

Feature normalization and the 16-frame context update in all valid states,
matching training. Dense/GRU inference runs only when playback and assistant
activity coincide. Leaving this scope clears confirmation without restarting
the context. Explicit disable/reset and invalid input reset causal state.
There is no shadow option, duck stage or detector cooldown.

## Configuration

Merge this fragment into a compatible full-duplex assistant configuration:

```yaml
aec_speexdsp:
  id: voice_audio
  # Keep the board's existing ADC, pins and reference slot mapping.
  frame_size: 256
  microphone_slots: [0, 2]  # Waveshare 7B; verify other boards.
  output_channel: first
  vad: true
  neural_barge_in:
    enabled: true
    voice_assistant_active: !lambda return id(voice_assistant_engine).is_running();
    consecutive_frames: 3
    speech_protection: false
    log_interval_ms: 5000
    # Omit threshold to use the checkpoint's calibrated 0.9995.
```

Requirements: 16 kHz capture, exactly two microphone slots, `frame_size: 256`,
`vad: true`, a synchronized usable reference, and a single processed output
or beamforming. `capture_only` is incompatible. The model is omitted from
ESP32 firmware unless `enabled: true` is configured. Explicitly configuring
`enabled: false` also omits it; the runtime enable setter cannot enable a
feature that was not compiled in.

| `neural_barge_in` option | Default | Meaning |
| --- | --- | --- |
| `enabled` | `false` | Compile and enable the detector. |
| `voice_assistant_active` | Required when enabled | Boolean lambda defining the active assistant session. |
| `threshold` | Exported model value, `0.9995` | Probability cutoff (0–1); a YAML override replaces calibration. |
| `consecutive_frames` | `3` | Consecutive qualifying frames, range 1–32, before confirmation. |
| `speech_protection` | `false` | Use GRU probability to protect near speech in the Speex preprocessor during barge-in scope. |
| `log_interval_ms` | `5000` | Sampled diagnostic logs; minimum 250 ms. |

Hardware reference samples reach the detector directly from their TDM slot. Validate reference integrity and assistant-only
false triggers before tuning the threshold. A high threshold can reject normal
speech, and lowering it can accept residual echo. Threshold tuning alone cannot
separate overlapping speech/echo scores. The normalizer adapts continuously,
but threshold and confirmation settings do not auto-tune.

## Capture gate and assistant integration

Enabling GRU changes VAD selection. It does not automatically enable microphone
upload gating or interrupt speaker playback. A compatible assistant can call
`set_barge_in_active(true)` on the microphone when entering its full-duplex
response and `set_barge_in_active(false)` on listening, response completion,
idle and error. These calls are idempotent; repeat flush notifications must
not truncate an utterance already in progress.

While the capture gate is armed, confirmed VAD opens delivery with retained
512 ms pre-roll, followed by continuous cleaned PCM through a one-second
hangover renewed by each confirmed frame. Rejected positions before opening
are masked; weak words during the open hangover are retained. Playback resuming
after at least 300 ms of reference silence clears the inherited hangover before
processing the new frame. Already queued user audio survives; fresh confirmed
speech can reopen delivery. Short playback gaps do not cancel it. AEC continues
adapting. Residual echo can still pass during the bounded open interval, so test
assistant-only playback as well as complete interruption questions.
An assistant integration must also manage upload capacity, response cancellation,
queue flushes, completion and error cleanup. The component offers
`set_delivery_condition(...)`, `request_pre_roll()`, `begin_pre_roll_replay()`
and `discard_pending_audio()` for that integration; these are not a substitute
for an assistant/provider interruption protocol.

For an ordinary non-barge-in session, use `set_response_playing(true)` during
TTS and release it when the response completes. This suppresses response audio
and its acoustic tail from delivery/history. Do not combine that muting policy
with a full-duplex response that needs to hear interruptions.

## Independent probabilities for testing

```yaml
sensor:
  - platform: aec_speexdsp
    aec_speexdsp_id: voice_audio
    speex_vad_probability:
      id: speex_vad_probability
      name: "SpeexDSP VAD Probability"
      disabled_by_default: true
    gru_probability:
      id: gru_barge_probability
      name: "GRU Barge-In Probability"
      disabled_by_default: true
    update_interval: 500ms
```

These optional entities report 0–100 percent. Either may be omitted. Displays
can read `id(speex_vad_probability).state` and `id(gru_barge_probability).state`.
Speex reports independently while GRU supplies VAD. GRU reports zero when
inference is inactive, disabled or not compiled, so zero outside the scope
means no current inference. Polling samples the latest frame, not a peak over
the interval. Direct getters `get_speex_vad_probability()` and
`get_neural_barge_probability()` return 0–1.

## Model provenance and validation

The bundled raw84 model uses 72 band-major Bark log powers (microphone 1,
microphone 2, reference) and 12 compact spatial delta values. A Dense32 layer,
GRU24 over 16 causal frames and one output contain 6,921 learned parameters.
These inputs are raw channels, not echo-cancelled residual features.

The model was trained on captured data from the Waveshare 7B board, using
corrected acoustic labels and separate fitting, calibration and held-out
evaluation sessions. Validate it on your own board, room and playback volume.

Checkpoint SHA-256:
`d0a8ce88f4021c72923ac7ac862066d70e4e374976d22d7ad5b27ab76b7dd41e`.
The header exports weights, training-only initial normalization/spatial
statistics and threshold together. `src/esphome/aec_speexdsp/barge_in_model.json`
retains capture-session split IDs and dataset, label and training-source hashes.
The training captures and checkpoint are not shipped in this component repository.

Use host routing/preprocessor tests and C++/PyTorch parity before changing
model semantics. On hardware, test playback-only segments, quiet speech over
TTS, early/repeated interruptions, wake-word handoff and error recovery at the
intended volume. `NEURAL_BARGE` logs active scope, probability, confirmation,
events and CPU budget; `NEURAL_FEATURES` logs context, resets and normalization.
The current model was exercised on ESP32-P4; measure resource usage before
enabling it on other targets.

## Optional bundled voice-assistant engine

Load both components from the same `v1.01` release when using the custom
full-duplex engine:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: v1.01
      path: src/esphome
    components: [aec_speexdsp, voice_assistant]
    refresh: 0s
```

The ordinary example keeps its standard one-request lifecycle. Loading the
optional engine alone does not enable barge-in or install capture callbacks.
For full duplex, set `voice_assistant.barge_in: true` and connect the capture gate:

```yaml
esphome:
  on_boot:
    priority: -90
    then:
      - lambda: |-
          id(voice_assistant_engine).set_barge_in_capture_gate(true);
          id(voice_assistant_engine).set_barge_in_audio_condition([]() {
            return id(voice_microphone).is_near_end_speech();
          });
          id(voice_microphone).set_delivery_condition([](size_t bytes) {
            return id(voice_assistant_engine).can_accept_microphone_bytes(bytes);
          });
voice_assistant:
  barge_in: true
  on_tts_start:
    - lambda: |-
        if (id(voice_microphone).set_barge_in_active(true))
          id(voice_assistant_engine).reset_microphone_upload();
  on_tts_stream_start:
    - lambda: |-
        if (id(voice_microphone).set_barge_in_active(true))
          id(voice_assistant_engine).reset_microphone_upload();
  on_listening:
    - lambda: id(voice_microphone).set_barge_in_active(false);
  on_tts_stream_end:
    - lambda: id(voice_microphone).set_barge_in_active(false);
  on_error:
    - lambda: |-
        id(voice_microphone).set_barge_in_active(false);
        id(voice_microphone).discard_pending_audio();
  on_idle:
    - lambda: |-
        id(voice_microphone).set_barge_in_active(false);
        id(voice_microphone).discard_pending_audio();
```

Merge these callbacks into your lifecycle rather than enabling the example's
response-muting policy at the same time. Use continuous conversation startup
and a state-aware idle/response watchdog when the provider supports it. Keep
wake-word pre-roll handoff and listening release in your integration.

Gemini/provider interruption is authoritative: the detector releases microphone
audio, and Core's flush-marked TTS stream start purges queued playback while
preserving user capture. Assist RUN_END can precede TTS playback completion;
the engine waits for stream end and speaker drain before starting the next
pipeline with the same conversation ID.

The engine retains and retries refused microphone packets. After two seconds
of sustained backpressure it stops and retires the API connection. A failed
stop packet also retires it. Startup acknowledgements have a ten-second limit;
failure stops capture and reconnects rather than waiting indefinitely. Late PCM
and TTS events during idle/startup/stopping are ignored. These are recovery
measures, not evidence that network or provider stalls have been eliminated.

`NEURAL_BARGE` reports `candidate_frames` and `confirmed_frames` alongside
`infer_frames` for each diagnostic window. Compare acceptance duration, not just
event counts. A flush while the user is silent is a false interruption; neither
packet delivery nor a successful host test proves intelligible acoustic input.
