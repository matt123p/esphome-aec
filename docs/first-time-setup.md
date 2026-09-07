---
title: First-Time Board Setup
---

# First-Time Board Setup and AEC Bring-Up

This guide describes how to bring up a new ESPHome display board for use with
the aec_audio component. It uses the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B)
and this repository's
`esp_1024_audio_test.yaml` as the known-good
example, but
the same staged process applies to another ESP32-S3 or ESP32-P4 board.

The most important rule is: **ignore AEC at first and get the basic board
working.** Display, touch, codec control, raw microphone capture, playback, and
the hardware reference are separate systems. Bring them up one at a time.

> **Note**
> A coding agent such as ChatGPT or Gemini can help port the 1024 x 600 LVGL
> example to a different screen size, translate pin assignments from a
> schematic into ESPHome YAML, and interpret build or startup logs. Give it the
> board model, schematic, display resolution, relevant configuration, and the
> complete error log. Always verify suggested GPIOs and power settings against
> the official schematic before flashing.

## Before you start

Collect the exact board name and revision, its schematic, and the datasheets
for the microphone ADC, playback DAC/codec, and amplifier. Identify:

- the ESP32 target, flash size, and PSRAM type and size;
- the display controller, interface, resolution, and touch controller;
- I2C pins and codec addresses;
- audio MCLK, BCLK, LRCLK, DIN, and DOUT;
- amplifier enable, mute, reset, and power controls;
- ADC microphone/reference routing and TDM slot order;
- the DAC's TDM receive slots.

For the Waveshare board, begin with the
[official 7B wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B)
and its schematic under **Resources / Schematic Diagram**. It uses an ESP32-P4,
ESP32-C6 network companion, ES7210 multi-channel ADC, ES8311 playback codec, two
onboard microphones, and a speaker connector.

Board vendors sometimes call the ES7210 an “AEC chip.” It is actually the ADC
that digitizes the microphones and hardware playback reference. The adaptive
echo-cancellation algorithm runs in ESP-SR on the ESP32-P4.

## 1. Bring up ESPHome, the display, and touch only

Do not add aec_audio, codecs, speaker, media player, or the audio test UI yet.
First prove the processor, flash, PSRAM, logger, display, LVGL, and touch setup.

For the Waveshare 7B, take the board-level settings from
`esp_1024_audio_test.yaml`:

- ESP32-P4 target and ESP-IDF framework;
- 32 MB flash and the correct PSRAM mode/speed;
- ESP32-C6/ESP-Hosted configuration if networking is required;
- the board's LDO configuration;
- the WAVESHARE-ESP32-P4-WIFI6-TOUCH-LCD-7B MIPI-DSI model;
- the GT911 touchscreen, I2C pins, reset pin, and transform.

Build a minimal LVGL page containing a large “Hello world” label and one button
that changes the label to “Touch works.” Adapt the syntax to the ESPHome version
in use. This stage passes when:

- firmware boots repeatedly without a reset loop;
- PSRAM is detected;
- the whole display has correct colour and orientation;
- the label is centered and readable;
- the button responds reliably in the expected location;
- logging, API, networking, and OTA work if the product requires them.

Fix rotation, mirroring, touch transforms, or display offsets now. Save this
minimal YAML as a recovery configuration. If later work becomes confusing,
return to it.

## 2. Load the AEC hardware test configuration

Once the basic board works, use
`esp_1024_audio_test.yaml` as the example. It
adds the ADC/DAC, aec_audio, test speaker and media player, audio meters, and
the LVGL page in
`pages_1024/audio_test.yaml`.

The test page is a diagnostic instrument rather than a final UI. It provides:

- **AFE OUTPUT**, which selects the processed microphone result;
- **RAW MIC 1–4**, which bypass the AFE and select individual TDM RX slots;
- live RMS/VU, peak, and three-second peak meters;
- numeric dBFS levels;
- a 32-bin spectrum display covering roughly 80 Hz to 8 kHz;
- **RECORD 2 SECONDS**, which captures the selected source in PSRAM;
- **PLAY RECORDING**, which sends that capture to the speaker;
- **PLAY TEST AUDIO**, which plays the bundled known audio.

The known Waveshare mapping is:

| Button | TDM slot | Waveshare signal |
| --- | ---: | --- |
| RAW MIC 1 | 0 | Microphone 1 |
| RAW MIC 2 | 2 | Microphone 2 |
| RAW MIC 3 | 1 | Hardware AEC reference |
| RAW MIC 4 | 3 | Unused |
| AFE OUTPUT | n/a | Enhanced mono output |

The “RAW MIC” labels are general slot selectors: on another board a slot may
contain a microphone, reference, silence, noise, or an incorrectly routed
signal. Do not assume the Waveshare slot order applies elsewhere.

