# ==============================================================================
# Snow — Stochastic Spectral Masking (phase-vocoder) effect
# ==============================================================================
cmake_minimum_required(VERSION 3.22)

project(Snow VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PNEUTRINO_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE"
    CACHE PATH "Path to the pinned JUCE source checkout")
set(PNEUTRINO_FFT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/signalsmith-fft"
    CACHE PATH "Path to the pinned Signalsmith FFT source checkout")

set(fft_header "${PNEUTRINO_FFT_SOURCE_DIR}/signalsmith-fft.h")
if(NOT EXISTS "${PNEUTRINO_JUCE_SOURCE_DIR}/CMakeLists.txt" OR NOT EXISTS "${fft_header}")
    message(FATAL_ERROR "A dependency is missing. Run git submodule update --init --recursive.")
endif()

# Keep the original project's explicit conversions in a generated header.
# The upstream checkout can be shared and stays untouched.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fft_header}")
file(READ "${fft_header}" fft_source)
string(REPLACE "{sin(rotPhase), -cos(rotPhase)}"
    "{static_cast<V>(sin(rotPhase)), static_cast<V>(-cos(rotPhase))}" fft_source "${fft_source}")
string(REPLACE "{cos(rotPhase), sin(rotPhase)}"
    "{static_cast<V>(cos(rotPhase)), static_cast<V>(sin(rotPhase))}" fft_source "${fft_source}")
set(SNOW_FFT_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/fft")
file(WRITE "${SNOW_FFT_INCLUDE_DIR}/signalsmith-fft.h" "${fft_source}")

add_subdirectory("${PNEUTRINO_JUCE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")
add_subdirectory(src/Plugin)
