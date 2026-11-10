# The reference synth Interop and Preset Workflow

Synthax1 reads and writes the reference synth `.fxp` programs and `.fxb` banks, and gives a fast way to assemble new banks from existing presets.

## Why the structures line up

Synthax1's native engine already has the reference synth's shape:

- two parts, each with two oscillators of up to eight voices;
- A/B filters with input routing and a shared filter control;
- two amp envelopes, two mod envelopes and two LFOs;
- eight mod slots with two destinations each;
- a 16-step arpeggiator;
- the same seven effects;
- a 4 x 128 = 512-program bank, the same size as a the reference synth bank.



## File format

This section is established from **270 real files**: the six factory banks plus 264 third-party banks and presets on this machine. All 268 the reference synth files parse; the other two belong to a different plugin and are rejected correctly. Writing a file back reproduces it byte for byte when its body is uncompressed, and value for value when it's zlib (recompression differs in bytes).

The public 010 Editor template for the reference synth files got the field order right, but the chunk header wrong. The real header is:

```
outer container, big-endian
  'CcnK' byteSize magic version 'syl1' fxVersion count
     FPCh  name[28] chunkSize chunk          program   (count = fields per program)
     FBCh  future[128] chunkSize chunk       bank      (count = 512)
     FxCk / FxBk                             parameter-list variants, also supported

The reference synth chunk, little-endian
  '1lys' version programCount fieldsPerProgram currentProgram      20 bytes
  programCount x { float fields[fieldsPerProgram]; char name[36]; }
```

| Version seen | Fields | Body | Files |
| --- | --- | --- | --- |
| 2200, 2202, 2211 | 243 = 242 + ZEQ mode | plain | 157 |
| 3031 | 243 | plain | 3 |
| 3045 | 244 = 243 + one more (0 in every observed program) | `'1lys' 3045` + **zlib** stream of the chunk | 108 |

How the bank is organised:

- Every bank has **512 slots** (4 x 128). Unused slots hold the reference synth's Init program; one Init variant accounts for 11,505 of 13,704 filler slots. That Init is `src/legacy/LegacyInit.inc`.
- The last header word is the program that was selected when the file was saved.
- Compression can't be inferred from the version number, because 3031 is plain and 3045 is zlib. The reader checks whether the plain interpretation fits, and otherwise inflates.
- Names: the reference synth doesn't clear its 36-byte name buffer, so stale bytes follow the terminator. They are kept, so an unchanged name rewrites exactly.
- Vendor `.fxp` files often still carry the name "Init". Imports name them after the file instead.

Values are **normalized VST floats in 0..1**. A selector stores `index / (count - 1)`; for example Saw is 1/7, mod destination is /32 (33 entries, 28..32 unnamed) and mod source is /11. Two entries in the template are byte-swapped typos: `LFO_Ramp2` is 0.2 and `Chord` is 1.0. Switches are thresholded at 0.5; for example ChorusMode is stored as 0.1, 0.5 or 0.83 in about a quarter of factory programs.

## Code map

| File | Role | Depends on |
| --- | --- | --- |
| `src/legacy/LegacyFields.inc` | the 242 base fields, generated from the `.bt` | — |
| `src/legacy/LegacyInit.inc` | The reference synth's Init program, taken from real banks | — |
| `src/legacy/LegacyFormat.*` | all containers and chunk layouts; zlib is supplied by the caller; writes stored zlib if no deflate is given | C++20 only |
| `src/legacy/LegacyPatchMapper.*` | The reference synth patch → registry IDs, values and choice labels, plus notes on approximations | format |
| `src/legacy/LegacyImport.*` | JUCE zlib; builds `SYNTHAX1_STATE`; keeps the original bytes for lossless export; `buildBank` pads to 512 | JUCE, registry |
| `PluginProcessor` | `.fxb` replaces all 512 programs slot for slot; `.fxp` replaces the current program; `exportLegacyBank(indices, file)`; `setStateInformation` accepts a raw `1lys` chunk | import |
| `scripts/legacy_bank.py` | CLI: `info`, `list`, `dump`, `extract`, `build` (pads to 512 by default) | Python 3 |
| `scripts/legacy_calibration.py` | calibration bank and curve fitter for the remaining estimated knobs | Python 3 |

Tests:

- `LegacyFormatTest` (JUCE-free). Pass a folder to check real files: `-DSYNTHAX1_LEGACY_SAMPLES=<dir>`. Without JUCE it inflates stored zlib only.
- `LegacyMapperTest` (JUCE-free). Checks selectors against the template's bit patterns, and the measured curves against reference examples.
- `LegacyImportTest` (JUCE). Checks every emitted ID and choice label against the live registry.

Export writes the original the reference synth bytes of unedited imported programs, keeping any renames. It refuses programs that were edited after import, or that never came from the reference synth, and names them. Exporting edited sounds needs the reverse mapping (plan, step 4).

## Mapping

Exact:

- every selector and switch;
- oscillator voices, octave, note, fine, phase and pan;
- filter type, slope and routing;
- arp mode, velocity mode and steps;
- mod sources and destinations;
- tempo-division tables.