The example page is designed for 1024 x 600. A coding agent can resize and
reposition it for another resolution while preserving the widget IDs and
button actions.

Before flashing:

1. Change network secrets and the external-component source.
2. Include the test WAV, or substitute signed 16-bit, 16 kHz test audio.
3. Keep PSRAM enabled for the spectrum and capture tools.
4. Keep the known-good AFE baseline: MMR input, FD low-cost mode, filter length
   4, normal NLP, and WakeNet disabled.

## 3. Adapt the hardware configuration from the schematic

Compare every Waveshare setting with the circuit diagram for the target board.
Change hardware-specific values now, but leave AFE processing settings alone.

### Processor and memory

Use an ESP32-S3 or ESP32-P4 supported by the full-duplex ESP-SR AFE, use
ESP-IDF, and configure the board's actual flash and PSRAM. A working display
does not prove enough memory remains for the AFE.

### I2C and codecs

Identify SDA/SCL, bus voltage, and codec addresses. Enable I2C scanning during
bring-up. Confirm that the expected devices appear without initialization
errors.

Replace the ES7210/ES8311 platforms if the board uses other parts. The ADC must
provide two microphones and preferably a playback-reference input in a
four-slot, 16-bit, 16 kHz TDM stream. The DAC must accept matching clocks and
TDM framing.

### I2S/TDM routing

Trace and configure:

- MCLK: master clock from the ESP32;
- BCLK: TDM bit clock;
- LRCLK: frame or word clock;
- DIN: ADC data into the ESP32;
- DOUT: playback data from the ESP32;
- I2S port and the two DAC TX slots.

Do not copy pins from a similarly named board. Revisions and screen sizes often
change routing.

### Power and amplifier control

Check codec rails, LDO configuration, reset, mute, amplifier enable, and their
active levels. Correct digital clocks still produce silence if an analogue rail
or amplifier is disabled.

Enter the slot mapping suggested by the schematic, but treat it as a hypothesis
until the next stage proves it. The two microphone slots must be distinct, and
an analog reference slot must not also be a microphone slot.

## 4. Verify both microphones and map every TDM slot

Test raw inputs before judging AEC. Select each **RAW MIC** button and record
what its TDM slot actually contains.

For each microphone:

1. Select its raw slot in a quiet room.
2. Speak normally from the intended distance.
3. Speak or rub fingers near each capsule in turn; the expected microphone
   should respond most strongly to its nearby source.
4. Watch RMS and peak. Speech should move both meters without pinning at 0 dBFS.
5. Watch the spectrum. Speech should create changing, broad voice-band energy,
   not a flat display, one permanently full bar, or one fixed electrical tone.
6. Press **RECORD 2 SECONDS**, say a short phrase, then press
   **PLAY RECORDING**.
7. Listen for intelligible audio without crackles, buzz, severe hiss, clipping,
   repeated samples, or incorrect speed/pitch.

A plausible spectrum is dynamic. Low-frequency voice energy is often stronger,
while consonants add changing high-frequency content. Exact values depend on
the microphone, room, distance, and gain, so compare channels and conditions
rather than expecting one universal dBFS number.

Record the discovered mapping:

| TDM slot | Observed signal | Quiet level | Speech/test peak | Notes |
| ---: | --- | ---: | ---: | --- |
| 0 |  |  |  |  |
| 1 |  |  |  |  |
| 2 |  |  |  |  |
| 3 |  |  |  |  |

Update microphone_slots with the verified slots and update the test button
labels/actions if appropriate. If only one microphone is clean, stop here: the
dual-microphone AFE cannot be evaluated yet.

## 5. Adjust microphone gain

Set gain in the ADC/codec component. On the ES7210 example this is mic_gain.
Use the same phrase, distance, and speaking volume for comparisons:

1. Record each microphone separately.
2. Raise gain one step if normal speech is extremely quiet and buried in noise.
3. Lower gain if ordinary or close speech reaches 0 dBFS, sounds harsh, or
   produces flat-topped peaks.
4. Test loud close speech and normal speech from the intended location.
5. Confirm both channels have reasonably similar sensitivity.

Leave headroom. A clipped ADC signal is permanently damaged; AGC and AEC cannot
repair it. Aim for clean speech comfortably above the quiet-room noise floor,
not the largest possible meter reading.

## 6A. Verify the hardware reference feedback path

Select the raw slot believed to be the reference, then press **PLAY TEST
AUDIO**. A valid hardware reference should:

- be quiet with playback stopped;
- become active consistently while test audio plays;
- track the music in its VU level and spectrum;
- remain free from clipping, crackling, large DC offsets, and fixed clock tones;
- not react strongly to someone speaking when playback is stopped.

