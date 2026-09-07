# AEC Audio for ESPHome

`aec_audio` is a custom ESPHome component for building a more capable voice
assistant. It adds significant features to the processing of the audio to and 
from the Home Assistant's voice assistant.

- It allows the device to **play audio and listen at the same time**,
  so it can continue detecting speech or a wake word while music, an alarm, or a
  voice-assistant response is playing. 

- It also improves the audio sent to speech
  recognition by cancelling the device's own loudspeaker echo, reducing background
  noise, enhancing speech, and applying automatic gain control (AGC) so quiet and
  loud speech arrive at a more useful level.

- It performs audio-rate matching so that differences in playback rate from the
  Home Assistant to the ESP satelite are automatically matched.  This prevents 
  buffer under or over runs when there is a long spoken reply.

- Adds a rolling buffer that ensures no speech is lost following the wake-word detection.

This component is an ESPHome wrapper and audio transport layer around
Espressif's [ESP-SR Audio Front-End (AFE)](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html).
It takes care of the full-duplex I2S/TDM hardware, converts the captured channels
into the layout expected by the AFE, configures the processing stages, and
exposes the result through standard ESPHome microphone and speaker interfaces.
The AFE itself performs the signal processing. The result is a cleaned
microphone stream intended for wake-word detection and voice-assistant speech
recognition.

Setting up a new board is highly complex.  For a staged hardware bring-up see [First-Time Board Setup and AEC Bring-Up](FIRST_TIME_SETUP.md).

If you want to "just get going" - I strongly recommend you simply purchase the Waveshre
board this component was developed against.  Note that, you will only get good results with 
certain hardware and you might find a cheap board is just too cheap - particulary if the
speaker is low quality.

If you do manage to port this library to a different board, please shared the configuration
for other users.  I will incorporate the example in to this repository.

The component exposes normal ESPHome `microphone` and `speaker` endpoints. The
microphone output is enhanced, signed 16-bit mono PCM at 16 kHz. The speaker
accepts signed 16-bit mono or stereo PCM at 16 kHz.

> [!IMPORTANT]
> This is a hardware-specific external component, not a general replacement for
> ESPHome's I2S audio components. It requires two microphone channels, a usable
> playback reference, ESP-IDF, and an ESP32-S3 or ESP32-P4 with
> adequate memory and processing headroom.

> [!WARNING]
> **Begin with the known-good configuration shown in this README and change one
> setting at a time.** Although the schema exposes the ESP-SR AFE controls that
> are useful for development and tuning, not every combination of AFE type,
> input format, processing stage, mode, and target chip works. This is a
> limitation of the Espressif ESP-SR AFE library and its target-specific binary
> pipelines, not just YAML validation. A configuration can be syntactically
> valid yet be rejected by ESP-SR during startup or produce an unsupported feed
> or fetch shape.

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
known-good configuration below before tuning one stage at a time.

## Reference audio

AEC needs to know what sound the device intended to play. That known signal is
called the **reference** or **far-end reference**. The AFE compares it with the
microphone signals to find the part of the recording caused by the device's own
speaker. Without a strong, correctly timed reference, the AFE cannot distinguish
speaker echo from the user's voice and cancellation will be poor.

This component can obtain the reference in two ways.

### Hardware or analog reference (`analog_slot`)

A board designed for AEC may route the DAC, amplifier input, or another point in
the speaker path back into one channel of its ADC. That channel arrives as a TDM
slot beside the microphones. Configure that slot as `reference_slot`.

This is the preferred approach when the board provides a well-designed
reference. It stays in the same hardware clock domain as the microphones and
can include real playback-path delay and gain. Depending on where the board
samples it, it may also include DAC or analog-chain effects. It still does not
perfectly describe the acoustic speaker output, so gain, polarity, routing, and
clipping must be checked.

When choosing hardware, inspect the schematic rather than relying only on an
“AEC” product label. Look for a signal from the speaker/DAC path routed back to
an ADC input, and confirm which TDM slot carries it. A spare ADC channel that is
not actually connected to playback is not a reference.

### Software reference (`playback`)

The software approach copies PCM accepted by the ESPHome speaker into a second
ring buffer and feeds a mono version to the AFE. It is useful for experiments or
boards with no hardware reference and requires no extra analog connection.
`reference_delay_samples` attempts to align this copy with the later acoustic
echo at the microphones.

