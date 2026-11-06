#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace Snow::Params
{
// ==============================================================================
// All 9 parameters — types/ranges/defaults per plan.md §2 (IDs immutable).
// ==============================================================================

// Exponential Phase mapping (plan.md §2 entry 5): value = 90*(exp(k*u)-1)/
// (exp(k)-1), k = 4, u = normalized knob position — the knob starts at 0°,
// moves slowly at first and accelerates, reaching 90° exactly at the top.
constexpr double phaseK = 4.0;

juce::NormalisableRange<float> makePhaseRange()
{
    // JUCE 8 remap-function API: each function receives (rangeStart,
    // rangeEnd, value) and returns the mapped value.
    return juce::NormalisableRange<float> (
        0.0f, 90.0f,
        [] (float start, float end, float u)
        {
            return start + (end - start) * (float) ((std::exp (phaseK * (double) u) - 1.0) / (std::exp (phaseK) - 1.0));
        },
        [] (float start, float end, float v)
        {
            const float p = (v - start) / (end - start);
            return (float) (std::log (1.0 + (double) p * (std::exp (phaseK) - 1.0)) / phaseK);
        });
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    // --- Stochastic mask ---
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { amount, 1 }, "Amount",
        NormalisableRange<float> (0.0f, 99.0f, 0.1f), 50.0f,
        AudioParameterFloatAttributes {}.withLabel ("%")));

    // WIDTH stored in milliseconds (10-1000); skew 0.5 centres the dial
    // around ~100 ms so the musically interesting short dwells are easy to
    // reach on a rotary.
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { width, 1 }, "Mask Width",
        NormalisableRange<float> (10.0f, 1000.0f, 1.0f, 0.5f), 90.0f,
        AudioParameterFloatAttributes {}.withLabel ("ms")));

    // CUTOFF = randomized band size (plan.md §2, logarithmic) with the
    // inverse-of-Depth hard mute: 0 -> ONLY the lowest 16 bins (1..16) are
    // randomized and everything above is cancelled to silence, 0.5 -> lowest
    // 128 bins randomized + the rest muted, 1 -> all bins randomized, nothing
    // muted. Default 1.0 (all random, nothing muted).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { cutoff, 1 }, "Cutoff",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f,
        AudioParameterFloatAttributes {}.withLabel ("")));

    // GRID = mask smoothing grid, 1..16 bins (integer). 1 = raw per-bin dice
    // (the harsh binary gating); 8+ = smooth band-correlated spectral envelope
    // (cosine-interpolated controls) — the "pure" Harmor-like texture.
    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { grid, 1 }, "Grid Size", 1, 16, 8));

    // --- Spectral phase / space ---
    // PHASE = masking-character angle, 0..90 degrees mapped EXPONENTIALLY
    // across the knob travel (plan.md §2 entry 5, makePhaseRange): slow at
    // the bottom of the travel, accelerating to the +3 dB orthogonal boost
    // exactly at the top. Default 0° (pure cancellation).
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { phase, 1 }, "Phase Offset",
        makePhaseRange(), 0.0f,
        AudioParameterFloatAttributes {}.withLabel ("deg")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { stereo, 1 }, "Stereo Decouple",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.7f,
        AudioParameterFloatAttributes {}.withLabel ("")));

    // --- Output ---
    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { gain, 1 }, "Makeup Gain",
        NormalisableRange<float> (0.0f, 12.0f, 0.1f), 6.0f,
        AudioParameterFloatAttributes {}.withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { dry, 1 }, "Dry",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f,
        AudioParameterFloatAttributes {}.withLabel ("")));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { wet, 1 }, "Wet",
        NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f,
        AudioParameterFloatAttributes {}.withLabel ("")));

    return layout;
}

} // namespace Snow::Params

