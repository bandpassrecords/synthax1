#!/usr/bin/env python3
"""Compare each effect's contribution in Synthax1 and the reference synth (docs/SOUND_PARITY.md).

  parity_effects.py LEGACY_DIR SYNTHAX1_DIR MANIFEST.json [--worst N]

Reads a set built by parity_presets.py --variants full,nofx --fx-each: per preset a dry variant
("nofx") and one variant per effect with only that effect on ("fx-reverb", ...). An effect's
contribution is what it changes against the dry variant of the same preset, in the same synth,
so the comparison does not depend on how well the dry sounds already match:

  spectral   mean absolute difference, in dB, between the two synths' effect responses
             (1/6-octave spectrum of the effect variant minus that of the dry variant, on the
             held note that is loudest in the reference synth's dry render, 60 Hz to 16 kHz)
  level      the effect's level change in the reference synth, then in Synthax1, in dB
  tail       the level in the second after the last note-off, relative to the held note,
             in each synth (decay of reverb and delay)

Prints one line per effect type (means over the presets that use it) and, with --worst N, the
N presets per type where the responses differ most. Needs NumPy and SciPy.
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parity_compare import smoothed_spectrum, steady_segment  # noqa: E402


def tail_level_db(renders: Path, program: dict, held_level: float) -> float:
    rate, data = wavfile.read(next(renders.glob(f"{program['program']:02d} *.wav")))
    x = data.astype(np.float64)
    x = x.mean(axis=1) if x.ndim == 2 else x
    last_off = max(note["off_s"] for note in program["notes"]) - program["start_s"]
    start = int((last_off + 0.05) * rate)
    segment = x[start:start + rate]
    level = np.sqrt((segment ** 2).mean()) if len(segment) else 0.0
    return 20 * np.log10((level + 1e-9) / (held_level + 1e-9))


def measure(renders: Path, program: dict, note: int) -> tuple[np.ndarray, float, float]:
    rate, segment, _ = steady_segment(renders, program, note)
    _, spectrum = smoothed_spectrum(segment, rate)
    held = float(np.sqrt((segment ** 2).mean())) if len(segment) else 0.0
    return spectrum, 20 * np.log10(held + 1e-12), tail_level_db(renders, program, held)


def main() -> None:
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    legacy, synthax1, manifest_path = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    worst = int(sys.argv[sys.argv.index("--worst") + 1]) if "--worst" in sys.argv else 0
    programs = json.loads(manifest_path.read_text(encoding="utf-8"))["programs"]
    by_preset: dict[tuple, dict[str, dict]] = defaultdict(dict)
    for program in programs:
        key = (program.get("source_bank", ""), program.get("source_program", program["name"].split()[0]))
        by_preset[key][program["module"]] = program

    rows = defaultdict(list)
    for key, variants in by_preset.items():
        dry = variants.get("nofx")
        if dry is None:
            continue
        # Every variant of a preset is measured on the note that is loudest in the reference synth's dry render.
        note = steady_segment(legacy, dry)[2]
        dry_measures = {side: measure(path, dry, note) for side, path in (("legacy", legacy), ("synthax1", synthax1))}
        for label, program in variants.items():
            if not label.startswith("fx-"):
                continue
            wet = {side: measure(path, program, note) for side, path in (("legacy", legacy), ("synthax1", synthax1))}
            response = {side: wet[side][0] - dry_measures[side][0] for side in wet}
            valid = (dry_measures["legacy"][0] > -150) & (dry_measures["synthax1"][0] > -150)
            spectral = float(np.abs(response["legacy"] - response["synthax1"])[valid].mean()) if valid.any() else float("nan")
            rows[label[3:]].append({
                "preset": f"{dry['name'].split()[0]} {dry['purpose']}",
                "spectral": spectral,
                "level_legacy": wet["legacy"][1] - dry_measures["legacy"][1],
                "level_synthax1": wet["synthax1"][1] - dry_measures["synthax1"][1],
                "tail_legacy": wet["legacy"][2],
                "tail_synthax1": wet["synthax1"][2],
            })

    print(f"{'effect':10s} {'presets':>7s} {'spectral':>9s} {'level ref':>10s} {'level syn':>10s} {'tail ref':>9s} {'tail syn':>9s}")
    for effect, entries in sorted(rows.items(), key=lambda item: -np.nanmean([e["spectral"] for e in item[1]])):
        mean = lambda field: float(np.nanmean([e[field] for e in entries]))  # noqa: E731
        print(f"{effect:10s} {len(entries):7d} {mean('spectral'):7.1f} dB {mean('level_legacy'):+7.1f} dB {mean('level_synthax1'):+7.1f} dB"
              f" {mean('tail_legacy'):+6.1f} dB {mean('tail_synthax1'):+6.1f} dB")
    if worst:
        for effect, entries in sorted(rows.items()):
            print(f"\n{effect}: largest differences")
            for entry in sorted(entries, key=lambda e: -e["spectral"])[:worst]:
                print(f"  {entry['spectral']:5.1f} dB  level {entry['level_legacy']:+5.1f}/{entry['level_synthax1']:+5.1f}"
                      f"  tail {entry['tail_legacy']:+6.1f}/{entry['tail_synthax1']:+6.1f}  {entry['preset']}")


if __name__ == "__main__":
    main()
