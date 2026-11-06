# Contributing

Thanks for helping with Synthax1. Open an issue for anything larger than a small fix so the direction can be agreed first.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSYNTHAX1_ENABLE_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake downloads JUCE 8.0.13. Use `-DSYNTHAX1_JUCE_PATH=/path/to/JUCE` for an existing checkout. `SYNTHAX1_BUILD_REFERENCE_HOST=ON` also builds a tool that renders through a locally installed the reference synth for the sound-matching work (see `docs/SOUND_PARITY.md`); it is off in CI.

## Pull requests

- Branch from `main` and keep one purpose per pull request.
- Use conventional commit messages and short conventional PR titles, for example `fix: release held notes when arp stops`.
- Say what the problem was, what changed, how you checked it (tests, renders, host checks) and what you could not check.
- The audio thread must not allocate, lock, touch files or log. `scripts/check-quality.sh` runs the realtime and type-safety checks, the build and the tests.
- Test behaviour you can hear or a plausible failure, not every line you touch.

By contributing you agree that your work is released under the [MIT license](LICENSE).
