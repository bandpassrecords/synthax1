#!/usr/bin/env python3
"""Build an ablation set from real the reference synth presets (docs/SOUND_PARITY.md).

  parity_presets.py OUTDIR BANK.fxb[@N] [BANK.fxb[@N] ...] [--count N] [--only 1,5,9]
                    [--variants full,nofx,...|ablation] [--fx-each] [--name presets]

Picks N presets spread across each bank (BANK.fxb@N overrides --count for that bank; for a
single bank, --only takes 1-based indices instead)
and writes, for each, cumulative variants, so the point where Synthax1 and the reference synth
converge shows which module is responsible for the remaining difference:

  full      the preset as it is
  nofx      + all effects off
  noarp     + arpeggiator off
  nomod     + every modulation amount zero
  nofilter  + both filters bypassed
  onevoice  + one oscillator voice, centred and without detune (what is left: the bare oscillators)

--variants keeps only the listed ones (default: all six). --fx-each adds, for every effect the
preset switches on, a variant with that effect alone on ("fx-reverb", "fx-delay", ...), so each
effect is compared on real material.

Output: OUTDIR/NAME.fxb, NAME.json (the manifest the other parity tools read) and, up to 128
programs, NAME.mid for the Cubase fallback. A bank holds 512 programs. Render with LegacyRenderHost
and Synthax1Render --legacy-bank, compare with parity_compare.py; its "module" column is the
variant, so its per-module mean is the ablation table.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import legacy_bank as sb  # noqa: E402
import legacy_reference_bank as ref  # noqa: E402
from legacy_reference_bank import Note, Program  # noqa: E402

NAMES = sb.field_names()
FX_SWITCHES = ["XSwChorusOnOff", "XSwCompOnOff", "XSwDelayOnOff", "XSwDistOnOff", "XSwEQOnOff", "XSwPhaserOnOff", "XSwReverbOnOff"]
FX_LABELS = {"XSwChorusOnOff": "chorus", "XSwCompOnOff": "comp", "XSwDelayOnOff": "delay", "XSwDistOnOff": "dist",
             "XSwEQOnOff": "eq", "XSwPhaserOnOff": "phaser", "XSwReverbOnOff": "reverb"}
ABLATION = ["full", "nofx", "noarp", "nomod", "nofilter", "onevoice"]
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


def fx_each(patch: sb.Patch) -> list[tuple[str, sb.Patch]]:
    out = []
    for switch in FX_SWITCHES:
        if patch.values[NAMES.index(switch)] < 0.5:
            continue
        alone = sb.Patch(patch.name, list(patch.values), list(patch.extra))
        set_values(alone, [other for other in FX_SWITCHES if other != switch], 0.0)
        out.append(("fx-" + FX_LABELS[switch], alone))
    return out


def used_programs(doc) -> list[int]:
    if hasattr(sb, "is_filler"):
        return [i for i, p in enumerate(doc.patches) if not sb.is_filler(p)]
    return [i for i, p in enumerate(doc.patches) if p.name not in ("Init", "")]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("outdir", type=Path)
    parser.add_argument("banks", nargs="+")
    parser.add_argument("--count", type=int, default=20, help="presets per bank")
    parser.add_argument("--only", help="1-based program indices (single bank only)")
    parser.add_argument("--variants", default="ablation", help="comma-separated variant labels, or 'ablation'")
    parser.add_argument("--fx-each", action="store_true", help="add one variant per effect the preset uses")
    parser.add_argument("--name", default="presets", help="base name of the output files")
    args = parser.parse_args()
    wanted = ABLATION if args.variants == "ablation" else [v for v in args.variants.split(",") if v]
    unknown = [v for v in wanted if v not in ABLATION]
    if unknown:
        raise SystemExit(f"unknown variants {unknown}; choose from {ABLATION}")
    if args.only and len(args.banks) != 1:
        raise SystemExit("--only needs exactly one bank")

    patches: list[sb.Patch] = []
    programs: list[Program] = []
    sources: list[dict] = []
    infos = []
    chosen_count = 0
    full = False
    for bank_number, bank_argument in enumerate(args.banks, 1):
        bank_path, _, bank_count = bank_argument.rpartition("@") if "@" in bank_argument else (bank_argument, "", "")
        bank_path = Path(bank_path)
        doc = sb.read(bank_path.read_bytes())
        infos.append(doc.info)
        used = used_programs(doc)
        if args.only:
            chosen = [int(x) - 1 for x in args.only.split(",")]
        else:
            count = min(int(bank_count) if bank_count else args.count, len(used))
            chosen = [used[round(k * (len(used) - 1) / max(1, count - 1))] for k in range(count)] if count > 0 else []
            chosen = list(dict.fromkeys(chosen))
        for index in chosen:
            made = [(label, patch) for label, patch in variants(doc.patches[index]) if label in wanted]
            if args.fx_each:
                made += fx_each(doc.patches[index])
            if not made:
                continue
            if len(patches) + len(made) > sb.PROGRAMS_PER_BANK:
                full = True
                break
            prefix = f"{index + 1:03d}" if len(args.banks) == 1 else f"{bank_number:02d}-{index + 1:03d}"
            for label, patch in made:
                patch.name = f"{prefix} {label}"[:35]
                patches.append(patch)
                programs.append(Program(patch.name, label, doc.patches[index].name, ["spectrum", "level"], {}, NOTES, BEATS))
                sources.append({"source_preset": doc.patches[index].name, "source_bank": bank_path.name,
                                "source_program": index + 1})
            chosen_count += 1
        if full:
            print(f"stopped at {len(patches)} programs: a bank holds {sb.PROGRAMS_PER_BANK}")
            break
    if not programs:
        raise SystemExit("no programs selected")

    args.outdir.mkdir(parents=True, exist_ok=True)
    info = sb.info_for_sources(infos, "FBCh")
    patches += [sb.init_patch() for _ in range(sb.PROGRAMS_PER_BANK - len(patches))]
    (args.outdir / f"{args.name}.fxb").write_bytes(sb.write_bank(patches, info))
    midi, timeline = ref.midi_file(programs) if len(programs) <= 128 else midi_timeline(programs)
    for entry, source in zip(timeline, sources):
        entry.update(source)
    if midi is not None:
        (args.outdir / f"{args.name}.mid").write_bytes(midi)
    total = timeline[-1]["start_s"] + timeline[-1]["length_s"] + 2.0
    (args.outdir / f"{args.name}.json").write_text(json.dumps({"tempo_bpm": ref.TEMPO_BPM, "total_s": total, "programs": timeline}, indent=1),
                                                  encoding="utf-8")
    print(f"{chosen_count} presets from {len(args.banks)} bank(s): {len(programs)} programs, {total:.0f} s"
          + ("" if midi is not None else " (no .mid: program change reaches 128 programs)"))


def midi_timeline(programs: list[Program]) -> tuple[None, list[dict]]:
    """The manifest timeline for more than 128 programs (the MIDI file itself is not written)."""
    timeline = []
    beat = ref.LEAD_IN_BEATS
    for index, program in enumerate(programs):
        timeline.append({"program": index + 1, "name": program.name, "module": program.module,
                         "purpose": program.purpose, "measure": program.measure,
                         "start_s": beat * ref.SECONDS_PER_BEAT, "length_s": program.beats * ref.SECONDS_PER_BEAT,
                         "notes": [{"pitch": n.pitch, "velocity": n.velocity, "on_s": (beat + n.start) * ref.SECONDS_PER_BEAT,
                                    "off_s": (beat + n.start + n.length) * ref.SECONDS_PER_BEAT} for n in program.notes],
                         "values": program.values})
        beat += program.beats
    return None, timeline


if __name__ == "__main__":
    main()
