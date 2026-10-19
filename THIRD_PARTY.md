# Third-party software

dsuite itself is licensed under the GNU Affero General Public License v3 (see `LICENSE`).
No third-party code is stored in this repository; CMake downloads the following at configure
time (`FetchContent` in `CMakeLists.txt`) and they are compiled into the plugins and tools.

| Component | Version | Used for | Licence |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4 | Plugin framework, GUI, audio I/O, Standalone app | AGPLv3 or commercial JUCE licence |
| [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) | commit `7adee3a` | CLAP plugin format (optional, `DUSTIFY_BUILD_CLAP`) | MIT |
| [CLAP](https://github.com/free-audio/clap) and clap-helpers | submodules of clap-juce-extensions | CLAP API headers | MIT |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.1 | Unit tests only (`DUSTIFY_BUILD_TESTS`) | BSL-1.0 |

JUCE vendors further libraries; the ones that end up in these plugins include:

| Component | Licence |
|---|---|
| VST3 SDK (Steinberg) | MIT |
| LV2, lilv, serd, sord, sratom | ISC |
| FLAC, libogg | BSD-3-Clause |
| zlib | Zlib |
| libpng | libpng-2.0 |
| IJG jpeglib | IJG |
| HarfBuzz | MIT |
| SheenBidi | Apache-2.0 |
| LunaSVG | MIT |

JUCE's complete inventory, with versions and licences, is `JUCE.spdx.json` in the JUCE source
tree (`build/_deps/juce-src/` after configuring).

The Python helper scripts in `tools/` use NumPy, SciPy and (optionally) Matplotlib. They are not
part of any plugin.

The external validators mentioned in the README ([pluginval](https://github.com/Tracktion/pluginval),
[clap-validator](https://github.com/free-audio/clap-validator)) are separate tools, not
downloaded or bundled by this build.

## Reference data

dpiano's default voicing was fitted by analysing piano recordings from the University of Iowa
Electronic Music Studios' Musical Instrument Samples. Only derived numbers (partial levels, decay
rates, loudness) are in the source code; no recordings are included.
