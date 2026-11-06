# Sound Parity with the reference synth

Synthax1 doesn't sound like the reference synth yet. Upstream wrote the engine from the manual and screenshots and never compared it with the original (see `CLASSIC_PARITY.md`, "missing original-reference evidence"). Its only audio comparison, `compare-reference-audio.py`, compares samples one by one. That can't judge a synth whose oscillator phases and unison voices run freely.

This plan replaces "it sounds worse" with measured differences per module, fixed in the order you'd hear them and re-measured after every change.

## The loop

```
             once per reference set                     every iteration (automatic)
 ┌──────────────────────────────────────┐   ┌────────────────────────────────────────────────┐
 │ Synthax1Reference.fxb + .mid          │   │ the reference synth programs ─► .Synthax1Preset (mapper)   │
 │   └─► the reference synth in Cubase ─► WAV      │   │   └─► Synthax1Render + same MIDI ─► WAV         │
 │        (answer key, kept)            │   │ split both by the manifest ─► measure ─► report │
 └──────────────────────────────────────┘   │ change DSP ─► rebuild ─► run again             │
                                            └────────────────────────────────────────────────┘
```

The reference synth's sound doesn't change, so its render is made once. Everything on the right runs without anyone touching Cubase.

## The reference set

`scripts/legacy_reference_bank.py research/parity` writes three files:

| File | What it is |
| --- | --- |
| `Synthax1Reference.fxb` | 45 the reference synth programs, each built from the reference synth's Init and changing one module only |
| `Synthax1Reference.mid` | one MIDI track at 120 BPM: one empty bar, then a program change plus test notes for each program, in turn (210 s) |
| `Synthax1Reference.json` | when each program sounds, its notes, the values it sets, and what it measures |

All values are set in real units (ms, Hz, dB) and converted with the measured curves. Every program maps into Synthax1 without approximation; the one import note is informational.

| Group | Programs | Signal | Measures |
| --- | --- | --- | --- |
| Level | 1 | sine A4 | overall gain, tuning |
| Oscillators | 7 waveforms | one voice, filter bypassed, A2 then A6 | harmonic profile; aliasing at A6 |
| Unison | 8 voices, detune 2/5/10, plus stereo 0 | saw | detune spread (cents), beating rate, stereo width |
| Filter | LP12, LP24, BP24, HP24 at 1 kHz; LP24 resonance 5/8/10; drive 5/10; warm drive; envelope sweep | noise (responses), saw (drive) | frequency response, slope, resonance peak, saturation harmonics, cutoff tracking |
| Envelopes | attack 300/1000 ms, decay 300/1000 ms, release 1.5 s | sine | amplitude curve shape, time to 10% / 90% |
| LFO | sine, triangle, saw, pulse at 2 Hz on Volume A | sine | modulation shape, rate, depth |
| Modulation | mod env on Pitch A, +12 semitones | sine | pitch curve |
| Effects | distortion ×5 types, phaser, chorus, EQ bass/treble, reverb, ping-pong delay, compressor | saw, plucks or noise | harmonics, notch movement, width, response, decay time, echo times, gain reduction |

The set can grow, up to 128 programs (MIDI program change reaches only those). A new program is one `add(Program(...))` line. Regenerate the files and re-render once.

## Rendering the reference synth without a DAW (the normal way)

`LegacyRenderHost` (`tools/legacy_host/`, built with the project) loads the installed the reference synth VST3 and renders the whole reference set offline:

```bat
build-win\LegacyRenderHost_artefacts\Release\LegacyRenderHost.exe --bank research\parity\Synthax1Reference.fxb ^
    --manifest research\parity\Synthax1Reference.json --out research\parity\renders\legacy
```

For each program the host:

1. Writes a bank with that program in slot 1, selected.
2. Hands the bank to the reference synth as VST3 component state, exactly as Cubase stores it (`VstW` header + `.fxb`).
3. Checks that every parameter it can match to a the reference synth field (198 of the 242) reports the program's value.
4. Renders the program's notes at 120 BPM.

It writes one WAV per program plus `LegacyReference.wav` on the manifest's timeline. With no DAW in the loop, the whole cycle can be re-run automatically, and so can calibration: sweep a knob over a set of programs and measure the audio.

### First measurements (2026-11-05, the reference synth 3.0.7.3)