Record two seconds during playback and listen back. It need not sound exactly
like the loudspeaker, but it should be recognizably related to the source and
clean enough for correlation. Silence, unrelated noise, or acoustic microphone
pickup is not a hardware reference.

Adjust reference-channel analogue gain if the codec supports it. It must be
strong enough for modelling but must not clip at maximum intended playback
volume. Do not blindly use microphone gain for a line-level feedback signal.

Set the verified slot as reference_slot, use the analog_slot reference source,
and leave reference delay at zero. If no feedback path exists, the software
playback reference can demonstrate routing, but it omits the DAC, amplifier,
speaker, and acoustic effects and should not be expected to give good AEC.

## 6B. Set up a software reference and determine its delay

Use a software reference only when the board has no usable hardware feedback
path. In this mode the component copies the PCM sent to the speaker and feeds a
mono copy to ESP-SR:

~~~yaml
aec_audio:
  # Keep the verified microphone and TX slot configuration.
  reference_source: playback
  reference_delay_samples: 0
~~~

The reference slot is not used in this mode. Mono playback is copied directly;
stereo playback is averaged to mono. Start with the same known-good MMR, FD
low-cost, filter length, NLP, and optional-stage settings used for the hardware
tests so reference type and delay are the only new variables.

> **Warning**
> A software reference will not work as well as a properly designed hardware
> reference. It is copied before the DAC, volume control, amplifier, speaker,
> enclosure, and acoustic path. It therefore does not contain their frequency
> response, gain changes, nonlinear distortion, or full delay. The AFE must
> infer much more from an incomplete reference. Delay tuning can align the
> signals in time, but it cannot add the missing analogue and acoustic
> information.

### What the delay represents

The software PCM reaches the reference buffer before the corresponding sound
has travelled through the output buffers, DAC, amplifier, speaker, air, and
microphone ADC. reference_delay_samples inserts silence ahead of the software
reference so it arrives at the AFE later and more closely matches that captured
echo.

The pipeline is fixed at 16 kHz:

~~~text
delay_ms = reference_delay_samples / 16
samples  = delay_ms * 16
~~~

For example, 512 samples is 32 ms, 1024 is 64 ms, 1600 is 100 ms, and 2048 is
128 ms. The accepted range is 0–4000 samples (0–250 ms).

### Measure an initial delay

The best initial value comes from cross-correlation:

1. Select a verified raw microphone slot so the AFE is bypassed.
2. Use a repeatable, non-repeating calibration signal. This repository includes
   `audio/aec_calibration.wav` and
   `tools/generate_aec_calibration.py`.
3. Play that exact signal through the board at a clean, moderate volume.
4. Capture the raw microphone to a file. The on-screen two-second capture is
   useful for listening, but an exported recording is needed for numerical
   cross-correlation. The repository's
   `tools/record_mic.py` can be used where its host
   recording workflow is available.
5. Cross-correlate the original playback samples with the microphone recording.
   The strongest physically plausible correlation peak gives the elapsed
   playback-to-microphone delay in samples.
6. Use that value as the first reference_delay_samples setting, rebuild/flash,
   and repeat the AFE playback test.

Make sure the files being compared have the same 16 kHz sample rate and account
for any extra delay introduced by the host recording path. A host/network
capture can add latency that is not part of the on-device AEC path; validate
the result by sweeping nearby values on the device.

### Tune by sweeping nearby values

If a reliable file capture is unavailable, or after cross-correlation gives a
starting point:

1. Use the same test clip, playback volume, microphone position, and room for
   every run.
2. Test delay in coarse steps around the estimate, such as ±256 or ±128
   samples.
3. With nobody speaking, record the AFE output during playback and choose the
   region with the least residual test audio.
4. Sweep that region again in smaller steps, such as 32 or 16 samples.
5. At each promising value, speak over playback. Reject settings that suppress
   the user's voice even if playback-only echo is lower.
6. Repeat at several speaker volumes. Choose a stable compromise rather than a
   value that works for only one clip or volume.

If telemetry is enabled, compare AEC_EFFECT entries between runs. Lower
cleaned/reference correlation and lower cleaned level during playback-only
sections are useful indicators, but recordings and real speech-recognition
tests remain the final test.

Delay is only one limitation. If no delay value gives useful cancellation,
check for buffer underruns, clock drift, volume changes, clipping, speaker
distortion, and excessive acoustic coupling. The likely conclusion may be that
the board needs a hardware reference or better audio hardware—not a more
extreme delay or NLP setting.

## 7. Verify the AFE output

Only after both microphones and the reference pass raw tests should you select
**AFE OUTPUT**.

### Without playback

1. Stop the test audio.
2. Speak from the intended user position and inspect meters and spectrum.
3. Record the same phrase used for the raw tests.
4. Play it back and compare it with both raw microphone recordings.

