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

If an AFE fetch fails, the component publishes the selected raw microphone
pair for that frame and increments its dropped-frame counter. Setting
`diagnostic_raw_slot` deliberately bypasses the AFE and publishes that raw slot
as mono, which is useful for finding the physical TDM mapping.

### Playback and reference

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

The playback task has priority 20 and the AFE/capture task priority 19; both
are pinned to core 0. Per-frame buffers use internal RAM. Playback and
reference queues use ESPHome ring buffers.