**NOTE:** This method is fundamentally limited and should not be expected to produce good
AEC results. The copied PCM is taken before the DAC, amplifier, volume-dependent
gain, loudspeaker, enclosure, and acoustic path. It therefore omits delay,
frequency response, nonlinear distortion, clock differences, and other changes
that the microphones actually hear. Manual delay can correct only timing; it
cannot reconstruct the missing analogue and acoustic behaviour. Use a real
hardware reference for dependable full-duplex performance.

### Physical audio quality still matters

AEC is not a substitute for competent acoustic and electrical design. Cheap or
overdriven speakers often distort, producing harmonics that are absent from the
reference and therefore difficult to cancel. Buzzing enclosures and poor
amplifiers cause the same problem. Keep playback below the point of audible or
measured distortion.

Do not place the loudspeaker immediately beside or mechanically coupled to the
microphones. Provide useful distance, orient the speaker away from them where
possible, isolate vibration, avoid enclosure resonances, and prevent the ADC
inputs from clipping. A clean hardware reference and clean playback path usually
improve AEC more than increasingly aggressive software settings.

## Microphones

This wrapper requires **two microphone channels** because it configures the
dual-microphone AFE pipeline. Two microphones give the AFE spatial information:
a nearby speaker, a person in front of the panel, and background noise reach the
two capsules at different levels and times. The AFE can use those differences
to select or enhance the clearer voice component in a way that one microphone
alone cannot.

The microphones should be the same type, use comparable analogue paths and
gain, and remain synchronized on the same ADC/TDM clock. Give them meaningful
physical separation; two capsules at effectively the same point provide little
spatial information. As a practical starting point, use several centimetres of
separation and follow Espressif's microphone-array guidance or the layout of a
known-good voice board. More distance is not automatically better: excessive
spacing, asymmetric enclosure ports, or one microphone much closer to the
loudspeaker can make the two channels inconsistent. Preserve a clear acoustic
opening for each microphone and avoid placing either in the speaker's immediate
pressure field.

Compared with publishing either raw microphone directly, the AFE can:

- use both channels to favour speech with the better signal-to-noise ratio;
- suppress some spatial interference and steady background noise;
- apply echo cancellation to both synchronized microphone observations;
- produce a single enhanced channel, simplifying wake-word and speech-to-text
  consumers;
- normalize the final level with AGC, improving recognition of users speaking
  at different distances.

Those advantages depend on correct slot mapping and matched, unclipped inputs.
If one channel is silent, swapped with the reference, badly clipped, reversed,
or has very different gain, dual-microphone enhancement may perform worse than
a clean raw microphone. Test every physical slot before enabling the complete
pipeline.

## Supported processors and required hardware

### Processor

This component pins `espressif/esp-sr` version `2.4.6` and selects its
full-duplex modes (`AEC_MODE_FD_LOW_COST` or `AEC_MODE_FD_HIGH_PERF`). Espressif
added full-duplex AEC and AFE for **ESP32-S3 and ESP32-P4** in ESP-SR 2.4.3.
Those are therefore the supported processor families for this component.

Espressif provides and tests the underlying full-duplex AFE binaries for the
ESP32-S3 and ESP32-P4 targets. This ESPHome wrapper is known to work on the
**ESP32-P4** in the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B),
which is the reference/test board for the configuration in this repository.
ESP32-S3 uses the same supported ESP-SR API family, but should be treated as a
port that still needs board-specific I2S, memory, codec, and acoustic validation
unless a particular S3 board has also been tested.

ESP-SR's overall target list also contains ESP32, ESP32-S2, ESP32-C3,
ESP32-C5, ESP32-C6, and ESP32-S31, but that does **not** mean this particular
full-duplex AFE pipeline is available or tested on them. Some of those targets
only support other ESP-SR features or models. ESP32-S31 support in the relevant
package line is preliminary and is not claimed here.

Use:

- ESP-IDF, not the Arduino framework;
- an ESP32-S3 or ESP32-P4;
- PSRAM. The AFE allocation policy uses a balance of internal RAM and PSRAM,
  and the optional three-second capture buffer alone uses 96 KB of PSRAM;
- enough CPU headroom for two real-time FreeRTOS tasks and the selected AFE
  stages.

ESP32-P4 has no integrated Wi-Fi, so a networked ESPHome device also needs a
supported companion radio arrangement, such as an ESP32-C6 using ESP-Hosted.
That radio is unrelated to the audio pipeline.

### Audio hardware

Look for a board or design with:

- a two-or-more-channel audio ADC/codec capable of 16-bit, 16 kHz, four-slot
  I2S/TDM output;
