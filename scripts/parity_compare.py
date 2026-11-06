#!/usr/bin/env python3
"""Rank the reference programs by how far Synthax1 is from the reference synth (docs/SOUND_PARITY.md).

  parity_compare.py LEGACY_DIR SYNTHAX1_DIR MANIFEST.json [--worst N]

Per program, on each program's loudest steady note: level difference in dB, and the spectral
distance: mean absolute dB difference between the two 1/6-octave-smoothed spectra from 60 Hz
to 16 kHz (0 = identical shape, bigger = more different). Free-running phases make
sample-by-sample comparison meaningless, so everything here is spectral or level based.
Needs NumPy and SciPy.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import welch


def steady_segment(renders: Path, program: dict) -> tuple[int, np.ndarray]:
    rate, data = wavfile.read(next(renders.glob(f"{program['program']:02d} *.wav")))
    x = data.astype(np.float64)
    x = x.mean(axis=1) if x.ndim == 2 else x
    best = None
    for note in program["notes"]:
        on, off = note["on_s"] - program["start_s"], note["off_s"] - program["start_s"]
        start, stop = int((on + min(0.35, (off - on) * 0.4)) * rate), int(off * rate)
        if stop - start < rate // 4:
            start = int(on * rate + 0.05 * rate)
        segment = x[start:stop]
        level = np.sqrt((segment ** 2).mean()) if len(segment) else 0.0
        if best is None or level > best[0]:
            best = (level, segment)
    return rate, best[1]


def smoothed_spectrum(x: np.ndarray, rate: int) -> tuple[np.ndarray, np.ndarray]:
    f, pxx = welch(x, rate, nperseg=min(16384, len(x)))
    centres = 60.0 * 2 ** (np.arange(0, np.log2(16000 / 60) * 6) / 6)
    out = []
    for c in centres:
        band = (f >= c / 2 ** (1 / 12)) & (f < c * 2 ** (1 / 12))
        out.append(10 * np.log10(pxx[band].mean() + 1e-20) if band.any() else -200.0)
    return centres, np.array(out)


def main() -> None:
    legacy, synthax1, manifest_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    worst = int(sys.argv[sys.argv.index("--worst") + 1]) if "--worst" in sys.argv else 0
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    rows = []
    for program in manifest["programs"]:
        rate, a = steady_segment(legacy, program)
        _, b = steady_segment(synthax1, program)
        level_a = 20 * np.log10(np.sqrt((a ** 2).mean()) + 1e-12)
        level_b = 20 * np.log10(np.sqrt((b ** 2).mean()) + 1e-12)
        centres, sa = smoothed_spectrum(a, rate)
        _, sb = smoothed_spectrum(b, rate)
        keep = (sa > sa.max() - 80) | (sb > sb.max() - 80)
        distance = float(np.abs(sa[keep] - sb[keep]).mean()) if keep.any() else 0.0
        rows.append((program["name"], program["module"], level_b - level_a, distance))
    rows.sort(key=lambda r: -r[3])
    print(f"{'program':26s} {'module':11s} {'level diff':>10s} {'spectral dist':>13s}")
    for name, module, level, distance in rows[: worst or len(rows)]:
        print(f"{name:26s} {module:11s} {level:+9.1f} dB {distance:11.1f} dB")
    by_module: dict[str, list[float]] = {}
    for _, module, _, distance in rows:
        by_module.setdefault(module, []).append(distance)
    print("\nmean spectral distance per module:",
          ", ".join(f"{m} {np.mean(v):.1f}" for m, v in sorted(by_module.items(), key=lambda kv: -np.mean(kv[1]))))


if __name__ == "__main__":
    main()
