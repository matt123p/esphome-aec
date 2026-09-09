---
title: Installation & Setup
---

# Installation & Setup

> **Using the Waveshare 7B?** The fastest path is to use the complete
> [audio-test and voice-assistant examples]({{ '/examples/' | relative_url }}).
> Run the audio test first, then move to the voice assistant after the complete
> capture, reference, and playback path is verified.

You do not need to manually download this repository. Instead simply reference this repository from `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: main
      path: src/esphome
    components: [aec_audio]
```

If you use the ES7210, its current ESPHome component does not expose TDM mode.
Until that support is merged, load the implementation from ESPHome pull request
18954 separately:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome
      ref: feature/es7210-aec-fix
      path: esphome/components
    components: [es7210]
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

> **Warning**
> **Begin with the known-good configuration below and change one
> setting at a time.** Although the schema exposes the ESP-SR AFE controls that
> are useful for development and tuning, not every combination of AFE type,
> input format, processing stage, mode, and target chip works. This is a
> limitation of the Espressif ESP-SR AFE library and its target-specific binary
> pipelines, not just YAML validation. A configuration can be syntactically
> valid yet be rejected by ESP-SR during startup or produce an unsupported feed
> or fetch shape.

For a staged hardware bring-up of a brand-new board, see
[First-Time Board Setup and AEC Bring-Up]({{ '/first-time-setup/' | relative_url }}).

The configuration below documents the core audio integration. For a complete
Waveshare 7B firmware—including display, touch, Wi-Fi companion, diagnostics,
and voice-assistant integration—use the [examples]({{ '/examples/' | relative_url }}).

## Known-good starting configuration

Known-good audio starting configuration for the Waveshare 7B:

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
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: main
      path: src/esphome
    components: [aec_audio]
  - source:
      type: git
      url: https://github.com/matt123p/esphome
      ref: feature/es7210-aec-fix
      path: esphome/components
    components: [es7210]

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
```

Do not also configure another component to own the same I2S peripheral or pins.
The `audio_adc` and `audio_dac` objects initialize their codecs. `aec_audio`
references the ADC so setup ordering is correct, then creates and owns the
paired ESP-IDF TDM RX/TX channels; the speaker child does not take an
`audio_dac` option.

Keep the AFE-related values in this example together for the first successful
bring-up. In particular, start with `afe_input_format: mmr`,
`aec_mode: fd_low_cost`, `filter_length: 4`, and `wakenet: false`. After the
baseline works, alter only one option per test and check the startup log. The
fact that an option appears in the [configuration reference]({{ '/configuration/' | relative_url }}) means the component
can request it; it does not guarantee that ESP-SR implements every combination
on every supported chip.
