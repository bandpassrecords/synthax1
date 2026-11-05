# Documentation Index

`../SPEC.md` is the source of truth for product requirements.

## Start here

- [../README.md](../README.md): what Synthax1 is and how to build it.
- [../SPEC.md](../SPEC.md): product specification.
- [../CONTEXT.md](../CONTEXT.md): vocabulary and decision lanes.
- [../AGENTS.md](../AGENTS.md): working rules for coding agents.
- [../CONTRIBUTING.md](../CONTRIBUTING.md): setup, checks and pull requests.

## Engineering

- [ARCHITECTURE.md](ARCHITECTURE.md): components and realtime boundaries.
- [PRESET_SCHEMA.md](PRESET_SCHEMA.md): preset and host-state shape.
- [SYLENTH_IMPORT.md](SYLENTH_IMPORT.md): reading and writing Sylenth1 `.fxp` / `.fxb` files.
- [SOUND_PARITY.md](SOUND_PARITY.md): how the sound is matched to Sylenth1, with the measurements.
- [CLASSIC_PARITY.md](CLASSIC_PARITY.md): control coverage and reference policy.
- [VALIDATION.md](VALIDATION.md): tests, renders and host checks.
- [QUALITY.md](QUALITY.md): formatting, linting and realtime-safety gates.
- [BUILD_RELEASE.md](BUILD_RELEASE.md): optimized builds, packaging and local install.
- [SCREENSHOTS.md](SCREENSHOTS.md): capturing the interface.
- [release/](release): release checklist and manifest.

## When documents disagree

1. `SPEC.md` wins on product requirements.
2. `ARCHITECTURE.md`, `VALIDATION.md` and `PRESET_SCHEMA.md` are implementation guidance and are updated to match the spec.
