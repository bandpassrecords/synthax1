#!/usr/bin/env python3
"""Build the sound-parity reference set: a the reference synth bank whose programs each isolate one
module, a MIDI file that plays every program in turn, and a manifest saying when each
program sounds and what it is meant to measure. See docs/SOUND_PARITY.md.

  legacy_reference_bank.py OUTDIR

writes OUTDIR/Synthax1Reference.fxb, Synthax1Reference.mid and Synthax1Reference.json.

Every program starts from the reference synth's Init (one saw voice on A1, filters bypassed, effects
off, retrigger on), so differences come from the module under test. Times, frequencies and
rates are chosen in real units and converted with the curves measured for the mapper.
"""

from __future__ import annotations

import json
import math
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import legacy_bank as sb  # noqa: E402

TEMPO_BPM = 120.0
SECONDS_PER_BEAT = 60.0 / TEMPO_BPM
PPQ = 480
LEAD_IN_BEATS = 4.0  # one empty bar before the first program

NAMES = sb.field_names()

# ------------------------------------------------------------------ value helpers (normalized)
WAVES = {"sine": 0, "saw": 1, "triangle": 2, "pulse": 3, "hpulse": 4, "qpulse": 5, "trisaw": 6, "noise": 7}
FILTER_TYPES = {"bypass": 0, "lowpass": 1, "bandpass": 2, "highpass": 3}
LFO_WAVES = {"sine": 0, "saw": 1, "ramp": 2, "triangle": 3, "pulse": 4}
DIST_TYPES = {"overdrive": 0, "foldback": 1, "clip": 2, "decimate": 3, "bitcrush": 4}
DEST_VOLUME_A = 1 / 32
DEST_PITCH_A = 4 / 32
DEST_CUTOFF_A = 13 / 32
NEUTRAL_FILTER_CONTROL = math.log(1.0011 / 0.064749088) / 10.152717  # control knob adding nothing


def wave(name: str) -> float:
    return WAVES[name] / 7


def voices(count: int) -> float:
    return count / 8


def amount(display: float) -> float:
    """Mod amount on the reference synth's -10..10 display scale."""
    return 0.5 + display / 20


def cutoff(hz: float) -> float:
    """Part filter value giving `hz` with the Filter Control at its neutral position."""
    return math.log(hz / 0.064749088) / 10.152717 - NEUTRAL_FILTER_CONTROL


def envelope_ms(x: float) -> float:
    if x <= 0.1:
        return 2326.388889 * x ** 3 + 650.5952381 * x ** 2 + 204.6646825 * x + 0.541666667
    if x < 0.51:
        return (-10825.28025 * x ** 6 + 51052.14783 * x ** 5 - 37084.52751 * x ** 4
                + 17464.58482 * x ** 3 - 2556.284025 * x ** 2 + 537.7264341 * x - 12.90202926)
    return 28.18696734 * math.exp(6.970916827 * x)


def envelope(ms: float) -> float:
    low, high = 0.0, 1.0
    for _ in range(60):
        mid = (low + high) / 2
        low, high = (mid, high) if envelope_ms(mid) < ms else (low, mid)
    return (low + high) / 2


def lfo_rate(hz: float) -> float:
    return math.log(hz / 0.0417) / 8.4352


# ------------------------------------------------------------------ programs
@dataclass
class Note:
    pitch: int
    start: float   # beats from the program's start
    length: float  # beats
    velocity: int = 100


@dataclass
class Program:
    name: str
    module: str
    purpose: str
    measure: list[str]
    values: dict[str, float]
    notes: list[Note]
    beats: float
    extra: dict[str, float] = field(default_factory=dict)


LOW_HIGH = [Note(45, 0.25, 3.5), Note(93, 4.25, 3.25)]   # A2 then A6, for spectra and aliasing
HELD = [Note(57, 0.25, 6.75)]                             # A3 held
ENVELOPE = [Note(57, 0.25, 4.0)]                          # A3, 2 s held then released
PLUCKS = [Note(57, 0.25, 0.5), Note(60, 1.25, 0.5), Note(64, 2.25, 0.5), Note(69, 3.25, 0.5)]
PLUCK_SHAPE = {"AmpEnvAAttack": envelope(1.0), "AmpEnvADecay": envelope(250.0), "AmpEnvASustain": 0.0,
               "AmpEnvARelease": envelope(150.0)}