| Check | Expected | The reference synth | |
| --- | --- | --- | --- |
| Tuning, A4 | 440 Hz | 439.88 Hz | ok |
| Attack 300 / 1000 ms, to 90% | 270 / 900 ms | 260 / 810 ms | measured curve holds |
| Delay 1/4 at 120 BPM | 500 ms | 502 ms | ok |
| LFO free rate | 2.0 Hz | 1.90 Hz | curve about 5% off |
| Filter LP12 / LP24, set for 1 kHz | 1000 Hz | 322 / 492 Hz | **filter mapping wrong**: cutoffs come out 1 to 1.6 octaves too high in Synthax1. First calibration target. |

## Rendering the reference synth in Cubase (fallback, about 5 minutes)

Only needed if the host can't load the reference synth, for example after an update.

1. **New empty project.** In **Project → Project Setup**, set the sample rate to **48 kHz** and the tempo to **120**, and turn the tempo track off.
2. **Add an instrument track with the reference synth.** In the reference synth's **MENU → Load Bank**, choose `research/parity/Synthax1Reference.fxb`. The display should show "REF Sine level".
3. **Import the MIDI:** **File → Import → MIDI File** → `research/parity/Synthax1Reference.mid`. Move the part onto the the reference synth track, or route the new MIDI track's output to the reference synth, so that it starts exactly at **bar 1**.
4. **Check program changes.** Play from bar 1 and watch the reference synth's display: it should step to "OSC saw", "OSC pulse" and so on, about every 4 seconds. If it stays on one program, enable program change reception in the reference synth's options and try again.
5. **Keep the signal path clean.** No inserts on the the reference synth channel or the stereo out, both faders at 0 dB, pan centred.
6. **Export:** **File → Export → Audio Mixdown**.
   - Range: bar 1 to the end of the part (about 3:32).
   - Format: WAV, 48 kHz, **32-bit float**, stereo.
   - No normalising, no dither, real-time export off.
7. **Save it as** `research/parity/LegacyReference.wav`. It's about 80 MB and git-ignored.

Before exporting, listen to the first minute: you should hear a sine, then waveforms low and high, unison, then noise through filters. Anything else means a program change or the bank didn't take.

## Automation still to build (by Claude, once the WAV exists)

1. **`LegacyConvert` (C++, reuses the mapper and registry).** Turns `.fxb` programs into `.Synthax1Preset` files: choices as labels, switches as booleans, using the same path as `LegacyImport`.
2. **`scripts/parity_render.py`.**
   - Writes one MIDI fixture per program from the manifest (notes shifted to time 0).
   - Runs `Synthax1Render --preset P --fixture F --wet --output W` for each program.
   - Assembles a Synthax1 WAV on the same timeline as the reference.
3. **`scripts/parity_analyze.py`** (NumPy/SciPy in an isolated venv).
   - Aligns the two WAVs: the first note's onset, then cross-correlation of the first second.
   - Cuts every program out of both.
   - Computes the metrics listed under the program's `measure` in the manifest.
4. **Report.** A table per module: the reference synth value, Synthax1 value, difference, and a pass or fail against the targets below, ordered by size of difference. It also includes spectrum and response plots for the worst entries, published as an artifact page.

### Metrics

| Metric | How |
| --- | --- |
| `harmonic_profile` | levels of the first 32 harmonics relative to the fundamental, from a 1 s Hann-windowed FFT of the steady part |
| `alias_energy_db` | energy away from harmonic bins at A6, relative to the total |
| `response_curve` | (output spectrum of noise through the module) ÷ (Init noise spectrum), averaged over 2 s, 1/12-octave smoothing |
| `cutoff_hz` / `slope_db_per_octave` / `resonance_peak_db` | read from the response curve |
| `amplitude_curve` / `time_to_*` | RMS envelope in 5 ms windows, normalised to the sustain level |
| `modulation_shape` / `rate` / `depth` | envelope of the LFO-modulated sine: period, depth in dB, shape compared after time-normalising one cycle |
| `pitch_track_semitones` | fundamental per 10 ms frame, from autocorrelation |
| `stereo_width` | side/mid energy ratio |
| `spectral_spread_cents` | width of the fundamental's peak cluster in an 8 s FFT |
| `decay_time_ms`, `echo_times_ms` | envelope fit of the tail; onsets of the echoes |

