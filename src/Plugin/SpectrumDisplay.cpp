#include "SpectrumDisplay.h"

#include <cmath>

namespace
{
// Labelled gridlines on the log frequency axis (plan.md §2 UI layout):
// 20/50/100/200/500/1k/2k/5k/10k/20k Hz.
constexpr double labelHz[] = { 20.0, 50.0, 100.0, 200.0, 500.0,
                               1000.0, 2000.0, 5000.0, 10000.0, 20000.0 };

// Background card geometry: the rounded card sits inset inside the group-box
// frame, and the plot is vertically compressed toward the centre (a small
// margin above and below so the spectrum doesn't fill the whole panel).
constexpr int cardInset = 3;
constexpr int vertInset = 8;
constexpr float cardRadius = 10.0f;
}

// ==============================================================================
SpectrumDisplay::SpectrumDisplay (std::shared_ptr<Snow::DSP::SpectrumSnapshot> snap,
                                  juce::Colour accentColour, juce::Colour midColour,
                                  juce::Colour bgColour)
    : snapshot (std::move (snap)), accent (accentColour), mid (midColour),
      background (bgColour),
      releaseCoeff ((float) std::exp (-1.0 / (refreshHz * releaseSeconds)))
{
    // Transparent: the editor's group-box border (drawn behind this component)
    // must stay fully visible — only the rounded card is painted, floating on
    // the editor's dark background inside the frame.
    setOpaque (false);
    startTimerHz (refreshHz);
}

void SpectrumDisplay::resized()
{
    const auto card = getLocalBounds().reduced (cardInset);

    plotX = card.getX() + labelGutter;
    plotW = juce::jmax (1, card.getRight() - plotX - 6);
    plotH = juce::jmax (1, card.getHeight() - 2 * vertInset);
    plotY = card.getY() + (card.getHeight() - plotH) / 2;

    history = juce::Image (juce::Image::ARGB, plotW, plotH, true);
    history.clear (history.getBounds(), background);

    binY.clear();                       // rebuilt on the next frame
    binMag.assign ((size_t) 1024, 0.0f);
}

// ==============================================================================
void SpectrumDisplay::rebuildGeometry()
{
    if (snapshot == nullptr || snapshot->sampleRate <= 0.0 || history.isNull())
        return;

    binY.assign ((size_t) 1024, plotH - 1);
    const double logRange = std::log (freqMax / freqMin);
    const double binFreq = snapshot->sampleRate / (double) Snow::DSP::SpectralMaskEngine::fftSize;
    for (int b = 1; b <= 1023; ++b)
    {
        // Bins above 20 kHz (b > 853 @ 48 kHz) clamp to the top gridline.
        const double f = juce::jlimit (freqMin, freqMax, (double) b * binFreq);
        const double t = std::log (f / freqMin) / logRange;                 // 0 bottom .. 1 top
        binY[(size_t) b] = plotH - 1 - (int) (t * (double) (plotH - 1));
    }
}

// ==============================================================================
void SpectrumDisplay::timerCallback()
{
    if (snapshot == nullptr || snapshot->numBins == 0)
        return;

    if (binY.empty())
        rebuildGeometry();
    if (binY.empty() || history.isNull())
        return;

    const std::int64_t count = snapshot->publishCount.load (std::memory_order_acquire);
    if (count <= lastCount)
        return;
    lastCount = count;

    // Seqlock-style read: copy the published frame, then verify the engine did
    // not republish (and overwrite this buffer) mid-copy — if it did, skip the
    // frame; the next tick shows the fresh one.
    const auto& frame = snapshot->frames[(size_t) ((count - 1) & 1)];
    float colMax = 0.0f;
    for (int b = 1; b <= 1023; ++b)
    {
        const float mag = juce::jmax (frame[0][(size_t) b], frame[1][(size_t) b]);   // max(L, R) — mono falls back to L
        binMag[(size_t) b] = mag;
        colMax = juce::jmax (colMax, mag);
    }
    if (snapshot->publishCount.load (std::memory_order_acquire) != count)
        return;    // racing republish — try again on the next tick

    // Relative-loudness reference: snap instantly to a louder column, then
    // release exponentially over ~500 ms — the normalization doesn't shimmer
    // with every fluctuation of the audio.
    peak = juce::jmax (colMax, peak * releaseCoeff);
    if (peak <= 1e-9f)
        return;    // effectively silent: the column stays background gray

    // Scroll the history left by one pixel and paint the new column at the
    // right edge: each bin is a horizontal line on the 3-stop gradient — the
    // loudest bin of the column is the accent itself, 50 % level is the mask
    // blue, quieter bins dim toward the panel gray, full silence is gray.
    history.moveImageSection (0, 0, 1, 0, plotW - 1, plotH);
    const float invMax = 1.0f / peak;
    for (int b = 1; b <= 1023; ++b)
    {
        const float level = std::sqrt (binMag[(size_t) b] * invMax);   // perceptual-ish brightness curve
        history.setPixelAt (plotW - 1, binY[(size_t) b], shadeForLevel (level));
    }
    repaint();
}

// ==============================================================================
void SpectrumDisplay::paint (juce::Graphics& g)
{
    // Only the rounded card (inset inside the group-box frame, 10 px corners)
    // — the editor's border and dark background show through the rest.
    g.setColour (background);
    g.fillRoundedRectangle (getLocalBounds().reduced (cardInset).toFloat(), cardRadius);

    if (! history.isNull())
        g.drawImageAt (history, plotX, plotY);

    // Frequency gridlines + labels (20 Hz .. 20 kHz, logarithmic).
    const juce::Colour gridColour = background.brighter (0.35f);
    g.setColour (gridColour);
    g.setFont (juce::Font (juce::FontOptions ("Helvetica", 9.0f, juce::Font::plain)));

    const double logRange = std::log (freqMax / freqMin);
    for (double f : labelHz)
    {
        const double t = std::log (f / freqMin) / logRange;
        const int y = plotY + plotH - 1 - (int) (t * (double) (plotH - 1));
        g.drawLine ((float) plotX, (float) y, (float) (plotX + plotW - 1), (float) y);
        g.drawText (labelForHz (f), 2, y - 8, plotX - 6, 16, juce::Justification::centredRight);
    }
}

juce::Colour SpectrumDisplay::shadeForLevel (float level) const
{
    // 3-stop gradient: muted -> panel gray #2A2A2A, 50 % -> mask blue #5E7F92,
    // loudest -> accent #EAF6FF.
    if (level <= 0.5f)
        return background.interpolatedWith (mid, level * 2.0f);
    return mid.interpolatedWith (accent, (level - 0.5f) * 2.0f);
}

juce::String SpectrumDisplay::labelForHz (double hz)
{
    if (hz >= 1000.0)
    {
        const int wholeK = (int) (hz / 1000.0 + 0.5);
        return juce::String (wholeK) + "k";
    }
    return juce::String ((int) (hz + 0.5));
}