def programs() -> list[Program]:
    out: list[Program] = []
    add = out.append

    add(Program("REF Sine level", "level", "Overall gain and tuning: one sine voice at A4.",
                ["rms_db", "fundamental_hz"], {"OscA1Wave": wave("sine")}, [Note(69, 0.25, 3.5)], 8))

    for name in ("saw", "pulse", "triangle", "sine", "hpulse", "qpulse", "trisaw", "noise"):
        add(Program(f"OSC {name}", "oscillator", f"Single {name} voice, filter bypassed: harmonic shape and aliasing at A6.",
                    ["harmonic_profile", "alias_energy_db", "rms_db"], {"OscA1Wave": wave(name)}, LOW_HIGH, 8))

    for detune in (0.2, 0.5, 1.0):
        add(Program(f"UNI saw x8 det {detune:.1f}", "unison", f"8 saw voices, detune {detune * 10:.0f}, stereo 10.",
                    ["spectral_spread_cents", "stereo_width", "beating_rate_hz", "rms_db"],
                    {"OscA1Voices": voices(8), "OscA1Detune": detune, "OscA1Stereo": 1.0}, LOW_HIGH, 8))
    add(Program("UNI saw x8 mono", "unison", "8 saw voices, detune 5, stereo 0: unison without spread.",
                ["spectral_spread_cents", "stereo_width"],
                {"OscA1Voices": voices(8), "OscA1Detune": 0.5, "OscA1Stereo": 0.0}, LOW_HIGH, 8))

    for kind, db24 in (("lowpass", False), ("lowpass", True), ("bandpass", True), ("highpass", True)):
        label = f"FLT {kind[0].upper()}P{24 if db24 else 12} 1k"
        add(Program(label, "filter", f"Noise through {kind} {24 if db24 else 12} dB at 1 kHz, no resonance: frequency response.",
                    ["response_curve", "cutoff_hz", "slope_db_per_octave"],
                    {"OscA1Wave": wave("noise"), "FilterAType": FILTER_TYPES[kind] / 3,
                     "FilterADB": 0.0 if db24 else 1.0,  # stored 0 = 24 dB/octave, 1 = 12 dB/octave
                     "FilterACutoff": cutoff(1000.0), "FilterCtlCutoff": NEUTRAL_FILTER_CONTROL}, HELD, 8))
    for reso in (0.5, 0.8, 1.0):
        add(Program(f"FLT LP24 reso {reso * 10:.0f}", "filter", f"Noise through lowpass 24 dB at 1 kHz, resonance {reso * 10:.0f}.",
                    ["response_curve", "resonance_peak_db", "peak_hz"],
                    {"OscA1Wave": wave("noise"), "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": reso,
                     "FilterACutoff": cutoff(1000.0), "FilterCtlCutoff": NEUTRAL_FILTER_CONTROL}, HELD, 8))
    for drive in (0.5, 1.0):
        add(Program(f"FLT drive {drive * 10:.0f}", "filter", f"Saw through lowpass 24 dB at 4 kHz, drive {drive * 10:.0f}: saturation.",
                    ["harmonic_profile", "rms_db"],
                    {"FilterAType": 1 / 3, "FilterADB": 0.0, "FilterADrive": drive,
                     "FilterACutoff": cutoff(4000.0), "FilterCtlCutoff": NEUTRAL_FILTER_CONTROL}, LOW_HIGH, 8))
    add(Program("FLT warm drive", "filter", "As 'FLT drive 5' with Warm Drive on.", ["harmonic_profile", "rms_db"],
                {"FilterAType": 1 / 3, "FilterADB": 0.0, "FilterADrive": 0.5, "FilterCtlWarmDrive": 1.0,
                 "FilterACutoff": cutoff(4000.0), "FilterCtlCutoff": NEUTRAL_FILTER_CONTROL}, LOW_HIGH, 8))
    add(Program("FLT env sweep", "filter", "Saw, lowpass 24 dB at 200 Hz, mod env 1 (attack 1 s) opening Cutoff A by +5.",
                ["cutoff_track_hz"],
                {"FilterAType": 1 / 3, "FilterADB": 0.0, "FilterACutoff": cutoff(200.0), "FilterCtlCutoff": NEUTRAL_FILTER_CONTROL,
                 "ModEnv1Attack": envelope(1000.0), "ModEnv1Sustain": 1.0, "YModEnv1Dest1": DEST_CUTOFF_A,
                 "XModEnv1Dest1Am": amount(5.0)}, HELD, 8))

    for attack_ms in (300.0, 1000.0):
        add(Program(f"ENV attack {attack_ms:.0f}ms", "envelope", f"Amp envelope attack {attack_ms:.0f} ms: curve shape.",
                    ["amplitude_curve", "time_to_90pct_ms"],
                    {"OscA1Wave": wave("sine"), "AmpEnvAAttack": envelope(attack_ms)}, ENVELOPE, 12))
    for decay_ms in (300.0, 1000.0):
        add(Program(f"ENV decay {decay_ms:.0f}ms", "envelope", f"Amp envelope decay {decay_ms:.0f} ms to zero sustain.",
                    ["amplitude_curve", "time_to_10pct_ms"],
                    {"OscA1Wave": wave("sine"), "AmpEnvADecay": envelope(decay_ms), "AmpEnvASustain": 0.0}, ENVELOPE, 12))
    add(Program("ENV release 1500ms", "envelope", "Amp envelope release 1.5 s from full sustain.",
                ["amplitude_curve", "time_to_10pct_ms"],
                {"OscA1Wave": wave("sine"), "AmpEnvARelease": envelope(1500.0)}, ENVELOPE, 12))

    for shape in ("sine", "triangle", "saw", "pulse"):
        add(Program(f"LFO {shape} 2Hz vol", "lfo", f"LFO 1 {shape}, free 2 Hz, on Volume A (amount 5).",
                    ["modulation_shape", "modulation_rate_hz", "modulation_depth_db"],
                    {"OscA1Wave": wave("sine"), "LFO1Wave": LFO_WAVES[shape] / 10, "LFO1Free": 1.0, "LFO1Rate": lfo_rate(2.0),
                     "LFO1Gain": 1.0, "YModLFO1Dest1": DEST_VOLUME_A, "XModLFO1Dest1Am": amount(5.0)}, HELD, 8))
    add(Program("MOD pitch env +12", "modulation", "Mod env 1 (decay 500 ms) on Pitch A, amount +1 (= 12 semitones).",
                ["pitch_track_semitones"],
                {"OscA1Wave": wave("sine"), "ModEnv1Decay": envelope(500.0), "ModEnv1Sustain": 0.0,
                 "YModEnv1Dest1": DEST_PITCH_A, "XModEnv1Dest1Am": amount(1.0)}, HELD, 8))

    for name in DIST_TYPES:
        add(Program(f"FX dist {name}", "effect", f"Distortion {name}, amount 5, fully wet, on a held saw.",
                    ["harmonic_profile", "rms_db"],
                    {"XSwDistOnOff": 1.0, "DistortType": DIST_TYPES[name] / 4, "DistortAmount": 0.5, "DistortDryWet": 1.0},
                    LOW_HIGH, 8))
    add(Program("FX phaser", "effect", "Phaser at Init settings with 50% mix, free 2 Hz LFO, on a held saw.",
                ["spectral_notch_track", "modulation_rate_hz"],
                {"XSwPhaserOnOff": 1.0, "PhaserDry_Wet": 0.5, "PhaserFeedback": 0.5, "PhaserCenterFreq": 0.5,
                 "PhaserLFOGain": 0.5, "PhaserLFORate": lfo_rate(2.0), "PhaserSpread": 0.5, "PhaserWidth": 1.0,
                 "PhaserLROffset": 0.5}, HELD, 8))
    add(Program("FX chorus", "effect", "Chorus at Init settings (dual, 50% mix) on plucks.",
                ["stereo_width", "modulation_rate_hz"], {"XSwChorusOnOff": 1.0, **PLUCK_SHAPE}, PLUCKS, 16))
    # +12 dB (0.8 of the 0..15 dB range): Synthax1's EQ stops at +12, and this test is about the shelf shape.
    add(Program("FX EQ bass +12", "effect", "Noise through the EQ, bass +12 dB at the middle frequency.",
                ["response_curve"], {"OscA1Wave": wave("noise"), "XSwEQOnOff": 1.0, "EQBass": 0.8, "EQTreble": 0.0}, HELD, 8))
    add(Program("FX EQ treble +12", "effect", "Noise through the EQ, treble +12 dB at the middle frequency.",
                ["response_curve"], {"OscA1Wave": wave("noise"), "XSwEQOnOff": 1.0, "EQBass": 0.0, "EQTreble": 0.8}, HELD, 8))
    add(Program("FX reverb", "effect", "Reverb size 5, damp 5, 40% mix, on plucks.",
                ["decay_time_ms", "stereo_width", "tail_spectrum"],
                {"XSwReverbOnOff": 1.0, "ReverbSize": 0.5, "ReverbDamp": 0.5, "ReverbDry_Wet": 0.4, "ReverbWidth": 1.0,
                 **PLUCK_SHAPE}, PLUCKS, 16))
    add(Program("FX delay 1/4 pp", "effect", "Ping-pong delay 1/4 (synced), feedback 50%, 50% mix, on plucks.",
                ["echo_times_ms", "echo_decay_db", "stereo_alternation"],
                {"XSwDelayOnOff": 1.0, "Sync": 1.0, "DelayTimeLeft": 0.8, "DelayTimeRight": 0.8, "DelayFeedback": 0.5,
                 "DelayDry_Wet": 0.5, "DelayPingPong": 1.0, **PLUCK_SHAPE}, PLUCKS, 16))
    add(Program("FX comp", "effect", "Compressor, threshold -30 dB, ratio ~10:1, on plucks.",
                ["gain_reduction_db", "envelope_change"],
                {"XSwCompOnOff": 1.0, "CompThreshold": 0.0, "CompRatio": 0.9167, "CompAttack": 0.05, "CompRelease": 0.3,
                 **PLUCK_SHAPE}, PLUCKS, 16))
    return out


