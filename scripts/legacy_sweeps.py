#!/usr/bin/env python3
"""Knob sweeps for calibrating LegacyPatchMapper against the reference synth's real output.

  legacy_sweeps.py OUTDIR SET      writes OUTDIR/SET.fxb, SET.mid and SET.json

Each set is a bank of programs that differ in one knob only, played the same way. Render a
set with LegacyRenderHost and measure it with legacy_measure.py. Sets:

  filter   noise through LP24 (resonance 8): part cutoff swept at control 0.25/0.5/1.0, and the
           control swept at part 0.5. The resonance peak shows the cutoff frequency.
  lfo      LFO 1 free rate swept on Volume A.
  env      amp attack, decay and release swept.
  level    oscillator volume, mix A and main volume swept on a sine.
  unison   saw voices 1..8 and detune swept.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import legacy_bank as sb  # noqa: E402
import legacy_reference_bank as ref  # noqa: E402
from legacy_reference_bank import Note, Program  # noqa: E402

STEPS = [i / 20 for i in range(21)]
NOISE = ref.wave("noise")
HELD_SHORT = [Note(57, 0.25, 3.25)]  # A3 for 1.6 s, then 0.4 s release


def filter_set() -> list[Program]:
    out = []
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8}
    for control in (0.25, 0.5, 1.0):
        for value in STEPS:
            out.append(Program(f"cut {value:.2f} ctl {control:.2f}", "filter", "", ["peak_hz"],
                               {**base, "FilterACutoff": value, "FilterCtlCutoff": control}, HELD_SHORT, 4,
                               {"part": value, "control": control}))
    for value in STEPS:
        out.append(Program(f"ctl {value:.2f} cut 0.50", "filter", "", ["peak_hz"],
                           {**base, "FilterACutoff": 0.5, "FilterCtlCutoff": value}, HELD_SHORT, 4,
                           {"part": 0.5, "control": value}))
    return out


def lfo_set() -> list[Program]:
    base = {"OscA1Wave": ref.wave("sine"), "LFO1Wave": 0.0, "LFO1Free": 1.0, "LFO1Gain": 1.0,
            "YModLFO1Dest1": ref.DEST_VOLUME_A, "XModLFO1Dest1Am": ref.amount(5.0)}
    return [Program(f"lfo rate {v:.2f}", "lfo", "", ["modulation_rate_hz"], {**base, "LFO1Rate": v},
                    [Note(57, 0.25, 15.5)], 16, {"value": v}) for v in STEPS]


def env_set() -> list[Program]:
    out = []
    sine = ref.wave("sine")
    for v in STEPS:
        out.append(Program(f"attack {v:.2f}", "env", "", ["attack"], {"OscA1Wave": sine, "AmpEnvAAttack": v},
                           [Note(57, 0.25, 31.0)], 32, {"stage": "attack", "value": v}))
    for v in STEPS:
        out.append(Program(f"decay {v:.2f}", "env", "", ["decay"],
                           {"OscA1Wave": sine, "AmpEnvADecay": v, "AmpEnvASustain": 0.0},
                           [Note(57, 0.25, 31.0)], 32, {"stage": "decay", "value": v}))
    for v in STEPS:
        out.append(Program(f"release {v:.2f}", "env", "", ["release"], {"OscA1Wave": sine, "AmpEnvARelease": v},
                           [Note(57, 0.25, 2.0)], 32, {"stage": "release", "value": v}))
    return out


def level_set() -> list[Program]:
    out = []
    sine = ref.wave("sine")
    for field in ("OscA1Volume", "MixA", "MainVolume"):
        for v in STEPS:
            out.append(Program(f"{field} {v:.2f}", "level", "", ["rms_db"], {"OscA1Wave": sine, field: v},
                               HELD_SHORT, 4, {"field": field, "value": v}))
    return out


def unison_set() -> list[Program]:
    out = []
    saw = ref.wave("saw")
    for count in range(1, 9):
        out.append(Program(f"voices {count}", "unison", "", ["rms_db"],
                           {"OscA1Wave": saw, "OscA1Voices": count / 8, "OscA1Detune": 0.0, "OscA1Stereo": 0.0},
                           [Note(45, 0.25, 7.5)], 8, {"voices": count, "detune": 0.0}))
    for v in STEPS:
        out.append(Program(f"detune {v:.2f}", "unison", "", ["spread_cents"],
                           {"OscA1Wave": ref.wave("sine"), "OscA1Voices": 1.0, "OscA1Detune": v, "OscA1Stereo": 0.0},
                           [Note(57, 0.25, 15.5)], 16, {"voices": 8, "detune": v}))
    return out


def lfodepth_set() -> list[Program]:
    base = {"OscA1Wave": ref.wave("sine"), "LFO1Wave": 0.0, "LFO1Free": 1.0, "LFO1Rate": ref.lfo_rate(2.0),
            "YModLFO1Dest1": ref.DEST_VOLUME_A}
    out = []
    for amount in [0.5 + i * 0.05 for i in range(11)]:        # display 0 .. +10
        out.append(Program(f"vol amount {amount:.2f}", "lfo", "", ["depth_db"],
                           {**base, "LFO1Gain": 1.0, "XModLFO1Dest1Am": amount}, [Note(57, 0.25, 7.5)], 8,
                           {"amount": amount, "gain": 1.0}))
    for gain in (0.25, 0.5, 0.75):
        out.append(Program(f"vol gain {gain:.2f}", "lfo", "", ["depth_db"],
                           {**base, "LFO1Gain": gain, "XModLFO1Dest1Am": 1.0}, [Note(57, 0.25, 7.5)], 8,
                           {"amount": 1.0, "gain": gain}))
    for amount in (0.0, 0.25):                                # negative amounts
        out.append(Program(f"vol amount {amount:.2f}", "lfo", "", ["depth_db"],
                           {**base, "LFO1Gain": 1.0, "XModLFO1Dest1Am": amount}, [Note(57, 0.25, 7.5)], 8,
                           {"amount": amount, "gain": 1.0}))
    return out


def volroute_set() -> list[Program]:
    # Where does a route to Volume A act, and what is Step Velocity without the arpeggiator? A sine on
    # A1 (A2 too where noted), velocity 127 into Volume A through Misc 1 A, with the oscillator volume,
    # the route amount and Mix A swept; then Step Velocity into Mix AB with the arpeggiator off and on.
    sine = ref.wave("sine")
    velocity_route = {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 1 / 32}
    out = []
    for volume in (0.0, 0.25, 0.5, 1.0):
        for amount in (0.5, 0.625, 0.75, 1.0):
            out.append(Program(f"volA vol {volume:.2f} amt {amount:.3f}", "mod", "", ["rms_db"],
                               {"OscA1Wave": sine, "OscA1Volume": volume, **velocity_route, "XModMisc1ADest1Am": amount},
                               [Note(57, 0.25, 3.5, 127)], 4, {"case": "volA", "volume": volume, "amount": amount, "mix": 0.5}))
    for mix in (0.0, 0.25, 1.0):
        for volume in (0.0, 0.5):
            out.append(Program(f"volA vol {volume:.2f} mix {mix:.2f}", "mod", "", ["rms_db"],
                               {"OscA1Wave": sine, "OscA1Volume": volume, "MixA": mix, **velocity_route, "XModMisc1ADest1Am": 1.0},
                               [Note(57, 0.25, 3.5, 127)], 4, {"case": "volA", "volume": volume, "amount": 1.0, "mix": mix}))
    for volume in (0.0, 0.5):
        out.append(Program(f"volA two osc vol {volume:.2f}", "mod", "", ["rms_db"],
                           {"OscA1Wave": sine, "OscA1Volume": volume, "OscA2Wave": sine, "OscA2Voices": 1 / 8,
                            "OscA2Volume": volume, **velocity_route, "XModMisc1ADest1Am": 1.0},
                           [Note(57, 0.25, 3.5, 127)], 4, {"case": "volA2", "volume": volume, "amount": 1.0, "mix": 0.5}))
    for velocity in (64, 127):
        out.append(Program(f"volA vol 0 vel {velocity}", "mod", "", ["rms_db"],
                           {"OscA1Wave": sine, "OscA1Volume": 0.0, **velocity_route, "XModMisc1ADest1Am": 1.0},
                           [Note(57, 0.25, 3.5, velocity)], 4, {"case": "volAvel", "volume": 0.0, "amount": 1.0, "velocity": velocity}))
    step_route = {"XModMisc1ASource": 11 / 11, "YModMisc1ADest1": 22 / 32}
    for arp in (0.0, 1.0):
        for amount in (0.0, 0.25, 0.5, 0.75, 1.0):
            out.append(Program(f"step mixAB arp {int(arp)} amt {amount:.2f}", "mod", "", ["rms_db"],
                               {"OscA1Wave": sine, "MixA": 0.5, "XSwArpOnOff": arp, "ArpGate": 1.0, **step_route,
                                "XModMisc1ADest1Am": amount}, [Note(57, 0.25, 3.5, 127)], 4,
                               {"case": "step", "arp": arp, "amount": amount}))
    return out


def arpgate_set() -> list[Program]:
    # Arp Up at 1/8 (250 ms steps at 120 BPM, Sync on) on one held note; the sine's amp envelope is
    # fast enough that the audible length of each step is the gate.
    base = {"OscA1Wave": ref.wave("sine"), "XSwArpOnOff": 1.0, "Sync": 1.0, "ArpTime": 0.5, "ArpMode": 0.0,
            "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
    return [Program(f"arp gate {g:.2f}", "arp", "", ["on_ms"], {**base, "ArpGate": g}, [Note(57, 0.25, 7.5)], 8,
                    {"gate": g}) for g in [i / 10 for i in range(0, 11)]]


def comp_set() -> list[Program]:
    # Static curve: a steady sine at ten input levels (oscillator volume, Mix A 1.0, Main Volume 0.5) through
    # the compressor at several threshold knob positions and two ratios; compressor off as baseline.
    base = {"OscA1Wave": ref.wave("sine"), "MixA": 1.0, "MainVolume": 0.5, "CompAttack": 0.05, "CompRelease": 0.3}
    out = []
    levels = (0.03, 0.06, 0.1, 0.15, 0.2, 0.3, 0.4, 0.6, 0.8, 1.0)
    for volume in levels:
        out.append(Program(f"comp off v{volume:.2f}", "comp", "", ["rms_db"], {**base, "OscA1Volume": volume},
                           [Note(57, 0.25, 5.5)], 6, {"thr": -1.0, "ratio": 0.0, "volume": volume}))
    for ratio in (1.0,):
        for thr in (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
            for volume in levels:
                values = {**base, "OscA1Volume": volume, "XSwCompOnOff": 1.0, "CompThreshold": thr, "CompRatio": ratio}
                out.append(Program(f"comp r{ratio:.1f} t{thr:.1f} v{volume:.2f}", "comp", "", ["rms_db"], values,
                                   [Note(57, 0.25, 5.5)], 6, {"thr": thr, "ratio": ratio, "volume": volume}))
    return out


def fxnoise_set() -> list[Program]:
    # White noise (and a held sine, for modulation) through chorus and phaser at several mixes; the
    # first program has every effect off as the spectrum baseline.
    noise = {"OscA1Wave": NOISE}
    out = [Program("fx off noise", "fx", "", ["response"], noise, HELD_SHORT, 4, {"fx": "off"})]
    for mode in (0.0, 1.0):
        for mix in (0.25, 0.5, 1.0):
            out.append(Program(f"chorus mode{mode:.0f} mix{mix:.2f}", "fx", "", ["response"],
                               {**noise, "XSwChorusOnOff": 1.0, "ChorusMode": mode, "ChorusDry_Wet": mix},
                               HELD_SHORT, 4, {"fx": "chorus", "mode": mode, "mix": mix}))
    for mix in (0.25, 0.5, 1.0):
        out.append(Program(f"phaser mix{mix:.2f}", "fx", "", ["response"],
                           {**noise, "XSwPhaserOnOff": 1.0, "PhaserDry_Wet": mix, "PhaserFeedback": 0.5,
                            "PhaserCenterFreq": 0.5, "PhaserLFOGain": 0.5, "PhaserLFORate": ref.lfo_rate(2.0),
                            "PhaserSpread": 0.5, "PhaserWidth": 1.0, "PhaserLROffset": 0.5},
                           HELD_SHORT, 4, {"fx": "phaser", "mix": mix}))
    return out


def cutmod_set() -> list[Program]:
    # Noise through LP24 (resonance 8, cutoff knob 0.5) with Mod Env 1 held at full sustain on Cutoff A;
    # the amount sweeps -10..+10. The resonance peak shows how far the cutoff moved.
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.5,
            "FilterCtlCutoff": ref.NEUTRAL_FILTER_CONTROL, "ModEnv1Attack": 0.0, "ModEnv1Sustain": 1.0,
            "YModEnv1Dest1": ref.DEST_CUTOFF_A}
    out = []
    for step in range(-10, 11):
        amount = 0.5 + step / 20
        out.append(Program(f"cutoff mod {step:+d}", "mod", "", ["peak_hz"], {**base, "XModEnv1Dest1Am": amount},
                           HELD_SHORT, 4, {"amount": amount, "step": step}))
    return out


def drive_set() -> list[Program]:
    # Saw A2 through LP24 with the cutoff wide open (so the filter itself is transparent), drive swept
    # 0..1, with Warm Drive off and on, at two input levels (oscillator volume 1.0 and 0.3).
    base = {"OscA1Wave": ref.wave("saw"), "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterACutoff": 0.95}
    out = []
    for warm in (0.0, 1.0):
        for volume in (1.0, 0.3):
            for drive in [i / 10 for i in range(11)]:
                out.append(Program(f"drive {drive:.1f} v{volume:.1f} w{warm:.0f}", "filter", "", ["rms_db"],
                                   {**base, "FilterADrive": drive, "OscA1Volume": volume, "FilterCtlWarmDrive": warm},
                                   [Note(45, 0.25, 3.5)], 4, {"drive": drive, "volume": volume, "warm": warm}))
    return out


def unipan_set() -> list[Program]:
    # 8 sine voices, detune 1.0 so every voice is a separate spectral peak, at three stereo settings and
    # for 2, 4 and 8 voices: each peak's left/right balance is that voice's pan position.
    out = []
    for voices in (8, 7, 6, 5, 4, 3, 2):
        for stereo in (1.0,):
            out.append(Program(f"pan v{voices} s{stereo:.1f}", "unison", "", ["pan"],
                               {"OscA1Wave": ref.wave("sine"), "OscA1Voices": voices / 8, "OscA1Detune": 1.0, "OscA1Stereo": stereo},
                               [Note(57, 0.25, 7.5)], 8, {"voices": voices, "stereo": stereo}))
    return out


def velocity_set() -> list[Program]:
    # Velocity as a source: plain level (no modulation), Velocity -> Cutoff AB and Velocity -> Mix AB at
    # +10, for five velocities. Misc 1 A is the velocity slot.
    velocities = (20, 50, 80, 110, 127)
    sine = {"OscA1Wave": ref.wave("sine")}
    noise = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.5,
             "FilterCtlCutoff": ref.NEUTRAL_FILTER_CONTROL}
    out = []
    for kind, base, extra in (("plain", sine, {}),
                              ("cutoff", noise, {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 15 / 32, "XModMisc1ADest1Am": 1.0}),
                              ("mix", sine, {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0})):
        for velocity in velocities:
            out.append(Program(f"vel {kind} {velocity}", "mod", "", ["rms_db", "peak_hz"], {**base, **extra},
                               [Note(57, 0.25, 3.5, velocity)], 4, {"kind": kind, "velocity": velocity}))
    # The same cutoff route fed by Mod Env 1 (held at full) through the Misc slot, as a check of the slot itself.
    for amount in (0.55, 0.65, 0.75, 1.0):
        out.append(Program(f"vel envmisc {amount:.2f}", "mod", "", ["peak_hz"],
                           {**noise, "XModMisc1ASource": 6 / 11, "YModMisc1ADest1": 15 / 32, "XModMisc1ADest1Am": amount,
                            "ModEnv1Attack": 0.0, "ModEnv1Sustain": 1.0}, [Note(57, 0.25, 3.5, 100)], 4,
                           {"kind": "envmisc", "velocity": int(round((amount - 0.5) * 20))}))
    return out


def wheel_set() -> list[Program]:
    # Does the mod wheel position stored in the preset act as a source? Mod Wheel -> Cutoff AB at +10 on
    # noise through LP24 (resonance 8), with the stored ModWheel field swept.
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.5,
            "FilterCtlCutoff": ref.NEUTRAL_FILTER_CONTROL, "XModMisc1ASource": 2 / 11, "YModMisc1ADest1": 15 / 32,
            "XModMisc1ADest1Am": 1.0}
    return [Program(f"wheel {v:.2f}", "mod", "", ["peak_hz"], {**base, "ModWheel": v}, HELD_SHORT, 4, {"wheel": v})
            for v in (0.0, 0.1, 0.25, 0.5, 0.75, 1.0)]


def mixroute_set() -> list[Program]:
    # A sine through Mix A 0.5 (unity). What does a route to Mix AB do when its amount is zero, or its
    # source is zero (mod wheel at 0), compared with no route at all?
    base = {"OscA1Wave": ref.wave("sine"), "MixA": 0.5}
    cases = [("none", {}),
             ("vel amount 0", {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 0.5}),
             ("vel amount +10", {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0}),
             ("wheel0 amount +10", {"XModMisc1ASource": 2 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0}),
             ("wheel0 amount -10", {"XModMisc1ASource": 2 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 0.0}),
             ("vel amount -10", {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 0.0}),
             ("vel->MixA +10", {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 20 / 32, "XModMisc1ADest1Am": 1.0}),
             ("vel->Volume AB 0", {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 3 / 32, "XModMisc1ADest1Am": 0.5})]
    return [Program(f"mixroute {label}", "mod", "", ["rms_db"], {**base, **extra}, [Note(57, 0.25, 3.5, 100)], 4, {"case": label})
            for label, extra in cases]


def keytrack_set() -> list[Program]:
    # Noise through LP24 (resonance 8, cutoff knob 0.5) with the Filter Control keytrack swept, one note per
    # program at five pitches: the resonance peak against the note gives the keytrack law.
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.5,
            "FilterCtlCutoff": ref.NEUTRAL_FILTER_CONTROL}
    out = []
    for knob in (0.0, 0.25, 0.5, 0.75, 1.0):
        for note in (36, 48, 60, 72, 84):
            out.append(Program(f"keytrack {knob:.2f} note {note}", "filter", "", ["peak_hz"], {**base, "FilterCtlKeyTrk": knob},
                               [Note(note, 0.25, 3.5, 100)], 4, {"knob": knob, "note": note}))
    return out


def pitch_set() -> list[Program]:
    # A sine at A3 with the oscillator's Octave (-3..+3), Note (-7..+7) and Fine (-1..+1) swept; the pitch
    # of the sine is the answer. Oscillators A1 and B1 are both checked (B1 lives on the other part).
    out = []
    for osc, prefix, extra in (("A1", "OscA1", {}), ("B1", "OscB1", {"OscA1Voices": 0.0, "OscB1Voices": 1 / 8, "OscB1Wave": 0.0, "FilterBInput": 0.0, "FilterAInput": 0.5})):
        base = {f"{prefix}Wave": ref.wave("sine"), **extra}
        for octave in range(-3, 4):
            out.append(Program(f"{osc} octave {octave:+d}", "osc", "", ["pitch"], {**base, f"{prefix}Octave": (octave + 3) / 6},
                               [Note(57, 0.25, 3.5, 100)], 4, {"osc": osc, "kind": "octave", "value": octave}))
        for note in (-7, -5, -1, 1, 5, 7):
            out.append(Program(f"{osc} note {note:+d}", "osc", "", ["pitch"], {**base, f"{prefix}Note": (note + 7) / 14},
                               [Note(57, 0.25, 3.5, 100)], 4, {"osc": osc, "kind": "note", "value": note}))
        for fine in (-1.0, -0.5, 0.5, 1.0):
            out.append(Program(f"{osc} fine {fine:+.1f}", "osc", "", ["pitch"], {**base, f"{prefix}Fine": (fine + 1) / 2},
                               [Note(57, 0.25, 3.5, 100)], 4, {"osc": osc, "kind": "fine", "value": fine}))
    return out


def arptrans_set() -> list[Program]:
    # Arp in Step Seq mode on one held note (A3), every step set to the same transpose: each sounding step
    # should be A3 plus that many semitones. Sync on, 1/8 steps, sine, fast envelope.
    out = []
    for t in (-12, -3, -1, 0, 1, 2, 3, 4, 5, 7, 12, 24):
        values = {"OscA1Wave": ref.wave("sine"), "XSwArpOnOff": 1.0, "Sync": 1.0, "ArpTime": 0.5, "ArpMode": 8 / 9,
                  "ArpGate": 0.8, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
        for step in range(1, 17):
            values[f"XArpTransp{step:02d}"] = (t + 24) / 48
            values[f"XArpVelo{step:02d}"] = 100 / 127
        out.append(Program(f"arp transpose {t:+d}", "arp", "", ["pitch"], values, [Note(57, 0.25, 3.5, 100)], 4, {"value": t}))
    return out


def arpbins_set() -> list[Program]:
    # Where does the transpose value switch from one semitone to the next? Every step set to the same raw
    # value, swept finely from 0.52 to 0.60 (about +1 .. +4).
    out = []
    for k in range(0, 33):
        raw = 0.52 + k * 0.0025
        values = {"OscA1Wave": ref.wave("sine"), "XSwArpOnOff": 1.0, "Sync": 1.0, "ArpTime": 0.5, "ArpMode": 8 / 9,
                  "ArpGate": 0.8, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
        for step in range(1, 17):
            values[f"XArpTransp{step:02d}"] = raw
            values[f"XArpVelo{step:02d}"] = 100 / 127
        out.append(Program(f"arp bin {raw:.4f}", "arp", "", ["pitch"], values, [Note(57, 0.25, 2.5, 100)], 3, {"raw": raw}))
    return out


def octbins_set() -> list[Program]:
    # Fine sweep of the oscillator octave control: where does it switch octave?
    out = []
    for k in range(0, 41):
        raw = k / 40
        values = {"OscA1Wave": ref.wave("sine"), "OscA1Octave": raw, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
        out.append(Program(f"oct {raw:.3f}", "osc", "", ["pitch"], values, [Note(57, 0.25, 2.5, 100)], 3, {"raw": raw}))
    return out


def univoices_set() -> list[Program]:
    # Where does each unison voice sit? Sine oscillator so every voice is one spectral line.
    out = []
    for voices in range(2, 9):
        for detune in (0.5, 1.0):
            values = {"OscA1Wave": ref.wave("sine"), "OscA1Voices": voices / 8, "OscA1Detune": detune, "OscA1Stereo": 0.0,
                      "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
            out.append(Program(f"uni {voices}v d{detune:.2f}", "unison", "", ["voice_cents"], values,
                               [Note(57, 0.25, 7.5, 100)], 8, {"voices": voices, "detune": detune}))
    return out


def wavespec_set() -> list[Program]:
    # Every waveform, one voice, at a low and a high note: harmonic amplitudes and aliasing floor.
    out = []
    for wave in range(8):
        for note in (45, 69, 93):
            values = {"OscA1Wave": wave / 7, "OscA1Voices": 1 / 8, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
            out.append(Program(f"wave {wave} n{note}", "wavespec", "", ["harmonics"], values,
                               [Note(note, 0.25, 2.5, 100)], 3, {"wave": wave, "note": note}))
    return out


def phasecancel_set() -> list[Program]:
    # A1 and an inverted A2 with the same sine: the level left over shows the phase offset that A2's Phase
    # control (with Retrig on) really applies: |1 - e^(j phi)|.
    out = []
    for k in range(0, 21):
        raw = k / 20
        values = {"OscA1Wave": ref.wave("sine"), "OscA1Retrig": 1.0, "OscA2Wave": ref.wave("sine"), "OscA2Voices": 1 / 8,
                  "OscA2Retrig": 1.0, "OscA2Invert": 1.0, "OscA2Phase": raw, "AmpEnvAAttack": ref.envelope(2.0),
                  "AmpEnvARelease": ref.envelope(5.0)}
        out.append(Program(f"phase {raw:.2f}", "phase", "", ["level"], values, [Note(57, 0.25, 2.5, 100)], 3, {"raw": raw}))
    return out


def startphase_set() -> list[Program]:
    # Two retriggered oscillators starting together: A1 is a sine, B1 another waveform. The sum's level shows
    # how the two start phases line up (waveform start value and direction).
    out = []
    for wave in range(0, 7):
        for invert in (0.0, 1.0):
            values = {"OscA1Wave": ref.wave("sine"), "OscA1Retrig": 1.0, "OscB1Wave": wave / 7, "OscB1Voices": 1 / 8,
                      "OscB1Retrig": 1.0, "OscB1Invert": invert, "AmpEnvAAttack": ref.envelope(2.0),
                      "AmpEnvARelease": ref.envelope(5.0), "AmpEnvBAttack": ref.envelope(2.0), "AmpEnvBRelease": ref.envelope(5.0),
                      "MixA": 0.5, "MixB": 0.5}
            out.append(Program(f"start w{wave} inv{int(invert)}", "phase", "", ["level"], values, [Note(57, 0.25, 2.5, 100)], 3, {}))
    return out


def startwave_set() -> list[Program]:
    # One retriggered voice per waveform, phase 0: the fundamental's phase just after note-on.
    out = []
    for wave in range(0, 7):
        for phase in (0.0, 0.25):
            values = {"OscA1Wave": wave / 7, "OscA1Retrig": 1.0, "OscA1Phase": phase, "AmpEnvAAttack": ref.envelope(2.0),
                      "AmpEnvARelease": ref.envelope(5.0)}
            out.append(Program(f"startwave w{wave} p{phase:.2f}", "phase", "", ["level"], values, [Note(57, 0.25, 1.0, 100)], 2, {}))
    return out


def startfree_set() -> list[Program]:
    # The same pairs with Retrig off: is the start phase of a free-running voice fixed or random?
    out = []
    for wave in range(0, 7):
        for take in range(2):
            values = {"OscA1Wave": ref.wave("sine"), "OscA1Retrig": 0.0, "OscB1Wave": wave / 7, "OscB1Voices": 1 / 8,
                      "OscB1Retrig": 0.0, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0),
                      "AmpEnvBAttack": ref.envelope(2.0), "AmpEnvBRelease": ref.envelope(5.0), "MixA": 0.5, "MixB": 0.5}
            out.append(Program(f"free w{wave} t{take}", "phase", "", ["level"], values,
                               [Note(57, 0.25, 2.5, 100)], 3, {}))
    return out


def unisonphase_set() -> list[Program]:
    # Unison voices with no detune: the harmonic levels of the sum show how the voices' phases are spread.
    out = []
    for voices in (1, 2, 3, 4, 8):
        for retrig in (1.0, 0.0):
            values = {"OscA1Wave": ref.wave("saw"), "OscA1Voices": voices / 8, "OscA1Detune": 0.0, "OscA1Stereo": 0.0,
                      "OscA1Retrig": retrig, "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
            out.append(Program(f"uphase {voices}v r{int(retrig)}", "unison", "", ["harmonics"], values,
                               [Note(45, 0.25, 2.5, 100)], 3, {}))
    return out


def arpmodes_set() -> list[Program]:
    # Every arp mode on a held chord (played G3, C3, E3 in that order so As Played differs from Up), for one
    # and two octaves, 1/8 steps at 120 BPM. The note sequence is read from the sine's pitch.
    out = []
    for mode in range(10):
        for octave in (0, 1):
            values = {"OscA1Wave": ref.wave("sine"), "XSwArpOnOff": 1.0, "Sync": 1.0, "ArpTime": 0.5, "ArpGate": 0.9,
                      "ArpMode": (mode + 0.5) / 10, "ArpOctave": (octave + 0.5) / 4,
                      "AmpEnvAAttack": ref.envelope(2.0), "AmpEnvARelease": ref.envelope(5.0)}
            for step in range(1, 17):
                values[f"XArpTransp{step:02d}"] = 0.5
                values[f"XArpVelo{step:02d}"] = 100 / 127
            out.append(Program(f"arpmode {mode} oct{octave + 1}", "arp", "", ["sequence"], values,
                               [Note(55, 0.25, 8.5, 100), Note(48, 0.5, 8.25, 100), Note(52, 0.75, 8.0, 100)], 9,
                               {"mode": mode, "octaves": octave + 1}))
    return out


def lfoshape_set() -> list[Program]:
    # Each LFO waveform modulating the pitch of a sine (LFO 2 Hz, free): the pitch curve is the LFO's shape.
    out = []
    for shape in range(11):
        values = {"OscA1Wave": ref.wave("sine"), "LFO1Wave": shape / 10, "LFO1Free": 1.0, "LFO1Rate": ref.lfo_rate(2.0),
                  "LFO1Gain": 1.0, "YModLFO1Dest1": ref.DEST_PITCH_A, "XModLFO1Dest1Am": ref.amount(1.0)}
        out.append(Program(f"lfoshape {shape}", "lfo", "", ["curve"], values, [Note(57, 0.25, 7.5)], 8, {"shape": shape}))
    return out


def reverb_set() -> list[Program]:
    # A short noise burst through the reverb alone (everything else neutral): decay time against Size,
    # onset against Pre-delay, tail colour against Damp, stereo spread against Width, level against Dry/Wet.
    base = {"OscA1Wave": ref.wave("noise"), "XSwReverbOnOff": 1.0, "ReverbDry_Wet": 1.0, "ReverbSize": 0.5,
            "ReverbDamp": 0.0, "ReverbPredelay": 0.0, "ReverbWidth": 1.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.5)}
    out = []

    def add(kind, value, **changes):
        out.append(Program(f"rvb {kind} {value:.2f}", "reverb", "", ["tail"], {**base, **changes},
                           [Note(57, 0.25, 0.25, 100)], 14, {"kind": kind, "value": value}))
    for v in (0.0, 0.25, 0.5, 0.75, 1.0):
        add("size", v, ReverbSize=v)
    for v in (0.0, 0.25, 0.5, 1.0):
        add("damp", v, ReverbDamp=v)
    for v in (0.0, 0.25, 0.5, 0.75, 1.0):
        add("predelay", v, ReverbPredelay=v)
    for v in (0.0, 0.5, 1.0):
        add("width", v, ReverbWidth=v)
    for v in (0.0, 0.1, 0.25, 0.5, 0.75, 1.0):
        add("mix", v, ReverbDry_Wet=v)
    return out


def reverbfine_set() -> list[Program]:
    # Finer reverb curves: decay time against Size, tail colour against Damp, onset against Pre-delay
    # (a 15 ms noise burst so the onset is not hidden by the note).
    base = {"OscA1Wave": ref.wave("noise"), "XSwReverbOnOff": 1.0, "ReverbDry_Wet": 1.0, "ReverbSize": 0.5,
            "ReverbDamp": 0.0, "ReverbPredelay": 0.0, "ReverbWidth": 1.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.5)}
    out = []

    def add(kind, value, length, **changes):
        out.append(Program(f"rvf {kind} {value:.2f}", "reverb", "", ["tail"], {**base, **changes},
                           [Note(57, 0.25, length, 100)], 16, {"kind": kind, "value": value}))
    for v in [i / 10 for i in range(11)]:
        add("size", v, 0.25, ReverbSize=v)
    for v in (0.0, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0):
        add("damp", v, 0.25, ReverbDamp=v)
    for v in [i / 8 for i in range(9)]:
        add("predelay", v, 0.03, ReverbPredelay=v)
    return out


def reverbdamp_set() -> list[Program]:
    # Fine damp values (used to invert Synthax1's damping against the reference synth's curve).
    base = {"OscA1Wave": ref.wave("noise"), "XSwReverbOnOff": 1.0, "ReverbDry_Wet": 1.0, "ReverbSize": 0.5,
            "ReverbDamp": 0.0, "ReverbPredelay": 0.0, "ReverbWidth": 1.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.5)}
    return [Program(f"rvd damp {v:.3f}", "reverb", "", ["tail"], {**base, "ReverbDamp": v}, [Note(57, 0.25, 0.25, 100)], 16,
                    {"kind": "damp", "value": v}) for v in (0.005, 0.01, 0.015, 0.02, 0.03, 0.04, 0.05, 0.065, 0.08, 0.1, 0.13, 0.17, 0.25, 0.4, 0.6, 0.8)]


def resosweep_set() -> list[Program]:
    # Resonance 0..1 in steps of 0.1 at three cutoffs: noise through LP24 and LP12 (linear response),
    # a saw at A1 through LP24 (shows nonlinear behaviour), and self-oscillation with no input.
    out = []
    reso_steps = [i / 10 for i in range(11)]
    for src, label, slope, sl, pitch in ((ref.wave("noise"), "n", 0.0, "24", 57), (ref.wave("noise"), "n", 1.0, "12", 57),
                                         (ref.wave("saw"), "s", 0.0, "24", 45)):
        for cutoff in (0.4, 0.5, 0.6):
            for reso in reso_steps:
                values = {"OscA1Wave": src, "FilterAType": 1 / 3, "FilterADB": slope, "FilterAReso": reso,
                          "FilterADrive": 0.0, "FilterACutoff": cutoff}
                out.append(Program(f"{label}{sl} c{cutoff:.1f} r{reso:.1f}", f"{label}{sl}", "", ["spectrum"],
                                   values, [Note(pitch, 0.25, 3.0, 100)], 4, {}))
    for cutoff in (0.3, 0.5, 0.7):
        for reso in (0.9, 0.95, 1.0):
            values = {"OscA1Wave": ref.wave("noise"), "OscA1Volume": 0.0, "FilterAType": 1 / 3, "FilterADB": 0.0,
                      "FilterAReso": reso, "FilterADrive": 0.0, "FilterACutoff": cutoff}
            out.append(Program(f"self c{cutoff:.1f} r{reso:.2f}", "self", "", ["spectrum"],
                               values, [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def resomodes_set() -> list[Program]:
    # Resonance 0..1 on bandpass and highpass, 24 and 12 dB, two cutoffs (noise in; see resosweep for lowpass).
    out = []
    for mode, label in ((2 / 3, "bp"), (1.0, "hp")):
        for slope, sl in ((0.0, "24"), (1.0, "12")):
            for cutoff in (0.4, 0.5):
                for reso in [i / 10 for i in range(11)]:
                    values = {"OscA1Wave": ref.wave("noise"), "FilterAType": mode, "FilterADB": slope, "FilterAReso": reso,
                              "FilterADrive": 0.0, "FilterACutoff": cutoff}
                    out.append(Program(f"{label}{sl} c{cutoff:.1f} r{reso:.1f}", f"{label}{sl}", "", ["spectrum"],
                                       values, [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def resoctl_set() -> list[Program]:
    # How the Filter Control resonance adds to the part's, and how drive interacts with resonance (LP24, noise, cutoff 0.5).
    out = []
    base = {"OscA1Wave": ref.wave("noise"), "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterACutoff": 0.5}
    for part in (0.0, 0.3, 0.6):
        for ctl in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
            out.append(Program(f"ctl p{part:.1f} c{ctl:.1f}", "ctl", "", ["spectrum"],
                               {**base, "FilterAReso": part, "FilterCtlReso": ctl}, [Note(57, 0.25, 3.0, 100)], 4, {}))
    for volume in (1.0, 0.3):
        for reso in (0.3, 0.6, 0.9):
            for drive in (0.0, 0.25, 0.5, 0.75, 1.0):
                out.append(Program(f"drv v{volume:.1f} r{reso:.1f} d{drive:.2f}", "drv", "", ["spectrum"],
                                   {**base, "OscA1Volume": volume, "FilterAReso": reso, "FilterADrive": drive},
                                   [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def cutenv_set() -> list[Program]:
    # Cutoff swept by Mod Env 1 over time (noise, LP24, resonance 0.8): decay, attack and release knobs at two amounts.
    # The resonance peak tracked over time gives the envelope's shape; compare with cutenv_track.py-style analysis.
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.3,
            "FilterCtlCutoff": ref.NEUTRAL_FILTER_CONTROL, "YModEnv1Dest1": ref.DEST_CUTOFF_A,
            "ModEnv1Attack": 0.0, "ModEnv1Decay": 0.5, "ModEnv1Sustain": 0.0, "ModEnv1Release": 0.2,
            "AmpEnvARelease": 0.5}
    out = []
    long_note = [Note(57, 0.25, 7.0, 100)]
    for amount in (0.75, 1.0):
        for decay in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7):
            out.append(Program(f"dec {decay:.1f} a{amount:.2f}", "dec", "", ["spectrum"],
                               {**base, "ModEnv1Decay": decay, "XModEnv1Dest1Am": amount}, long_note, 8, {}))
    for attack in (0.2, 0.3, 0.4, 0.5, 0.6):
        out.append(Program(f"att {attack:.1f}", "att", "", ["spectrum"],
                           {**base, "ModEnv1Attack": attack, "ModEnv1Decay": 1.0, "ModEnv1Sustain": 1.0,
                            "XModEnv1Dest1Am": 0.8, "FilterACutoff": 0.2}, long_note, 8, {}))
    for release in (0.2, 0.3, 0.4, 0.5, 0.6):
        out.append(Program(f"rel {release:.1f}", "rel", "", ["spectrum"],
                           {**base, "ModEnv1Decay": 1.0, "ModEnv1Sustain": 1.0, "ModEnv1Release": release,
                            "XModEnv1Dest1Am": 0.8, "FilterACutoff": 0.2, "AmpEnvARelease": 0.9},
                           [Note(57, 0.25, 3.0, 100)], 8, {}))
    return out


def envshape_set() -> list[Program]:
    # Mod Env 1 on the pitch of a sine (full amount): the sine's pitch over time is the envelope's shape.
    base = {"OscA1Wave": ref.wave("sine"), "YModEnv1Dest1": ref.DEST_PITCH_A, "XModEnv1Dest1Am": 1.0,
            "ModEnv1Attack": 0.0, "ModEnv1Decay": 0.5, "ModEnv1Sustain": 0.0, "ModEnv1Release": 0.2,
            "AmpEnvARelease": 0.5}
    out = []
    long_note = [Note(57, 0.25, 15.0, 100)]
    for decay in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9):
        out.append(Program(f"dec {decay:.1f} s0.0", "dec", "", ["pitch"], {**base, "ModEnv1Decay": decay}, long_note, 16, {}))
    for decay in (0.3, 0.5, 0.7):
        out.append(Program(f"dec {decay:.1f} s0.5", "dec", "", ["pitch"],
                           {**base, "ModEnv1Decay": decay, "ModEnv1Sustain": 0.5}, long_note, 16, {}))
    for attack in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
        out.append(Program(f"att {attack:.1f}", "att", "", ["pitch"],
                           {**base, "ModEnv1Attack": attack, "ModEnv1Decay": 1.0, "ModEnv1Sustain": 1.0}, long_note, 16, {}))
    for release in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
        out.append(Program(f"rel {release:.1f}", "rel", "", ["pitch"],
                           {**base, "ModEnv1Decay": 1.0, "ModEnv1Sustain": 1.0, "ModEnv1Release": release, "AmpEnvARelease": 0.95},
                           [Note(57, 0.25, 3.0, 100)], 16, {}))
    return out


def sustain_set() -> list[Program]:
    # Amp envelope sustain knob against steady level (sine, fast decay), and the same on Mod Env 1 -> pitch.
    out = []
    sine = ref.wave("sine")
    for v in [i / 10 for i in range(11)]:
        out.append(Program(f"amp sustain {v:.1f}", "amp", "", ["rms_db"],
                           {"OscA1Wave": sine, "AmpEnvADecay": 0.3, "AmpEnvASustain": v}, [Note(57, 0.25, 7.0, 100)], 8, {}))
    for v in [i / 10 for i in range(11)]:
        out.append(Program(f"mod sustain {v:.1f}", "mod", "", ["pitch"],
                           {"OscA1Wave": sine, "YModEnv1Dest1": ref.DEST_PITCH_A, "XModEnv1Dest1Am": 0.55, "ModEnv1Attack": 0.0,
                            "ModEnv1Decay": 0.3, "ModEnv1Sustain": v}, [Note(57, 0.25, 7.0, 100)], 8, {}))
    return out


def unilevel_set() -> list[Program]:
    # Level and left/right balance of a unison saw: voices 1/2/4/8, stereo 0/0.5/1, detune 0/0.3/1.
    # Three notes per program (voice phases may be random per note), measured as the mean power.
    out = []
    notes = [Note(45, 0.25 + 3.0 * i, 2.5, 100) for i in range(3)]
    for voices in (1, 2, 4, 8):
        for stereo in (0.0, 0.5, 1.0):
            for detune in (0.0, 0.3, 1.0):
                if voices == 1 and (stereo > 0.0 or detune > 0.0):
                    continue
                out.append(Program(f"uni v{voices} s{stereo:.1f} d{detune:.1f}", "uni", "", ["rms_db"],
                                   {"OscA1Wave": ref.wave("saw"), "OscA1Voices": voices / 8, "OscA1Detune": detune,
                                    "OscA1Stereo": stereo}, notes, 9, {}))
    return out


def uniphase2_set() -> list[Program]:
    # 8 saw voices at the same pitch (detune 0 or tiny), Retrig on/off, many short notes: the distribution of the
    # summed power over notes shows how the voices' start phases are drawn when nothing detunes them apart.
    out = []
    notes = [Note(45, 0.25 + 1.5 * i, 1.0, 100) for i in range(24)]
    for retrig in (0.0, 1.0):
        for voices in (2, 8):
            for detune in (0.0, 0.05):
                out.append(Program(f"ph r{int(retrig)} v{voices} d{detune:.2f}", "ph", "", ["rms_db"],
                                   {"OscA1Wave": ref.wave("saw"), "OscA1Voices": voices / 8, "OscA1Detune": detune,
                                    "OscA1Retrig": retrig, "OscA1Octave": 0.5, "AmpEnvARelease": ref.envelope(0.0)}, notes, 37, {}))
    return out


def lfooffset_set() -> list[Program]:
    # LFO 1 Offset and Gain on the pitch of a sine (Pitch A, amount 0.75): the pitch over time shows where the
    # LFO sits (mean) and how far it swings. Slow free sine LFO.
    out = []
    base = {"OscA1Wave": ref.wave("sine"), "LFO1Wave": 0.0, "LFO1Free": 1.0, "LFO1Rate": ref.lfo_rate(0.5),
            "YModLFO1Dest1": ref.DEST_PITCH_A, "XModLFO1Dest1Am": 0.75}
    for gain in (0.0, 0.12, 0.5, 1.0):
        for offset in (0.0, 0.25, 0.5, 0.75, 1.0):
            out.append(Program(f"lfo g{gain:.2f} o{offset:.2f}", "lfo", "", ["pitch"],
                               {**base, "LFO1Gain": gain, "LFO1Offset": offset}, [Note(57, 0.25, 7.5)], 8, {}))
    return out


def arpvel_set() -> list[Program]:
    # What the Velocity source reads while the arpeggiator plays: Velocity into Mix AB at +1 on a sine, arp on (Up, gate 1),
    # the ArpVelo knob, every step velocity x, and two played velocities.
    sine = ref.wave("sine")
    route = {"XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0}
    out = []
    for note_velocity in (60, 127):
        for arp_velo in (0.0, 0.25, 0.5, 0.75, 1.0):
            for x in (0.3, 0.6, 1.0):
                values = {"OscA1Wave": sine, "MixA": 0.5, "ArpGate": 1.0, "XSwArpOnOff": 1.0, "ArpMode": 0.0, "ArpVelo": arp_velo,
                          **route, **{f"XArpVelo{i:02d}": x for i in range(1, 17)}}
                out.append(Program(f"av n{note_velocity} v{arp_velo:.2f} x{x:.1f}", "mod", "", ["rms_db"], values,
                                   [Note(57, 0.25, 3.5, note_velocity)], 4, {}))
    return out


def mixtwo_set() -> list[Program]:
    # Two Velocity slots (Misc 1 A Dest 1 and 2) into Mix AB: how amounts to one destination add, and a third into Cutoff.
    sine = ref.wave("sine")
    out = []
    for a1, a2 in ((0.5, 0.5), (1.0, 0.5), (0.75, 0.5), (0.5, 0.75), (1.0, 0.64), (0.75, 0.75), (1.0, 1.0), (0.25, 0.25), (1.0, 0.0), (0.6, 0.6)):
        values = {"OscA1Wave": sine, "MixA": 0.5, "XModMisc1ASource": 1 / 11, "YModMisc1ADest1": 22 / 32, "YModMisc1ADest2": 22 / 32,
                  "XModMisc1ADest1Am": a1, "XModMisc1ADest2Am": a2}
        out.append(Program(f"mix2 {a1:.2f} {a2:.2f}", "mod", "", ["rms_db"], values, [Note(57, 0.25, 3.5, 100)], 4, {}))
    return out


def phstatic2_set() -> list[Program]:
    # Phaser response as (phaser on) / (phaser off) of the same noise, LFO gain 0, each program twice to see whether
    # the response repeats. Centre 0.3/0.5/0.7, feedback 0/0.5/1, mix 0.5.
    base = {"OscA1Wave": ref.wave("noise"), "XSwPhaserOnOff": 1.0, "PhaserDry_Wet": 0.5, "PhaserLFOGain": 0.0,
            "PhaserSpread": 0.5, "PhaserLROffset": 0.5, "PhaserWidth": 1.0, "PhaserFeedback": 0.5, "PhaserCenterFreq": 0.5}
    notes = [Note(57, 0.25, 3.5, 100)]
    out = []
    for i in range(2):
        out.append(Program(f"ph off {i}", "phaser", "", ["curve"], {**base, "XSwPhaserOnOff": 0.0}, notes, 8, {}))
    for center in (0.3, 0.5, 0.7):
        for fb in (0.0, 0.5, 1.0):
            for rep in range(2):
                out.append(Program(f"ph c{center:.1f} f{fb:.1f} {rep}", "phaser", "", ["curve"],
                                   {**base, "PhaserCenterFreq": center, "PhaserFeedback": fb}, notes, 8, {}))
    return out


def phstatic3_set() -> list[Program]:
    # As phstatic2 (response = phaser on / off, LFO gain 0) for Spread, LR Offset, Width, Mix and the centre knob in fine steps.
    base = {"OscA1Wave": ref.wave("noise"), "XSwPhaserOnOff": 1.0, "PhaserDry_Wet": 0.5, "PhaserLFOGain": 0.0,
            "PhaserSpread": 0.5, "PhaserLROffset": 0.5, "PhaserWidth": 1.0, "PhaserFeedback": 0.0, "PhaserCenterFreq": 0.5}
    notes = [Note(57, 0.25, 3.5, 100)]
    out = [Program("ph off 0", "phaser", "", ["curve"], {**base, "XSwPhaserOnOff": 0.0}, notes, 8, {}),
           Program("ph off 1", "phaser", "", ["curve"], {**base, "XSwPhaserOnOff": 0.0}, notes, 8, {})]
    for kind, field, values in (("spread", "PhaserSpread", (0.0, 0.25, 0.75, 1.0)), ("lr", "PhaserLROffset", (0.0, 0.25, 0.75, 1.0)),
                                ("width", "PhaserWidth", (0.0, 0.5)), ("mix", "PhaserDry_Wet", (0.25, 0.75, 1.0)),
                                ("center", "PhaserCenterFreq", (0.0, 0.1, 0.2, 0.4, 0.6, 0.8, 0.9, 1.0))):
        for v in values:
            out.append(Program(f"ph {kind} {v:.2f}", "phaser", "", ["curve"], {**base, field: v}, notes, 8, {}))
    return out


def delay2_set() -> list[Program]:
    # A 20 ms noise burst through the delay alone (sync off, full wet, feedback 0.6, left and right times different):
    # echo levels per channel against Ping-Pong, Spread, Smear, High Cut, Low Cut and Width.
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 0.0, "DelayDry_Wet": 1.0,
            "DelayTimeLeft": 0.55, "DelayTimeRight": 0.65, "DelayFeedback": 0.6, "DelayPingPong": 0.0,
            "DelaySmear": 0.0, "DelaySpread": 0.5, "DelayWidth": 1.0, "DelayLowCut": 0.0, "DelayHighCut": 0.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    out = []

    def add(kind, value, **changes):
        out.append(Program(f"d2 {kind} {value:.2f}", "delay", "", ["echoes"], {**base, **changes},
                           [Note(57, 0.25, 0.04, 100)], 16, {"kind": kind, "value": value}))
    add("base", 0.0)
    for v in (0.0, 0.5, 1.0):
        add("pp-spread", v, DelayPingPong=1.0, DelaySpread=v)
    for v in (0.25, 0.5, 0.75, 1.0):
        add("smear", v, DelaySmear=v)
    for v in (0.2, 0.4, 0.6, 0.8, 1.0):
        add("highcut", v, DelayHighCut=v)
    for v in (0.2, 0.4, 0.6, 0.8, 1.0):
        add("lowcut", v, DelayLowCut=v)
    for v in (0.0, 0.5):
        add("width", v, DelayWidth=v)
    for v in (0.0, 0.5, 1.0):
        add("spread-nopp", v, DelaySpread=v)
    return out


def delayflt_set() -> list[Program]:
    # Sustained noise through the delay alone (full wet, no feedback): Low Cut and High Cut 0..1, the response of the
    # echo path is wet spectrum / the spectrum with both at 0.
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 0.0, "DelayDry_Wet": 1.0, "DelayTimeLeft": 0.5,
            "DelayTimeRight": 0.5, "DelayFeedback": 0.0, "DelayPingPong": 0.0, "DelaySmear": 0.0, "DelayWidth": 1.0,
            "DelayLowCut": 0.0, "DelayHighCut": 0.0}
    notes = [Note(57, 0.25, 3.0, 100)]
    out = [Program("dflt base 0", "delay", "", ["curve"], base, notes, 4, {}),
           Program("dflt base 1", "delay", "", ["curve"], base, notes, 4, {})]
    for kind, field in (("low", "DelayLowCut"), ("high", "DelayHighCut"), ("smear", "DelaySmear")):
        for v in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
            out.append(Program(f"dflt {kind} {v:.1f}", "delay", "", ["curve"], {**base, field: v}, notes, 4, {}))
    return out


def delaysync_set() -> list[Program]:
    # Sync on: echo time of a 20 ms noise burst against the Left and Right time knobs (both channels read at once).
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 1.0, "DelayDry_Wet": 1.0, "DelayFeedback": 0.0,
            "DelayPingPong": 0.0, "DelaySmear": 0.0, "DelayWidth": 1.0, "DelayLowCut": 0.0, "DelayHighCut": 0.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    out = []
    for i in range(0, 21):
        v = i / 20
        out.append(Program(f"dsync {v:.2f}", "delay", "", ["echoes"], {**base, "DelayTimeLeft": v, "DelayTimeRight": 1.0 - v},
                           [Note(57, 0.25, 0.04, 100)], 12, {}))
    return out


def delaycombo_set() -> list[Program]:
    # The delay settings of a real preset ("stab it") applied to a noise burst, then one setting at a time put back to its init value.
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 1.0, "DelayDry_Wet": 0.45, "DelayFeedback": 0.54,
            "DelayPingPong": 1.0, "DelaySmear": 0.257, "DelaySpread": 0.932, "DelayWidth": 1.0, "DelayLowCut": 0.0, "DelayHighCut": 0.437,
            "DelayTimeLeft": 0.787, "DelayTimeRight": 0.775,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    notes = [Note(57, 0.25, 0.04, 100)]
    out = [Program("dc full", "delay", "", ["echoes"], base, notes, 16, {})]
    for label, changes in (("nosmear", {"DelaySmear": 0.0}), ("nopp", {"DelayPingPong": 0.0}), ("nohigh", {"DelayHighCut": 0.0}),
                           ("mix1", {"DelayDry_Wet": 1.0}), ("fb0.54->0.3", {"DelayFeedback": 0.3}), ("nosync", {"Sync": 0.0, "DelayTimeLeft": 0.8, "DelayTimeRight": 0.8}),
                           ("spread0.5", {"DelaySpread": 0.5}), ("width0.5", {"DelayWidth": 0.5})):
        out.append(Program(f"dc {label}", "delay", "", ["echoes"], {**base, **changes}, notes, 16, {}))
    return out


def delaypan_set() -> list[Program]:
    # Ping-Pong with a one-sided input (oscillator panned hard left or right) and with a centred one: where the input goes.
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 0.0, "DelayDry_Wet": 1.0,
            "DelayTimeLeft": 0.55, "DelayTimeRight": 0.65, "DelayFeedback": 0.6, "DelayPingPong": 1.0,
            "DelaySmear": 0.0, "DelayWidth": 1.0, "DelayLowCut": 0.0, "DelayHighCut": 0.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    out = []
    for pan, label in ((0.0, "left"), (1.0, "right"), (0.5, "mid")):
        for spread in (0.0, 0.5, 1.0):
            out.append(Program(f"dp {label} s{spread:.1f}", "delay", "", ["echoes"], {**base, "OscA1Pan": pan, "DelaySpread": spread},
                               [Note(57, 0.25, 0.04, 100)], 12, {}))
    return out


def panlaw_set() -> list[Program]:
    # Left and right level of a sine against the oscillator Pan knob, and against the part's Mix/pan.
    out = []
    for pan in (0.0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0):
        out.append(Program(f"pan {pan:.3f}", "pan", "", ["rms_db"], {"OscA1Wave": ref.wave("sine"), "OscA1Pan": pan},
                           [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def modedrive_set() -> list[Program]:
    # Drive on each filter mode with the filter transparent (LP open, HP at its lowest cutoff, BP at mid), saw at three input levels.
    out = []
    for mode, label, cutoff, slope in ((1 / 3, "lp", 0.95, 0.0), (1.0, "hp24", 0.05, 0.0), (1.0, "hp12", 0.05, 1.0), (2 / 3, "bp", 0.5, 0.0)):
        for volume in (0.05, 0.3, 1.0):
            for drive in (0.0, 0.25, 0.5, 0.75, 1.0):
                values = {"OscA1Wave": ref.wave("saw"), "FilterAType": mode, "FilterADB": slope, "FilterACutoff": cutoff,
                          "FilterADrive": drive, "OscA1Volume": volume}
                out.append(Program(f"md {label} v{volume:.2f} d{drive:.2f}", "filter", "", ["rms_db"], values,
                                   [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def hpdrive_set() -> list[Program]:
    # Highpass drive in steps of 0.1 (saw at two levels, 24 and 12 dB, and at a cutoff well inside the band).
    out = []
    for label, slope, cutoff in (("hp24", 0.0, 0.05), ("hp12", 1.0, 0.05), ("hp24c", 0.0, 0.3)):
        for volume in (0.3, 1.0):
            for i in range(11):
                drive = i / 10
                values = {"OscA1Wave": ref.wave("saw"), "FilterAType": 1.0, "FilterADB": slope, "FilterACutoff": cutoff,
                          "FilterADrive": drive, "OscA1Volume": volume}
                out.append(Program(f"hd {label} v{volume:.1f} d{drive:.1f}", "filter", "", ["rms_db"], values,
                                   [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def wheelcut_set() -> list[Program]:
    # Mod wheel (stored at 1.0) on Cutoff AB through Misc 2 A at several amounts, with cutoffs below and at the top of the range.
    out = []
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "ModWheel": 1.0,
            "XModMisc2ASource": 2 / 11, "YModMisc2ADest1": 15 / 32}
    for part, ctl in ((0.4, 0.5), (0.5, 0.5), (0.7, 0.5), (1.0, 0.5), (0.5, 1.0), (1.0, 1.0)):
        for amount in (0.5, 0.25, 0.0, 0.75, 1.0):
            out.append(Program(f"wc p{part:.1f} c{ctl:.1f} a{amount:.2f}", "wheel", "", ["spectrum"],
                               {**base, "FilterACutoff": part, "FilterCtlCutoff": ctl, "XModMisc2ADest1Am": amount},
                               [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def dist0_set() -> list[Program]:
    # Distortion at very low amounts (0 to 0.1), types Overdrive/Fold/Clip, saw and sine at two levels, fully wet and half wet.
    out = []
    for kind in (0, 1, 2):
        for wave in ("sine", "saw"):
            for volume in (0.3, 1.0):
                for amount in (0.0, 0.02, 0.05, 0.1):
                    values = {"OscA1Wave": ref.wave(wave), "OscA1Volume": volume, "XSwDistOnOff": 1.0,
                              "DistortType": kind / 4, "DistortAmount": amount, "DistortDryWet": 1.0}
                    out.append(Program(f"d0 t{kind} {wave} v{volume:.1f} a{amount:.2f}", "dist", "", ["rms_db"], values,
                                       [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def eqsweep_set() -> list[Program]:
    # Noise through the EQ alone: response = EQ on / EQ off. Bass and Treble gains at mid frequencies, then the two frequency knobs
    # with the gain at its extreme.
    base = {"OscA1Wave": ref.wave("noise"), "XSwEQOnOff": 1.0, "EQBass": 0.5, "EQTreble": 0.5, "EQBassFreq": 0.5, "EQTrebleFreq": 0.5}
    notes = [Note(57, 0.25, 3.0, 100)]
    out = [Program("eq off 0", "eq", "", ["curve"], {**base, "XSwEQOnOff": 0.0}, notes, 4, {}),
           Program("eq off 1", "eq", "", ["curve"], {**base, "XSwEQOnOff": 0.0}, notes, 4, {})]
    for v in (0.0, 0.125, 0.25, 0.375, 0.625, 0.75, 0.875, 1.0):
        out.append(Program(f"eq bass {v:.3f}", "eq", "", ["curve"], {**base, "EQBass": v}, notes, 4, {}))
    for v in (0.0, 0.125, 0.25, 0.375, 0.625, 0.75, 0.875, 1.0):
        out.append(Program(f"eq treble {v:.3f}", "eq", "", ["curve"], {**base, "EQTreble": v}, notes, 4, {}))
    for v in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
        out.append(Program(f"eq bassfreq {v:.1f} g0", "eq", "", ["curve"], {**base, "EQBass": 0.0, "EQBassFreq": v}, notes, 4, {}))
        out.append(Program(f"eq bassfreq {v:.1f} g1", "eq", "", ["curve"], {**base, "EQBass": 1.0, "EQBassFreq": v}, notes, 4, {}))
    for v in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
        out.append(Program(f"eq treblefreq {v:.1f} g0", "eq", "", ["curve"], {**base, "EQTreble": 0.0, "EQTrebleFreq": v}, notes, 4, {}))
        out.append(Program(f"eq treblefreq {v:.1f} g1", "eq", "", ["curve"], {**base, "EQTreble": 1.0, "EQTrebleFreq": v}, notes, 4, {}))
    return out


def keysrc_set() -> list[Program]:
    # The KeyTrack modulation source (Misc 1 A, source 3) on Cutoff A at +1 and -1, noise through LP24 with resonance:
    # peak frequency against the played note.
    out = []
    base = {"OscA1Wave": NOISE, "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterAReso": 0.8, "FilterACutoff": 0.4,
            "XModMisc1ASource": 3 / 11, "YModMisc1ADest1": 13 / 32}
    for amount in (1.0, 0.0, 0.75):
        for note in (24, 36, 48, 60, 72, 84, 96):
            out.append(Program(f"ks a{amount:.2f} n{note}", "ks", "", ["spectrum"], {**base, "XModMisc1ADest1Am": amount},
                               [Note(note, 0.25, 3.0, 100)], 4, {}))
    out.append(Program("ks off n60", "ks", "", ["spectrum"], {**base, "XModMisc1ADest1Am": 0.5}, [Note(60, 0.25, 3.0, 100)], 4, {}))
    return out


def filtermodes_set() -> list[Program]:
    # Noise through every filter mode, slope, resonance and drive at two cutoffs; compared as spectra
    # (parity_compare.py groups by the second field, here the mode).
    out = []
    for mode, label in ((1 / 3, "lp"), (2 / 3, "bp"), (1.0, "hp")):
        for slope, sl in ((0.0, "24"), (1.0, "12")):
            for reso in (0.2, 0.6, 0.9):
                for drive in (0.0, 0.5):
                    for cutoff in (0.35, 0.6):
                        values = {"OscA1Wave": ref.wave("noise"), "FilterAType": mode, "FilterADB": slope, "FilterAReso": reso,
                                  "FilterADrive": drive, "FilterACutoff": cutoff}
                        out.append(Program(f"{label}{sl} r{reso:.1f} d{drive:.1f} c{cutoff:.2f}", f"{label}{sl}", "", ["spectrum"],
                                           values, [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def drive2_set() -> list[Program]:
    # Output level against input level for three sources through an open LP24 at several drives.
    out = []
    for wave in ("sine", "saw", "noise"):
        for volume in (0.1, 0.3, 1.0):
            for drive in (0.0, 0.25, 0.5, 0.75, 1.0):
                values = {"OscA1Wave": ref.wave(wave), "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterACutoff": 0.95,
                          "FilterADrive": drive, "OscA1Volume": volume}
                out.append(Program(f"drive2 {wave} v{volume:.1f} d{drive:.2f}", "filter", "", ["rms_db"], values,
                                   [Note(45, 0.25, 3.0, 100)], 4, {}))
    return out


def lfocut_set() -> list[Program]:
    # A sine LFO (2 Hz) on Filter A cutoff at several amounts and starting cutoffs: the brightness swing
    # of a noise source shows how many semitones the LFO moves the cutoff.
    out = []
    for cutoff in (0.4, 0.6):
        for amount in (1.0, 2.0, 4.0):
            values = {"OscA1Wave": ref.wave("noise"), "FilterAType": 1 / 3, "FilterADB": 0.0, "FilterACutoff": cutoff,
                      "LFO1Wave": 0.0, "LFO1Free": 1.0, "LFO1Rate": ref.lfo_rate(2.0), "LFO1Gain": 1.0,
                      "YModLFO1Dest1": ref.DEST_CUTOFF_A, "XModLFO1Dest1Am": ref.amount(amount)}
            out.append(Program(f"lfocut c{cutoff:.1f} a{amount:.0f}", "lfo", "", ["swing"], values, [Note(57, 0.25, 7.5)], 8, {}))
    return out


def hpresp_set() -> list[Program]:
    # Noise through the highpass (and lowpass for reference) at several cutoffs and both slopes, no resonance:
    # the spectrum is the frequency response.
    out = []
    for mode, label in ((1.0, "hp"), (1 / 3, "lp")):
        for slope, sl in ((0.0, "24"), (1.0, "12")):
            for cutoff in (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
                values = {"OscA1Wave": ref.wave("noise"), "FilterAType": mode, "FilterADB": slope, "FilterAReso": 0.0,
                          "FilterACutoff": cutoff}
                out.append(Program(f"{label}{sl} c{cutoff:.1f}", "filter", "", ["response"], values, [Note(57, 0.25, 5.0, 100)], 6, {}))
    return out


def dist_set() -> list[Program]:
    # Each distortion type on a sine at two levels and five amounts, fully wet: output level and harmonics.
    out = []
    for kind in range(5):
        for volume in (0.3, 1.0):
            for amount in (0.1, 0.25, 0.5, 0.75, 1.0):
                values = {"OscA1Wave": ref.wave("sine"), "OscA1Volume": volume, "XSwDistOnOff": 1.0,
                          "DistortType": kind / 4, "DistortAmount": amount, "DistortDryWet": 1.0}
                out.append(Program(f"dist t{kind} v{volume:.1f} a{amount:.2f}", "dist", "", ["harmonics"], values,
                                   [Note(57, 0.25, 3.0, 100)], 4, {}))
    return out


def delay_set() -> list[Program]:
    # A 20 ms noise burst through the delay alone, sync off, left and right equal, no ping-pong or smear:
    # echo spacing against the time knob, echo decay against Feedback, level against Dry/Wet.
    base = {"OscA1Wave": ref.wave("noise"), "XSwDelayOnOff": 1.0, "Sync": 0.0, "DelayDry_Wet": 1.0,
            "DelayTimeLeft": 0.5, "DelayTimeRight": 0.5, "DelayFeedback": 0.5, "DelayPingPong": 0.0,
            "DelaySmear": 0.0, "DelaySpread": 0.0, "DelayWidth": 1.0, "DelayLowCut": 0.0, "DelayHighCut": 0.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    out = []

    def add(kind, value, **changes):
        out.append(Program(f"dly {kind} {value:.2f}", "delay", "", ["echoes"], {**base, **changes},
                           [Note(57, 0.25, 0.04, 100)], 16, {"kind": kind, "value": value}))
    for v in (0.0, 0.2, 0.4, 0.6, 0.8, 0.9, 1.0):
        add("feedback", v, DelayFeedback=v)
    for v in (0.0, 0.025, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
        add("time", v, DelayTimeLeft=v, DelayTimeRight=v, DelayFeedback=0.5)
    for v in (0.0, 0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9, 1.0):
        add("mix", v, DelayDry_Wet=v, DelayFeedback=0.0)
    return out


def fbtail_set() -> list[Program]:
    # A 40 ms noise burst through the phaser, then the chorus, alone and fully wet: how long the feedback rings.
    common = {"OscA1Wave": ref.wave("noise"), "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.0)}
    out = []
    for v in (0.0, 0.25, 0.5, 0.75, 0.9, 1.0):
        out.append(Program(f"phf fb {v:.2f}", "phaser", "", ["tail"], {**common, "XSwPhaserOnOff": 1.0, "PhaserDry_Wet": 1.0,
                   "PhaserFeedback": v}, [Note(57, 0.25, 0.04, 100)], 12, {"kind": "phaser", "value": v}))
    for v in (0.0, 0.25, 0.5, 0.75, 0.9, 1.0):
        out.append(Program(f"chf fb {v:.2f}", "chorus", "", ["tail"], {**common, "XSwChorusOnOff": 1.0, "ChorusDry_Wet": 1.0,
                   "ChorusFeedback": v}, [Note(57, 0.25, 0.04, 100)], 12, {"kind": "chorus", "value": v}))
    return out


def phstatic_set() -> list[Program]:
    # Sustained noise through a motionless phaser (LFO gain 0): notch positions against Center, Feedback and Spread.
    base = {"OscA1Wave": ref.wave("noise"), "XSwPhaserOnOff": 1.0, "PhaserDry_Wet": 0.5, "PhaserLFOGain": 0.0,
            "PhaserSpread": 0.5, "PhaserLROffset": 0.5, "PhaserWidth": 1.0, "PhaserFeedback": 0.5, "PhaserCenterFreq": 0.5}
    out = []

    def add(kind, value, **changes):
        out.append(Program(f"phs {kind} {value:.2f} {len(out)}", "phaser", "", ["curve"], {**base, **changes},
                           [Note(57, 0.25, 3.5, 100)], 8, {"kind": kind, "value": value}))
    for center in (0.3, 0.5, 0.7):
        for v in (0.0, 0.25, 0.5, 0.75, 1.0):
            add("fb", v, PhaserCenterFreq=center, PhaserFeedback=v)
    for v in (0.0, 0.5, 1.0):
        add("spread", v, PhaserSpread=v)
    return out


def compdyn_set() -> list[Program]:
    # Compressor on signals with a different crest factor than a sine (noise, saw): output level against the
    # same settings in the other synth shows whether the detector follows the peak or the RMS.
    out = []
    for wave in ("noise", "saw"):
        for ratio in (0.5, 1.0):
            for thr in (0.2, 0.5, 0.8):
                for volume in (0.2, 0.6):
                    values = {"OscA1Wave": ref.wave(wave), "OscA1Volume": volume, "MixA": 1.0, "MainVolume": 0.5,
                              "CompAttack": 0.05, "CompRelease": 0.3, "XSwCompOnOff": 1.0, "CompThreshold": thr, "CompRatio": ratio}
                    out.append(Program(f"cmd {wave} r{ratio:.1f} t{thr:.1f} v{volume:.1f}", "comp", "", ["rms_db"], values,
                                       [Note(57, 0.25, 3.5)], 6, {}))
    return out


def compmakeup_set() -> list[Program]:
    # A quiet sine (well below every threshold) through the compressor: the output change against compressor
    # off is the makeup gain, swept over Ratio and Threshold.
    base = {"OscA1Wave": ref.wave("sine"), "OscA1Volume": 0.04, "MixA": 1.0, "MainVolume": 0.5, "CompAttack": 0.05, "CompRelease": 0.3}
    out = [Program("cmu off", "comp", "", ["rms_db"], base, [Note(57, 0.25, 2.0)], 5, {})]
    for ratio in (0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
        for thr in (0.1, 0.3, 0.5, 0.7, 0.9, 1.0):
            out.append(Program(f"cmu r{ratio:.1f} t{thr:.1f}", "comp", "", ["rms_db"],
                               {**base, "XSwCompOnOff": 1.0, "CompThreshold": thr, "CompRatio": ratio}, [Note(57, 0.25, 2.0)], 5, {}))
    return out


def crush_set() -> list[Program]:
    # Bitcrush: a sine at eight levels through amounts 0.3 ... 1.0 (the amplitude below which the output goes silent
    # shows the quantiser step); Decimate: a sine through amounts 0 ... 1 (the hold rate shows in the aliases).
    out = []
    for amount in (0.3, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
        for volume in (0.02, 0.04, 0.08, 0.15, 0.3, 0.5, 0.75, 1.0):
            values = {"OscA1Wave": ref.wave("sine"), "OscA1Volume": volume, "XSwDistOnOff": 1.0, "DistortType": 1.0,
                      "DistortAmount": amount, "DistortDryWet": 1.0}
            out.append(Program(f"crush a{amount:.2f} v{volume:.2f}", "crush", "", ["harmonics"], values, [Note(57, 0.25, 1.5, 100)], 4, {}))
    for i in range(0, 21):
        values = {"OscA1Wave": ref.wave("sine"), "OscA1Volume": 0.3, "XSwDistOnOff": 1.0, "DistortType": 0.75,
                  "DistortAmount": i / 20, "DistortDryWet": 1.0}
        out.append(Program(f"deci a{i / 20:.2f}", "crush", "", ["harmonics"], values, [Note(57, 0.25, 1.5, 100)], 4, {}))
    return out


def stepvel_set() -> list[Program]:
    # What Step Velocity reads with the arpeggiator off. Step Velocity into Mix AB at +1 on a sine: first with
    # the arpeggiator on and every step velocity x (the level then maps x to a mix gain), then with it off and
    # different step velocities and note velocities; the second group is read against the first.
    sine = ref.wave("sine")
    step_route = {"XModMisc1ASource": 11 / 11, "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0}
    base = {"OscA1Wave": sine, "MixA": 0.5, "ArpGate": 1.0, **step_route}
    velos = lambda first, rest: {f"XArpVelo{i:02d}": (first if i == 1 else rest) for i in range(1, 17)}
    out = []
    for x in (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0):
        out.append(Program(f"svon x {x:.2f}", "mod", "", ["rms_db"], {**base, "XSwArpOnOff": 1.0, **velos(x, x)},
                           [Note(57, 0.25, 3.5, 127)], 4, {"case": "on", "x": x}))
    for first in (0.2, 0.5, 1.0):
        out.append(Program(f"svoff first {first:.2f} rest 0.90", "mod", "", ["rms_db"], {**base, "XSwArpOnOff": 0.0, **velos(first, 0.9)},
                           [Note(57, 0.25, 3.5, 127)], 4, {"case": "off", "first": first}))
    for rest in (0.2, 1.0):
        out.append(Program(f"svoff first 0.60 rest {rest:.2f}", "mod", "", ["rms_db"], {**base, "XSwArpOnOff": 0.0, **velos(0.6, rest)},
                           [Note(57, 0.25, 3.5, 127)], 4, {"case": "off", "first": 0.6}))
    for velocity in (32, 64, 127):
        out.append(Program(f"svoff note vel {velocity}", "mod", "", ["rms_db"], {**base, "XSwArpOnOff": 0.0, **velos(0.6, 0.6)},
                           [Note(57, 0.25, 3.5, velocity)], 4, {"case": "off", "velocity": velocity}))
    return out


def mixneg_set() -> list[Program]:
    # Step Velocity (arpeggiator off, last step velocity 1.0) into Mix AB across the whole amount range, at three
    # Mix A knob positions: what a negative amount does when it drives the mix below zero.
    sine = ref.wave("sine")
    out = []
    for mix in (0.1, 0.5, 0.9):
        for i in range(21):
            amount = i / 20
            values = {"OscA1Wave": sine, "MixA": mix, "XSwArpOnOff": 0.0, "XArpVelo16": 1.0, "XModMisc1ASource": 1.0,
                      "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": amount}
            out.append(Program(f"mxn mix {mix:.1f} amt {amount:.2f}", "mod", "", ["rms_db"], values, [Note(57, 0.25, 4.0, 127)], 6,
                               {"mix": mix, "amount": amount}))
    return out


def stepwho_set() -> list[Program]:
    # Which step's velocity does Step Velocity read with the arpeggiator off? Every step 0.9 except one at 0.2.
    sine = ref.wave("sine")
    base = {"OscA1Wave": sine, "MixA": 0.5, "XSwArpOnOff": 0.0, "ArpGate": 1.0, "XModMisc1ASource": 1.0,
            "YModMisc1ADest1": 22 / 32, "XModMisc1ADest1Am": 1.0}
    out = []
    for low in range(1, 17):
        values = {**base, **{f"XArpVelo{i:02d}": (0.2 if i == low else 0.9) for i in range(1, 17)}}
        out.append(Program(f"sw low step {low:02d}", "mod", "", ["rms_db"], values, [Note(57, 0.25, 4.0, 127)], 6, {"low": low}))
    return out


SETS = {"keysrc": keysrc_set, "eqsweep": eqsweep_set, "dist0": dist0_set, "wheelcut": wheelcut_set, "hpdrive": hpdrive_set, "modedrive": modedrive_set, "panlaw": panlaw_set, "delaypan": delaypan_set, "delaycombo": delaycombo_set, "delaysync": delaysync_set, "delayflt": delayflt_set, "delay2": delay2_set, "phstatic3": phstatic3_set, "phstatic2": phstatic2_set, "mixtwo": mixtwo_set, "arpvel": arpvel_set, "lfooffset": lfooffset_set, "uniphase2": uniphase2_set, "unilevel": unilevel_set, "sustain": sustain_set, "envshape": envshape_set, "cutenv": cutenv_set, "resoctl": resoctl_set, "resomodes": resomodes_set, "resosweep": resosweep_set, "stepwho": stepwho_set, "mixneg": mixneg_set, "stepvel": stepvel_set, "crush": crush_set, "compmakeup": compmakeup_set, "compdyn": compdyn_set, "phstatic": phstatic_set, "fbtail": fbtail_set, "delay": delay_set, "volroute": volroute_set, "dist": dist_set, "hpresp": hpresp_set, "lfocut": lfocut_set, "drive2": drive2_set, "filtermodes": filtermodes_set, "reverbdamp": reverbdamp_set, "reverbfine": reverbfine_set, "reverb": reverb_set, "lfoshape": lfoshape_set, "arpmodes": arpmodes_set, "unisonphase": unisonphase_set, "startfree": startfree_set, "startwave": startwave_set, "startphase": startphase_set, "phasecancel": phasecancel_set, "wavespec": wavespec_set, "univoices": univoices_set, "octbins": octbins_set, "arpbins": arpbins_set, "arptrans": arptrans_set, "pitch": pitch_set, "keytrack": keytrack_set, "mixroute": mixroute_set, "wheel": wheel_set, "velocity": velocity_set, "unipan": unipan_set, "drive": drive_set, "cutmod": cutmod_set, "fxnoise": fxnoise_set, "comp": comp_set, "arpgate": arpgate_set, "lfodepth": lfodepth_set, "filter": filter_set, "lfo": lfo_set, "env": env_set, "level": level_set, "unison": unison_set}


def main() -> None:
    if len(sys.argv) != 3 or sys.argv[2] not in SETS:
        raise SystemExit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    name = sys.argv[2]
    programs = SETS[name]()
    if len(programs) > 128:
        raise SystemExit(f"{name}: {len(programs)} programs; split the set (128 at most)")
    names = sb.field_names()
    patches = []
    for program in programs:
        patch = sb.init_patch()
        patch.name = program.name
        for field_name, value in program.values.items():
            patch.values[names.index(field_name)] = float(value)
        patches.append(patch)
    patches += [sb.init_patch() for _ in range(sb.PROGRAMS_PER_BANK - len(patches))]
    (out / f"{name}.fxb").write_bytes(sb.write_bank(patches, sb.Info()))
    midi, timeline = ref.midi_file(programs)
    for entry, program in zip(timeline, programs):
        entry["sweep"] = program.extra
    (out / f"{name}.mid").write_bytes(midi)
    total = timeline[-1]["start_s"] + timeline[-1]["length_s"] + 2.0
    (out / f"{name}.json").write_text(json.dumps({"tempo_bpm": ref.TEMPO_BPM, "total_s": total, "programs": timeline},
                                                 indent=1), encoding="utf-8")
    print(f"{name}: {len(programs)} programs, {total:.0f} s")


if __name__ == "__main__":
    main()
