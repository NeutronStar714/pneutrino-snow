#pragma once

#include "PluginProcessor.h"
#include "SpectrumDisplay.h"

// ==============================================================================
// Custom rotary-slider look-and-feel — Pneutrino convention (same shape as the
// Snowfall/Dynamic Resonator editors), themed for Snow (#EAF6FF).
class SnowLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SnowLookAndFeel();

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override;

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override;

    juce::Font getTextButtonFont (juce::TextButton&, int) override
    {
        return juce::Font (juce::FontOptions ("Courier New", 20.0f, juce::Font::bold));
    }

private:
    juce::Colour accentColour;
    juce::Colour bgColour;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnowLookAndFeel)
};

// ==============================================================================
// The plugin's editor — header, four themed group boxes in a 3x4 grid
// (plan.md §2 UI layout), attribution footer:
//   STOCHASTIC MASK box: rows 1-2, columns 1-2 (2x2 knob grid)
//   SPACE box:           row  3,  columns 1-2 (PHASE, STEREO)
//   OUTPUT box:          rows 1-3, column 3   (GAIN, DRY, WET stacked)
//   VISUALIZATION panel: rows 1-3, column 4   (scrolling frequency display)
class SnowAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SnowAudioProcessorEditor (SnowAudioProcessor&);
    ~SnowAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    SnowAudioProcessor& processorRef;
    SnowLookAndFeel lnf;

    // --- MASK box (stochastic masking) ---
    juce::Slider amountKnob;
    juce::Slider widthKnob;
    juce::Slider cutoffKnob;
    juce::Slider gridKnob;
    juce::Label amountLbl;
    juce::Label widthLbl;
    juce::Label cutoffLbl;
    juce::Label gridLbl;

    // --- PHASE & SPACE box ---
    juce::Slider phaseKnob;
    juce::Slider stereoKnob;
    juce::Label phaseLbl;
    juce::Label stereoLbl;

    // --- OUTPUT box (makeup gain + dry/wet) ---
    juce::Slider gainKnob;
    juce::Slider dryKnob;
    juce::Slider wetKnob;
    juce::Label gainLbl;
    juce::Label dryLbl;
    juce::Label wetLbl;

    // --- Group headers ---
    juce::Label maskHeader;
    juce::Label spaceHeader;
    juce::Label outputHeader;

    // --- Visualization panel (rows 1-3, column 4) ---
    std::unique_ptr<SpectrumDisplay> displayPanel;

    // --- Group-box rectangles (computed in resized(), mirrored in paint) ---
    juce::Rectangle<int> maskRect, spaceRect, outputRect, displayRect;

    // --- Header / attribution ---
    juce::Label nameLabel;
    juce::Label attributionLabel;

    // --- Attachments ---
    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;

    std::unique_ptr<SliderAttach> amountAttach;
    std::unique_ptr<SliderAttach> widthAttach;
    std::unique_ptr<SliderAttach> cutoffAttach;
    std::unique_ptr<SliderAttach> gridAttach;
    std::unique_ptr<SliderAttach> phaseAttach;
    std::unique_ptr<SliderAttach> stereoAttach;
    std::unique_ptr<SliderAttach> gainAttach;
    std::unique_ptr<SliderAttach> dryAttach;
    std::unique_ptr<SliderAttach> wetAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SnowAudioProcessorEditor)
};
