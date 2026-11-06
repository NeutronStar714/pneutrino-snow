#include "PluginEditor.h"

// ==============================================================================
// Colour constants — Pneutrino dark theme, Snow accent #EAF6FF (ice white;
// the group-box borders walk the accent ramp deep -> theme, mirroring the
// Snowfall/Dynamic Resonator accent-shade boxes).
static const juce::Colour accentColour { 0xFFEAF6FF };
static const juce::Colour nameColour   { 0xFFF7FCFF };   // accent.brighter — header tint
static const juce::Colour darkBg       { 0xFF1E1E1E };
static const juce::Colour panelBg      { 0xFF2A2A2A };
static const juce::Colour textColour   { 0xFFE0E0E0 };
static const juce::Colour muteText     { 0xFF888888 };

// Group-box border colours (accent-shade ramp: MASK deepest -> OUTPUT -> VIS theme).
static const juce::Colour maskBoxColour    { 0xFF5E7F92 };
static const juce::Colour phaseBoxColour   { 0xFF86A8BB };
static const juce::Colour outputBoxColour  { 0xFFB4C8D5 };   // OUTPUT box — distinct stroke
static const juce::Colour vizBoxColour     { 0xFFEAF6FF };   // visualization panel — the theme

// ==============================================================================
// SnowLookAndFeel
// ==============================================================================
SnowLookAndFeel::SnowLookAndFeel()
    : LookAndFeel_V4(),
      accentColour (::accentColour),
      bgColour     (panelBg)
{
    setColour (juce::Slider::rotarySliderFillColourId,     accentColour);
    setColour (juce::Slider::rotarySliderOutlineColourId,  muteText);
    setColour (juce::Slider::textBoxTextColourId,          textColour);
    setColour (juce::Slider::textBoxBackgroundColourId,    darkBg);
    setColour (juce::Slider::textBoxOutlineColourId,       darkBg);
    setColour (juce::Label::textColourId,                  textColour);
}

void SnowLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y,
                                        int width, int height,
                                        float sliderPos,
                                        float rotaryStartAngle,
                                        float rotaryEndAngle,
                                        juce::Slider& /*s*/)
{
    const float diameter = float (juce::jmin (width, height)) - 8.0f;
    const float cx       = float (x) + float (width)  * 0.5f;
    const float cy       = float (y) + float (height) * 0.5f;
    const float radius   = diameter * 0.5f * 0.75f;
    const float lineW    = radius * 0.17f;
    const float borderW  = lineW * 0.5f;

    g.setColour (bgColour);
    g.fillEllipse (cx - radius, cy - radius, diameter * 0.75f, diameter * 0.75f);

    g.setColour (muteText);
    g.drawEllipse (cx - radius, cy - radius, diameter * 0.75f, diameter * 0.75f, borderW);

    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    juce::Path arc;
    arc.addCentredArc (cx, cy, radius - borderW, radius - borderW,
                       0.0f, rotaryStartAngle, angle, true);
    g.setColour (accentColour);
    g.strokePath (arc, juce::PathStrokeType (borderW, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::butt));

    juce::Path tick;
    tick.startNewSubPath (cx + radius * 0.25f * std::sin (angle),
                          cy - radius * 0.25f * std::cos (angle));
    tick.lineTo (cx + (radius - borderW - 1.0f) * std::sin (angle),
                 cy - (radius - borderW - 1.0f) * std::cos (angle));
    const float tickW = lineW * 0.9f;
    g.setColour (accentColour);
    g.strokePath (tick, juce::PathStrokeType (tickW, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::butt));
}

void SnowLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y,
                                        int width, int height,
                                        float sliderPos,
                                        float /*minSliderPos*/,
                                        float /*maxSliderPos*/,
                                        const juce::Slider::SliderStyle /*style*/,
                                        juce::Slider& /*slider*/)
{
    const float trackW = std::max (3.0f, float (width) * 0.15f);
    const float trackX = float (x) + (float (width) - trackW) * 0.5f;

    // Above thumb -> panel
    g.setColour (bgColour);
    g.fillRoundedRectangle (trackX, float (y), trackW, std::max (0.0f, sliderPos - float (y)), trackW * 0.5f);

    // Below thumb -> accent
    g.setColour (accentColour);
    g.fillRoundedRectangle (trackX, sliderPos, trackW, std::max (0.0f, float (y + height) - sliderPos), trackW * 0.5f);

    // Thumb: horizontal rectangle (50 % narrower)
    const float thumbH = 8.0f;
    const float thumbW = (float (width) - 4.0f) * 0.5f;
    const float thumbX = float (x) + (float (width) - thumbW) * 0.5f;
    const float thumbY = sliderPos - thumbH * 0.5f;
    g.setColour (accentColour.brighter (0.25f));
    g.fillRoundedRectangle (thumbX, thumbY, thumbW, thumbH, 2.0f);
}

