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

AEC is very sensitive to timing alignment between the reference and the microphones. It
will only work effectively with a hardware reference captured in sync with the
microphone channels.

See [testing and tuning]({{ '/tuning/' | relative_url }}) and
[configuration]({{ '/configuration/' | relative_url }}) for the remaining controls.
