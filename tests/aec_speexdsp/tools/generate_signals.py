#!/usr/bin/env python3
"""Generate realistic test signals and reference data for the aec_speexdsp
PC test suite.

Everything is deterministic (fixed seeds) so failures are reproducible.

Content produced in tests/aec_speexdsp/test_data:
  - synthesized speech (source-filter: glottal pulses + formant resonators,
    unvoiced fricatives, syllabic envelopes, natural pauses)
  - noise beds: white, pink, babble (modulated multiband), fan/hum
  - echo paths: delay + sparse early reflections + diffuse decaying tail
  - real far-end material reused from ../../audio (music, alarm)
  - beamforming scenarios with per-mic fractional TDOA and directional
    interference
  - manifest.json with per-sample ground-truth label tracks (.f32) and
    scenario parameters (echo delay/gain, mic delays, SNRs)
"""

import json
import sys
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000
SEED = 0xAEC5EED

OUT_DIR = Path(__file__).resolve().parent.parent / "test_data"
REPO_AUDIO = Path(__file__).resolve().parents[3] / "audio"

rng = np.random.default_rng(SEED)


# ---------------------------------------------------------------- utilities
def write_wav(name, data):
    data = np.clip(data, -32768, 32767).astype(np.int16)
    with wave.open(str(OUT_DIR / name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(data.tobytes())
    return name


def write_labels(name, labels):
    (OUT_DIR / name).write_bytes(np.asarray(labels, dtype=np.float32).tobytes())
    return name


def read_wav_int16(path):
    with wave.open(str(path)) as w:
        assert w.getframerate() == SAMPLE_RATE, f"{path}: expected 16 kHz"
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit"
        raw = w.readframes(w.getnframes())
        data = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
        if w.getnchannels() == 2:
            data = data.reshape(-1, 2).mean(axis=1)
    return data


def db(x):
    return 10.0 ** (x / 20.0)


def normalize_peak(x, peak=0.9):
    m = np.max(np.abs(x))
    return x * (peak / m) if m > 0 else x


def rms(x):
    return np.sqrt(np.mean(x * x)) if len(x) else 0.0


def scale_to_rms_db(x, rms_dbfs):
    target = 32768.0 * db(rms_dbfs)
    current = max(rms(x), 1e-9)
    return x * (target / current)


# ---------------------------------------------------------------- speech
def glottal_pulse_train(n, f0_series):
    """Impulse train with jitter and the given per-sample F0 contour."""
    out = np.zeros(n)
    phase = 0.0
    for i in range(n):
        phase += f0_series[i] / SAMPLE_RATE
        if phase >= 1.0:
            phase -= 1.0
            out[i] = 1.0
    return out


def formant_resonator(x, f_hz, bw_hz):
    """Second-order resonator (Klatt-style parallel branch)."""
    # discretized pole pair
    r = np.exp(-np.pi * bw_hz / SAMPLE_RATE)
    theta = 2.0 * np.pi * f_hz / SAMPLE_RATE
    a = np.array([1.0, -2 * r * np.cos(theta), r * r])
    b = np.array([1.0 - r, 0.0, -(1.0 - r) * 0.0])  # gain-ish, keep simple
    y = np.zeros_like(x)
    # simple IIR via lfilter equivalent (avoid scipy dependency)
    for i in range(2, len(x)):
        y[i] = x[i] - a[1] * y[i - 1] - a[2] * y[i - 2]
    return y


def one_pole_lowpass(x, cutoff_hz):
    alpha = 1.0 - np.exp(-2.0 * np.pi * cutoff_hz / SAMPLE_RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += alpha * (x[i] - acc)
        y[i] = acc
    return y


class FormantVoice:
    """Tiny parallel-formant synthesizer; good enough to excite AEC/NS/VAD
    with speech-band, harmonic, syllabically modulated audio."""

    # (F1,F2,F3) formant triplets like [a], [e], [i], [o], [u] vowels
    VOWELS = [
        (730, 1090, 2440),  # a
        (530, 1840, 2480),  # e
        (270, 2290, 3010),  # i
        (570, 840, 2410),   # o
        (300, 870, 2240),   # u
        (660, 1720, 2410),  # ae
    ]

    def __init__(self, f0_base=120.0, seed=1):
        self.rng = np.random.default_rng(seed)
        self.f0_base = f0_base

    def syllable(self, dur_s, voiced=True):
        n = int(dur_s * SAMPLE_RATE)
        t = np.arange(n) / SAMPLE_RATE
        # F0 contour: declination + vibrato + jitter
        f0 = self.f0_base * (1.0 + 0.12 * np.sin(2 * np.pi * 0.9 * t) + 0.02 * self.rng.standard_normal(n))
        f0 *= np.linspace(1.06, 0.94, n)
        if voiced:
            src = glottal_pulse_train(n, f0)
            src = one_pole_lowpass(src, 800.0)  # spectral tilt
        else:
            src = self.rng.standard_normal(n) * 0.35

        f1, f2, f3 = self.VOWELS[self.rng.integers(len(self.VOWELS))]
        # formant glides within the syllable (diphthong-ish)
        glide = np.linspace(0.9, 1.1, n)
        y = formant_resonator(src, f1 * glide[0] * 1.0, 80.0)
        y += 0.7 * formant_resonator(src, f2, 110.0)
        y += 0.35 * formant_resonator(src, f3, 160.0)
        if not voiced:
            # fricative shaping: emphasize 3-8 kHz
            hp = src - one_pole_lowpass(src, 2500.0)
            y += 2.0 * hp

        # syllabic envelope: 20 ms attack/release
        env_n = max(1, int(0.02 * SAMPLE_RATE))
        env = np.ones(n)
        env[:env_n] = np.linspace(0, 1, env_n)
        env[-env_n:] = np.linspace(1, 0, env_n)
        return y * env

    def sentence(self, total_s, seed=0):
        """A sequence of syllable bursts separated by short pauses."""
        self.rng = np.random.default_rng(1000 + seed)
        pieces = []
        labels = []
        remaining = total_s
        first = True
        while remaining > 0.05:
            # 2-5 syllable "word", then 0.15-0.45 s pause
            word_dur = min(remaining, float(self.rng.uniform(0.45, 1.1)))
            n_syll = int(max(1, word_dur / 0.22))
            for s in range(n_syll):
                voiced = self.rng.random() > 0.25
                dur = float(np.clip(word_dur / n_syll * self.rng.uniform(0.8, 1.2), 0.08, 0.35))
                y = self.syllable(dur, voiced)
                pieces.append(y)
                labels.append(np.ones(len(y)))
                if s < n_syll - 1:
                    gap = float(self.rng.uniform(0.02, 0.06))
                    pieces.append(np.zeros(int(gap * SAMPLE_RATE)))
                    labels.append(np.zeros(int(gap * SAMPLE_RATE)))
            remaining -= word_dur
            pause = float(min(remaining, self.rng.uniform(0.15, 0.45)))
            pieces.append(np.zeros(int(pause * SAMPLE_RATE)))
            labels.append(np.zeros(int(pause * SAMPLE_RATE)))
            remaining -= pause
            if first:
                first = False
        y = np.concatenate(pieces)
        lab = np.concatenate(labels)
        n_target = int(total_s * SAMPLE_RATE)
        return y[:n_target], lab[:n_target]


# ---------------------------------------------------------------- noises
def white_noise(n, rms_dbfs, seed):
    r = np.random.default_rng(seed)
    return scale_to_rms_db(r.standard_normal(n), rms_dbfs)


def pink_noise(n, rms_dbfs, seed):
    r = np.random.default_rng(seed)
    x = r.standard_normal(n)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1.0 / SAMPLE_RATE)
    f[0] = f[1]
    X *= 1.0 / np.sqrt(f)
    y = np.fft.irfft(X, n)
    return scale_to_rms_db(y, rms_dbfs)


def babble_noise(n, rms_dbfs, seed):
    """Speech-like babble: sum of bandpassed, speech-envelope-modulated noise."""
    r = np.random.default_rng(seed)
    n_bands = 9
    total = np.zeros(n)
    for b in range(n_bands):
        lo = 200.0 * (1.35 ** b)
        hi = lo * 1.4
        x = r.standard_normal(n * 2)
        X = np.fft.rfft(x)
        f = np.fft.rfftfreq(len(x), 1.0 / SAMPLE_RATE)
        X[(f < lo) | (f > hi)] = 0.0
        y = np.fft.irfft(X, len(x))
        # speech-like envelope: syllabic AM ~4 Hz with random depth
        t = np.arange(n * 2) / SAMPLE_RATE
        depth = r.uniform(0.3, 0.7)
        env = 1.0 - depth + depth * (0.5 + 0.5 * np.sin(2 * np.pi * r.uniform(2.5, 5.5) * t + r.uniform(0, 6)))
        y *= env
        total += y[:n]
    return scale_to_rms_db(total, rms_dbfs)


def fan_noise(n, rms_dbfs, seed):
    r = np.random.default_rng(seed)
    t = np.arange(n) / SAMPLE_RATE
    hum = 0.4 * np.sin(2 * np.pi * 50 * t) + 0.25 * np.sin(2 * np.pi * 100 * t) + 0.15 * np.sin(2 * np.pi * 150 * t)
    broadband = one_pole_lowpass(r.standard_normal(n), 900.0)
    broadband /= max(rms(broadband), 1e-9)
    y = hum + 0.7 * broadband
    return scale_to_rms_db(y, rms_dbfs)


# ---------------------------------------------------------------- echo path
def room_ir(delay_samples, seed, tail_taps=160, gain=1.0):
    r = np.random.default_rng(seed)
    ir = np.zeros(tail_taps)
    d = int(delay_samples)
    ir[d] = 1.0
    for off, g in ((9, 0.32), (25, 0.18), (57, 0.10)):
        if d + off < tail_taps:
            ir[d + off] = g
    for i in range(d + 80, tail_taps):
        decay = np.exp(-4.0 * (i - d - 80.0) / max(1, tail_taps - d - 80))
        ir[i] = 0.05 * r.standard_normal() * decay
    return ir * gain


def convolve(x, ir):
    n = len(x)
    y = np.convolve(x, ir)[:n]
    return np.clip(y, -32768, 32767)


def frac_delay(x, delay):
    """Delay x by `delay` samples (may be fractional; negative delays shift
    the signal earlier), zero-padded at the edges."""
    if abs(delay) < 1e-9:
        return x.copy()
    sign = 1.0 if delay >= 0 else -1.0
    n = int(np.floor(abs(delay)))
    frac = abs(delay) - n
    y = np.zeros_like(x)
    if frac < 1e-9:
        if n < len(x):
            if sign > 0:
                y[n:] = x[: len(x) - n]
            else:
                y[: len(x) - n] = x[n:]
        return y
    if sign > 0:
        if n + 1 < len(x):
            y[n + 1 :] = (1 - frac) * x[: len(x) - n - 1] + frac * x[1 : len(x) - n]
    else:
        if n + 1 < len(x):
            y[: len(x) - n - 1] = (1 - frac) * x[n + 1 :] + frac * x[n : len(x) - 1]
    return y


# ---------------------------------------------------------------- scenarios
scenarios = []


def add_scenario(name, description, tracks, labels=None, params=None):
    entry = {
        "name": name,
        "description": description,
        "sample_rate": SAMPLE_RATE,
        "samples": len(tracks["mic0"]),
    }
    for key, data in tracks.items():
        if data is not None:
            entry[key] = write_wav(f"{name}_{key}.wav", data)
    labels = labels or {}
    for key, lab in labels.items():
        if lab is not None:
            entry[key] = write_labels(f"{name}_{key}.f32", lab)
    if params:
        entry.update(params)
    scenarios.append(entry)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    dur = 12.0
    n = int(dur * SAMPLE_RATE)

    # ---------------- source signals ----------------
    voice = FormantVoice(f0_base=115.0, seed=21)
    speech, speech_lab = voice.sentence(dur, seed=1)
    speech = scale_to_rms_db(speech, -24.0)  # normal talking level
    speech_lab = (speech_lab > 0.5).astype(np.float32)

    voice_hi = FormantVoice(f0_base=190.0, seed=22)
    speech_hi, _ = voice_hi.sentence(dur, seed=2)

    speech_quiet, quiet_lab = FormantVoice(f0_base=105.0, seed=23).sentence(8.0, seed=3)
    speech_quiet = scale_to_rms_db(speech_quiet, -35.0)
    quiet_lab = (quiet_lab > 0.5).astype(np.float32)

    speech_loud, loud_lab = FormantVoice(f0_base=125.0, seed=24).sentence(8.0, seed=4)
    speech_loud = scale_to_rms_db(speech_loud, -9.0)
    loud_lab = (loud_lab > 0.5).astype(np.float32)

    # far-end: real device material where available, else shaped noise probe
    music = None
    music_path = REPO_AUDIO / "Seven_Thirty_Sharp_16k.wav"
    if music_path.exists():
        music = read_wav_int16(music_path)[:n]
        if len(music) < n:
            music = np.pad(music, (0, n - len(music)))
    else:
        music = pink_noise(n, -20.0, 31)
    alarm = None
    alarm_path = REPO_AUDIO / "alarm_music_16k.wav"
    if alarm_path.exists():
        alarm = read_wav_int16(alarm_path)
        reps = int(np.ceil(n / len(alarm)))
        alarm = np.tile(alarm, reps)[:n]
    else:
        alarm = white_noise(n, -20.0, 32)

    # shaped noise far-end (like the on-device calibration probe)
    probe_r = np.random.default_rng(33)
    lp = 0.0
    prev_lp = 0.0
    probe = np.zeros(n)
    w = probe_r.uniform(-1, 1, n)
    for i in range(n):
        lp += 0.32 * (w[i] - lp)
        probe[i] = lp - prev_lp * 0.985
        prev_lp = lp
    probe = normalize_peak(probe, 0.6 * 32768.0)

    noise_white = white_noise(n, -45.0, 41)
    noise_babble = babble_noise(n, -38.0, 42)
    noise_fan = fan_noise(n, -28.0, 43)
    noise_quiet_hiss = white_noise(8 * SAMPLE_RATE, -50.0, 44)

    # ---------------- far-end-only scenarios ----------------
    echo_delay = 28  # ~1.75 m of acoustic path
    ir = room_ir(echo_delay, seed=51, gain=1.0)
    echo_music = convolve(music, ir) * db(-8.0)
    ref_music_active = (np.abs(music) > 300).astype(np.float32)

    mic = np.clip(echo_music + noise_fan * 0.3, -32768, 32767)
    add_scenario(
        "far_only_music",
        "Real music far-end through a synthetic room; mic has echo + light fan noise",
        {"reference": music, "mic0": mic, "echo_copy": echo_music},
        {"speech_labels": np.zeros(n), "echo_labels": ref_music_active},
        {"echo_delay_samples": echo_delay, "echo_gain_db": -8},
    )

    echo_probe = convolve(probe, ir) * db(-6.0)
    ref_probe_active = (np.abs(probe) > 300).astype(np.float32)
    mic = np.clip(echo_probe + noise_white, -32768, 32767)
    add_scenario(
        "far_only_noise",
        "Shaped-noise probe far-end; canonical AEC convergence signal",
        {"reference": probe, "mic0": mic, "echo_copy": echo_probe},
        {"speech_labels": np.zeros(n), "echo_labels": ref_probe_active},
        {"echo_delay_samples": echo_delay, "echo_gain_db": -6},
    )

    echo_alarm = convolve(alarm, ir) * db(-7.0)
    ref_alarm_active = (np.abs(alarm) > 300).astype(np.float32)
    mic = np.clip(echo_alarm + noise_babble * 0.3, -32768, 32767)
    add_scenario(
        "far_only_alarm",
        "Alarm tone far-end (tonal, hard for correlation-based AEC)",
        {"reference": alarm, "mic0": mic, "echo_copy": echo_alarm},
        {"speech_labels": np.zeros(n), "echo_labels": ref_alarm_active},
        {"echo_delay_samples": echo_delay, "echo_gain_db": -7},
    )

    # ---------------- near-end-only scenarios ----------------
    mic = np.clip(speech + noise_babble, -32768, 32767)
    add_scenario(
        "near_only_babble",
        "Speech in babble at +6 dB SNR; no echo",
        {"reference": np.zeros(n), "mic0": mic, "near_clean": speech},
        {"speech_labels": speech_lab, "echo_labels": np.zeros(n)},
        {"snr_db": 6},
    )

    mic = np.clip(speech + noise_fan, -32768, 32767)
    add_scenario(
        "speech_fan",
        "Speech in fan/hum noise at ~0 dB SNR",
        {"reference": np.zeros(n), "mic0": mic, "near_clean": speech},
        {"speech_labels": speech_lab, "echo_labels": np.zeros(n)},
        {"snr_db": 0},
    )

    quiet_n = len(speech_quiet)
    mic = np.clip(speech_quiet + noise_quiet_hiss, -32768, 32767)
    add_scenario(
        "near_quiet",
        "Quiet speech (-35 dBFS RMS) with faint hiss; AGC must lift it",
        {"reference": np.zeros(quiet_n), "mic0": mic, "near_clean": speech_quiet},
        {"speech_labels": quiet_lab, "echo_labels": np.zeros(quiet_n)},
    )

    loud_n = len(speech_loud)
    mic = np.clip(speech_loud, -32768, 32767)
    add_scenario(
        "near_loud",
        "Loud speech (-9 dBFS RMS); AGC must hold the ceiling",
        {"reference": np.zeros(loud_n), "mic0": mic, "near_clean": speech_loud},
        {"speech_labels": loud_lab, "echo_labels": np.zeros(loud_n)},
    )

    # ---------------- double talk ----------------
    echo = convolve(music, ir) * db(-6.0)
    mic = np.clip(echo + speech, -32768, 32767)
    add_scenario(
        "doubletalk_music",
        "Near speech over music echo; both active for most of the clip",
        {"reference": music, "mic0": mic, "near_clean": speech, "echo_copy": echo},
        {"speech_labels": speech_lab, "echo_labels": ref_music_active},
        {"echo_delay_samples": echo_delay, "echo_gain_db": -6},
    )

    echo = convolve(music, room_ir(echo_delay, seed=52, gain=1.0)) * db(-2.0)
    quiet_padded = np.concatenate([speech_quiet, np.zeros(n - len(speech_quiet))])
    quiet_lab_padded = np.concatenate([quiet_lab, np.zeros(n - len(quiet_lab))])
    mic = np.clip(echo + quiet_padded, -32768, 32767)
    add_scenario(
        "doubletalk_quiet_speech",
        "Quiet near speech under loud echo; the hard barge-in case",
        {"reference": music, "mic0": mic, "near_clean": quiet_padded},
        {"speech_labels": quiet_lab_padded, "echo_labels": ref_music_active},
        {"echo_delay_samples": echo_delay, "echo_gain_db": -2},
    )

    # ---------------- beamforming ----------------
    def beam_scenario(name, mic_delays, interference_delay, n_mics):
        target, tlab = FormantVoice(f0_base=120.0, seed=61).sentence(10.0, seed=5)
        target = scale_to_rms_db(target, -22.0)
        bn = len(target)
        interference_raw, _ = FormantVoice(f0_base=100.0, seed=62).sentence(bn / SAMPLE_RATE, seed=6)
        interference = scale_to_rms_db(interference_raw, -28.0)
        r = np.random.default_rng(63)
        tracks = {}
        for m in range(n_mics):
            mic_m = frac_delay(target, mic_delays[m]) + frac_delay(interference, interference_delay + mic_delays[m])
            mic_m += r.standard_normal(bn) * db(-38.0) * 32768.0  # uncorrelated sensor noise
            tracks[f"mic{m}"] = np.clip(mic_m, -32768, 32767)
        add_scenario(
            name,
            f"{n_mics}-mic array: target speech at TDOA {mic_delays}, interference at {interference_delay}",
            tracks,
            {"speech_labels": tlab, "echo_labels": np.zeros(bn)},
            {"mic_delays": mic_delays, "interference_delay": interference_delay},
        )

    beam_scenario("beamforming_2mic", [0.0, 3.5], -5.2, 2)
    beam_scenario("beamforming_4mic", [0.0, 2.2, 4.4, 6.6], -5.0, 4)

    # Beamforming + echo combined (used by the pipeline suite): far-end echo
    # hits both mics with slightly different paths; near speech during the
    # second half exercises DT.
    ir0 = room_ir(24, seed=71, gain=0.5)
    ir1 = room_ir(28, seed=72, gain=0.45)
    e0 = convolve(music, ir0)
    e1 = convolve(music, ir1)
    half = n // 2
    t_near, t_lab = FormantVoice(f0_base=115.0, seed=73).sentence(dur / 2, seed=7)
    t_near = scale_to_rms_db(t_near, -22.0)
    speech_full = np.concatenate([np.zeros(half), t_near])[:n]
    lab_full = np.concatenate([np.zeros(half), (t_lab > 0.5).astype(np.float32)])[:n]
    r = np.random.default_rng(74)
    mic0 = np.clip(e0 + speech_full + r.standard_normal(n) * db(-42.0) * 32768.0, -32768, 32767)
    mic1 = np.clip(e1 + frac_delay(speech_full, 3.5) + r.standard_normal(n) * db(-42.0) * 32768.0, -32768, 32767)
    add_scenario(
        "beamforming_echo_2mic",
        "2-mic array with music echo on both mics; near speech in the second half",
        {"reference": music, "mic0": mic0, "mic1": mic1, "near_clean": speech_full},
        {"speech_labels": lab_full, "echo_labels": ref_music_active},
        {"mic_delays": [0.0, 3.5], "echo_delay_samples": 24},
    )

    # ---------------- silence ----------------
    sil = r.standard_normal(n) * db(-72.0) * 32768.0
    add_scenario(
        "silence",
        "Room tone only; VAD false-positive and AGC stability check",
        {"reference": np.zeros(n), "mic0": np.clip(sil, -32768, 32767)},
        {"speech_labels": np.zeros(n), "echo_labels": np.zeros(n)},
    )

    manifest = {
        "generator": "tools/generate_signals.py",
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "scenarios": scenarios,
    }
    (OUT_DIR / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"generated {len(scenarios)} scenarios in {OUT_DIR}")


if __name__ == "__main__":
    sys.exit(main())
