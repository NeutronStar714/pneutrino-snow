# ==============================================================================
# Snow Plugin Target — VST3 + Standalone
# ==============================================================================

juce_add_plugin(Snow
    VERSION 0.0.1
    COMPANY_NAME "NeutronStar714"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    PLUGIN_MANUFACTURER_CODE NStr
    PLUGIN_CODE Sn01
    FORMATS VST3 Standalone
    PRODUCT_NAME "Snow"
    BUNDLE_ID "com.neutronstar714.Snow"
)

# ------------------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------------------
target_sources(Snow
    PRIVATE
        PluginProcessor.cpp
        PluginProcessor.h
        PluginEditor.cpp
        PluginEditor.h
        SpectrumDisplay.cpp
        SpectrumDisplay.h

        # DSP (Spacer/Snowfall ../DSP/ pattern)
        ../DSP/SpectralMaskEngine.cpp
        ../DSP/SpectralMaskEngine.h
)

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
target_include_directories(Snow
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../DSP
        ${SNOW_FFT_INCLUDE_DIR}
)

# ------------------------------------------------------------------------------
# Compile definitions
# ------------------------------------------------------------------------------
target_compile_definitions(Snow
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# ------------------------------------------------------------------------------
# Linked libraries
# ------------------------------------------------------------------------------
target_link_libraries(Snow
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