// ==============================================================================
SnowAudioProcessor::SnowAudioProcessor()
    : AudioProcessor (BusesProperties()
                      .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                      .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , apvts (*this, nullptr, "SnowParams", Snow::Params::createParameterLayout())
{
}

SnowAudioProcessor::~SnowAudioProcessor() = default;

// ==============================================================================
const juce::String SnowAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SnowAudioProcessor::acceptsMidi() const  { return false; }
bool SnowAudioProcessor::producesMidi() const { return false; }
bool SnowAudioProcessor::isMidiEffect() const { return false; }

double SnowAudioProcessor::getTailLengthSeconds() const
{
    // The effect has no feedback; the tail is the wet delay line plus the
    // slew tail of the masking state (well under this bound).
    return juce::jmax (0.25, (double) maxWetLatencySamples / 44100.0);
}

// ==============================================================================
int SnowAudioProcessor::getNumPrograms()       { return 1; }
int SnowAudioProcessor::getCurrentProgram()    { return 0; }
void SnowAudioProcessor::setCurrentProgram(int) {}
const juce::String SnowAudioProcessor::getProgramName(int) { return "Default"; }
void SnowAudioProcessor::changeProgramName(int, const juce::String&) {}

// ==============================================================================
void SnowAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Zero-latency contract: the wet path is inherently delayed (1536 aligned
    // / 2048 lagged — SpectralMaskEngine::process), but the plugin reports 0
    // ms: the dry path is the immediate input tap and the late wet is blended
    // in via the fade-through crossfade (live-monitoring friendly).
    setLatencySamples (0);
    dryDelay = 0;

    // Size the DSP chain here — zero allocations on the audio thread.
    const int numChannels = juce::jmax (1, getTotalNumOutputChannels());

    // Visualization hook: the editor's display reads the post-mask spectrum
    // through this snapshot (lock-free double buffer); attach before
    // prepare so the buffers are sized with the engine.
    maskEngine.setSpectrumSnapshot (spectrumSnapshot);
    maskEngine.prepare (sampleRate, numChannels);

    for (auto& ring : dryRing)
        ring.assign ((size_t) maxWetLatencySamples, 0.0f);
    dryWrite = 0;

    // Tail-end scalers (~5 ms ramps per plan.md §4 step 5).
    drySmoother.reset (sampleRate, 0.005);
    wetSmoother.reset (sampleRate, 0.005);
    gainSmoother.reset (sampleRate, 0.005);
    drySmoother.setTargetValue (apvts.getRawParameterValue (Snow::Params::dry)->load());
    wetSmoother.setTargetValue (apvts.getRawParameterValue (Snow::Params::wet)->load());
    gainSmoother.setTargetValue (juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue (Snow::Params::gain)->load()));

    wetActive = false;
}

void SnowAudioProcessor::releaseResources()
{
    drySmoother.setCurrentAndTargetValue (drySmoother.getTargetValue());
    wetSmoother.setCurrentAndTargetValue (wetSmoother.getTargetValue());
    gainSmoother.setCurrentAndTargetValue (gainSmoother.getTargetValue());
}

bool SnowAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Effect: single input bus, single output bus; mono or stereo (in == out).
    auto in  = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