The result should be intelligible and stable. Noise suppression may lower the
background, speech enhancement may favour the clearer spatial source, and AGC
may make level more consistent. It should not heavily chop words, pump noise,
or add severe metallic distortion.

### With playback

1. Start **PLAY TEST AUDIO** and confirm the speaker and reference are active.
2. Remain silent initially. The AFE output should contain much less test audio
   than either raw microphone.
3. Speak a fixed phrase over playback and confirm voice still reaches the AFE.
4. Record two seconds while speaking over playback; play the capture after test
   audio stops.
5. Repeat at low, normal, and maximum intended playback volume.

Success means the near-end voice stays understandable while the device's audio
is significantly reduced; it does not require mathematical silence. If the
no-playback result is clean but playback leaves strong echo or destroys speech,
check reference routing and level, clipping, speaker distortion, enclosure
vibration, and microphone/speaker placement before changing AFE settings.

Once the hardware passes, tune one AFE option at a time from the known-good
configuration.

## Troubleshooting

### Initial setup has too many failures

Remove the entire audio/AEC configuration and return to the Stage 1 “Hello
world” firmware. **Ignore the AEC module and just get the board working.**
Confirm boot, PSRAM, logging, display, touch, networking, and OTA independently.

### Blank display or incorrect touch

- Use the exact model, pins, and transform for the board revision.
- Test backlight separately from display initialization.
- Fix rotation/mirroring with the minimal page.
- Remove audio to rule out memory pressure or an unrelated boot failure.

### AFE fails to start or the board resets

- Confirm PSRAM is detected.
- Use MMR, FD low-cost, filter length 4, normal NLP, and WakeNet off.
- Check for allocation failure, rejected configuration, or unsupported
  feed/fetch shape in logs.
- Disable optional telemetry and spectrum while isolating resource problems.
- Not every ESP-SR option combination works even when individual YAML options
  validate.

### Codecs are not detected

- Check SDA/SCL, addresses, pull-ups, supply rails, LDO, and reset.
- Run an I2C scan and compare it with the schematic.
- Confirm the configured component matches the fitted part.

### All raw slots are silent

- Verify MCLK, BCLK, LRCLK, DIN direction, power, and ADC initialization.
- Confirm four-slot TDM rather than ordinary stereo I2S.
- Ensure DIN is connected to ADC output, not the DAC data line.

### One microphone is missing or slots are swapped

- Scan all four slots; example labels are not authoritative.
- Check ADC channel and TDM-slot mapping in the schematic and datasheet.
- Check microphone bias, coupling, soldering, and channel gain.

### Raw audio crackles, buzzes, clips, or has wrong speed

- Recheck 16-bit, four-slot TDM framing and the 16 kHz clock.
- Look for alternating-sample patterns or a strong fixed spectral line.
- Reduce gain if peaks approach 0 dBFS.
- Check power integrity and shared clocks.
- Do not evaluate AEC until raw audio is clean.

### Test audio does not play

- Verify DOUT, TX slots, DAC volume/mute, amplifier enable, and speaker wiring.
- Confirm the test file is valid 16-bit, 16 kHz WAV.
- Begin at low volume to avoid distortion or damage.

### Reference is silent or behaves like a microphone

- Confirm playback is physically routed to an ADC input.
- Do not trust an “AEC” marketing label; inspect the circuit diagram.
- Verify the ADC channel and TDM slot with playback on and off.
- If no hardware feedback exists, document that limitation rather than naming
  an unused slot as the reference.

### AFE output is worse than raw audio

- Reconfirm both microphones and the reference slot.
- Verify both microphones have similar, unclipped levels.
- Verify reference is clean and active only during playback.
- Test AFE without playback first.
- Reduce NLP or disable optional processing one setting at a time.
- Reduce speaker volume and eliminate acoustic/mechanical distortion.

### AEC works at low volume but fails at high volume

The speaker, amplifier, reference input, or microphone ADC is probably
nonlinear or clipping. AEC cannot subtract distortion missing from the
reference. Lower playback gain, improve the speaker/enclosure/amplifier,
increase acoustic separation, and repeat all raw tests.

## Record the working configuration

Keep these details for every board revision:

~~~text
Board and revision:
ESP target / ESPHome version:
Display and touch result:
ADC / address:
DAC / address:
MCLK / BCLK / LRCLK / DIN / DOUT:
TDM slot 0:
TDM slot 1:
TDM slot 2:
TDM slot 3:
TX slots:
Microphone gain:
Reference gain:
Quiet and speech levels:
Maximum clean playback volume:
AFE settings tested:
Known problems:
~~~

This turns a working experiment into a reproducible board configuration and
makes later ESPHome, component, or ESP-SR upgrades easier to test.
