#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "../DSP/SpectralMaskEngine.h"   // Snow::DSP::SpectrumSnapshot

// ==============================================================================
// Real-time scrolling frequency display (Harmor-style): the vertical axis is
// the spectrum — 20 Hz at the bottom to 20 kHz at the top, logarithmic, with
// labelled gridlines — and the horizontal axis is time flowing leftwards as
// new frames enter at the right edge. Each of the 1023 true FFT bins is drawn
// as a one-pixel horizontal line in a shade of the theme accent: the loudest
// bin of a column draws in the accent colour itself, fully-muted bins show the
// panel background gray, and in-between bins are brighter with level
// (per-column normalization, so the display is independent of the absolute
// input level — and of the plugin's Gain parameter, which only scales the wet
// path).
//
// The data is the engine's POST-MASK spectrum — the 1023 bins after the
// stochastic mask has been applied — published once per hop tick through the
// lock-free double-buffered SpectrumSnapshot (audio thread -> UI thread). The
// relative-loudness reference (per-column max) follows the loudest bin
// instantly and releases over ~500 ms, and each bin is shaded through a 3-stop
// gradient: accent #EAF6FF at full level, mask blue #5E7F92 at half, panel
// gray #2A2A2A when muted.
class SpectrumDisplay final : public juce::Component, private juce::Timer
{
public:
    SpectrumDisplay (std::shared_ptr<Snow::DSP::SpectrumSnapshot> snapshot,
                     juce::Colour accent, juce::Colour mid, juce::Colour background);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildGeometry();
    static juce::String labelForHz (double hz);
    juce::Colour shadeForLevel (float level) const;   // 0 -> background, 0.5 -> mid, 1 -> accent

    std::shared_ptr<Snow::DSP::SpectrumSnapshot> snapshot;
    juce::Colour accent;
    juce::Colour mid;           // gradient midpoint — #5E7F92 at 50 % level
    juce::Colour background;

    juce::Image history;        // scrolling plot image (plotW x plotH, ARGB)
    std::vector<int> binY;      // precomputed plot y per bin (index 1..1023)
    std::vector<float> binMag;  // scratch: per-bin combined L/R magnitude
    std::int64_t lastCount = 0; // last consumed publication count

    float peak = 0.0f;          // smoothed per-column max (instant attack, ~500 ms release)
    const float releaseCoeff;   // per-tick decay factor for the peak

    int plotX = 0;              // plot rect (label gutter on the left of the card,
    int plotY = 0;              // vertically compressed toward the centre)
    int plotW = 0;
    int plotH = 0;

    static constexpr double freqMin = 20.0;
    static constexpr double freqMax = 20000.0;
    static constexpr int labelGutter = 26;   // narrow: labels hug the card's left edge
    static constexpr int refreshHz = 60;          // timer cadence
    static constexpr double releaseSeconds = 0.5; // loudness-reference release (500 ms)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumDisplay)
};
