# Public release cleanup

A working list of everything that still points at the product Synthax1 was measured against. It exists so the repository can be made public once the interface no longer resembles that product. Delete this file when the list is done.

Status as of 2026-11-06. The name was removed from code, docs and file names in `bca80e0`; the items below remain.

## Still names the product (tracked files)

| File | What it says | Decision needed |
| --- | --- | --- |
| `README.md`, `gh-pages` site, release notes | Nothing: compatibility is described as `.fxp`/`.fxb` support only, with no product name or trademark notice (owner's decision, 2026-11-12) | Keep it that way |
| Messages shown to users (import, export, browser) | Nothing: they say `.fxp`/`.fxb` instead of "the reference synth" | Keep new messages the same |

## Git history

- `fce81ad` (initial commit) still contains the name in file contents, and `bca80e0` shows it in its diff. Making the repository public publishes this history. Decide whether that is acceptable or whether to start from a squashed history (only the owner can authorize rewriting history).

## Describes the product without naming it ("reference synth", "classic")

58 tracked files use the phrase "reference synth". Most are comments in DSP code that record a measurement. The biggest:

- `docs/SOUND_PARITY.md` (64), `src/legacy/LegacyPatchMapper.cpp` (24), `docs/LEGACY_IMPORT.md` (19), `src/legacy/LegacyFormat.h` (12), `src/dsp/fx/FxChain.cpp` (10), `src/legacy/LegacyFormat.cpp` (9), `src/legacy/LegacyImport.cpp` (8), `scripts/legacy_bank.py` (8).
- `docs/CLASSIC_PARITY.md`, `AGENTS.md`, `SPEC.md`, `CONTEXT.md`: they define the goal as recreating "the classic reference synth control surface".

Decision: keep the measured facts, drop the framing, or move the parity documents out of the public tree.

## Code that only exists to match or import the product

- Import and export of its patch files: `src/legacy/`, `tests/legacy/`, `tests/references/legacy-reference-manifest.json`, `docs/LEGACY_IMPORT.md`.
- Measurement tooling: `tools/legacy_host/LegacyRenderHost.cpp` (loads the original plugin), `scripts/legacy_*.py`, `scripts/parity_*.py`, `scripts/compare-reference-*.py`.
- `src/voice`, `src/dsp`: constants measured on the original (envelope, filter, LFO, delay), commented with the measurement.

Decision: the importer is a feature (README sells it). The measurement tooling and parity scripts are not needed by users.

## Files present locally but ignored by git (never commit)

the reference manuals (`*Manual.pdf`), the `.bt` field template, `serum2.jpg`, `init.png`, `research/` (renders and sweeps made with the original plugin). `.gitignore` already keeps them out. Do not move them into a tracked folder.

## Interface

The layout is the main resemblance: panel arrangement, the LCD in the middle, part A/B buttons, knob styling, and the control names (for example "Filter Control", "Mod Env", "Misc"). The redesign of the modulation row (curves, drag handles) is the first step; the remaining panels have not started.

## Also check before going public

- `docs/assets/screenshots/` shows the current interface.
- `.github/workflows/build.yml` mentions the reference product in its text.
- Factory presets (`presets/factory`, 18 files) are owned content; confirm none was exported from the original.
- GitHub Actions is failing for billing reasons while the repository is private; public repositories run it for free.
