# CBT - Click Before Tick v1.0 Alpha

Experimental Android64 Geometry Dash 2.2081 Geode mod.

## Features

- Input Precision slider: **-65ms to +65ms** in 1ms steps.
  - `0ms`: no timestamp bias.
  - Negative: advances the queued input timestamp relative to the physics timeline.
  - Positive: delays the queued input timestamp.
- Experimental **CBT Extrapolate** toggle.
- Experimental **TPS bypass** from 240 to 1000 TPS.
- Minimal QoL-style status overlay.
- Safe Mode prevents experimental timing/TPS runs from being treated as normal completions.
- GitHub Actions Android64 build.

## Important alpha limitation

This version is a genuine timestamp-bias prototype, not a claim of impossible hardware latency reduction. A negative setting changes the input's timestamp inside the game simulation; it cannot make Android deliver a physical touch earlier than the OS delivered it. Full sub-tick CBT/CBS-style step splitting and a robust extrapolator are the next engineering stage.

## Build

Push the repository to GitHub. The workflow uses `geode-sdk/build-geode-mod@main` with target `Android64`. The build action outputs the generated `.geode` package as an Actions artifact.
