---
title: Automatic Reference Alignment
---

# Optional automatic reference alignment

`auto_reference_alignment` tracks the sample delay between an analogue
playback-reference slot and the first microphone. It is disabled by default.
Use it after verifying the board's reference wiring, level and fixed delay;
it cannot correct clipping, a missing reference or an incorrect slot mapping.
It is independent of GRU barge-in detection and does not enable assistant
interruption. The standard voice-assistant example keeps both features disabled.

This optimized implementation is on `main`; it is not in the `v1.00` release.

## Enabling it

Merge this into an existing analogue-reference configuration:

```yaml
aec_speexdsp:
  id: voice_audio
  reference_source: analog_slot
  # Keep the verified ADC, pins, microphone_slots and reference_slot.
  reference_delay_samples: 7  # Example starting value for the Waveshare 7B.
  auto_reference_alignment: true
```

The fixed delay seeds the tracker and remains applied until confident evidence
supports a change. The supported automatic range is 0–256 samples (0–16 ms at
16 kHz). A positive value delays only the analogue reference; microphone and
speaker playback are not delayed. Runtime estimates are not persisted across
boots. This option does not align the software `playback` reference.

## Search and CPU budget

Correlation is not monotonic, so binary search is unsuitable. Skipping a coarse
grid of delays can also miss a narrow peak. Acquisition checks every integer
delay, while bounding work to eight candidates per audio frame.

| State | Search |
| --- | --- |
| Acquiring | Exhaustive 0–256 sample scan, spread across frames |
| Tracking | Within ±8 samples of the detected peak |
| Weak, ambiguous or boundary peak | Full scan on the same captured snapshot before applying a change |
| Every 16 successful tracking updates | Full-range refresh |
| Quiet reference or capture error | Cancel pending work and collect fresh valid evidence |

All candidates in a scan use one frozen audio window. The existing tracker PCM
rings hold that snapshot; only tracker history capture pauses during searching.
ADC delivery, AEC, optional GRU processing and microphone publication continue
every frame. The tracker allocates no heap and adds approximately 1.1 kB of
state compared with the previous implementation.

First-difference normalized correlation uses single-precision arithmetic and
contiguous indexing. Microphone energy is calculated once per snapshot, and
reference energy is updated between adjacent lags. This removes repeated
software-double calculations and per-sample circular indexing.

Tracking starts approximately every 0.5 s while the reference is excited.
With 256-sample frames, the first full scan needs approximately 0.5 s of
capture plus another 0.5 s of scanning; a 17-candidate local scan needs three
calls. Larger frames lengthen scan wall time because the work cap is per call.
Short or frequently interrupted playback may finish before acquisition; the
seed delay is retained. Small accepted changes slew one sample per update;
the existing strong-correlation/confidence rule allows a larger acquisition jump.
Drift is calculated from snapshot sample timestamps, not scan-completion time.

## Testing on the board

The following getters can be read from ESPHome lambdas:

```cpp
id(voice_audio).get_reference_delay_samples();
id(voice_audio).get_reference_alignment_peak_us();
```

The first returns the applied integer delay. The second returns the highest
alignment time observed per audio frame since boot, in microseconds, including
reference excitation RMS and tracker processing. It is zero if alignment has
never run. Compare it with the frame budget (16,000 µs at frame_size 256), the
overall DSP peak/load and underrun counters. It measures only alignment work,
not all audio processing.

Start with playback alone, then quiet gaps, near-end speech over playback and
long playback at the intended volume. Check that delay estimates remain stable
and that microphone delivery is continuous. If evidence is weak, the tracker
keeps the current applied delay rather than forcing an alignment. Room changes
and ambiguous periodic material still require acoustic validation.

## Verification and performance

The host regression test `tests/test_reference_delay_tracker.py` exercises the
production tracker under ASan/UBSan: acquisition, inverted polarity, interleaved
microphones, drift-sized steps, boundary expansion, large path changes,
periodic full refresh, silence, invalid input, range endpoints, frame sizes
128/256/512/1024, independent speech, ambiguous tones and DC input. It checks
the eight-candidate work bound on every call.

An illustrative host benchmark using identical 60-second captured-style
synthetic input and a 73-sample path measured:

| Metric | Previous tracker | Optimized tracker |
| --- | ---: | ---: |
| Peak call time | 934.240 µs | 28.475 µs |
| P99 call time | 711.645 µs | 12.553 µs |
| Total tracker time | 85,346.735 µs | 5,288.596 µs |
| Final delay | 73 samples | 73 samples |

This is approximately 33× lower observed peak cost and 16× lower total cost
on the host. It is not an ESP32-P4 timing measurement or an acoustic accuracy
claim; the board getter is provided for that verification. Benchmark timings
exclude the component's reference RMS loop and the rest of the DSP pipeline.
Automatic alignment remains opt-in, and GRU barge-in is not required to use it.