# ------------------------------------------------------------------ MIDI (type 0, one track)
def var_length(value: int) -> bytes:
    out = [value & 0x7F]
    value >>= 7
    while value:
        out.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(out))


def midi_file(program_list: list[Program]) -> tuple[bytes, list[dict]]:
    events: list[tuple[int, int, bytes]] = []  # (tick, order, bytes); order keeps program change before notes
    timeline = []
    beat = LEAD_IN_BEATS
    for index, program in enumerate(program_list):
        start_tick = round(beat * PPQ)
        events.append((start_tick, 0, bytes([0xC0, index])))
        for note in program.notes:
            on = round((beat + note.start) * PPQ)
            off = round((beat + note.start + note.length) * PPQ)
            events.append((on, 2, bytes([0x90, note.pitch, note.velocity])))
            events.append((off, 1, bytes([0x80, note.pitch, 0])))
        timeline.append({"program": index + 1, "name": program.name, "module": program.module,
                         "purpose": program.purpose, "measure": program.measure,
                         "start_s": beat * SECONDS_PER_BEAT, "length_s": program.beats * SECONDS_PER_BEAT,
                         "notes": [{"pitch": n.pitch, "velocity": n.velocity, "on_s": (beat + n.start) * SECONDS_PER_BEAT,
                                    "off_s": (beat + n.start + n.length) * SECONDS_PER_BEAT} for n in program.notes],
                         "values": program.values})
        beat += program.beats
    end_tick = round((beat + 4) * PPQ)

    tempo = round(60_000_000 / TEMPO_BPM)
    track = bytearray(b"\x00\xff\x51\x03" + tempo.to_bytes(3, "big"))
    track += b"\x00\xff\x58\x04\x04\x02\x18\x08"  # 4/4
    last = 0
    for tick, _, data in sorted(events):
        track += var_length(tick - last) + data
        last = tick
    track += var_length(end_tick - last) + b"\xff\x2f\x00"
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ)
    return header + b"MTrk" + struct.pack(">I", len(track)) + bytes(track), timeline


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    program_list = programs()
    if len(program_list) > 128:
        raise SystemExit("MIDI program change reaches only the first 128 programs")

    patches = []
    for program in program_list:
        patch = sb.init_patch()
        patch.name = program.name
        for name, value in program.values.items():
            if name not in NAMES:
                raise SystemExit(f"{program.name}: unknown field {name}")
            patch.values[NAMES.index(name)] = float(value)
        patches.append(patch)
    patches += [sb.init_patch() for _ in range(sb.PROGRAMS_PER_BANK - len(patches))]

    (out / "Synthax1Reference.fxb").write_bytes(sb.write_bank(patches, sb.Info()))
    midi, timeline = midi_file(program_list)
    (out / "Synthax1Reference.mid").write_bytes(midi)
    total = timeline[-1]["start_s"] + timeline[-1]["length_s"] + 2.0
    manifest = {"tempo_bpm": TEMPO_BPM, "sample_rate_hint": 48000, "lead_in_s": LEAD_IN_BEATS * SECONDS_PER_BEAT,
                "total_s": total, "programs": timeline}
    (out / "Synthax1Reference.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    print(f"{len(program_list)} programs, {total:.0f} s of audio -> {out}")


if __name__ == "__main__":
    main()
