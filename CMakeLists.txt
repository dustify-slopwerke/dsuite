cmake_minimum_required(VERSION 3.22)
project(dustify VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Dustify suite: dsynth (analog synth), dpiano (modelled grand), damp (guitar amp). Each plugin keeps its DSP in a plain C++ library
# (no JUCE) shared by the plugin, its tests and its offline tools; generic DSP lives in dustify_dsp.

option(DUSTIFY_BUILD_TESTS "Build the unit tests" ON)
option(DUSTIFY_BUILD_CLAP "Also build CLAP plugins (via clap-juce-extensions)" ON)
option(DUSTIFY_COPY_PLUGINS "Copy the built plugins to the user plugin folders (~/.vst3 etc.)" ON)
# Old option names, still honoured
if(DEFINED DSYNTH_BUILD_TESTS)
    set(DUSTIFY_BUILD_TESTS ${DSYNTH_BUILD_TESTS})
endif()
if(DEFINED DSYNTH_BUILD_CLAP)
    set(DUSTIFY_BUILD_CLAP ${DSYNTH_BUILD_CLAP})
endif()

# MSVC: sources are UTF-8, and the big test and plugin translation units need /bigobj.
# Executables get Linux's 8 MB main-thread stack: the tests and renderers keep a whole
# dpiano::Piano (~1.4 MB) on the stack, which overflows Windows' 1 MB default.
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
    add_link_options($<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:/STACK:8388608>)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(DUSTIFY_BUILD_CLAP)
    FetchContent_Declare(clap_juce_extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG 7adee3a1bd4684d4caa5601100e364abccff4b4d # main 2026-10-06: first with JUCE 9 support, no release tag yet
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(clap_juce_extensions)
endif()

if(DUSTIFY_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.16.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    include(CTest)
    include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
endif()

include(cmake/DustifyPlugin.cmake)

# Warnings and optimisation for the plain C++ DSP libraries. Synth engines are unplayable
# unoptimised (8 dsynth HQ voices took ~75% of a core), so even Debug builds compile them with
# -O2; assertions stay on.
function(dustify_dsp_options target)
    if(MSVC)
        # MSVC can't combine /O2 with the Debug runtime checks, so Debug stays unoptimised there
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
                                                 $<$<CONFIG:Debug>:-O2>)
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# ---- Shared DSP (no JUCE): filters, effects, oversampling, parameter tables, preset text ----
add_library(dustify_dsp STATIC Source/common/dsp/Convolver.cpp Source/common/dsp/PresetText.cpp)
target_include_directories(dustify_dsp PUBLIC Source)
dustify_dsp_options(dustify_dsp)

# =========================================== dsynth ===========================================

add_library(dsynth_dsp STATIC
    Source/dsynth/dsp/Arpeggiator.cpp
    Source/dsynth/dsp/Engine.cpp
    Source/dsynth/dsp/LayeredSynth.cpp
    Source/dsynth/dsp/Params.cpp
    Source/dsynth/dsp/Preset.cpp
    Source/dsynth/dsp/Voice.cpp)
target_include_directories(dsynth_dsp PUBLIC Source/dsynth)
target_link_libraries(dsynth_dsp PUBLIC dustify_dsp)
dustify_dsp_options(dsynth_dsp)

file(GLOB DSYNTH_PRESETS CONFIGURE_DEPENDS presets/dsynth/*.dsynth)
dustify_add_plugin(dsynth
    IS_SYNTH
    PLUGIN_CODE Dsyn
    LV2URI "https://dustify.com/plugins/dsynth"
    CLAP_ID "com.dustify.dsynth"
    CLAP_FEATURES instrument synthesizer stereo
    DSP_LIB dsynth_dsp
    PRESETS ${DSYNTH_PRESETS}
    SOURCES
        Source/dsynth/PluginProcessor.cpp
        Source/dsynth/PluginEditor.cpp
    SNAPSHOT_DEFINITIONS DUSTIFY_SNAPSHOT_DSYNTH=1
    STATE_FIXTURE tests/fixtures/dsynth_state_v1.bin)

# Offline renderer: preset + notes -> WAV
add_executable(dsynth_render tools/dsynth/render.cpp)
target_link_libraries(dsynth_render PRIVATE dsynth_dsp)

if(DUSTIFY_BUILD_TESTS)
    add_executable(dsynth_tests
        tests/common/Analysis.h
        tests/dsynth/AnalysisHelpers.h
        tests/dsynth/ArpTests.cpp
        tests/dsynth/EngineTests.cpp
        tests/dsynth/DspTests.cpp
        tests/dsynth/PresetTests.cpp)
    target_include_directories(dsynth_tests PRIVATE tests)
    target_link_libraries(dsynth_tests PRIVATE dsynth_dsp Catch2::Catch2WithMain)
    target_compile_definitions(dsynth_tests PRIVATE DSYNTH_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets/dsynth")
    catch_discover_tests(dsynth_tests)
endif()

# =========================================== dpiano ===========================================

add_library(dpiano_dsp STATIC
    Source/dpiano/dsp/KeyData.cpp
    Source/dpiano/dsp/Params.cpp
    Source/dpiano/dsp/Piano.cpp
    Source/dpiano/dsp/PianoKey.cpp
    Source/dpiano/dsp/Preset.cpp
    Source/dpiano/dsp/Soundboard.cpp)
target_include_directories(dpiano_dsp PUBLIC Source/dpiano)
target_link_libraries(dpiano_dsp PUBLIC dustify_dsp)
dustify_dsp_options(dpiano_dsp)
# The mode loops are written for SIMD (#pragma omp simd): vectorise them, including the sums
# (MSVC ignores the pragmas: /openmp would pull in the OpenMP runtime)
if(NOT MSVC)
    target_compile_options(dpiano_dsp PRIVATE -fopenmp-simd -O3)
endif()

file(GLOB DPIANO_PRESETS CONFIGURE_DEPENDS presets/dpiano/*.dpiano)
dustify_add_plugin(dpiano
    IS_SYNTH
    PLUGIN_CODE Dpno
    LV2URI "https://dustify.com/plugins/dpiano"
    CLAP_ID "com.dustify.dpiano"
    CLAP_FEATURES instrument piano stereo
    DSP_LIB dpiano_dsp
    PRESETS ${DPIANO_PRESETS}
    SOURCES
        Source/dpiano/PluginProcessor.cpp
        Source/dpiano/PluginEditor.cpp
    STATE_FIXTURE tests/fixtures/dpiano_state_v1.bin)

# Offline renderer: preset + notes -> WAV
add_executable(dpiano_render tools/dpiano/render.cpp)
target_link_libraries(dpiano_render PRIVATE dpiano_dsp)

if(DUSTIFY_BUILD_TESTS)
    add_executable(dpiano_tests
        tests/dpiano/PianoTests.cpp)
    target_include_directories(dpiano_tests PRIVATE tests)
    target_link_libraries(dpiano_tests PRIVATE dpiano_dsp Catch2::Catch2WithMain)
    target_compile_definitions(dpiano_tests PRIVATE DPIANO_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets/dpiano")
    catch_discover_tests(dpiano_tests)
endif()

# ============================================ damp ============================================

add_library(damp_dsp STATIC
    Source/damp/dsp/Amp.cpp
    Source/damp/dsp/AmpModels.cpp
    Source/damp/dsp/Cabinet.cpp
    Source/damp/dsp/Metronome.cpp
    Source/damp/dsp/Params.cpp
    Source/damp/dsp/PitchDetector.cpp
    Source/damp/dsp/Preset.cpp
    Source/damp/dsp/Rig.cpp
    Source/damp/dsp/looper/LoopPool.cpp
    Source/damp/dsp/looper/LoopSession.cpp
    Source/damp/dsp/looper/Looper.cpp
    Source/damp/dsp/pedals/Board.cpp
    Source/damp/dsp/pedals/Compressor.cpp
    Source/damp/dsp/pedals/Delay.cpp
    Source/damp/dsp/pedals/Drives.cpp
    Source/damp/dsp/pedals/Eq.cpp
    Source/damp/dsp/pedals/Modulation.cpp
    Source/damp/dsp/pedals/Octave.cpp
    Source/damp/dsp/pedals/Pedal.cpp
    Source/damp/dsp/pedals/Pitch.cpp
    Source/damp/dsp/pedals/Reverb.cpp
    Source/damp/dsp/pedals/Wah.cpp
    Source/damp/dsp/ToneStack.cpp
    Source/damp/dsp/Triode.cpp)
target_include_directories(damp_dsp PUBLIC Source/damp)
target_link_libraries(damp_dsp PUBLIC dustify_dsp)
dustify_dsp_options(damp_dsp)

file(GLOB DAMP_PRESETS CONFIGURE_DEPENDS presets/damp/*.damp)
dustify_add_plugin(damp
    PLUGIN_CODE Damp
    LV2URI "https://dustify.com/plugins/damp"
    CLAP_ID "com.dustify.damp"
    CLAP_FEATURES audio-effect distortion stereo mono
    DSP_LIB damp_dsp
    PRESETS ${DAMP_PRESETS}
    SOURCES
        Source/damp/LoopFlac.cpp
        Source/damp/PluginProcessor.cpp
        Source/damp/PluginEditor.cpp
        Source/damp/ui/LooperView.cpp
        Source/damp/ui/LoopWaveformEditor.cpp
        Source/damp/ui/PedalboardView.cpp
        Source/damp/ui/TunerView.cpp
    SNAPSHOT_DEFINITIONS DUSTIFY_SNAPSHOT_DAMP=1
    CHECKS_DEFINITIONS DUSTIFY_CHECKS_DAMP=1
    STATE_FIXTURE tests/fixtures/damp_state_v1.bin)

# Offline renderer: WAV or test tone in -> WAV out
add_executable(damp_render tools/damp/render.cpp)
target_link_libraries(damp_render PRIVATE damp_dsp)

if(DUSTIFY_BUILD_TESTS)
    add_executable(damp_tests
        tests/damp/AmpTests.cpp
        tests/damp/PedalTests.cpp
        tests/damp/FrontPedalTests.cpp
        tests/damp/ModulationPedalTests.cpp
        tests/damp/DelayPedalTests.cpp
        tests/damp/ReverbPedalTests.cpp
        tests/damp/PitchPedalTests.cpp
        tests/damp/EqPedalTests.cpp
        tests/damp/LooperTests.cpp
        tests/damp/LoopEditMathTests.cpp
        tests/damp/TempoTests.cpp)
    target_include_directories(damp_tests PRIVATE tests)
    target_link_libraries(damp_tests PRIVATE damp_dsp Catch2::Catch2WithMain)
    target_compile_definitions(damp_tests PRIVATE DAMP_PRESET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets/damp")
    catch_discover_tests(damp_tests)
endif()
