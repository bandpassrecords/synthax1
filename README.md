<img src="packaging/branding/synthax1-icon.svg" alt="Synthax1 logo" width="96" align="right">

# Synthax1

A free, open-source virtual analog synthesizer for Windows, macOS and Linux (VST3, AU on macOS, and a standalone app), with a preset browser built for very large libraries.

Website: [synthax1.bandpassrecords.com](https://synthax1.bandpassrecords.com)

![The Synthax1 main window](docs/assets/main-window.png)

## Highlights

- **Two stereo parts:** four oscillators with up to eight unison voices each, a filter per part plus a shared filter control, two amp envelopes, two modulation envelopes, two LFOs, a 16-step arpeggiator and seven effects.
- **Modulation you can see:** envelopes and LFOs draw their curves, LFOs can be drawn step by step, and dragging a source's handle onto a knob routes it there.
- **A preset browser for big libraries:** point it at one folder and it finds every bank and preset below it. Browse by category, bank, vendor or tag; search, keep favorites and notes, and hear a preset with a single click. Drag a program out as a `.fxp` file (Ctrl for its whole `.fxb` bank) or copy its path.
- **Sylenth1® compatibility:** opens `.fxp` programs and `.fxb` banks saved by Sylenth1, and a program dragged from the browser can be dropped straight into Sylenth1 (hold Ctrl for its whole bank).
- **Unlimited undo and redo** (Ctrl+Z, Ctrl+Y), and every control is a host parameter, the wheels included.

![The preset browser](docs/assets/preset-browser.png)

## Build

Requires CMake 3.24 or newer, a C++20 compiler and, on Linux, the usual JUCE development packages (see `.github/workflows/build.yml`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake downloads JUCE 8.0.13; pass `-DSYNTHAX1_JUCE_PATH=/path/to/JUCE` to use an existing checkout. The plugins are written to `build/Synthax1Plugin_artefacts/Release`.

## Documentation

The [documentation index](docs/index.md) lists the specification, architecture, preset schema, validation and release notes.

## License

[MIT](LICENSE) © BandPass Records.

Synthax1 uses [JUCE](https://juce.com), which is available under the AGPLv3 or a commercial license. The release binaries are built under the AGPLv3 option, so they are distributed under the [AGPLv3](packaging/licenses/AGPL-3.0.txt); builds you distribute must comply with one of the two licenses.

Sylenth1 is a registered trademark of LennarDigital. Synthax1 is an independent project and is not affiliated with, endorsed by or sponsored by LennarDigital; the name is used only to describe file compatibility.