// ==============================================================================
void SnowAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear();

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples == 0)
        return;

    // Parameter reads: atomic loads on the audio thread; DRY/WET/GAIN smoothed
    // with ~5 ms ramps (plan.md §4 step 5).
    const float amount01 = apvts.getRawParameterValue (Snow::Params::amount)->load() * 0.01f;
    const float widthMs   = apvts.getRawParameterValue (Snow::Params::width)->load();
    const float cutoff01  = apvts.getRawParameterValue (Snow::Params::cutoff)->load();
    const int   gridVal   = (int) apvts.getRawParameterValue (Snow::Params::grid)->load();
    const float phaseDeg  = apvts.getRawParameterValue (Snow::Params::phase)->load();
    const float stereo01  = apvts.getRawParameterValue (Snow::Params::stereo)->load();
    const float gainDb    = apvts.getRawParameterValue (Snow::Params::gain)->load();
    const float dryVal    = apvts.getRawParameterValue (Snow::Params::dry)->load();
    const float wetVal    = apvts.getRawParameterValue (Snow::Params::wet)->load();

    drySmoother.setTargetValue (dryVal);
    wetSmoother.setTargetValue (wetVal);
    gainSmoother.setTargetValue (juce::Decibels::decibelsToGain (gainDb));

    // 1. Dry tap — MUST be captured before the STFT consumes/mutates the block
    //    (plan.md §3 step 7 / §4 step 5). Pushes the raw input into the dry
    //    delay ring (sized maxWetLatencySamples; the read lags dryDelay).
    const int channels = juce::jmin (numChannels, (int) dryRing.size());
    for (int c = 0; c < channels; ++c)
    {
        const float* src = buffer.getReadPointer (c);
        float* ring = dryRing[(size_t) c].data();
        int w = dryWrite;
        for (int n = 0; n < numSamples; ++n)
        {
            ring[w] = src[n];
            w = (w + 1 == maxWetLatencySamples) ? 0 : w + 1;
        }
    }
    dryWrite = (dryWrite + numSamples) % maxWetLatencySamples;

    // 2. STFT (wet path). WET = 0 shortcut: skip the whole STFT chain — the
    //    wet contribution is silent (the block is zeroed; it holds the raw
    //    input until here). The engine state is reset when WET returns > 0 so
    //    the wet path resumes from a consistent state (the first 1536 wet
    //    samples are the natural ring-fill transient).
    if (wetVal > 0.0f)
    {
        if (! wetActive)
        {
            maskEngine.reset();
            wetActive = true;
        }

        // Parameter push (atomic loads -> plain floats, same audio thread as
        // the frame loop).
        maskEngine.setMaskParameters (
            amount01,
            widthMs * 0.001f,          // ms -> s
            cutoff01,
            phaseDeg,
            stereo01);
        maskEngine.setGridSize (gridVal);

        const float* inputChannels[Snow::DSP::SpectralMaskEngine::maxChannels]
            = { buffer.getReadPointer (0), numChannels > 1 ? buffer.getReadPointer (1) : nullptr };
        float* outputChannels[Snow::DSP::SpectralMaskEngine::maxChannels]
            = { buffer.getWritePointer (0), numChannels > 1 ? buffer.getWritePointer (1) : nullptr };

        maskEngine.process (inputChannels, outputChannels, numChannels, numSamples);
    }
    else
    {
        wetActive = false;
        buffer.clear();              // no wet contribution; block holds raw input
    }

    // 3. Tail-end fade-through crossfade (plan.md §3 step 7 / §4 step 5):
    //    out = DRY*dip * input + WET*dip * makeupGain * wetSignal
    //    DRY/WET/GAIN ramp over ~5 ms. The dry read is the immediate input
    //    tap (dryDelay = 0) — truly zero-latency dry.
    int readIdx[Snow::DSP::SpectralMaskEngine::maxChannels];
    for (int c = 0; c < channels; ++c)
    {
        readIdx[c] = dryWrite - dryDelay - numSamples;
        while (readIdx[c] < 0)
            readIdx[c] += maxWetLatencySamples;
    }

    for (int n = 0; n < numSamples; ++n)
    {
        const float dryGain = drySmoother.getNextValue();
        const float wetGain = wetSmoother.getNextValue();
        const float makeup  = gainSmoother.getNextValue();

        // Fade-through crossfade between the zero-latency dry and the delayed
        // wet: at the blend point k = WET/(DRY+WET) an equal-power dip
        // 1 - 0.5*sin(pi*k) is applied to both sides, masking the comb/echo
        // of the misaligned middle blend. Pure dry (k=0) and pure wet (k=1)
        // are unaffected.
        const float sum = dryGain + wetGain;
        const float k = sum > 0.0f ? wetGain / sum : 1.0f;
        const float dip = 1.0f - 0.5f * std::sin (juce::MathConstants<float>::pi * k);
        const float dryScale = dryGain * dip;
        const float wetScale = wetGain * dip;

        for (int c = 0; c < channels; ++c)
        {
            const float drySample = dryRing[(size_t) c][(size_t) readIdx[c]];
            buffer.setSample (c, n, dryScale * drySample
                                     + wetScale * makeup * buffer.getSample (c, n));
            readIdx[c] = (readIdx[c] + 1 == maxWetLatencySamples) ? 0 : readIdx[c] + 1;
        }
    }
}

// ==============================================================================
bool SnowAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* SnowAudioProcessor::createEditor()
{
    return new SnowAudioProcessorEditor (*this);
}

// ==============================================================================
void SnowAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void SnowAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto tree = juce::ValueTree::fromXml (*xml);
        if (tree.isValid())
            apvts.replaceState (tree);
    }
}

// ==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SnowAudioProcessor();
}