- two microphone signals in distinct TDM receive slots;
- a DAC/codec capable of receiving 16-bit, 16 kHz TDM audio;
- an amplifier and loudspeaker;
- shared MCLK, BCLK and LRCLK, plus one data input and one data output;
- either an analog playback/reference signal captured in a third ADC slot, or
  use of the component's digital `playback` reference;
- five suitable GPIOs for MCLK, BCLK, LRCLK, DIN and DOUT, plus any I2C/control
  and amplifier-enable pins required by the codecs.

This implementation specifically requires **four-slot I2S/TDM**, not ordinary
two-channel I2S. At every 16 kHz frame boundary, the bus carries four 16-bit
time slots on a shared data wire. On receive, two slots contain microphones and
another can contain the hardware reference; the remaining slot may be unused.
On transmit, `tx_slots` selects the two slots consumed by the DAC. MCLK, BCLK,
and LRCLK are shared between RX and TX so capture, reference, and playback stay
synchronized. The component configures Philips-format TDM as the master and
drives all four slots. Before buying a board or codec, verify in its datasheet
and schematic that it can operate in this mode and that its slot positions are
configurable or documented.

Do not choose hardware based only on a feature list saying “dual microphone” or
“AEC.” Check the schematic and codec documentation for all of the following:

- both microphones are routed to separate ADC channels and separate TDM slots;
- the ADC can output four 16-bit slots at 16 kHz;
- the DAC can consume the same clocking and TDM frame while RX is active;
- a genuine playback-derived hardware reference is routed into an ADC channel;
- the reference slot, microphone slots, and DAC TX slots can be identified;
- the ESP processor exposes the required I2S/TDM peripheral and GPIO routing;
- PSRAM is fitted and enabled;
- the amplifier and speaker can produce clean speech without clipping or severe
  enclosure vibration.

The original implementation uses an ES7210 microphone ADC and ES8311 DAC, but
the component is configured by ESPHome `audio_adc`, pins, and slot numbers and
is not intrinsically tied to those parts. The ADC and DAC must share the bus
format expected here. This component is the I2S master and always configures
four active, 16-bit Philips-format TDM slots at 16 kHz.

Board manufacturers, including Waveshare, sometimes describe the **ES7210 as an
“AEC chip” or “echo cancellation chip.”** That description is misleading. The
ES7210 is a multi-channel audio ADC: it digitizes the microphones and can
digitize a hardware reference routed to one of its inputs, but it does not run
the adaptive echo-cancellation algorithm used here. Actual AEC, nonlinear
processing, noise suppression, speech enhancement, and AGC run in the ESP-SR
AFE on the ESP32-S3 or ESP32-P4. An ES7210 is useful AEC-supporting hardware
only when the board routes the necessary microphone and reference signals to it.

The known-good hardware is the
[Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-7B).
It combines an ESP32-P4 with PSRAM, two onboard microphones, an ES7210 capture
ADC, an ES8311 playback codec, a speaker connection, and an ESP32-C6 companion
for networking. The repository's `esp_1024_audio_test.yaml` contains the tested
pin and slot mapping for that board. Similar-looking Waveshare models should
not be assumed to use the same audio routing without checking their schematics.

For best results, place the two microphones consistently, avoid mechanical
coupling from the loudspeaker, prevent analog clipping, and provide a clean
reference. An analog reference should represent the real playback chain as
closely as possible. A digital reference is easier to wire but does not include
DAC/amplifier delay, gain, nonlinear distortion, or speaker coloration.

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

## Installation and setup

Publish or copy the whole `aec_audio` directory, including both Python platform
files and the C++ sources. Reference its repository from `external_components`:

```yaml
external_components:
  - source: github://matt123p/esphome-aec@main
    components: [aec_audio]
```

For local development:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [aec_audio]
```

Then:

1. Configure ESPHome for an ESP32-S3 or ESP32-P4 using `framework: type:
   esp-idf` and enable PSRAM.
2. Configure the ADC and DAC/codecs for 16-bit, 16 kHz TDM. Codec-specific
   components are not included with `aec_audio`.
3. Determine the board's MCLK, BCLK, LRCLK, DIN, and DOUT pins.
4. Determine which four RX slots contain microphone and analog-reference data,
   and which TX slots the DAC consumes.
5. Add the hub and its microphone and speaker children.
6. Start in low-cost mode with conservative processing settings.
7. Flash over USB, inspect the startup log, then map and tune the reference.

Known-good audio starting configuration for the Waveshare 7B (replace the
repository URL with the published component location):

```yaml
esp32:
  board: esp32-p4
  variant: ESP32P4
  engineering_sample: true
  flash_size: 32MB
  cpu_frequency: 360MHz
  framework:
    type: esp-idf
    advanced:
      enable_idf_experimental_features: true

