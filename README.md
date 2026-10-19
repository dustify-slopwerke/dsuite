# dsuite

[![CI](https://github.com/dustify-slopwerke/dsuite/actions/workflows/ci.yml/badge.svg)](https://github.com/dustify-slopwerke/dsuite/actions/workflows/ci.yml)
[![Release](https://github.com/dustify-slopwerke/dsuite/actions/workflows/release.yml/badge.svg)](https://github.com/dustify-slopwerke/dsuite/actions/workflows/release.yml)

dsuite is Dustify's suite of three audio plugins, built from one C++20 / JUCE codebase:

- **dsynth**: an analog-modelling polysynth. One configurable voice covers the classic 70s/80s
  instruments (Minimoog, Prophet-5, Juno / Jupiter, Oberheim SEM / OB, MS-20, ARP, CS-80 styles).
- **dpiano**: a physically modelled grand piano. Everything is computed; there are no samples.
- **damp**: a guitar amp simulator with a pedalboard, cabinets and IR loading, a tuner and an
  8-track looper.

Formats: VST3, CLAP, LV2 and Standalone.

Platforms: Linux and Windows (x86_64). macOS is untested and unsupported for now; building from
source there may work.

User guides: [docs/dsynth.md](docs/dsynth.md), [docs/dpiano.md](docs/dpiano.md),
[docs/damp.md](docs/damp.md).

- Source: <https://github.com/dustify-slopwerke/dsuite>
- Downloads: [releases](https://github.com/dustify-slopwerke/dsuite/releases)
- Bug reports and feature requests: [issues](https://github.com/dustify-slopwerke/dsuite/issues)

## The plugins

### dsynth

- 3 band-limited oscillators with hard sync and combined waveforms, sub-oscillator, noise, ring
  modulator, cross-mod and Poly-Mod; VCO (drifting) or DCO (stable) character.
- Filter models: transistor ladder (24/12 dB), CEM-style OTA, IR3109 OTA (24/12 dB), SEM
  state-variable (LP / notch / HP, bandpass), MS-20 Korg35, ARP 4075-style ladder; Juno high-pass
  and a resonant per-voice pre-HPF.
- Analog-RC envelopes, global LFO with delay, per-voice LFO 2, 4-slot mod matrix, wheel mod.
- Mono / legato / poly / unison / Duo voice modes, glide, Jupiter-8 key assign modes, two layers
  (Single, Dual, Split).
- Arpeggiator (host-synced or free, latch), BBD chorus I / II / I+II, stereo / ping-pong delay,
  FDN reverb.
- Velocity curves, aftertouch, poly aftertouch and MPE; historical tunings on any root.
- 2x oversampling, or 4x in HQ mode.

### dpiano

- Per-key strings with inharmonic partials, unison detuning (prompt and aftersound decay),
  nonlinear hysteretic felt hammers coupled to the strings, dampers.
- Stretched tuning and historical temperaments.
- Modal soundboard, lid, sympathetic resonance of undamped strings, synthesised mechanical noises.
- Continuous sustain pedal (half-pedalling), sostenuto, una corda.
- Player or audience perspective, microphone distance, room reverb.
- A Voicing tab exposing the model's physical constants (hammer felt, soundboard bloom and
  radiation).

### damp

- Amp models: Clean, Chime, Crunch, High Gain, Modern High Gain; tube preamp, tone stack and a
  push-pull power stage with negative feedback, presence, resonance and sag.
- Cabinets: 1x12 open, 2x12 open, 4x12 closed with dynamic / ribbon / condenser mics, or a
  user impulse response (WAV, AIFF, FLAC) saved in the session.
- Pedalboard: boost, mid drive, blues drive, distortion, rodent, two fuzzes, compressor, wah,
  octave, pitch shifter, graphic EQ, chorus, flanger, phaser, tremolo, vibrato, delay (digital /
  analog / tape), reverb (spring / plate / hall). Pedals can be reordered; some can sit before or
  after the amp.
- Noise gate, input gain, 2x / 4x / 8x oversampling.
- Chromatic tuner.
- 8-track looper at the end of the chain: tracks can sync to the first loop's length, overdub, undo / redo,
  non-destructive trim and padding editor, reverse, half speed; loops are saved with the session
  (FLAC-compressed).

## Building

Requirements: CMake 3.22 or newer, Ninja (recommended), Git, and a C++20 compiler. JUCE,
clap-juce-extensions and Catch2 are downloaded by CMake at configure time (see
[THIRD_PARTY.md](THIRD_PARTY.md)).

### Linux

GCC or Clang, plus the JUCE development packages (Debian / Ubuntu names; the build does not use
JUCE's web browser, curl or OpenGL, so WebKitGTK, libcurl and GL are not needed):

```sh
sudo apt install build-essential cmake ninja-build git \
    libasound2-dev libjack-jackd2-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
    libxrandr-dev libxrender-dev libxi-dev
```

The Python helper scripts in `tools/` also need `python3-numpy` and `python3-scipy`
(`python3-matplotlib` for spectrogram plots).

### Windows

Visual Studio 2022 with the "Desktop development with C++" workload (it includes CMake and
Ninja). Run the commands below from a "x64 Native Tools Command Prompt for VS 2022".

### Configure, build, test

```sh
git clone https://github.com/dustify-slopwerke/dsuite.git
cd dsuite
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Effect |
|---|---|---|
| `DUSTIFY_BUILD_TESTS` | `ON` | Catch2 unit tests and the `<plugin>_plugin_checks` ctest entries |
| `DUSTIFY_BUILD_CLAP` | `ON` | CLAP builds via clap-juce-extensions |
| `DUSTIFY_COPY_PLUGINS` | `ON` | Copy the built plugins into the plugin folders after each build |

Use a Release build for playing. With GCC / Clang, Debug builds still compile the DSP libraries
with optimisation; with MSVC they don't, so Debug builds there are too slow to play.

Building a plugin target (`dsynth`, `dpiano`, `damp`, or `all`) copies the plugins into the user's
plugin folders after each build (JUCE's `COPY_PLUGIN_AFTER_BUILD`, unless
`-DDUSTIFY_COPY_PLUGINS=OFF`): on Linux `~/.vst3`, `~/.lv2`
and `~/.clap`; on Windows the system VST3 / CLAP folders under `Common Files`, which needs
write access there. The built files are also under
`build/<plugin>_artefacts/<config>/`. To build only the tests and tools without installing
anything, name their targets:

```sh
cmake --build build --target dsynth_tests dpiano_tests damp_tests \
    dsynth_plugin_checks dpiano_plugin_checks damp_plugin_checks
```

## Tests and tools

- `ctest` runs the Catch2 DSP tests (`<plugin>_tests`: pitch, aliasing, filters, envelopes,
  amp and pedal responses, looper behaviour, preset loading) and `<plugin>_plugin_checks`, which
  check state save / restore through a fresh instance, byte-for-byte reproducible state,
  parameter value / text round trips, and that the saved states in `tests/fixtures/` still load.
- `<plugin>_render`: offline renderers that run the DSP without a host and write a WAV. Run with
  no arguments for usage. Examples:

  ```sh
  build/dsynth_render --preset presets/dsynth/30_01_mini_bass.dsynth -o bass.wav
  build/dsynth_render --set filter_reso=0.8 --notes 36:0:1,48:0.5:1 -o test.wav   # note:start:dur[:vel]
  build/dpiano_render --notes 60:0:4:0.7,64:0:4:0.7 --pedal 0:5 -o chord.wav
  build/damp_render --preset presets/damp/30_01_classic_crunch.damp --guitar demo -o crunch.wav
  ```

- `<plugin>_ui_snapshot <dir> [preset-index] ...`: paints every editor tab to PNG without
  opening a window (not built by default: `cmake --build build --target dsynth_ui_snapshot`).
- `tools/common/level_presets.py`: measures each factory preset's loudness (K-weighted, loudest
  400 ms) through the renderer and, with `--apply`, sets its output volume to a target level.
- `tools/dpiano/analyse.py`: measures a rendered note's partials (frequency, inharmonicity,
  level, decay) and plots a spectrogram.

Plugins can also be checked with external validators such as
[pluginval](https://github.com/Tracktion/pluginval) and
[clap-validator](https://github.com/free-audio/clap-validator).

## Presets

Factory presets live in `presets/<plugin>/` and are compiled into the plugin. Each is a text file
of `id = value` lines (`#` starts a comment; parameters not listed take their defaults). The
`name` and `category` lines set how it appears in the preset menu, and files are named
`<category number>_<index>_<name>.<plugin>` so they sort into place. dsynth presets may add a
`[layer b]` section for the second layer.

Song presets are sounds inspired by well-known recordings and are named after the song.

User presets are saved to the user application data folder: `~/.config/<plugin>/presets` on
Linux, `%APPDATA%\<plugin>\presets` on Windows.

## Running the Standalone on Linux

For low latency, use JACK (or PipeWire's JACK layer) and pick **JACK** under
Options > Audio/MIDI Settings:

```sh
env PIPEWIRE_QUANTUM=128/48000 build/dsynth_artefacts/Release/Standalone/dsynth
```

The synth editors can be played with the mouse or the computer keyboard (`Z S X D C V ...` from
middle C; click the on-screen keyboard to focus it).

## Project layout

| Path | Contents |
|---|---|
| `Source/common/dsp/` | Shared DSP in plain C++20 without JUCE (`dustify_dsp`): filters, oversampling, delay / chorus / reverb, convolution, LFO / envelope, smoothing, tunings, parameter tables, preset text format |
| `Source/common/plugin/`, `Source/common/ui/` | Shared JUCE code: host parameters built from a parameter table, look and feel, preset bar, generated control sections |
| `Source/<plugin>/dsp/` | Each plugin's engine in plain C++ (`<plugin>_dsp`); `Params.*` is the plugin's single parameter table |
| `Source/<plugin>/` | The plugin's JUCE processor and editor |
| `cmake/DustifyPlugin.cmake` | `dustify_add_plugin()`: formats, CLAP, presets and the headless tools for one plugin |
| `presets/<plugin>/` | Factory presets |
| `tests/` | Catch2 tests per plugin, shared analysis helpers, saved-state fixtures |
| `tools/` | Offline renderers, headless UI snapshot and plugin checks, Python analysis scripts |
| `docs/` | User guides |

## License

Copyright (C) 2026 Adrian Johnson.

dsuite is free software: you can redistribute it and/or modify it under the terms of the GNU
Affero General Public License as published by the Free Software Foundation, version 3 of the
License. It is distributed WITHOUT ANY WARRANTY; see [LICENSE](LICENSE) for the full terms.

The plugins are built with JUCE, used under its AGPLv3 licence. Third-party components and their
licences are listed in [THIRD_PARTY.md](THIRD_PARTY.md). Product and instrument names mentioned
in the code, docs and presets belong to their owners and are used only to describe a sound.

## Contributing

Bug reports and pull requests are welcome on
[GitHub](https://github.com/dustify-slopwerke/dsuite/issues). Please keep changes focused, run `ctest` before
submitting, and add a test for DSP changes where one makes sense. Parameter ids are part of
saved projects: add new parameters at the end of a plugin's table and never rename or reorder
existing ones. By contributing you agree that your contribution is licensed under the AGPLv3.

Contact: Adrian Johnson <aj@dustify.com>
