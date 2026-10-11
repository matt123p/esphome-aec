---
title: Reference Timing
---

# Reference timing

## Hardware reference

With `reference_source: analog_slot`, the microphone channels and hardware
reference share the TDM capture clock. The configured `reference_slot` is
passed directly to AEC, diagnostics and the neural detector. Keep the slot
mapping correct and the reference unclipped at the loudest intended playback
level. The reference should follow speaker playback and respond little to
near-end room speech.

The adaptive echo filter models the causal speaker and room path within its
configured `filter_length`. Validate both assistant-only playback and user
speech over playback when choosing the filter length and suppression levels.

## Software playback reference

With `reference_source: playback`, the component feeds a mono copy of accepted
speaker PCM into a reference ring buffer. `reference_delay_samples` pre-fills
that buffer with silence to compensate playback-to-microphone latency. It
accepts 0 to 4000 samples and defaults to zero; at 16 kHz, 16 samples equal 1 ms.
This setting applies only to the software playback reference.

Measure the latency with a repeatable non-repeating signal, then compare nearby
settings using cancellation quality and speech intelligibility. A software
reference cannot represent amplifier distortion or rescue clipping.

See [testing and tuning]({{ '/tuning/' | relative_url }}) and
[configuration]({{ '/configuration/' | relative_url }}) for the remaining controls.
