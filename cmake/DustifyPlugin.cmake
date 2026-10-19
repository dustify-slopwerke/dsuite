# dustify_add_plugin: one plugin of the suite, plus its headless tools.
#
#   dustify_add_plugin(<name>
#       PLUGIN_CODE Dsyn  LV2URI https://...  CLAP_ID com.dustify.<name>  CLAP_FEATURES instrument ...
#       [IS_SYNTH]  PRESETS <files...>  SOURCES <plugin .cpp files...>  DSP_LIB <name>_dsp
#       [SNAPSHOT_DEFINITIONS <defs...>]  [CHECKS_DEFINITIONS <defs...>]
#       [STATE_FIXTURE tests/fixtures/<name>_state_v1.bin])
#
# Creates:
#   <name>                 VST3 / LV2 / Standalone (+ CLAP), copied to ~/.vst3 etc. after building
#                          (unless DUSTIFY_COPY_PLUGINS is OFF)
#   <name>_presets         factory presets as binary data
#   <name>_ui_snapshot     paints every editor tab to PNG, no window (EXCLUDE_FROM_ALL)
#   <name>_plugin_checks   state / value-text round trips, run by ctest as <name>_plugin_checks
#
# JUCE modules are compiled into each target that links them, so the plugin's sources (and the
# shared UI) are compiled into the plugin and both tools rather than into a shared library.

set(DUSTIFY_UI_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../Source/common/plugin/ParamLayout.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Source/common/ui/ParamSection.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Source/common/ui/PresetBar.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../Source/common/ui/DustifyLookAndFeel.cpp)

set(DUSTIFY_TOOLS_DIR ${CMAKE_CURRENT_LIST_DIR}/../tools/common)

function(dustify_add_plugin NAME)
    cmake_parse_arguments(ARG "IS_SYNTH" "PLUGIN_CODE;LV2URI;CLAP_ID;DSP_LIB;STATE_FIXTURE"
                          "CLAP_FEATURES;PRESETS;SOURCES;SNAPSHOT_DEFINITIONS;CHECKS_DEFINITIONS" ${ARGN})

    if(ARG_IS_SYNTH)
        set(is_synth TRUE)
    else()
        set(is_synth FALSE)
    endif()

    juce_add_binary_data(${NAME}_presets SOURCES ${ARG_PRESETS})
    set_target_properties(${NAME}_presets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    juce_add_plugin(${NAME}
        COMPANY_NAME "Dustify"
        COMPANY_COPYRIGHT "Copyright (C) 2026 Adrian Johnson"
        COMPANY_EMAIL "aj@dustify.com"
        PRODUCT_NAME "${NAME}"
        PLUGIN_MANUFACTURER_CODE Dsty
        PLUGIN_CODE ${ARG_PLUGIN_CODE}
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT ${is_synth}
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS ${is_synth}
        COPY_PLUGIN_AFTER_BUILD ${DUSTIFY_COPY_PLUGINS}
        LV2URI "${ARG_LV2URI}"
        FORMATS VST3 LV2 Standalone)

    target_sources(${NAME} PRIVATE ${ARG_SOURCES} ${DUSTIFY_UI_SOURCES})
    target_include_directories(${NAME} PRIVATE ${PROJECT_SOURCE_DIR}/Source ${PROJECT_SOURCE_DIR}/Source/${NAME})
    target_compile_definitions(${NAME} PUBLIC
        # Standalone can use JACK on Linux (PipeWire's JACK layer): far less latency than ALSA "default"
        JUCE_JACK=$<IF:$<PLATFORM_ID:Linux>,1,0>
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${NAME}
        PRIVATE
            ${ARG_DSP_LIB}
            ${NAME}_presets
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    if(DUSTIFY_BUILD_CLAP)
        clap_juce_extensions_plugin(TARGET ${NAME}
            CLAP_ID "${ARG_CLAP_ID}"
            CLAP_FEATURES ${ARG_CLAP_FEATURES})
    endif()

    # Headless tools: the plugin's code in a console app
    foreach(tool ui_snapshot plugin_checks)
        set(target ${NAME}_${tool})
        juce_add_console_app(${target} PRODUCT_NAME "${target}")
        target_sources(${target} PRIVATE ${DUSTIFY_TOOLS_DIR}/${tool}.cpp ${ARG_SOURCES} ${DUSTIFY_UI_SOURCES})
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/Source ${PROJECT_SOURCE_DIR}/Source/${NAME})
        target_compile_definitions(${target} PRIVATE
            JucePlugin_Name="${NAME}"
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
        target_link_libraries(${target} PRIVATE
            ${ARG_DSP_LIB}
            ${NAME}_presets
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    endforeach()
    set_target_properties(${NAME}_ui_snapshot PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_compile_definitions(${NAME}_ui_snapshot PRIVATE ${ARG_SNAPSHOT_DEFINITIONS})
    target_compile_definitions(${NAME}_plugin_checks PRIVATE ${ARG_CHECKS_DEFINITIONS})

    if(DUSTIFY_BUILD_TESTS)
        if(ARG_STATE_FIXTURE)
            add_test(NAME ${NAME}_plugin_checks COMMAND ${NAME}_plugin_checks ${PROJECT_SOURCE_DIR}/${ARG_STATE_FIXTURE})
        else()
            add_test(NAME ${NAME}_plugin_checks COMMAND ${NAME}_plugin_checks)
        endif()
    endif()
endfunction()