psram:
  mode: hex
  speed: 200MHz
  ignore_not_found: false

esp_ldo:
  - channel: 3
    voltage: 2.5V

external_components:
  - source: github://matt123p/esphome-aec@main
    components: [aec_audio, es7210] # omit es7210 if supplied elsewhere

i2c:
  - id: audio_i2c
    sda: GPIO7
    scl: GPIO8
    frequency: 400kHz

audio_adc:
  - platform: es7210
    id: mic_adc
    i2c_id: audio_i2c
    address: 0x40
    bits_per_sample: 16bit
    sample_rate: 16000
    mic_gain: 33db
    tdm: true

audio_dac:
  - platform: es8311
    id: speaker_dac
    i2c_id: audio_i2c
    address: 0x18
    bits_per_sample: 16bit
    sample_rate: 16000
    use_mclk: true

aec_audio:
  id: voice_audio
  audio_adc: mic_adc
  mclk_pin: GPIO13
  bclk_pin: GPIO12
  lrclk_pin: GPIO10
  din_pin: GPIO11
  dout_pin: GPIO9
  i2s_port: 0
  tdm_slots: 4
  microphone_slots: [0, 2]
  reference_source: analog_slot
  reference_slot: 1
  reference_delay_samples: 0
  tx_slots: [0, 1]
  afe_input_format: mmr
  aec_mode: fd_low_cost
  nlp_level: normal
  filter_length: 4
  agc: true
  noise_suppression: true
  speech_enhancement: true
  resampler: true
  wakenet: false

microphone:
  - platform: aec_audio
    id: cleaned_microphone
    aec_audio_id: voice_audio

speaker:
  - platform: aec_audio
    id: full_duplex_speaker
    aec_audio_id: voice_audio
    audio_dac: speaker_dac
```

Do not also configure another component to own the same I2S peripheral or pins.
The `audio_adc` object is used to initialize/configure the ADC, while
`aec_audio` itself creates and owns the paired ESP-IDF TDM RX/TX channels.

Keep the AFE-related values in this example together for the first successful
bring-up. In particular, start with `afe_input_format: mmr`,
`aec_mode: fd_low_cost`, `filter_length: 4`, and `wakenet: false`. After the
baseline works, alter only one option per test and check the startup log. The
fact that an option appears in the configuration reference means the component
can request it; it does not guarantee that ESP-SR implements every combination
on every supported chip.

## Configuration reference

### `aec_audio` hub

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `id` | yes | — | Hub ID referenced by the child platforms. |
| `audio_adc` | yes | — | Configured ESPHome `audio_adc` ID. It must provide the expected 16 kHz, 16-bit TDM stream. |
| `mclk_pin` | yes | — | I2S master-clock output. |
| `bclk_pin` | yes | — | I2S bit-clock output. |
| `lrclk_pin` | yes | — | I2S word-select/frame-clock output. |
| `din_pin` | yes | — | TDM receive-data input from the ADC. |
| `dout_pin` | yes | — | TDM transmit-data output to the DAC. |
| `i2s_port` | no | `0` | ESP-IDF I2S port number, validated from `0` to `2`; available ports remain chip-dependent. |
| `tdm_slots` | no | `4` | Number of TDM slots. The current schema accepts exactly `4`. |
| `microphone_slots` | no | `[0, 1]` | Two distinct RX slots, each from `0` to `3`. |
| `reference_source` | no | `analog_slot` | `analog_slot` uses captured ADC data; `playback` uses speaker PCM. |
| `reference_slot` | no | `2` | RX slot used only by `analog_slot`. It must differ from both microphone slots. |
| `reference_delay_samples` | no | `0` | Software-reference delay, `0`–`4000` samples. Must be `0` with `analog_slot`. At 16 kHz, 16 samples = 1 ms. |
| `tx_slots` | no | `[0, 1]` | Two TDM TX slots, each from `0` to `3`. |
| `afe_input_format` | no | `mmnr` | `mmr` feeds mic/mic/reference; `mmnr` inserts a zero unused channel before the reference. Use the shape supported by the selected ESP-SR target/build. |
| `aec_mode` | no | `fd_low_cost` | `fd_low_cost` or `fd_high_perf`. Start with low cost; high performance uses more PSRAM. |
| `nlp_level` | no | `aggressive` | `normal`, `aggressive`, or `very_aggressive`. More suppression can damage near-end speech. |
| `filter_length` | no | `4` | AEC filter length from `1` to `16`. Longer filters cover longer echo tails but use more resources. |
| `agc` | no | `true` | Enable AFE automatic gain control. |
| `noise_suppression` | no | `true` | Enable AFE noise suppression. |
| `speech_enhancement` | no | `true` | Enable the dual-microphone speech-enhancement stage. |
| `resampler` | no | `false` | Compile playback drift compensation around the nominal 16 kHz rate. |
| `wakenet` | no | `false` | Experimental ESP-SR WakeNet path. Also enables AFE VAD and changes the AFE type/output behavior. Leave off for ESPHome Micro Wake Word. |
| `diagnostic_raw_slot` | no | disabled | Publish raw RX slot `0`–`3` instead of AFE output. Remove it after mapping/testing. |
| `telemetry` | no | `false` | Compile periodic `AEC_EFFECT` attenuation/correlation logging. |
| `slot_logs` | no | `false` | Compile five-second raw-slot and output level logging. |
| `diagnostics` | no | `false` | Compile five-second error, timing, buffer, and reference statistics. |
| `meters` | no | disabled | Compile an `AECAudioMetersComponent`; accepts a nested component `id`. |

`aec_mode`, NLP, filter length, and the optional stages are passed into ESP-SR.
ESP-SR recommends FD low-cost as the general balance of quality and resource
use. Its published ESP32-P4 single-channel figures for the standalone FD AEC
are about 19 KB internal RAM plus 102 KB PSRAM in low-cost mode, and 8 KB plus
138 KB in high-performance mode; the complete dual-microphone AFE and this
component's buffers require more.

### Microphone child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_audio`. |
| `id` | yes | — | ESPHome microphone ID. |
| `aec_audio_id` | yes | — | Parent hub ID. |
| `bits_per_sample` | no | `16` | Only `16` is accepted. |
| `num_channels` | no | `1` | Only mono is accepted. |
| `sample_rate` | no | `16000` | Only 16 kHz is accepted. |

