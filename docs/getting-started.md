---
title: Installation & Setup
---

# Installation & Setup

This repository contains one component, `aec_speexdsp`: an open-source
SpeexDSP-based echo canceller whose adaptive filter runs with significantly
longer tails (up to ~1 s of echo path), providing significantly better echo
suppression, and which builds for every ESP32 variant with an FPU.

> **Using the Waveshare 7B?** The fastest path is to use the complete
> [audio-test and voice-assistant examples]({{ '/examples/' | relative_url }}).
> Run the audio test first, then move to the voice assistant after the complete
> capture, reference, and playback path is verified.

You do not need to manually download this repository. Reference it from
`external_components`:

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/matt123p/esphome-aec
      ref: main
      path: src/esphome
    components: [aec_speexdsp]
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

1. Configure ESPHome for an ESP32 variant with a hardware FPU (`ESP32`,
   `ESP32-S2`, `ESP32-S3`, or `ESP32-P4`) using `framework: type: esp-idf`,
   and enable PSRAM.
2. Compile the DSP with performance optimization — add both keys to the
   `esp32:` framework `sdkconfig_options` (they form a Kconfig choice, so the
   size default must be explicitly cleared):
   ```yaml
   sdkconfig_options:
     CONFIG_COMPILER_OPTIMIZATION_SIZE: "n"
     CONFIG_COMPILER_OPTIMIZATION_PERF: "y"
   ```
3. Configure the ADC and DAC/codecs for 16-bit, 16 kHz TDM. Codec-specific
   components are not included with `aec_speexdsp`.
4. Determine the board's MCLK, BCLK, LRCLK, DIN, and DOUT pins.
5. Determine which four RX slots contain microphone and analog-reference data,
   and which TX slots the DAC consumes.
6. Add the hub and its microphone and speaker children.
7. Start with the known-good settings below, then map and tune the reference.

### Known-good `aec_speexdsp` starting configuration

Known-good audio starting configuration for the Waveshare 7B using the analog
hardware reference:

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
      loop_task_stack_size: 16384
      enable_idf_experimental_features: true
    sdkconfig_options:
      # Compile for speed, not size: the default (-Os) makes the
      # floating-point DSP loops several times slower. Both options are
      # needed - they form a Kconfig choice and the size default must be
      # explicitly cleared.
      CONFIG_COMPILER_OPTIMIZATION_SIZE: "n"
      CONFIG_COMPILER_OPTIMIZATION_PERF: "y"
      # Size the ESP-DSP twiddle tables for the largest frame this component
      # allows (frame_size 1024 -> 2048-point window).
      CONFIG_DSP_MAX_FFT_SIZE_2048: "y"

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
    components: [aec_speexdsp]
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

aec_speexdsp:
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
  reference_delay_samples: 7
  tx_slots: [0, 1]
  frame_size: 256
  filter_length: 2048
  noise_suppression: true
  noise_suppression_level_db: 10
  echo_suppress_db: 25
  agc: true
  vad: true
  playback_gain_db: -12
  resampler: true

microphone:
  - platform: aec_speexdsp
    id: cleaned_microphone
    aec_speexdsp_id: voice_audio

speaker:
  - platform: aec_speexdsp
    id: full_duplex_speaker
    aec_speexdsp_id: voice_audio
```

On an ESP32-S3 the same configuration works with S3 pins and
`board: esp32-s3-devkitc-1` / `variant: ESP32S3`. On a board without an analog
loopback slot, use `reference_source: playback` and tune
`reference_delay_samples` (0–4000) instead; expect weaker cancellation than a
hardware reference.

`filter_length` is a plain sample count of echo tail (16 samples = 1 ms at
16 kHz). It can run long: 2048 samples
(~128 ms) is the default and a typical indoor tail, 4096 (~256 ms) suits large
or reflective rooms, and up to 16384 (~1 s) is accepted. Longer filters cancel
more echo but cost CPU and memory — see
[Configuration Reference]({{ '/configuration/' | relative_url }}#choosing-an-echo-filter-length)
and the [performance notes]({{ '/configuration/' | relative_url }}#performance-checklist).

Do not configure another component to own the same I2S peripheral or pins. The
`audio_adc` object only initializes the codec; the hub creates and owns the
paired ESP-IDF TDM RX/TX channels. The speaker child does not take an
`audio_dac` option; configure the board's top-level `audio_dac:` separately.

For a staged hardware bring-up of a brand-new board, see
[First-Time Board Setup and AEC Bring-Up]({{ '/first-time-setup/' | relative_url }}).

