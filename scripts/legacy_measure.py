#!/usr/bin/env python3
"""Measure rendered sweep sets (see legacy_sweeps.py). Needs NumPy and SciPy.

  legacy_measure.py RENDERS_DIR MANIFEST.json SET [--json OUT.json]

Prints one row per program and writes the raw measurements as JSON for curve fitting.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import hilbert, welch


def load(renders: Path, program: dict) -> tuple[int, np.ndarray]:
    path = next(renders.glob(f"{program['program']:02d} *.wav"))
    rate, data = wavfile.read(path)
    data = data.astype(np.float64)
    return rate, data


def mono(data: np.ndarray) -> np.ndarray:
    return data.mean(axis=1) if data.ndim == 2 else data


def envelope(signal: np.ndarray, rate: int, window_ms: float = 2.0) -> tuple[np.ndarray, float]:
    hop = max(1, int(rate * window_ms / 1000))
    frames = len(signal) // hop
    return np.sqrt((signal[: frames * hop].reshape(frames, hop) ** 2).mean(axis=1)), hop / rate


def note_times(program: dict) -> tuple[float, float]:
    note = program["notes"][0]
    return note["on_s"] - program["start_s"], note["off_s"] - program["start_s"]


def db(x: float) -> float:
    return 20 * np.log10(max(x, 1e-12))


# ------------------------------------------------------------------ per set
def measure_filter(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.3) * rate): int(off * rate)]
    f, pxx = welch(x, rate, nperseg=16384)
    band = (f > 15) & (f < 22000)
    peak = f[band][np.argmax(pxx[band])]
    return {"peak_hz": float(peak)}


def measure_lfo(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.5) * rate): int(off * rate)]
    amplitude = np.abs(hilbert(x))           # envelope without the carrier
    step = 0.001
    hop = int(rate * step)
    amplitude = amplitude[: len(amplitude) // hop * hop].reshape(-1, hop).mean(axis=1)
    seg = amplitude - amplitude.mean()
    n = 1 << int(np.ceil(np.log2(len(seg) * 16)))
    spectrum = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    freqs = np.fft.rfftfreq(n, step)
    valid = (freqs > 0.02) & (freqs < 400)
    return {"rate_hz": float(freqs[valid][np.argmax(spectrum[valid])])}


def crossing(env, start, level, step, falling):
    seg = env[start:]
    hits = np.where(seg <= level)[0] if falling else np.where(seg >= level)[0]
    return float(hits[0] * step * 1000) if len(hits) else float("nan")


def measure_env(rate, data, program):
    on, off = note_times(program)
    # Analytic-signal envelope: a plain RMS window ripples with the carrier (a 220 Hz sine has a
    # 4.5 ms period) and triggers threshold crossings early.
    amplitude = np.abs(hilbert(mono(data)))
    hop = max(1, int(rate * 0.001))
    env = amplitude[: len(amplitude) // hop * hop].reshape(-1, hop).mean(axis=1)
    step = hop / rate
    stage = program["sweep"]["stage"]
    i_on, i_off = int(on / step), int(off / step)
    if stage == "attack":
        level = env[i_off - 200:i_off - 20].mean()
        return {f"to_{p}pct_ms": crossing(env, i_on, p / 100 * level, step, False) for p in (10, 50, 90, 99)}
    if stage == "decay":
        level = env[i_on:i_on + 50].max()
        return {f"to_{p}pct_ms": crossing(env, i_on + int(np.argmax(env[i_on:i_on + 50])), p / 100 * level, step, True)
                for p in (50, 10, 1)}
    level = env[i_off - 50:i_off - 5].mean()
    return {f"to_{p}pct_ms": crossing(env, i_off, p / 100 * level, step, True) for p in (50, 10, 1)}


def measure_level(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.3) * rate): int(off * rate)]
    return {"rms_db": float(db(np.sqrt((x ** 2).mean())))}


def measure_drive(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.3) * rate): int(off * rate)]
    rms = np.sqrt((x ** 2).mean())
    return {"rms_db": float(db(rms)), "peak_db": float(db(np.abs(x).max())), "crest_db": float(db(np.abs(x).max()) - db(rms))}


def measure_unison(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.3) * rate): int(off * rate)]
    result = {"rms_db": float(db(np.sqrt((x ** 2).mean())))}
    if program["sweep"].get("detune", 0) > 0 or "detune" in program["name"]:
        n = 1 << int(np.ceil(np.log2(len(x) * 4)))
        spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
        freqs = np.fft.rfftfreq(n, 1 / rate)
        band = (freqs > 220 / 1.5) & (freqs < 220 * 1.5)
        s, fb = spectrum[band], freqs[band]
        # local maxima within 30 dB of the strongest
        floor = s.max() / 31.6
        peaks = [i for i in range(1, len(s) - 1) if s[i] > s[i - 1] and s[i] >= s[i + 1] and s[i] > floor]
        if peaks:
            cents = 1200 * np.log2(fb[peaks] / 220.0)
            result.update({"peaks": len(peaks), "spread_cents": float(cents.max() - cents.min()),
                           "outer_cents": float(np.abs(cents).max())})
    return result


def measure_lfodepth(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.5) * rate): int(off * rate)]
    amplitude = np.abs(hilbert(x))
    hop = int(rate * 0.005)
    env = amplitude[: len(amplitude) // hop * hop].reshape(-1, hop).mean(axis=1)
    high, low = np.percentile(env, 98), np.percentile(env, 2)
    return {"depth_db": float(db(high) - db(low)), "high_db": float(db(high)), "low_db": float(db(low))}


def measure_arpgate(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.1) * rate): int(off * rate)]
    amplitude = np.abs(hilbert(x))
    hop = int(rate * 0.001)
    env = amplitude[: len(amplitude) // hop * hop].reshape(-1, hop).mean(axis=1)
    loud = env > 0.25 * env.max() if env.max() > 1e-6 else np.zeros(len(env), bool)
    edges = np.flatnonzero(np.diff(loud.astype(int)))
    rises, falls = edges[np.diff(loud.astype(int))[edges] > 0], edges[np.diff(loud.astype(int))[edges] < 0]
    lengths = [int(falls[falls > r][0] - r) for r in rises if (falls > r).any()]
    period = float(np.median(np.diff(rises))) if len(rises) > 2 else float("nan")
    return {"on_ms": float(np.median(lengths)) if lengths else 0.0, "period_ms": period, "steps": int(len(rises))}


def measure_velocity(rate, data, program):
    return {**measure_level(rate, data, program), **measure_filter(rate, data, program)}


def measure_pitch(rate, data, program):
    on, off = note_times(program)
    x = mono(data)[int((on + 0.3) * rate): int(off * rate)]
    n = 1 << int(np.ceil(np.log2(len(x) * 8)))
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x)), n))
    freqs = np.fft.rfftfreq(n, 1 / rate)
    return {"hz": float(freqs[10:][np.argmax(spectrum[10:])])}


MEASURES = {"octbins": measure_pitch, "arpbins": measure_pitch, "arptrans": measure_pitch, "pitch": measure_pitch, "keytrack": measure_filter, "mixroute": measure_level, "wheel": measure_filter, "velocity": measure_velocity, "drive": measure_drive, "cutmod": measure_filter, "comp": measure_level, "arpgate": measure_arpgate, "lfodepth": measure_lfodepth, "filter": measure_filter, "lfo": measure_lfo, "env": measure_env, "level": measure_level,
            "unison": measure_unison}


def main() -> None:
    args = sys.argv[1:]
    if len(args) < 3 or args[2] not in MEASURES:
        raise SystemExit(__doc__)
    renders, manifest_path, name = Path(args[0]), Path(args[1]), args[2]
    out_json = Path(args[args.index("--json") + 1]) if "--json" in args else None
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    rows = []
    for program in manifest["programs"]:
        rate, data = load(renders, program)
        row = {"name": program["name"], **program.get("sweep", {}), **MEASURES[name](rate, data, program)}
        rows.append(row)
        print("  ".join(f"{k}={v:.4g}" if isinstance(v, float) else f"{k}={v}" for k, v in row.items()))
    if out_json:
        out_json.parent.mkdir(parents=True, exist_ok=True)
        out_json.write_text(json.dumps(rows, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
