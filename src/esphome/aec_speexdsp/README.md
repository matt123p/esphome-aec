# AEC SpeexDSP for ESPHome

This component provides synchronized microphone capture, SpeexDSP echo
cancellation/preprocessing, optional beamforming, and ESPHome microphone and
speaker interfaces. Hardware references pass directly from their TDM slot to
AEC; software playback references retain optional delay compensation.

The optional raw84 Dense32/GRU24 detector supplies VAD during reference playback
with an active assistant. Outside that scope, SpeexDSP supplies VAD. Independent
probability sensors expose both detectors. A confirmed capture gate retains
candidate onset and continuous cleaned speech through its one-second hangover;
playback resumption after 300 ms of reference silence cancels the old tail.

- [Installation](../../../docs/getting-started.md)
- [Configuration](../../../docs/configuration.md)
- [Architecture and microphone handoff](../../../docs/architecture.md)
- [Wake-word confirmation](../../../docs/wake-word-confirmation.md)
- [Reference timing](../../../docs/reference-timing.md)
- [GRU, capture gate and optional voice-assistant engine](../../../docs/neural-barge-in.md)
- [Testing and tuning](../../../docs/tuning.md)
- [Troubleshooting](../../../docs/troubleshooting.md)
- [Audio test](../../../examples/waveshare-7b-audio-test/)
- [Standard voice assistant](../../../examples/waveshare-7b-voice-assistant/)

These features and recovery fixes are included in `v1.01`. Host checks do not
establish acoustic accuracy: validate assistant-only playback, normal/quiet interruptions and
replacement replies on the intended hardware.