### Speaker child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_audio`. |
| `id` | yes | — | ESPHome speaker ID. |
| `aec_audio_id` | yes | — | Parent hub ID. |
| `audio_dac` | hardware-dependent | — | DAC used by ESPHome's speaker schema/setup. |
| `bits_per_sample` | no | `16` | Only `16` is accepted. |
| `num_channels` | no | `1` | `1` or `2`. |
| `sample_rate` | no | `16000` | Only 16 kHz is accepted. |

### Optional meters and C++ test API

```yaml
aec_audio:
  # ...
  meters:
    id: audio_meters
```

The meter object exposes raw-slot RMS/peak, reference level, cleaned-output
level and peaks, clipping/alternation statistics, and a 32-bin spectrum. The
spectrum costs CPU in the real-time audio task and is off until explicitly
enabled:

```yaml
esphome:
  on_boot:
    then:
      - lambda: id(audio_meters).set_spectrum_enabled(true);
```

The hub also has a C++/lambda-only diagnostic capture API. `start_capture()`
records up to three seconds of cleaned mono audio into PSRAM;
`get_capture_state()` and `get_capture_frames()` report progress; and
`play_capture()` queues the recording to the speaker. There are currently no
native ESPHome actions for this API.

## Testing and tuning

### 1. Verify startup

Compile and flash over USB for the first bring-up, then inspect logs. A healthy
startup reports paired TDM initialization, the final enabled AFE stages, an AFE
feed shape matching `MMR` or `MMNR`, one fetch channel, and creation of both
audio tasks. Treat allocation failures, `ESP-SR rejected the AFE configuration`,
or `Unsupported ESP-SR AFE shape` as setup failures rather than tuning issues.

### 2. Map TDM slots

Temporarily enable:

```yaml
aec_audio:
  # ...
  diagnostic_raw_slot: 0
  slot_logs: true
```

Speak close to each microphone and play a tone. Repeat slots `0` through `3`.
Use the logs/listening tests to identify both microphone slots and, if present,
the analog reference. Update `microphone_slots` and `reference_slot`, then
remove `diagnostic_raw_slot` so the published stream comes from the AFE.

### 3. Verify full-duplex routing

Play a known 16-bit/16 kHz WAV through the `aec_audio` speaker while listening
to or recording the `aec_audio` microphone. Check that:

