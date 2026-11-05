#!/usr/bin/env python3
"""Knob sweeps for calibrating SylenthPatchMapper against Sylenth1's real output.

  sylenth_sweeps.py OUTDIR SET      writes OUTDIR/SET.fxb, SET.mid and SET.json

Each set is a bank of programs that differ in one knob only, played the same way. Render a
set with SylenthRenderHost and measure it with sylenth_measure.py. Sets:

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
import sylenth_bank as sb  # noqa: E402
import sylenth_reference_bank as ref  # noqa: E402
from sylenth_reference_bank import Note, Program  # noqa: E402

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
    # Fine damp values (used to invert Synthax1's damping against Sylenth1's curve).
    base = {"OscA1Wave": ref.wave("noise"), "XSwReverbOnOff": 1.0, "ReverbDry_Wet": 1.0, "ReverbSize": 0.5,
            "ReverbDamp": 0.0, "ReverbPredelay": 0.0, "ReverbWidth": 1.0,
            "AmpEnvAAttack": ref.envelope(0.0), "AmpEnvARelease": ref.envelope(0.5)}
    return [Program(f"rvd damp {v:.3f}", "reverb", "", ["tail"], {**base, "ReverbDamp": v}, [Note(57, 0.25, 0.25, 100)], 16,
                    {"kind": "damp", "value": v}) for v in (0.005, 0.01, 0.015, 0.02, 0.03, 0.04, 0.05, 0.065, 0.08, 0.1, 0.13, 0.17, 0.25, 0.4, 0.6, 0.8)]


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


SETS = {"dist": dist_set, "hpresp": hpresp_set, "lfocut": lfocut_set, "drive2": drive2_set, "filtermodes": filtermodes_set, "reverbdamp": reverbdamp_set, "reverbfine": reverbfine_set, "reverb": reverb_set, "lfoshape": lfoshape_set, "arpmodes": arpmodes_set, "unisonphase": unisonphase_set, "startfree": startfree_set, "startwave": startwave_set, "startphase": startphase_set, "phasecancel": phasecancel_set, "wavespec": wavespec_set, "univoices": univoices_set, "octbins": octbins_set, "arpbins": arpbins_set, "arptrans": arptrans_set, "pitch": pitch_set, "keytrack": keytrack_set, "mixroute": mixroute_set, "wheel": wheel_set, "velocity": velocity_set, "unipan": unipan_set, "drive": drive_set, "cutmod": cutmod_set, "fxnoise": fxnoise_set, "comp": comp_set, "arpgate": arpgate_set, "lfodepth": lfodepth_set, "filter": filter_set, "lfo": lfo_set, "env": env_set, "level": level_set, "unison": unison_set}


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
