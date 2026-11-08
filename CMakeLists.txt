cmake_minimum_required(VERSION 3.22)
project(NeonRuin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# JUCE framework (cloned into ./JUCE)
add_subdirectory(JUCE)

juce_add_plugin(NeonRuin
    COMPANY_NAME "Octave"
    PRODUCT_NAME "Neon Ruin"
    PLUGIN_MANUFACTURER_CODE Octv
    PLUGIN_CODE NrVx
    FORMATS VST3 AU Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Vocals)

target_sources(NeonRuin PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(NeonRuin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(NeonRuin
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Small helper app that renders the UI to a PNG (used for previews)
option(NEON_BUILD_SNAPSHOT "Build UI snapshot tool" OFF)
if(NEON_BUILD_SNAPSHOT)
    juce_add_console_app(UISnapshot PRODUCT_NAME "UISnapshot")
    target_sources(UISnapshot PRIVATE tools/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(UISnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Neon Ruin" JucePlugin_IsSynth=0 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
    target_link_libraries(UISnapshot PRIVATE juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
endif()

# Offline test bench for the pitch corrector (renders WAVs for listening/analysis)
if(NEON_BUILD_SNAPSHOT)
    juce_add_console_app(TuneTest PRODUCT_NAME "TuneTest")
    target_sources(TuneTest PRIVATE tools/TuneTest.cpp)
    target_compile_definitions(TuneTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(TuneTest PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_recommended_config_flags)
endif()
