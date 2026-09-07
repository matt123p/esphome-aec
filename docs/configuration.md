---
title: Configuration Reference
---

# Configuration Reference

All options accepted by the `aec_audio` component. Start from the
[known-good configuration]({{ '/getting-started/' | relative_url }}) and change one
setting at a time.

## `aec_audio` hub

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

## Microphone child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_audio`. |
| `id` | yes | — | ESPHome microphone ID. |
| `aec_audio_id` | yes | — | Parent hub ID. |
| `bits_per_sample` | no | `16` | Only `16` is accepted. |
| `num_channels` | no | `1` | Only mono is accepted. |
| `sample_rate` | no | `16000` | Only 16 kHz is accepted. |

## Speaker child

| Option | Required | Default | Description |
| --- | :---: | --- | --- |
| `platform` | yes | — | Must be `aec_audio`. |
| `id` | yes | — | ESPHome speaker ID. |
| `aec_audio_id` | yes | — | Parent hub ID. |
| `audio_dac` | hardware-dependent | — | DAC used by ESPHome's speaker schema/setup. |
| `bits_per_sample` | no | `16` | Only `16` is accepted. |
| `num_channels` | no | `1` | `1` or `2`. |
| `sample_rate` | no | `16000` | Only 16 kHz is accepted. |

## Optional meters and C++ test API

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
