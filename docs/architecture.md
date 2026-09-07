---
title: How It Works
---

# How It Works

This page describes the signal-processing pipeline the component configures and
the audio data flow through the wrapper. See
[Hardware & Audio Design]({{ '/hardware/' | relative_url }}) for the physical
signals these pipelines consume.

## The AFE pipeline

Think of the AFE as a sequence of audio-cleaning stages between the physical
microphones and speech recognition. This wrapper supplies two microphone
signals and one reference signal, and receives one enhanced microphone signal
back.

The pipeline works as follows:

1. **Capture synchronized signals.** Two microphones and the playback reference
   must describe the same moment in time. The component continuously reads them
   from the shared audio clock domain or aligns the software reference as
   closely as it can.
2. **Acoustic echo cancellation (AEC).** The AFE compares the known reference
   with the sound captured by each microphone. It estimates how the amplifier,
   speaker, enclosure, room, and signal delay transformed that reference, then
   subtracts the estimated echo. This is adaptive: it learns and follows the
   echo path while audio runs.
3. **Nonlinear processing (NLP).** Linear subtraction cannot remove every
   residual, especially when a small speaker distorts. NLP suppresses remaining
   echo. Higher NLP levels suppress more aggressively but can also cause more
   distortion.
4. **Two-microphone speech enhancement.** Because both microphones hear the
   wanted voice and interference differently, the AFE can combine their
   information to favour the clearer speech signal and reject some interfering
   sound. Espressif may describe parts of this stage as speech enhancement,
   BSS (blind source separation), or MISO channel selection depending on the
   selected AFE pipeline.
5. **Noise suppression (NS).** The AFE reduces relatively steady non-speech
   noise such as fans, electrical hiss, or room noise. It is not a general sound
   remover and cannot perfectly isolate speech in every environment.
6. **Voice activity and wake word, when requested.** The AFE can report whether
   speech is present and can host Espressif WakeNet. In this wrapper those stages
   are enabled together by the experimental `wakenet` option. The normal
   recommendation is to leave it disabled and feed the cleaned stream to
   ESPHome Micro Wake Word.
7. **Automatic gain control (AGC).** AGC raises quiet speech and controls loud
   speech so the final signal stays in a range that downstream recognition can
   use. It cannot repair an ADC signal that was already clipped.
8. **Publish enhanced mono audio.** The AFE returns one 16-bit, 16 kHz channel.
   The wrapper buffers it and publishes it as an ESPHome microphone.

These stages interact rather than behaving like independent desktop audio
filters. Enabling every option is not necessarily better, and some combinations
do not exist in Espressif's target-specific AFE binaries. Start with the
known-good configuration before tuning one stage at a time.

## Audio pipeline

### Capture and processing

```text
TDM ADC (4 interleaved RX slots, 16-bit/16 kHz)
  -> select microphone_slots[0] and microphone_slots[1]
  -> obtain reference from reference_slot or playback reference buffer
  -> arrange an ESP-SR MMR or MMNR interleaved frame
  -> AEC
  -> speech enhancement / dual-mic source processing (optional)
  -> noise suppression (optional)
  -> VAD and WakeNet (only when experimental wakenet is enabled)
  -> automatic gain control (optional)
  -> enhanced mono PCM
  -> pre-roll and live microphone ring buffers
  -> ESPHome microphone consumers
```

Each step has a specific job:

1. **Full-duplex TDM RX** continuously reads four synchronized input slots.
   The bus never has to switch between capture and playback.
2. **Slot selection** extracts exactly two configured microphone slots. This
   makes the logical microphones independent of the codec's physical slot map.
3. **Reference acquisition** supplies the sound AEC should remove:
   `analog_slot` reads it from the ADC, while `playback` uses a mono copy of
   PCM accepted by the speaker endpoint.
4. **AFE frame construction** produces either `MMR` (mic, mic, reference) or
   `MMNR` (mic, mic, zero-filled unused channel, reference). ESP-SR requires
   signed 16-bit, 16 kHz, channel-interleaved input.
