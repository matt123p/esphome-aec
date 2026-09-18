---
title: How It Works
---

# How It Works

This page describes the signal-processing pipelines the components configure
and the audio data flow through their wrappers. See
[Hardware & Audio Design]({{ '/hardware/' | relative_url }}) for the physical
signals these pipelines consume.

![System diagram showing capture, echo-cancellation processing, Home Assistant, playback, and the AEC feedback paths]({{ '/assets/diagrams/aec-system.svg' | relative_url }})

*The capture and playback paths run together: the reference lets the echo
canceller identify the device's own loudspeaker audio in the microphone
signals.*

## The SpeexDSP pipeline

The pipeline uses the open-source
[Xiph SpeexDSP](https://github.com/xiph/speexdsp) library, vendored inside the
component. Per processed microphone channel it runs:

1. **Capture synchronized signals.** One to four microphones and the playback
   reference are read from the shared TDM clock domain, or the software
   reference is aligned as closely as it can be.
2. **Linear acoustic echo cancellation.** `speex_echo_cancellation()` adapts a
   frequency-domain filter — up to 16,384 samples (~1 s) of echo tail —
   between the reference and each microphone, and subtracts the estimated
   echo. The long filter is this component's main advantage: the tail must
   cover speaker-plus-room decay plus the reference offset, or residual echo
   remains after convergence.
3. **Channel handling.** Without beamforming, each selected microphone passes
   through its own preprocessor and the published channel is `first`, `second`,
   or the average of two processed channels (`mixed`). With `beamforming`, each
   microphone keeps its own adaptive echo filter inside one Speex multichannel
   state; an
   allocation-free fixed-point localizer (normalized cross-correlation with
   Q15 sub-sample interpolation) estimates the inter-microphone delay, and an
   adaptive delay-and-sum beamformer produces one mono stream before a single
   shared preprocessor. Weak or ambiguous correlations retain the previous
   stable direction.
4. **Noise suppression, residual-echo suppression, AGC, and VAD.**
   `speex_preprocess_run()` consumes the canceller state, applies separate echo
   suppression targets for far-end-only audio and double-talk, reduces
   stationary noise, normalizes the level toward `agc.target_level`, and
   reports voice activity via `get_vad_state()` / `get_vad_probability()`.
   The optional reference-aware AGC gate limits amplification during
   playback: ineligible audio loses boost and stops updating the loudness
   estimate, but samples are never muted and the AEC keeps adapting.
5. **Publish enhanced mono audio.** The cleaned 16-bit, 16 kHz stream feeds
   the same pre-roll and live microphone ring buffers described below.

Processing runs on a dedicated task pinned to core 0 at priority 4. The audio
task logs its own load every five seconds at INFO level:

```text
DSP load: 313 frames in 5002 ms: processing avg 2210 us/frame (13.8% of real time), peak 4980 us (31.1%), frame budget 16000 us
```

The percentage is the share of one core used to keep up with 16 kHz. Load
scales with `filter_length`, `frame_size`, channel count, and the meters;
values near 100% mean the canceller can no longer keep up and frames will be
dropped. At startup the component also logs where the canceller state was
allocated:

```text
SpeexDSP state memory: 118 KB internal, 0 KB PSRAM
```

## Rolling pre-buffer and wake-word hand-off

Wake-word detection and voice-assistant capture are separate microphone
consumers. When the detector recognizes a wake word, it has already consumed
audio containing the beginning of the request. Stopping that listener and
starting the voice assistant also takes time. Without a hand-off buffer, the
speech-to-text stream can therefore begin late and lose a short command or the
first word after the wake phrase.

![Timeline showing the rolling history, wake-word hand-off, preserved request prefix, live audio, and excluded response tail]({{ '/assets/diagrams/prebuffer-timeline.svg' | relative_url }})

The microphone wrapper avoids that gap with two bounded PSRAM buffers:

- a continuously overwritten **one-second history buffer**, independent of
  whether a consumer is currently listening; and
- a **four-second utterance queue** used to deliver the preserved prefix and
  subsequent live audio in order.

When wake-word detection fires, `request_pre_roll()` snapshots audio beginning
100 ms before the last sample delivered to the detector. It also includes any
newer samples already captured but not yet delivered. The snapshot remains
armed while the wake-word listener stops and the voice-assistant listener
starts. `begin_pre_roll_replay()` then releases the queued audio before normal
live delivery continues, so the downstream stream is a single chronological
utterance rather than a separate replay followed by live audio.

The one-second history is a maximum retention window, not one second of audio
blindly prepended to every request. The detector's recorded stream position is
used to choose the hand-off point, with only 100 ms of extra headroom. Storage
is fixed-size: history overwrites its oldest samples, while an active utterance
preserves FIFO order and reports dropped bytes if its four-second queue is
exhausted.

Audio produced while the device is playing a voice-assistant response, plus a
300 ms acoustic tail, is excluded from both live delivery and rolling history.
This reduces the chance that residual synthesized speech is replayed as the
start of the user's next request. The integration must call the hand-off methods
at the correct wake-word and voice-assistant lifecycle points; the microphone
platform cannot infer those transitions on its own.

## Playback, automatic rate matching, and reference

The ESPHome speaker accepts signed 16-bit, 16 kHz mono or stereo PCM and holds
it in a one-second playback buffer. `playback_gain_db` attenuates each sample
before both the TDM output and playback-reference tap, so the reference remains
identical to what is sent to the DAC while preserving headroom in the analogue
amplifier/loopback path. If automatic rate matching is enabled, the
component makes a small timing correction before buffering the audio. Mono is
duplicated into the two configured TX slots; stereo uses one slot for each
channel. The four-slot TDM transmitter then carries those samples to the DAC,
amplifier, and loudspeaker.

When `reference_source: playback` is selected, the component also keeps a mono
copy for the AEC reference. A stereo stream is averaged to produce that copy.
With an analog reference, AEC instead uses the configured ADC slot and no
playback copy is needed.

### Why rate matching is needed

The Home Assistant host and the satellite do not share an audio clock. The host
may label its PCM as 16 kHz while delivering it at a sustained average rate that
is slightly faster or slower than the satellite's physical 16 kHz TDM clock.
Network packet timing can add short bursts and gaps, but over a long response a
small underlying rate error steadily fills or drains the playback buffer. The
eventual result is an overrun, underrun, click, or truncated audio even though a
short response sounds correct.

![Comparison of playback-buffer drift with and without automatic host-to-satellite rate matching]({{ '/assets/diagrams/rate-matching.svg' | relative_url }})

With `resampler: true`, the component automatically matches the incoming host
stream to the satellite clock:

1. Playback starts at the nominal 16,000 samples per second.
2. The component measures the rate at which complete frames are written to the
   TDM peripheral. That hardware measurement is the satellite's real playback
   clock, and it is the only correction target: network delivery is bursty,
   and its throughput is not the PCM sample rate.
3. The measured rate is clamped to 15,000–17,000 Hz, and the active correction
   moves toward it one hertz at a time instead of changing abruptly.
4. A continuous fractional-phase linear interpolator produces slightly more or
   fewer samples for the fixed 16 kHz output. Phase and the preceding sample
   carry across packet boundaries, avoiding a discontinuity at each packet.

The learned playback rate is retained between streams so a later response can
begin with the previous correction. Stopping or clearing playback resets
interpolation state so samples from two unrelated streams are never blended.
Short-term delivery variation — bursts and gaps from the network — is absorbed
by the one-second playback buffer rather than by changing voice pitch.

This is automatic transport-clock compensation. It does not decode compressed
audio, accept a genuinely different sample rate, repair severe network
starvation, or change the hardware clock. Input must still be signed 16-bit,
nominally 16 kHz PCM. Mono and stereo are both supported.

### Task scheduling, CPU affinity, and priority

The DSP processing task runs on core 0 at priority 4. On dual-core targets the
playback/I2S TX task runs on core 1 at priority 20; on the single-core ESP32-S2
it also runs on core 0. Playback normally blocks in the I2S driver, so sharing
the core on S2 does not mean it continuously consumes CPU. If a frame exceeds
its real-time budget (`frame_size` / 16 kHz), the processing task yields so
system work is not starved. Diagnostics report the processing peak; if backlog
reaches the transport, a later read may also record an `rx_error`.

FreeRTOS schedules larger priority numbers first. These priorities are
intentionally asymmetric. Priority 20 protects continuous I2S transmission
from ordinary application work, while priority 4 does not mean that audio
processing is unimportant. Per-frame buffers use internal RAM; playback and
reference queues use ESPHome ring buffers.