// ==============================================================================
// SnowAudioProcessorEditor — constructor
// ==============================================================================
SnowAudioProcessorEditor::SnowAudioProcessorEditor (SnowAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p)
{
    // --- Header ---
    nameLabel.setText ("Snow", juce::dontSendNotification);
    nameLabel.setFont (juce::Font (juce::FontOptions ("Courier New", 30.0f, juce::Font::bold)));
    nameLabel.setColour (juce::Label::textColourId, nameColour);
    nameLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (nameLabel);

    attributionLabel.setText ("from Pneutrino by NeutronStar714", juce::dontSendNotification);
    attributionLabel.setFont (juce::Font (juce::FontOptions ("Courier New", 16.0f, juce::Font::plain)));
    attributionLabel.setColour (juce::Label::textColourId, accentColour);
    attributionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (attributionLabel);

    // --- Group headers ---
    auto setupHeader = [&](juce::Label& header, const juce::String& text)
    {
        header.setText (text, juce::dontSendNotification);
        header.setFont (juce::Font (juce::FontOptions ("Helvetica", 12.0f, juce::Font::bold)));
        header.setColour (juce::Label::textColourId, textColour);
        header.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (header);
    };
    setupHeader (maskHeader,   "STOCHASTIC MASK");
    setupHeader (spaceHeader,  "SPACE");
    setupHeader (outputHeader, "OUTPUT");

    // --- Helper: set up one knob + label (Pneutrino convention) ---
    auto setupKnob = [&](juce::Slider& knob, juce::Label& label,
                          const juce::String& text, double defaultValue,
                          double rangeMin, double rangeMax, double interval)
    {
        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setRange (rangeMin, rangeMax, interval);
        knob.setValue (defaultValue, juce::dontSendNotification);
        knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, 16);
        knob.setDoubleClickReturnValue (true, defaultValue);
        knob.setLookAndFeel (&lnf);
        addAndMakeVisible (knob);

        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions ("Helvetica", 10.0f, juce::Font::bold)));
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, muteText);
        addAndMakeVisible (label);
    };

    auto& apvts = processorRef.getAPVTS();

    // --- MASK box: AMOUNT (%), WIDTH (ms), CUTOFF (0..1), GRID (1..16 bins) ---
    // Parameter ranges per plan.md §2.
    setupKnob (amountKnob, amountLbl, "AMOUNT", 50.0,   0.0, 99.0, 0.1);
    setupKnob (widthKnob,  widthLbl,  "WIDTH",  90.0,   10.0, 1000.0, 1.0);
    setupKnob (cutoffKnob,  cutoffLbl,  "CUTOFF", 1.0,    0.0, 1.0, 0.001);
    setupKnob (gridKnob,   gridLbl,   "GRID",   8.0,    1.0, 16.0, 1.0);

    // --- PHASE & SPACE box: PHASE (deg), STEREO (0..1) ---
    setupKnob (phaseKnob,  phaseLbl,  "PHASE",  0.0,    0.0, 90.0, 0.1);
    setupKnob (stereoKnob, stereoLbl, "STEREO", 0.7,    0.0, 1.0, 0.001);

    // --- OUTPUT box: GAIN (dB), DRY (0..1), WET (0..1) ---
    setupKnob (gainKnob,   gainLbl,   "GAIN",   6.0,    0.0, 12.0, 0.1);
    setupKnob (dryKnob,    dryLbl,    "DRY",    0.0,    0.0, 1.0, 0.001);
    setupKnob (wetKnob,    wetLbl,    "WET",    1.0,    0.0, 1.0, 0.001);

    // --- Attachments ---
    amountAttach = std::make_unique<SliderAttach> (apvts, Snow::Params::amount, amountKnob);
    widthAttach  = std::make_unique<SliderAttach> (apvts, Snow::Params::width,  widthKnob);
    cutoffAttach = std::make_unique<SliderAttach> (apvts, Snow::Params::cutoff, cutoffKnob);
    gridAttach   = std::make_unique<SliderAttach> (apvts, Snow::Params::grid,   gridKnob);
    phaseAttach  = std::make_unique<SliderAttach> (apvts, Snow::Params::phase,  phaseKnob);
    stereoAttach = std::make_unique<SliderAttach> (apvts, Snow::Params::stereo, stereoKnob);
    gainAttach   = std::make_unique<SliderAttach> (apvts, Snow::Params::gain,   gainKnob);
    dryAttach    = std::make_unique<SliderAttach> (apvts, Snow::Params::dry,    dryKnob);
    wetAttach    = std::make_unique<SliderAttach> (apvts, Snow::Params::wet,    wetKnob);

    // Nicer readouts: WIDTH in whole ms, GRID as a whole number of bins,
    // CUTOFF/STEREO/DRY/WET as 2-decimal fractions.
    widthKnob.textFromValueFunction = [] (double val) { return juce::String (int (val + 0.5)); };
    widthKnob.updateText();
    gridKnob.textFromValueFunction = [] (double val) { return juce::String (int (val + 0.5)); };
    gridKnob.updateText();

    // --- Visualization panel (rows 1-3, column 4): Harmor-style scrolling
    //     frequency display fed by the engine's post-mask spectrum snapshot ---
    displayPanel = std::make_unique<SpectrumDisplay> (processorRef.getSpectrumSnapshot(), accentColour, maskBoxColour, panelBg);
    addAndMakeVisible (*displayPanel);

    setSize (442, 394);
    setResizable (false, false);
}

