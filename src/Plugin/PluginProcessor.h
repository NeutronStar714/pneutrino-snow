#pragma once

#include <array>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "../DSP/SpectralMaskEngine.h"

// ==============================================================================
// Parameter contract (plan.md §2 — IDs immutable; one APVTS parameter each)
// ==============================================================================
namespace Snow::Params
{
    inline const juce::String amount { "AMOUNT" };   // Float, 0-99 %     , default 50 %
    inline const juce::String width  { "WIDTH" };    // Float, 10-1000 ms , default 90 ms
    inline const juce::String cutoff { "CUTOFF" };  // Float, 0-1        , default 1.0 (0 = 16 bins random + rest muted, log)
    inline const juce::String grid   { "GRID" };     // Int,   1-16 bins  , default 8 (mask smoothing grid)
    inline const juce::String phase  { "PHASE" };    // Float, 0-90 deg (exponential knob), default 0 deg (0 = cancellation, 90 = +3 dB boost)
    inline const juce::String stereo { "STEREO" };   // Float, 0-1        , default 0.7 (decoupling)
    inline const juce::String gain   { "GAIN" };     // Float, 0-12 dB    , default 6 dB (wet makeup)
    inline const juce::String dry    { "DRY" };      // Float, 0-1        , default 0.0 (linear)
    inline const juce::String wet    { "WET" };      // Float, 0-1        , default 1.0 (linear)

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}

// ==============================================================================
class SnowAudioProcessor final : public juce::AudioProcessor
{
public:
    SnowAudioProcessor();
    ~SnowAudioProcessor() override;

    // --- JUCE AudioProcessor overrides ---
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- Snow API ---
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    // Latency contract (plan.md §3/§4): the WOLA wet path is inherently
    // delayed — N - hop = 2048 - 512 = 1536 samples (32 ms @ 48 kHz) on
    // hop-aligned host blocks, one extra hop (2048) on the lagged path (hosts
    // whose block size is not a multiple of the hop; see
    // SpectralMaskEngine::process) — but the plugin reports ZERO latency:
    // the DRY path passes the input through immediately (live-monitoring
    // friendly) and the late wet is blended in via a fade-through crossfade
    // (equal-power dip at the midpoint) so the dry->wet transition sounds as
    // natural as possible.
    static constexpr int latencySamples = Snow::DSP::SpectralMaskEngine::latencySamples;   // wet-path delay — NOT reported
    static constexpr int maxWetLatencySamples = Snow::DSP::SpectralMaskEngine::fftSize;    // 2048 — dry ring capacity

    // --- Visualization ---
    // Shared lock-free snapshot of the engine's post-mask spectrum: the
    // audio thread publishes once per hop tick, the editor's scrolling
    // SpectrumDisplay reads it.
    std::shared_ptr<Snow::DSP::SpectrumSnapshot> getSpectrumSnapshot() const noexcept
    {
        return spectrumSnapshot;
    }

private:
    juce::AudioProcessorValueTreeState apvts;

    // --- Stage 3 DSP ---
    // Hop-based STFT/WOLA engine with the stochastic spectral masking kernel:
    // consumes the input block and writes the wet output (input delayed by
    // 1536/2048 samples, depending on the host block size) back into the block.
    Snow::DSP::SpectralMaskEngine maskEngine;

    // Post-mask spectrum for the UI display (see getSpectrumSnapshot).
    std::shared_ptr<Snow::DSP::SpectrumSnapshot> spectrumSnapshot = std::make_shared<Snow::DSP::SpectrumSnapshot>();

    // Dry path: immediate-input tap (ring of maxWetLatencySamples with ZERO
    // lag). The tap is captured BEFORE the engine overwrites the block, then
    // the tail-end fade-through crossfade (plan.md §4 step 5) blends:
    //   out = drySmooth * dip * dry + wetSmooth * dip * makeup * wet
    std::array<std::vector<float>, 2> dryRing;
    int dryWrite = 0;
    int dryDelay = 0;             // zero-latency dry (the immediate input tap)

    // Tail-end scalers (~5 ms ramps — plan.md §4 step 5).
    juce::LinearSmoothedValue<float> drySmoother;
    juce::LinearSmoothedValue<float> wetSmoother;
    juce::LinearSmoothedValue<float> gainSmoother;   // makeup gain, linear

    // WET = 0 shortcut state: when WET is 0 the STFT chain is skipped entirely
    // (output = dry x DRY); the engine state is reset when WET returns > 0 so
    // the wet path re-engages from a consistent (pristine) state.
    bool wetActive = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnowAudioProcessor)
};