5. **AEC** estimates the echo path from reference to microphones and subtracts
   it. The FD modes also apply nonlinear processing (NLP) to residual echo.
6. **Speech enhancement** enables ESP-SR's dual-microphone enhancement stage.
7. **Noise suppression** reduces mainly stationary, non-speech noise.
8. **VAD/WakeNet** are both enabled only by `wakenet: true`. Normal use should
   leave this experimental path off and run ESPHome Micro Wake Word on the
   cleaned microphone instead.
9. **AGC** raises weak output and limits stronger output toward the AFE target.
10. **Publication** returns one enhanced mono channel. A one-second rolling
    pre-roll is retained for fast wake-word-to-voice-assistant hand-off.

## Rolling pre-buffer and wake-word hand-off

Wake-word detection and voice-assistant capture are separate microphone
consumers. When the detector recognizes a wake word, it has already consumed
audio containing the beginning of the request. Stopping that listener and
starting the voice assistant also takes time. Without a hand-off buffer, the
speech-to-text stream can therefore begin late and lose a short command or the
first word after the wake phrase.

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

If an AFE fetch fails, the component falls back to the first selected raw
microphone channel for that frame and increments its dropped-frame counter. Setting
`diagnostic_raw_slot` deliberately bypasses the AFE and publishes that raw slot
as mono, which is useful for finding the physical TDM mapping.

### Playback, automatic rate matching, and reference

```text
ESPHome speaker (16-bit, 16 kHz, mono/stereo)
  -> optional small drift-correction resampler
  -> one-second playback ring buffer
  -> copy/average to the software reference buffer (playback mode)
  -> place samples in tx_slots[0] and tx_slots[1]
  -> four-slot full-duplex TDM TX
  -> DAC -> amplifier -> loudspeaker
```

Mono is duplicated into both TX slots. Stereo left and right go to the two TX
slots and are averaged for a mono software reference. The resampler is only for
small clock/source drift around 16 kHz; it is not a decoder or an arbitrary
sample-rate converter.

#### Why rate matching is needed

The Home Assistant host and the satellite do not share an audio clock. The host
may label its PCM as 16 kHz while delivering it at a sustained average rate that
is slightly faster or slower than the satellite's physical 16 kHz TDM clock.
Network packet timing can add short bursts and gaps, but over a long response a
small underlying rate error steadily fills or drains the playback buffer. The
eventual result is an overrun, underrun, click, or truncated audio even though a
short response sounds correct.

With `resampler: true`, the component automatically matches the incoming host
stream to the satellite clock:

1. Playback starts at the nominal 16,000 samples per second.
2. The component measures how many accepted source frames arrive over time. It
   forms a new estimate only after at least three seconds and two seconds of
   audio, which avoids reacting to individual network packets.
3. It also measures the rate at which complete frames are written to the TDM
   peripheral. That hardware measurement is used until a host-rate estimate is
   available.
4. The target is limited to 15,000–17,000 Hz, and the active correction moves
   toward it one hertz at a time instead of changing abruptly.
5. A continuous fractional-phase linear interpolator produces slightly more or
   fewer samples for the fixed 16 kHz output. Phase and the preceding sample
   carry across packet boundaries, avoiding a discontinuity at each packet.

The learned host rate is retained between playback streams so a later response
can begin with the previous correction. A gap of more than one second resets
the measurement window, allowing the next stream to establish a fresh estimate.
Stopping or clearing playback resets interpolation state so samples from two
unrelated streams are never blended.

This is automatic transport-clock compensation. It does not decode compressed
audio, accept a genuinely different sample rate, repair severe network
starvation, or change the hardware clock. Input must still be signed 16-bit,
nominally 16 kHz PCM. Mono and stereo are both supported.

The playback task has priority 20 and the AFE/capture task priority 19; both
are pinned to core 0. Per-frame buffers use internal RAM. Playback and
reference queues use ESPHome ring buffers.