SnowAudioProcessorEditor::~SnowAudioProcessorEditor()
{
    amountKnob.setLookAndFeel  (nullptr);
    widthKnob.setLookAndFeel   (nullptr);
    cutoffKnob.setLookAndFeel  (nullptr);
    gridKnob.setLookAndFeel    (nullptr);
    phaseKnob.setLookAndFeel   (nullptr);
    stereoKnob.setLookAndFeel  (nullptr);
    gainKnob.setLookAndFeel    (nullptr);
    dryKnob.setLookAndFeel     (nullptr);
    wetKnob.setLookAndFeel     (nullptr);
}

// ==============================================================================
void SnowAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (darkBg);

    // Group boxes — rounded rectangles around each panel; the rects are the
    // 3x4 grid computed in resized() and mirrored here exactly.
    const float margin = 7.0f;
    const float radius = 10.0f;
    const float borderW = 2.5f;

    g.setColour (maskBoxColour);
    g.drawRoundedRectangle (maskRect.toFloat().reduced (margin), radius, borderW);
    g.setColour (phaseBoxColour);
    g.drawRoundedRectangle (spaceRect.toFloat().reduced (margin), radius, borderW);
    g.setColour (outputBoxColour);
    g.drawRoundedRectangle (outputRect.toFloat().reduced (margin), radius, borderW);
    g.setColour (vizBoxColour);
    g.drawRoundedRectangle (displayRect.toFloat().reduced (margin), radius, borderW);
}

void SnowAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // === Header ===
    nameLabel.setBounds (area.removeFromTop (24).withY (8).reduced (0, 4));

    // === Attribution (fixed footer below the grid) ===
    attributionLabel.setBounds (juce::Rectangle<int> (12, getHeight() - 24, getWidth() - 24, 18).reduced (4, 0));

    // === 3x4 grid (plan.md §2 UI layout) ===
    //   Stochastic Mask box: rows 1-2, columns 1-2 (2x2 knob grid)
    //   Space box:           row  3,  columns 1-2 (PHASE, STEREO)
    //   Output box:          rows 1-3, column 3   (GAIN, DRY, WET stacked)
    //   Visualization panel: rows 1-3, column 4   (scrolling frequency display)
    //
    // Fixed geometry driven by the 48 px knobs: every knob control is exactly
    // 72x72 (the rotary is min(w,h)-8 scaled by 0.75 = 48 px), every box
    // edge lies on the shared grid lines, and every gap between boxes is the
    // same 4 px. The visualization panel is as wide as the left group, so the
    // OUTPUT box lands dead-centre of the window, directly below the header.
    constexpr int gap    = 4;
    constexpr int margin = 7;
    constexpr int header = 20;
    constexpr int knob   = 72;                         // control size -> 48 px visual rotary
    constexpr int cellH  = knob + 14;                  // knob cell + label strip

    constexpr int gridTop = 34;
    constexpr int maskW   = 2 * knob + gap + 2 * margin;         // 166
    constexpr int outputW = knob + 2 * margin;                   // 86
    constexpr int vizW    = maskW;                               // 166 — mirrors the left group
    constexpr int maskH   = header + 2 * cellH + 2 * margin;     // 206 (rows 1-2)
    constexpr int spaceH  = header + cellH + 2 * margin;         // 120 (row 3)

    maskRect = { 12, gridTop, maskW, maskH };
    spaceRect = { 12, gridTop + maskH + gap, maskW, spaceH };
    outputRect = { 12 + maskW + gap, gridTop, outputW, maskH + gap + spaceH };
    displayRect = { 12 + maskW + gap + outputW + gap, gridTop, vizW, maskH + gap + spaceH };

    // Shared knob placer: control normalized to exactly 72x72 (48 px visual
    // rotary), label strip below. labelLift shifts the label up relative to
    // the cell bottom, to follow knobs that were nudged upward by hand.
    auto placeKnob = [&] (juce::Slider& knobCtl, juce::Label& lbl, juce::Rectangle<int> cell, int labelLift = 0)
    {
        auto content = cell.withTrimmedBottom (14);
        knobCtl.setBounds (content.withSizeKeepingCentre (juce::jmin (72, content.getWidth()),
                                                          juce::jmin (72, content.getHeight())));
        lbl.setBounds (cell.withTop (cell.getBottom() - 14 - labelLift).withHeight (14).reduced (4, 0));
    };

    // === Stochastic Mask box: header + 2x2 knob grid ===
    maskHeader.setBounds (maskRect.reduced (margin, 0).withHeight (header+20));
    auto maskArea = maskRect.reduced (margin).withTrimmedTop (header);
    const int maskCellW = maskArea.getWidth() / 2;
    const int maskCellH = maskArea.getHeight() / 2;
    auto maskCell = [&] (int col, int row)
    {
        return maskArea.withX (maskArea.getX() + col * maskCellW)
                       .withY (maskArea.getY() + row * maskCellH * 0.95)
                       .withWidth (maskCellW)
                       .withHeight (maskCellH);
    };
    placeKnob (amountKnob, amountLbl, maskCell (0, 0));
    placeKnob (widthKnob,  widthLbl,  maskCell (1, 0));
    placeKnob (cutoffKnob, cutoffLbl, maskCell (0, 1));
    placeKnob (gridKnob,   gridLbl,   maskCell (1, 1));

    // === Space box: header + PHASE, STEREO side by side ===
    spaceHeader.setBounds (spaceRect.reduced (margin, 0).withHeight (header+20));
    auto spaceArea = spaceRect.reduced (margin).withTrimmedTop (header - 10);
    const int spaceCellW = spaceArea.getWidth() / 2;
    auto spaceCell = [&] (int col)
    {
        return spaceArea.withX (spaceArea.getX() + col * spaceCellW)
                        .withWidth (spaceCellW)
                        .withHeight (spaceArea.getHeight());
    };
    placeKnob (phaseKnob,  phaseLbl,  spaceCell (0), 4);
    placeKnob (stereoKnob, stereoLbl, spaceCell (1), 4);

    // === Output box: header + GAIN / DRY / WET stacked ===
    outputHeader.setBounds (outputRect.reduced (margin, 0).withHeight (header+20));
    auto outputArea = outputRect.reduced (margin).withTrimmedTop (header);
    const int outCellH = outputArea.getHeight() / 3;
    auto outCell = [&] (int row)
    {
        return outputArea.withY (outputArea.getY() - 6 + row * outCellH * 1.02)
                         .withWidth (outputArea.getWidth())
                         .withHeight (outCellH);
    };
    placeKnob (gainKnob, gainLbl, outCell (0), 6);
    placeKnob (dryKnob,  dryLbl,  outCell (1), 6);
    placeKnob (wetKnob,  wetLbl,  outCell (2), 6);

    // === Visualization panel ===
    displayPanel->setBounds (displayRect.reduced (margin));
}