- playback is clean and reaches the intended DAC channels;
- microphone capture continues throughout playback;
- the reference meter is active during playback;
- `rx_errors`, `tx_errors`, underruns, and dropped frames do not continually
  increase when `diagnostics: true` is enabled.

If long-form audio slowly underruns or overruns while short clips work, try
`resampler: true`. It is meant for small sustained rate mismatches only.

### 4. Align a software reference

With `reference_source: playback`, the reference must line up with the echo at
the microphones. `reference_delay_samples` pre-fills the reference ring buffer
with silence, delaying the reference relative to playback:

```text
delay_ms = reference_delay_samples / 16
samples  = delay_ms * 16
```

Useful starting points are 512 samples (32 ms), 1024 (64 ms), 1600 (100 ms),
and 2048 (128 ms). Play a deterministic, non-repeating calibration signal,
record a raw microphone slot, and cross-correlate the recording with the source
WAV. Use the correlation peak as the initial delay, then sweep nearby values
while measuring and listening. Delay is not configurable for an analog slot
because that signal arrives synchronously in the captured TDM frame.

### 5. Measure cancellation and double-talk

Enable `telemetry: true`, play a repeatable speech/noise clip, and compare its
`AEC_EFFECT` lines. Effective cancellation normally reduces cleaned/reference
correlation and reduces cleaned RMS during playback-only sections. Also speak
while playback continues: a setting that removes echo but destroys near-end
speech is not acceptable full-duplex performance.

Tune in this order:

1. correct slot mapping and unclipped ADC/DAC levels;
2. correct reference source and delay;
3. `nlp_level`, starting with `normal` or `aggressive`;
4. `filter_length`, starting at `4`;
5. `fd_high_perf` only if the low-cost pipeline is stable and insufficient;
6. optional noise suppression, speech enhancement, AGC, meters, and resampling.

Test at several playback volumes and distances. Include playback-only, speech-
only, and simultaneous speech/playback cases, then test the real wake-word and
voice-assistant hand-off. Objective telemetry is useful, but listening and
recognition success during double-talk are the final quality tests.

## Troubleshooting

- **Silence or the wrong microphone:** map every slot with
  `diagnostic_raw_slot`; do not assume the codec's channel order.
- **No cancellation:** verify that the reference contains playback, confirm
  reference polarity/gain, then measure delay. A silent or badly shifted
  reference cannot cancel echo.
- **Speech sounds chopped or metallic:** reduce NLP aggressiveness, shorten the
  filter, test with noise suppression/speech enhancement disabled, and check
  that the ADC is not clipping.
- **Crackles or gaps:** enable `diagnostics`, reduce AFE cost, disable spectrum
  metering, and look for I2S errors, underruns, dropped frames, or excessive
  processing time.
- **Failure to allocate:** confirm PSRAM is enabled and working; remove meters,
  use `fd_low_cost`, and reduce other memory-heavy features.
- **Long playback drifts:** enable `resampler`; if the input is not nominally
  16 kHz PCM, convert it before it reaches this component.
- **Wake word fails only during playback:** first validate AEC independently,
  then run ESPHome Micro Wake Word from `cleaned_microphone`. Leave the
  experimental ESP-SR `wakenet` option off unless its model path is intentionally
  integrated and tested.

## Current limitations

- ESP-SR AFE exposes configuration choices that are not composable in every
  combination. Some syntactically valid configurations are rejected during AFE
  creation or return a feed/fetch layout this component cannot use. Start from
  the known-good configuration above and tune incrementally.
- ESP32-S3 and ESP32-P4 only; ESP-IDF only.
- Four-slot Philips TDM, 16-bit PCM, and 16 kHz are fixed.
- Exactly two microphone inputs and one mono AFE output.
- A single component owns one paired I2S RX/TX peripheral.
- Manual reference delay/gain/acoustic tuning; no automatic calibration.
- No runtime selection of task core/priority or ESP-SR version.
- Capture controls and VAD state are C++ APIs rather than polished YAML actions
  or entities.
- No automatic recovery from persistent I2S or AFE failure.
- AEC cannot fully model loudspeaker nonlinearities or rescue clipped signals.

## References

- [ESP-SR 2.4.6 component registry](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/readme?language=en)
- [ESP-SR changelog](https://components.espressif.com/components/espressif/esp-sr/versions/2.4.6/changelog?language=en)
- [Espressif AFE framework and input formats](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html)
- [Espressif full-duplex AEC modes, NLP, and resource data](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/acoustic_echo_cancellation/README.html)
