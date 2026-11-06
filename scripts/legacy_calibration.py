#!/usr/bin/env python3
"""Calibrate the knob curves LegacyPatchMapper still estimates, using the reference synth in any host.

  legacy_calibration.py bank  OUT.fxb [--csv readings.csv]
      Writes a bank of five programs ("CAL 0.00" .. "CAL 1.00") that set every estimated knob
      to 0, 0.25, 0.5, 0.75 and 1, plus a CSV to fill in. Load the bank in the reference synth, select
      each CAL program in turn, touch each listed knob without moving it (the reference synth shows its
      value on the display) and type that value into the CSV.

  legacy_calibration.py fit   readings.csv
      Fits each knob's readings with linear, exponential and power curves and prints the best
      fit with its worst error, ready to paste into the `curves` namespace.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import legacy_bank as sb  # noqa: E402

POINTS = [0.0, 0.25, 0.5, 0.75, 1.0]

# Knobs whose display curve has not been measured. Sync stays off so times read in ms/Hz.
KNOBS = [
    ("PortaTime", "Portamento time (top bar)"),
    ("ChorusRate", "Chorus page: Rate"),
    ("PhaserCenterFreq", "Phaser page: Center Freq"),
    ("PhaserLFORate", "Phaser page: LFO Rate"),
    ("CompAttack", "Compressor page: Attack"),
    ("CompRelease", "Compressor page: Release"),
    ("MixA", "Mixer: Mix A"),
    ("MainVolume", "Main Volume"),
    ("DelayTimeLeft", "Delay page: Delay L (Sync off)"),
]


def command_bank(args) -> None:
    names = sb.field_names()
    patches = []
    for point in POINTS:
        patch = sb.init_patch()
        patch.name = f"CAL {point:.2f}"
        for field_name, _ in KNOBS:
            patch.values[names.index(field_name)] = point
        patch.values[names.index("Sync")] = 0.0
        patches.append(patch)
    patches += [sb.init_patch() for _ in range(sb.PROGRAMS_PER_BANK - len(patches))]
    args.output.write_bytes(sb.write_bank(patches, sb.Info()))
    print(f"wrote {args.output}: {len(POINTS)} calibration programs")

    csv_path = args.csv or args.output.with_suffix(".csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["field", "where", *[f"CAL {p:.2f}" for p in POINTS]])
        for field_name, where in KNOBS:
            writer.writerow([field_name, where, *[""] * len(POINTS)])
    print(f"wrote {csv_path}: fill in the displayed number for each knob and program (units not needed)")


def fit(points: list[tuple[float, float]]) -> list[tuple[float, str]]:
    xs, ys = zip(*points)
    results = []

    def error(model) -> float:
        span = max(ys) - min(ys) or 1.0
        return max(abs(model(x) - y) for x, y in points) / span

    # Linear: y = a + b x.
    n = len(points)
    mean_x, mean_y = sum(xs) / n, sum(ys) / n
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / sum((x - mean_x) ** 2 for x in xs)
    intercept = mean_y - slope * mean_x
    results.append((error(lambda x: intercept + slope * x), f"{intercept:.6g} + {slope:.6g} * n"))

    # Exponential: y = a e^(b x), fitted in log space (needs positive readings).
    if all(y > 0 for y in ys):
        logs = [math.log(y) for y in ys]
        mean_l = sum(logs) / n
        b = sum((x - mean_x) * (l - mean_l) for x, l in zip(xs, logs)) / sum((x - mean_x) ** 2 for x in xs)
        a = math.exp(mean_l - b * mean_x)
        results.append((error(lambda x: a * math.exp(b * x)), f"{a:.6g}f * std::exp({b:.6g}f * n)"))

    # Power: y = y0 + (y1 - y0) x^k, k chosen by search.
    y0, y1 = ys[xs.index(min(xs))], ys[xs.index(max(xs))]
    best = min((error(lambda x, k=k: y0 + (y1 - y0) * x ** k), k) for k in [i / 20 for i in range(4, 121)])
    results.append((best[0], f"{y0:.6g}f + {y1 - y0:.6g}f * std::pow(n, {best[1]:.3g}f)"))
    return sorted(results)


def command_fit(args) -> None:
    with open(args.csv, newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    for row in rows:
        points = []
        for point in POINTS:
            text = (row.get(f"CAL {point:.2f}") or "").strip().replace(",", ".")
            number = "".join(ch for ch in text if ch in "-.0123456789")
            if number:
                points.append((point, float(number)))
        if len(points) < 3:
            print(f"{row['field']:18s} needs at least 3 readings")
            continue
        (worst, formula), *_ = fit(points)
        print(f"{row['field']:18s} {formula:55s} worst error {worst * 100:.1f}% of range")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    sub = commands.add_parser("bank")
    sub.add_argument("output", type=Path)
    sub.add_argument("--csv", type=Path)
    sub.set_defaults(handler=command_bank)
    sub = commands.add_parser("fit")
    sub.add_argument("csv", type=Path)
    sub.set_defaults(handler=command_fit)
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