**Measured curves.** These come from Per Ivar Nerseth's measurements of the reference synth's display (`perivar/AudioVSTToolbox`, its preset class and Zebra2 adapter). They are reimplemented here, not copied.

| Knob | Curve |
| --- | --- |
| Envelope time | piecewise fit; 0 → 0.54 ms, 1 → about 30 s |
| Filter cutoff | 1.0011·e^(9.9673x) |
| Part + Filter Control | they add in the normalized domain: 0.0647·e^(10.153·(part + control)), limited to 22,028 Hz. Synthax1 carries this as a part cutoff plus a shared offset, chosen so the sum is exact for both filters |
| LFO free rate | 0.0417·e^(8.435x), up to about 192 Hz |
| Delay low cut / high cut | low cut rises with the value; high cut **falls** with it (21,120 Hz at 0) |
| EQ frequencies | exponential |
| Compressor ratio | 37 knots from the 190-step table, within 2% |
| Pitch modulation | ±120 semitones at full scale |
| Arp, delay and LFO divisions | the measured selector tables |

**Estimated, to be calibrated** with `legacy_calibration.py`: portamento time, chorus rate, phaser centre and LFO rate, compressor attack and release, mix and main volume levels, and Sync-off delay and arp times.

**EQ gain** is boost-only, 0..15 dB. In 5,600 real programs with the EQ on, the commonest bass value is 0.0 and the commonest treble value is 1.0.

### Engine gaps found by mapping 9,383 real programs

Import notes appear only for settings that can be heard: a routed LFO or mod envelope, an enabled effect, an active filter, a part with voices. These are the remaining gaps, in order of how many programs they affect:

| Programs | Gap | Fix in the engine |
| --- | --- | --- |
| ~2,900 | envelope stage longer than 10 s (the reference synth goes to ~30 s) | raise `*_env.*_ms` maxima in `ParameterRegistry.cpp` and the clamps at `SynthEngine.cpp:203-269` |
| ~2,500 | Pitch/Phase A or B modulate both oscillators of a part; shown on the first oscillator, with hidden depths for the second | add part-wide destinations to `modulationDestinationCatalog()` (UI only; sound is already correct) |
| ~1,300 | EQ boost above +12 dB | widen `fx.eq_*_gain_db` to +15 dB |
| ~1,100 | pitch modulation deeper than ±48 semitones | widen pitch depth to ±120 |
| not recounted (~2,200 together with the LFOs) | Phaser division longer than 1 bar (the reference synth: up to 8/1D). The LFOs now have every division from 8/1D to 1/256T, imported exactly. | append 2, 4 and 8 bar divisions to `fx.phaser_sync_division` |
| 609 | `Dist Amount` mod destination | add a distortion-amount destination |
| 568 | EQ 2-pole mode | add a slope option to the EQ |
| 344 | LFO above 40 Hz | widen `lfo.rate_hz` |

The sync-division choice lists differ in label order between parameters (`lfo.sync_division` vs `fx.delay_sync_division`). The mapper resolves them by label, never by index.

## Plan

1. **Windows build.** Install the VS 2026 "Desktop development with C++" workload, then build VST3 and Standalone.
2. **Calibrate and close engine gaps.** Run the calibration bank in Cubase, fit the curves, then make the engine-range changes listed above.
   **Sound parity** (making Synthax1 *sound* like the reference synth, not only load its presets) has its own plan: `SOUND_PARITY.md`.
3. **Bank builder UI.** A library across all `.fxb`/`.fxp` files and Synthax1 programs, with search, multi-select, drag to reorder, and one click to save a new `.fxb`. It calls `exportLegacyBank(indices, …)`, and `legacy_bank.py build` is the reference behaviour.
4. **Modulation drawer.** A collapsible bottom extension of the window, leaving the 908 x 591 canvas untouched. It brings Serum-style drag-a-source-onto-a-knob, halo rings and alt-drag depth, writing `transmod.N.native.*` depths. Port it from upstream `1303563`, which the classic rebuild (`1f246f9`) removed.
5. **Reverse mapping.** Synthax1 state → the reference synth values, so edited sounds can be exported too.

## Plugin identity (local experiments only)

The DAW identifies a plugin by its IDs, not its display name:

| Format | ID |
| --- | --- |
| VST2 | 4-char unique ID (`syl1` for the reference synth) |
| VST3 | class GUID |
| AU | type/subtype/manufacturer codes |

The IDs live in `CMakeLists.txt` (`PLUGIN_CODE`, `PLUGIN_MANUFACTURER_CODE`, `JUCE_VST3_CAN_REPLACE_VST2`). VST2 needs the legacy VST2 SDK through `juce_set_vst2_sdk_path`. When a host hands this plugin a raw the reference synth chunk, `setStateInformation` imports it. Don't ship a build that reuses another product's IDs, name or vendor: it collides with the real plugin for anyone who has it installed, and it presents this plugin as someone else's product.