Free-running phase makes sample-by-sample comparison meaningless, so every metric is spectral, envelope-based or statistical.

### Targets (first pass)

| Module | Target |
| --- | --- |
| Level | within 0.5 dB |
| Harmonics | each of the first 16 within 1.5 dB |
| Aliasing | no worse than the reference synth's by more than 3 dB |
| Filter responses | within 1.5 dB over 50 Hz – 10 kHz |
| Resonance | peak within 2 dB and 3% in frequency |
| Envelopes | `time_to_*` within 10%; curve RMS error under 3% of full scale |
| Unison | spread within 10%; width within 0.05 |
| Effects | decay and echo times within 10%; responses within 2 dB |

Tighten these after the first report shows what's realistic.

## Order of work

Fix the modules in the order you'd hear them, re-running the full report after each change so a fix in one place can't silently break another:

1. **Level and tuning.** Everything else is measured relative to these.
2. **Oscillators.** Waveform shapes, band-limiting and aliasing (the reference synth is notably clean at high notes), and phase at retrigger.
3. **Unison.** Detune law (how voices spread with the knob), stereo distribution and level compensation. This is a big part of the "The reference synth supersaw" sound.
4. **Filter.** Response shapes and slope, the resonance curve and self-oscillation, drive and Warm Drive saturation, and keytracking. This is probably the largest single source of character.
5. **Envelopes.** Curve shapes (the reference synth's measured times run up to 30 s; see the engine gaps in `LEGACY_IMPORT.md`).
6. **LFO and modulation scaling.**
7. **Effects,** one at a time.

Engine range gaps found by the import work (envelopes over 10 s, LFO divisions longer than one bar, EQ above +12 dB, pitch modulation beyond ±48 semitones) are fixed alongside the module they belong to.

## Risks

- **Program changes.** If the reference synth ignores MIDI program changes in Cubase, the fallback is to split the reference set into banks played one program at a time, or to automate the program parameter. Step 4 above checks this before the export.
- **Alignment.** Cubase may start the mixdown a few samples late, or compensate plugin latency. The analyzer aligns using the first onset and cross-correlation, never by trusting time zero.
- **Randomness.** Some the reference synth behaviour may be random per note (unison phases, noise). Metrics are averaged over each note's steady part, and the noise-based tests compare averaged spectra, never waveforms.
- **The ear decides last.** Matching numbers make sure nothing is wildly off. Once a module passes, A/B it by ear on real presets from your banks.

## Results so far (2026-11-05, the reference synth 3.0.7.3)

Rendered by `LegacyRenderHost` (the reference synth) and `Synthax1Render --legacy-bank` (Synthax1, same bank, same MIDI), then measured with `scripts/legacy_measure.py` (sweeps) and `scripts/parity_compare.py` (whole reference set). Sweep sets are generated by `scripts/legacy_sweeps.py`, rendered the same way, and kept in `research/parity/`.

| Module | Before | Now |
| --- | --- | --- |
| Level (osc, Mix, Main volume) | Synthax1 11.25 dB too loud, saturating the output | within 0.1 dB over 63 points |
| Envelope attack / decay / release | linear ramps; times read from a noisy measurement | within 1–2% over the whole range; the reference synth's S-shaped attack and its decay/release curve are in `Envelope.cpp` |
| Filter slope switch | **inverted** (the template comment is misleading: 0 = 24 dB, 1 = 12 dB) | corrected |
| Filter response | one-pole cascade, cutoff 1–1.6 octaves too high | zero-delay-feedback ladder; lowpass responses within 1–2 dB down to −50 dB, cutoff within ~0.7 semitone, resonance peak within ~1.7 dB |
| LFO rate | capped at 40 Hz | exact up to 192 Hz |
| Unison level | `1/sqrt(n)` | `(0.8 + 0.2 n)/n` per voice: within 0.1 dB for 1–8 voices |
| Unison detune width | outer voice at 45·d² cents (45 at most) | 369·d^2.85 cents: within 2% over the range |
| Noise oscillator level | 10.2 dB too quiet | matched |
| Saw, pulse, half/quarter pulse | — | already within 1 dB |
| LFO depth on part volume | dB-based, 34 dB swing at full amount | linear gain (1 ± 0.5·amount·gain), within 0.02 dB at every setting |
| EQ shelves | flat -10.8 dB loss, narrow bass shelf | The reference synth's corners (1.7x bass freq, 0.57x treble freq) and 31% normalisation; within 1-2 dB |
| Output stage | hard clip at 0 dBFS inside the FX chain | soft limiter after the master gain (transparent below 0.9, never above 1.0) |
| Arpeggiator gate | — | note lengths within 2 ms over the whole range; step timing identical |
| Compressor | no makeup gain, threshold the wrong way round, ratio capped at 8:1 | The reference synth's threshold law (T = -5 - 28 k dB), makeup gain (0 to 11.2 dB) and 100:1 ratio; static curve within 0.14 dB mean over 110 points |
| Chorus, phaser | linear dry/wet blend (level fell 2-5 dB), phaser offset not centred | equal-power blend, dual mode level, mono-correlated phaser: levels within 1-2 dB |
| Filter cutoff modulation | 7.2 semitones per display unit | 12.2 (one octave per unit); within 1 semitone up to +6 |
| Filter drive | gain `1 + 9d`, +17 dB at full drive | measured gain table and a closing limiter; within 0.4 dB at low level, ~2 dB high at the maximum |

Mean spectral distance over the reference set (`parity_compare.py`; 0 = same spectrum): filter 20.6 → 6.2 dB, oscillator 19.0 → 2.5 dB (the "11.8" it shows later comes from one pure-sine artefact), envelope 4.5 → 2.7 dB.

Everything above ships through the mapper and the engine: all tests pass except `Synthax1NativeProcessorTest`, which already crashed on upstream's code before any of this work.

## Real presets with ablation

Module tests pass while real presets still sound different, because presets combine everything. `scripts/parity_presets.py` takes real presets from a bank and renders each at six cumulative levels of simplification: **full**, then **no effects**, **no arpeggiator**, **no modulation**, **no filters**, and **one voice per oscillator** (the bare oscillators). The point where Synthax1 converges on the reference synth shows which module is responsible.

```
python scripts/parity_presets.py research/parity/presets <bank.fxb> --count 20
LegacyRenderHost --bank research/parity/presets/presets.fxb --manifest research/parity/presets/presets.json --out research/parity/renders/presets-legacy
Synthax1Render --legacy-bank research/parity/presets/presets.fxb --manifest research/parity/presets/presets.json --output-dir research/parity/renders/presets-synthax1
python scripts/parity_compare.py research/parity/renders/presets-legacy research/parity/renders/presets-synthax1 research/parity/presets/presets.json
```

20 presets from FactoryBank1, mean spectral distance in dB (lower is better), at three points in this work:

| Variant | First run | After mono + velocity fixes | After keytrack + additive Mix |
| --- | --- | --- | --- |
| full | 25.6 | 24.8 | **15.7** |
| no effects | 26.8 | 25.7 | 18.9 |
| no arpeggiator | 29.8 | 26.4 | 19.5 |
| no modulation | 18.0 | 17.4 | **10.2** |
| no filters | 12.7 | 9.4 | 9.4 |
| bare oscillators | 10.1 | 7.3 | 7.3 |

Reading it: the biggest remaining step is still the **arpeggiator and effects** (about 5-9 dB on top of the filter and modulation steps), then the filters (about 1 dB) and the bare oscillators' free-running phase (7 dB, partly the metric). The corrections that moved this table, each found by chasing one badly wrong preset:

- Mono Legato forces mono mode regardless of the polyphony knob.
- Note velocity does not change the level in the reference synth (Synthax1 scaled it).
- The engine's hidden depth limits cut cutoff modulation to 72 and pitch to 48 semitones (now 122 and 120).
- **Filter keytrack**: its slope matches, but the reference note is MIDI 9.4, not 60, so presets with keytrack on were up to 50 semitones too dark.
- **Mix modulation is additive** to the Mix knob (a "gate" preset has Mix at 0 and Step Velocity supplies the whole level); Synthax1 multiplied, so those presets were silent.
- The stored mod wheel position acts as a live source when a preset loads.

First finding from it: Mono Legato is a mode button that makes the reference synth monophonic whatever its polyphony knob says; the mapper only did that for polyphony 1.

## Large corpus (2026-11-06)

Two sets built from 234 real presets (the six factory banks plus 45 third-party banks), rendered by both synths and kept in `research/parity/corpus` (git-ignored, like the banks themselves):

```
python scripts/parity_presets.py research/parity/corpus FactoryBank1.fxb@24 ... other.fxb@2 --variants full,nofx --name corpus
python scripts/parity_presets.py research/parity/corpus FactoryBank1.fxb@10 ... other.fxb@2 --variants full,nofx --fx-each --name corpus-fx
LegacyRenderHost --plugin <the reference synth.vst3> --bank research/parity/corpus/corpus.fxb --manifest research/parity/corpus/corpus.json --out research/parity/renders/corpus-legacy
Synthax1Render --legacy-bank research/parity/corpus/corpus.fxb --manifest research/parity/corpus/corpus.json --output-dir research/parity/renders/corpus-synthax1
python scripts/parity_compare.py research/parity/renders/corpus-legacy research/parity/renders/corpus-synthax1 research/parity/corpus/corpus.json
python scripts/parity_effects.py research/parity/renders/corpus-fx-legacy research/parity/renders/corpus-fx-synthax1 research/parity/corpus/corpus-fx.json --worst 3
```

`corpus` (468 programs, 31 minutes) is the reference point for engine changes that should not change the sound. With it, control-rate modulation every 4 samples left the mean distance unchanged (full 8.4, nofx 9.1 dB). `corpus-fx` (106 presets, 508 programs) adds, per preset, one variant per effect it uses with only that effect on; `parity_effects.py` compares what each effect changes against the same preset dry, so it measures the effects without the dry sound's own differences.

**Metric fix.** `parity_compare.py` picked the loudest note separately in each render, so when two notes were about equally loud it compared different notes. It now measures the note that is loudest in the reference synth's render in both. Earlier figures in this document were inflated by this: the reference set's oscillator module drops from 10.3 to 1.1 dB, and the 20-preset ablation from full 13.2 / bare oscillators 9.6 to 10.6 / 2.7 dB.

**Level outliers.** 29 of the 234 presets differ in level by more than 10 dB; two are silent in Synthax1:

- *BS THAT Psy Bass* (FactoryBank5 #290): oscillators A1, A2 and B1 sit at Volume 0 and a velocity route to Volume A and Volume B brings them in. Synthax1 mapped Volume A/B to the part level, after the oscillators, so nothing sounded. **Fixed (`sweeps/volroute`, 36 programs):** the route adds linear gain to each oscillator's volume knob; the preset went from 133 dB to 3.8 dB of spectral distance. The depth is set to 0.5 per unit, which matches the LFO sweep (`lfodepth`) exactly, but a velocity source measures twice that in the reference synth (`volroute` is 6 dB low for velocity at +10). Why a unipolar source gets twice the bipolar LFO's depth is not explained; test it with an envelope and a macro source before choosing a per-source factor.
- *SFX - METADIGI BELLZ* (FactoryBank3 #445): Step Velocity to Mix AB at -0.82. Synthax1's step velocity is 1 when no arp step plays, which drives the mix to zero; the reference synth still sounds. `volroute` measured Step Velocity into Mix AB with the arpeggiator on and off: the reference synth gives the same levels both ways (-28.1, -21.6, -18.0, -15.4 dB at amounts 0.25 to 1.0), Synthax1 matches with the arpeggiator on and is off by up to 3 dB with it off. Not fixed yet; the preset itself has the arpeggiator on, so its silence has another cause (single-field reset shows the Misc 1 B Step Velocity route to Mix AB).

**Effects** (means over the presets that use each effect; level and tail as reference synth / Synthax1):

| Effect | Presets | Response difference | Level change | Tail 1 s after release |
| --- | --- | --- | --- | --- |
| Distortion | 38 | 6.0 dB | +0.3 / -1.0 dB | |
| Compressor | 48 | 5.4 dB | -0.4 / +1.8 dB | |
| Delay | 66 | 4.9 dB | +0.7 / +0.3 dB | -15.5 / -7.4 dB |
| Phaser | 16 | 4.5 dB | -1.0 / -0.4 dB | -71 / -93 dB |
| Chorus | 19 | 3.8 dB | -1.0 / +0.4 dB | -42 / -58 dB |
| Reverb | 50 | 3.0 dB | -1.3 / -1.5 dB | -20.3 / -19.5 dB |
| EQ | 59 | 2.9 dB | +1.0 / +2.0 dB | |

Next targets from it: the delay's echoes last about 8 dB longer than the reference synth's (feedback or its filtering); the phaser and chorus tails die much sooner (the reference synth's feedback rings on); the compressor adds about 2 dB more makeup; and Decimate and Bitcrush are still unmeasured.

## Cross-check against the manual

The reference manual has no numbers, but it states behaviour. Checked against the measurements:

| Manual says | Status |
| --- | --- |
| Mono Legato forces mono mode and discards the polyphony setting | confirmed; mapper fixed |
| Compressor threshold: 0 dB compresses nothing, -30 dB compresses everything louder | matches the measured threshold law |
| Gate = fraction of the step | measured (within 2 ms) |
| Main Volume comes after the master effects; Mix A/B set the part volumes | consistent with the measurements |
| EQ normalises the volume when boosting | measured (31% of the boost) |
| Phase works only when Retrigger is on | **not yet tested** |
| Normal portamento slides only when another key is held; Slide always | **not yet tested** (and the glide-time curve is unmeasured) |
| Step velocity "-" skips the note | **not yet tested** |
| LFO gain 0 turns the phaser LFO off; Free LFO neither retriggers nor syncs | partly covered |
| Delay Dry/Wet at 10 mutes the original | **not yet tested** |

The manual cannot supply the numbers; those have to be measured.

## Remaining gaps from the module tests, largest first

1. **Detuned unison saws** (spectral distance 15-21 dB): the sine tests match, saws do not; probably voice phases and the saw's band-limiting.
2. **Filter modes:** bandpass is 3-4 dB too strong at its centre, highpass leaks more bass, resonance runs about 1.7 dB hot, and the drive limiter is ~2 dB high at the maximum.
3. **Trisaw** waveform (-2.9 dB) and the pitch-modulation envelope shape ("MOD pitch env").
4. **Phaser and compressor dynamics:** attack/release times and the phaser LFO law are unmeasured (the static curves match).
5. **Other modulation depths** (pitch, pan, resonance, LFO gain destinations): only part volume and filter cutoff are calibrated.
6. **Reverb, delay, distortion types** have not been measured in detail; **Sync-off** delay and arp times and **portamento** likewise.

## Status

- Done: reference set, offline the reference synth host, Synthax1 renderer for the same set, sweep and comparison tools, and the corrections in the table above.
- Next: items 1–6 above, one module at a time, re-running `parity_compare.py` after each.

## Selector decode (measured)

Fine sweeps of oscillator octave and arpeggiator step transpose (`octbins`, `arpbins` in `scripts/legacy_sweeps.py`) show the reference synth switches value at multiples of 1/count: the selector index is `floor(raw * count)`, clamped to `count - 1`, not `round(raw * (count - 1))`. Values stored by the editor sit on the grid in the middle of a bin or on its lower edge, so the two rules agree for them; presets with off-grid values (for example octave 0.418) played an octave off. `selectorIndex` now uses the floor rule for every selector; octave and arp transpose are verified against the reference synth, the other selectors follow from the same rule and have not each been swept. ARP Acid Burn PROTO: no-modulation distance 25.0 to 6.3 dB, bare-oscillator distance 11.2 to 3.2 dB; 20-preset ablation full 15.7 to 15.6, nomod 10.2 to 9.8.

## Unison detune per voice count (measured)

`univoices` (sine oscillator, 2 to 8 voices, detune 0.5 and 1.0, spectral peaks in cents) shows the outer voice's offset at full detune depends on the voice count: about 100, 148, 195, 240, 284, 327 and 370 cents for 2 to 8 voices, falling as detune^2.9; the voices between are spread roughly evenly. Synthax1 used the 8-voice value for every count (a 2-voice patch at full detune was 369 cents wide instead of 100). Fixed in `OscillatorStack::detuneOffsetCents`; positions now match within a few cents. Effect on the 20-preset ablation is mixed (full 15.6 to 15.9, no-effects 18.3 to 16.6, no-arp 18.7 to 17.1, no-filter 9.4 to 12.7), so another unison difference (voice phases or levels) is still hidden behind the old error. ARP Acid Burn is unchanged (one voice).

## Waveforms, TriSaw, start phase and aliasing (measured)

- **Waveform spectra** (`wavespec`, one voice at A2, A4, A6): saw, pulse, half pulse, quarter pulse, triangle and sine harmonics already matched the reference synth to the fundamental's level.
- **TriSaw** is not a saw/triangle blend: the reference synth's has odd harmonics only, falling as 0.85/n after a stronger fundamental (a square plus a little sine). Rebuilt that way; level and harmonics match.
- **Aliasing at A6**: the reference synth's aliasing floor is about -103 dB; the two-sample polyBLEP gave -75 dB. A four-sample polyBLEP gives -92 to -98 dB (pulse waves -86 to -98). About 10 dB remains.
- **Start phase with Retrig on** (`startwave`, `startphase`): relative to the sine, the reference synth's saw starts half a cycle later, the triangle a quarter, the pulse waves inverted, TriSaw 0.09. Layered pairs now match within 0.3 dB. With Retrig off the reference synth's start phase is random per note (two renders of the same pair differ by up to 15 dB), so layered same-pitch oscillators cannot be matched there; single-run comparisons of such presets (for example ablation "one voice") are noise of that size.

## Arpeggiator modes and LFO shapes (measured)

- **Arp modes** (`arpmodes`: a held chord played G3, C3, E3 through every mode, one and two octaves, 1/8 steps): Up, Down, Up/Down, Up/Down 2, Down/Up, Down/Up 2 and Ordered produce the same note sequence in the reference synth and Synthax1. Random is random. Step plays the most recently pressed key (Synthax1 played the lowest); fixed.
- **LFO to pitch** (`lfoshape`, 2 Hz LFO at amount +1): an LFO moves pitch half as far as an envelope at the same amount (+-6 against +-12 semitones). The mapper now halves LFO pitch depth. Other destinations are not checked for this.
- **Random and Sample & Hold LFOs** draw a new value twice per cycle (on both square-wave edges). Random glides to each new value (cosine); Synthax1's Noise shape was audio-rate white noise, so the Random LFO barely moved a filter. Fixed. Lorenz (a near-constant offset of about -2.5 semitones with a small ripple) and the Half/Quarter pulse LFO shapes (25% and 12.5% duty, played as 50% square) are still approximated.
- Random LFOs are random per note in the reference synth, so presets that use them can only be matched on average.
- 20-preset ablation after these changes: full 14.9, no effects 15.0, no arp 14.4, no modulation 8.5, no filter 10.7, one voice 8.6 (full started this session at 25.6).

## Reverb (measured)

`reverb`, `reverbfine` and `reverbdamp` sweeps (a 15 to 125 ms noise burst through the reverb alone):

- **Size** sets the decay time (to -60 dB): 0.24, 0.33, 0.54, 0.85, 1.24, 1.81, 2.62, 3.97, 6.53, 12.14 and 7.10 s at 0, 0.1 ... 1.0 (it falls back at full size). Synthax1's old comb reverb had 0.2 to 7.6 s with a different shape and started 15 to 120 ms late; now the loops are short and fixed, the feedback is set per loop from the table, and onset and decay match.
- **Pre-delay** is 200 ms times the knob exactly.
- **Damp** darkens the tail: centroid 12.2 kHz at 0, 8.6, 6.1, 4.2, 3.1, 2.1 and 1.5 kHz at 0.1, 0.2, 0.35, 0.5, 0.75 and 1.0. Synthax1's loops were already dark at Damp 0 because linear-interpolated reads inside a feedback loop act as a low-pass; whole-sample reads fixed that, and the damping amount follows a fitted table.
- **Width** (stereo correlation 1, 0.6, 0) matched already.
- **Dry/Wet**: the dry signal follows a quarter sine and the wet gain is about mix^1.9 (0.0094, 0.073, 0.288, 0.61 at 0.1, 0.25, 0.5, 0.75), not a linear crossfade.
- The wet level also depends on Size in the reference synth; a measured trim makes the tail level match to within 1 dB across Size, and within about 1 dB across Damp (Synthax1's tail is a little shorter-lived at high Damp: RT 1.6 s against 1.43 to 1.8 s).
- 20-preset ablation full: 14.9 to 14.2.

## Controls

Knobs read 0 to 10 (or -10 to 10 for bipolar ones) like the reference synth, the Filter Control knobs 0 to 10 with the default at 5; oscillator octave runs -3 to 3 and semitone -7 to 7; the arp rate list runs 1/1 to 1/64 in the reference synth's order.

## Drive and LFO to cutoff (measured)

- **Drive** (`drive2`: sine, saw and noise at three levels and five drive settings, plus waveform cycles): the drive stage is not a saturator. Its output follows the input up to a knee and then falls again (a loud sine at full drive comes out with a dip at its crest), and the knee moves to lower levels as drive rises, so a hot input gets quieter with more drive. Synthax1's old tanh limiter kept rising by 10 dB or more on noise. The stage is now `x / (1 + (x/K)^4)` with K fitted per drive; sine and saw levels are within 0.5 dB at every drive, noise within about 2 dB. Filter-mode spectra at drive 0.5: LP/BP 5.9 to 2.3-2.7 dB mean distance.
- **LFO to cutoff** (`lfocut`): an LFO swings the cutoff about half as far as an envelope at the same amount (as for pitch); the mapper now scales it by 0.52 and the swing matches at three amounts and two cutoffs.
- ARP Acid Burn PROTO full preset: 10.2 to 4.9 dB; 20-preset ablation full 13.7.
- **Highpass remains wrong** at high cutoff: the reference synth's HP24 rolls off at roughly 12 dB/oct with a finite stop-band floor (about -50 dB), Synthax1's is a true 24 dB/oct (mean distance hp24 30, hp12 17 dB).

## Cutoff scale and distortion (measured)

- **Cutoff:** The reference synth's part cutoff and Filter Control cutoff add in the knob domain (Hz = 0.0647 e^(10.15 (part + control))). Synthax1's part cutoff spanned 0 to 136 semitones with the control neutral at 0.27, so knob 5 on both sounded very different (Synthax1 about 400 Hz, the reference synth 1.7 kHz). The part range is now 0 to 176 semitones (clipped at 136, about 20 kHz, in the filter) and the Filter Control spans -88 to +88 with its centre neutral; knob 5 gives the same cutoff as the reference synth's.
- **Polyphony** reads raw x 16 (Init: 0.1875 shows 3 voices), one fewer than the importer used.
- **Distortion** (`dist`: five types, a sine at two levels, five amounts, fully wet; the manual names them Overdrive, Foldback, Clip, Decimate, Bitcrush): Amount is a pre-gain of about -15 dB at 0 rising about 50 dB over the knob (so 0.1 is quiet and clean) into a ceiling near -17.5 dB. Overdrive rounds the ceiling (tanh) and ends close to a square wave at 1.0 (third harmonic -10.7 dB); Clip cuts it. Synthax1's old soft saturator already added 13 dB at 0.1 and could not reach a hard overdrive. Overdrive and Clip now match within about 1 dB in level and a few dB in harmonics. Foldback is only roughly matched; Decimate and Bitcrush are unchanged and not yet measured.

## Preset browser

A small list button inside the preset-name field on the LCD opens the browser as an overlay over the instrument (the keyboard stays visible and playable); Close or Escape dismisses it. It catalogues the factory and user preset folders plus any folders added under FOLDERS / SCAN, reading `.Synthax1Preset` files and every program of the reference synth `.fxb` / `.fxp` banks. Scans read names and metadata only and are cached in `library-index.json` (by file size and modification time) beside the user presets, so reopening is fast: 33 banks, 6,400 programs. Browse by category, bank, vendor or tag, search with plain words or `cat:`, `bank:`, `vendor:`, `tag:`, `author:`, `fav:`, `rating:`, sort by any column, rate and favourite presets and keep notes (`library-user.json`), load on select, and optionally play middle C when a preset is picked. The reference synth categories are inferred from the name code (BA, LD, PD, PL, ARP, ...).

The browser hides identical sounds by default (a hash of the patch values; the HIDE DUPLICATES switch turns it off) and groups single-program `.fxp` files under "Loose presets", with the folder they sit in as their pack. On the author's library of 33 folders of banks, 279,154 programs reduce to 76,268 distinct sounds. Pitch bend and mod wheel are parameters (`performance.pitch_bend`, `performance.mod_wheel`) so a host can automate them, and a right click on a control offers the host's own automation menu (create or show the lane).
