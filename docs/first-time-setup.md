---
title: First-Time Board Setup
---

# First-Time Board Setup and AEC Bring-Up

Porting `aec_audio` to a new board is primarily a hardware-validation exercise.
Display, networking, codec control, raw capture, playback, and the AEC reference
are separate systems; prove each one before evaluating echo cancellation.

The Waveshare ESP32-P4-WIFI6-Touch-LCD-7B settings in the
[installation guide]({{ '/getting-started/' | relative_url }}) are the tested
reference. Do not copy its GPIOs or TDM slots to a similar-looking board without
checking that board's exact revision and schematic.

If you have the Waveshare 7B itself, use the ready-made
[audio-test example]({{ '/examples/' | relative_url }}#audio-test) for the steps
below. Its touch UI exposes all four raw slots, levels, spectrum, recording, and
test playback without requiring a rebuild for every diagnostic selection.

## Before you start

Collect the schematic and datasheets for the processor, microphone ADC,
playback DAC/codec, and amplifier. Record:

- ESP32 target, flash size, and PSRAM type and size;
- I2C pins, codec addresses, and power rails;
- MCLK, BCLK, LRCLK, DIN, and DOUT pins;
- amplifier enable, mute, reset, and their active levels;
- ADC input routing and four-slot TDM order; and
- the two TDM slots consumed by the DAC.

The component requires ESP-IDF, PSRAM, an ESP32-S3 or ESP32-P4, two synchronized
microphone channels, and four-slot, 16-bit, 16 kHz TDM. A hardware playback
reference is strongly preferred. See [Hardware & Audio Design]({{ '/hardware/' | relative_url }})
before selecting or wiring a board.

## 1. Prove the board without audio

Start with a minimal ESPHome configuration. Confirm that the processor boots
reliably, PSRAM is detected, logs are stable, and API/OTA networking works. If
the device also has a display or touch panel, prove those separately before
adding the codecs or `aec_audio`.

Save this minimal configuration as a recovery image. It gives you a known-good
baseline if later audio work causes memory pressure or boot failures.

## 2. Initialize the codecs

Add the I2C bus and the ADC/DAC control components, but keep the rest of the
configuration simple. Enable I2C scanning during bring-up and confirm both
devices acknowledge at their expected addresses.

Configure 16-bit samples at 16 kHz. The ADC must output four-slot TDM and the DAC
must accept the same clocks and framing. If using the ES7210, follow the extra
TDM-component note in the [installation guide]({{ '/getting-started/' | relative_url }}).
Correct digital clocks can still produce silence if a codec rail, mute line, or
amplifier enable is wrong, so verify those signals against the schematic.

## 3. Add the audio hub in diagnostic mode

Copy the hub, microphone, and speaker structure from the installation guide,
then replace the board-specific pins. Begin with the conservative AFE baseline:

```yaml
aec_audio:
  # audio_adc and the five I2S/TDM pins go here
  tdm_slots: 4
  afe_input_format: mmr
  aec_mode: fd_low_cost
  nlp_level: normal
  filter_length: 4
  wakenet: false
  diagnostic_raw_slot: 0
  slot_logs: true
  diagnostics: true
```

Do not configure another component to own the same I2S peripheral or GPIOs.
Flash over USB and inspect the startup log. Resolve allocation, I2S, or codec
errors before assessing audio quality.

## 4. Map every receive slot

Set `diagnostic_raw_slot` to each value from `0` through `3`, rebuilding as
needed. Listen to or record the published `aec_audio` microphone while you:

1. speak or rub a finger near each microphone in turn;
2. play a repeatable test signal through the speaker; and
3. observe the raw-slot RMS/peak logs.

Record what each slot actually contains:

| TDM slot | Observed signal | Quiet level | Speech/playback peak | Notes |
| ---: | --- | ---: | ---: | --- |
| 0 |  |  |  |  |
| 1 |  |  |  |  |
| 2 |  |  |  |  |
| 3 |  |  |  |  |

Both microphone channels must be intelligible, similarly sensitive, and free
of clipping, buzz, repeated samples, and incorrect pitch. Adjust gain in the
ADC component. Leave headroom: AEC and AGC cannot repair samples already clipped
by the ADC.

Update `microphone_slots` only after this test. On the reference Waveshare 7B,
the tested microphone slots are `[0, 2]` and slot `1` is the hardware reference;
another board may differ.

## 5. Verify playback

Send signed 16-bit, 16 kHz mono or stereo PCM through the `aec_audio` speaker.
Check that playback reaches the intended DAC channels cleanly and that capture
continues during playback. If there is silence or distortion, verify DOUT,
`tx_slots`, codec volume/mute, amplifier control, speaker wiring, and TDM framing.

`resampler: true` compensates for small sustained rate differences. It is not a
decoder or a general sample-rate converter, so source audio must still be
nominally 16 kHz PCM.

## 6. Verify the reference

For `reference_source: analog_slot`, test the proposed slot in diagnostic mode.
It should be quiet when playback is stopped, closely follow playback when it is
active, and respond little to room speech. A spare or microphone-connected ADC
channel is not a playback reference. Set the verified slot as `reference_slot`
and keep `reference_delay_samples: 0`.

If the board has no hardware feedback path, use:

```yaml
aec_audio:
  reference_source: playback
  reference_delay_samples: 0
```

The software reference is copied before the DAC, amplifier, speaker, enclosure,
and acoustic path, so it cannot represent their gain, frequency response, delay,
or nonlinear distortion. It is useful for experiments but should not be expected
to match a well-designed hardware reference.

At 16 kHz, 16 samples equal 1 ms. For a software reference, measure the
playback-to-microphone delay with a repeatable, non-repeating signal and use the
correlation peak as an initial `reference_delay_samples` value. Then sweep nearby
values while keeping the clip, volume, room, and microphone position fixed. The
accepted range is 0–4000 samples (0–250 ms).

## 7. Enable and evaluate the AFE

Remove `diagnostic_raw_slot` to publish the enhanced AFE output. Test the same
phrase in three conditions: speech only, playback only, and simultaneous speech
and playback. A successful configuration substantially reduces playback while
keeping near-end speech intelligible; complete silence is not a realistic goal.

Tune in this order:

1. correct slot mapping, clean levels, and stable TDM transport;
2. correct reference source, level, and software-reference delay;
3. `nlp_level`;
4. `filter_length`;
5. `fd_high_perf`, if the low-cost mode is stable but insufficient; and
6. optional noise suppression, speech enhancement, AGC, meters, and resampling.

Change one setting per test. Test several playback volumes and reject a setting
that suppresses the user's voice during double-talk, even if its playback-only
result sounds quieter. See [Testing & Tuning]({{ '/tuning/' | relative_url }})
for the repeatable test procedure.

## Troubleshooting the bring-up

- **AFE fails or the board resets:** confirm PSRAM, return to MMR/FD low-cost,
  and disable optional meters and telemetry.
- **All slots are silent:** verify codec power, MCLK/BCLK/LRCLK, DIN direction,
  and four-slot TDM mode.
- **One microphone is missing:** scan all four slots, then check bias, routing,
  soldering, and per-channel gain.
- **Crackles or wrong pitch:** recheck 16-bit framing, the 16 kHz clock, shared
  clock wiring, and diagnostics counters.
- **Reference behaves like a microphone:** the proposed ADC input is not a real
  playback feedback path; inspect the schematic.
- **AEC fails only at high volume:** the speaker, amplifier, reference input, or
  microphone ADC is probably clipping or nonlinear. Lower gain or improve the
  physical audio path.

Keep the final board revision, codec addresses, pins, verified RX/TX slots,
gains, maximum clean playback volume, ESPHome version, and AFE settings with the
working configuration. Those details make a port reproducible and upgrades
testable.

After these checks pass on a Waveshare 7B, the complete
[voice-assistant example]({{ '/examples/' | relative_url }}#voice-assistant) is
the recommended next step.
