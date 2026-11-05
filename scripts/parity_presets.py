#!/usr/bin/env python3
"""Build an ablation set from real Sylenth1 presets (docs/SOUND_PARITY.md).

  parity_presets.py OUTDIR BANK.fxb [--count N] [--only 1,5,9]

Picks N presets spread across the bank (or the 1-based indices of --only) and writes, for each,
cumulative variants, so the point where Synthax1 and Sylenth1 converge shows which module is
responsible for the remaining difference:

  full      the preset as it is
  nofx      + all effects off
  noarp     + arpeggiator off
  nomod     + every modulation amount zero
  nofilter  + both filters bypassed
  onevoice  + one oscillator voice, centred and without detune (what is left: the bare oscillators)

Output: OUTDIR/presets.fxb, presets.mid and presets.json (the same manifest format the other parity
tools read). Render with SylenthRenderHost and Synthax1Render --sylenth-bank, compare with
parity_compare.py; its "module" column is the variant, so its per-module mean is the ablation table.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sylenth_bank as sb  # noqa: E402
import sylenth_reference_bank as ref  # noqa: E402
from sylenth_reference_bank import Note, Program  # noqa: E402

NAMES = sb.field_names()
FX_SWITCHES = ["XSwChorusOnOff", "XSwCompOnOff", "XSwDelayOnOff", "XSwDistOnOff", "XSwEQOnOff", "XSwPhaserOnOff", "XSwReverbOnOff"]
MOD_AMOUNTS = [n for n in NAMES if n.startswith("XMod") and n.endswith("Am")]
OSC_VOICES = ["OscA1Voices", "OscA2Voices", "OscB1Voices", "OscB2Voices"]
OSC_DETUNE = [n.replace("Voices", "Detune") for n in OSC_VOICES]
OSC_STEREO = [n.replace("Voices", "Stereo") for n in OSC_VOICES]

# A short performance: a low held fifth, then a higher note, so pads, basses and leads all say something.
NOTES = [Note(45, 0.25, 5.0), Note(52, 0.25, 5.0), Note(69, 6.0, 1.5)]
BEATS = 8


def set_values(patch: sb.Patch, names: list[str], value: float) -> None:
    for name in names:
        patch.values[NAMES.index(name)] = value


def variants(patch: sb.Patch) -> list[tuple[str, sb.Patch]]:
    out = []

    def step(label: str, mutate) -> None:
        nonlocal patch
        patch = sb.Patch(patch.name, list(patch.values), list(patch.extra))
        mutate(patch)
        out.append((label, patch))

    step("full", lambda p: None)
    step("nofx", lambda p: set_values(p, FX_SWITCHES, 0.0))
    step("noarp", lambda p: set_values(p, ["XSwArpOnOff"], 0.0))
    step("nomod", lambda p: set_values(p, MOD_AMOUNTS, 0.5))
    step("nofilter", lambda p: set_values(p, ["FilterAType", "FilterBType"], 0.0))

    def one_voice(p: sb.Patch) -> None:
        for name in OSC_VOICES:
            if p.values[NAMES.index(name)] > 0:
                p.values[NAMES.index(name)] = 1 / 8
        set_values(p, OSC_DETUNE, 0.0)
        set_values(p, OSC_STEREO, 0.0)

    step("onevoice", one_voice)
    return out


def main() -> None:
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    outdir, bank_path = Path(args[0]), Path(args[1])
    doc = sb.read(bank_path.read_bytes())
    used = [i for i, p in enumerate(doc.patches) if not sb.is_filler(p) if hasattr(sb, "is_filler")] if hasattr(sb, "is_filler") \
        else [i for i, p in enumerate(doc.patches) if p.name not in ("Init", "")]
    if "--only" in args:
        chosen = [int(x) - 1 for x in args[args.index("--only") + 1].split(",")]
    else:
        count = int(args[args.index("--count") + 1]) if "--count" in args else 20
        count = min(count, len(used))
        chosen = [used[round(k * (len(used) - 1) / max(1, count - 1))] for k in range(count)]

    patches: list[sb.Patch] = []
    programs: list[Program] = []
    for index in chosen:
        for label, patch in variants(doc.patches[index]):
            patch.name = f"{index + 1:03d} {label}"[:35]
            patches.append(patch)
            programs.append(Program(patch.name, label, doc.patches[index].name, ["spectrum", "level"], {}, NOTES, BEATS))
    if len(programs) > 128:
        raise SystemExit(f"{len(programs)} programs; use --count {128 // 6} or fewer")

    outdir.mkdir(parents=True, exist_ok=True)
    patches += [sb.init_patch() for _ in range(sb.PROGRAMS_PER_BANK - len(patches))]
    (outdir / "presets.fxb").write_bytes(sb.write_bank(patches, sb.Info(chunk_version=doc.info.chunk_version, fields=doc.info.fields, compressed=doc.info.compressed)))
    midi, timeline = ref.midi_file(programs)
    for entry, program in zip(timeline, programs):
        entry["source_preset"] = program.purpose
    (outdir / "presets.mid").write_bytes(midi)
    total = timeline[-1]["start_s"] + timeline[-1]["length_s"] + 2.0
    (outdir / "presets.json").write_text(json.dumps({"tempo_bpm": ref.TEMPO_BPM, "total_s": total, "programs": timeline}, indent=1), encoding="utf-8")
    print(f"{len(chosen)} presets x {len(programs) // len(chosen)} variants = {len(programs)} programs, {total:.0f} s")
    for index in chosen:
        print(f"  {index + 1:3d}  {doc.patches[index].name}")


if __name__ == "__main__":
    main()
