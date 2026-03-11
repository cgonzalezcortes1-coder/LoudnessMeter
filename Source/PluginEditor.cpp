#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
LoudnessMeterAudioProcessorEditor::LoudnessMeterAudioProcessorEditor (LoudnessMeterAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    // --- Loudness Meter section ---
    addAndMakeVisible (actionButton);
    actionButton.onClick = [this]
    {
        if (currentState == State::Measuring)
        {
            measureDurationMs = juce::Time::currentTimeMillis() - measureStartMs;
            audioProcessor.stopMeasurement();
        }
        else
        {
            measureStartMs = juce::Time::currentTimeMillis();
            audioProcessor.startMeasurement();
        }
    };

    // --- Compressor section ---
    presetLabel.setText ("Preset", juce::dontSendNotification);
    presetLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (presetLabel);

    populatePresetSelector();
    presetSelector.onChange = [this]()
    {
        int index = presetSelector.getSelectedId() - 1;
        if (index >= 0)
            audioProcessor.loadPreset (index);
    };
    addAndMakeVisible (presetSelector);

    savePresetButton.onClick = [this]()
    {
        auto dlg = std::make_shared<juce::AlertWindow> (
            "Save Preset", "Enter a name for the preset:", juce::MessageBoxIconType::QuestionIcon);
        dlg->addTextEditor ("name", "My Preset");
        dlg->addButton ("Save",   1);
        dlg->addButton ("Cancel", 0);

        auto* dlgPtr = dlg.get();
        dlg->enterModalState (true,
            juce::ModalCallbackFunction::create ([this, dlg, dlgPtr](int result)
            {
                if (result == 1)
                {
                    auto name = dlgPtr->getTextEditorContents ("name");
                    if (name.isNotEmpty())
                    {
                        audioProcessor.saveCurrentAsPreset (name);
                        populatePresetSelector();
                        presetSelector.setSelectedId (audioProcessor.getPresets().size());
                    }
                }
            }),
            true);
    };
    addAndMakeVisible (savePresetButton);

    setupCompressorSection (compA, "compA_", "COMP A");
    setupCompressorSection (compB, "compB_", "COMP B");
    setupCompressorSection (compC, "compC_", "COMP C");

    setupSlider (makeupGainSlider, makeupGainLabel, "Makeup Gain", " dB");
    makeupGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, "makeupGain", makeupGainSlider);

    setSize (kCompressorWidth + kMeterWidth, kPluginHeight);
    startTimerHz (20);
}

LoudnessMeterAudioProcessorEditor::~LoudnessMeterAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void LoudnessMeterAudioProcessorEditor::timerCallback()
{
    const auto newState = (State) audioProcessor.measurementState.load();

    if (newState != currentState)
    {
        currentState = newState;
        actionButton.setButtonText (newState == State::Measuring ? "Stop Measurement"
                                                                 : "Start Measurement");

        if (newState == State::Done)
        {
            momentary    = audioProcessor.loudnessMomentary .load();
            shortTerm    = audioProcessor.loudnessShortTerm .load();
            integrated   = audioProcessor.loudnessIntegrated.load();
            lra          = audioProcessor.loudnessRange      .load();
            truePeakDB   = audioProcessor.truePeak           .load();
            displayLevel = 0.0f;
        }

        repaint();
    }

    if (currentState == State::Measuring)
    {
        const float newPeak = audioProcessor.currentPeakLevel.load();
        displayLevel = (newPeak > displayLevel) ? newPeak : displayLevel * 0.85f;
        repaint();
    }
}

//==============================================================================
void LoudnessMeterAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Left panel background (Loudness Meter)
    g.fillAll (juce::Colour (0xFF1C1C1E));

    // Right panel background (Compressor)
    const auto rightPanel = juce::Rectangle<int> (kMeterWidth, 0, kCompressorWidth, kPluginHeight);
    g.setColour (juce::Colour (0xFF1A1A2E));
    g.fillRect (rightPanel);

    // Vertical separator
    g.setColour (juce::Colour (0xFF333355));
    g.drawVerticalLine (kMeterWidth, 0.0f, (float) kPluginHeight);

    // Right panel title
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("HASAN COMPRESSOR",
                juce::Rectangle<int> (kMeterWidth, 0, kCompressorWidth, 36),
                juce::Justification::centred);

    // Right panel section dividers
    auto drawDivider = [&](int y)
    {
        g.setColour (juce::Colour (0xFF333355));
        g.drawHorizontalLine (y, (float) (kMeterWidth + 10), (float) (kMeterWidth + kCompressorWidth - 10));
    };
    drawDivider (228);
    drawDivider (388);

    // Left panel: Loudness Meter content
    paintLoudnessMeter (g, juce::Rectangle<int> (0, 0, kMeterWidth, kPluginHeight));
}

void LoudnessMeterAudioProcessorEditor::resized()
{
    // --- Right panel: Compressor controls ---
    auto leftArea = juce::Rectangle<int> (kMeterWidth, 0, kCompressorWidth, kPluginHeight).reduced (10);
    leftArea.removeFromTop (30);  // title

    auto presetRow = leftArea.removeFromTop (28);
    presetLabel.setBounds (presetRow.removeFromLeft (50));
    savePresetButton.setBounds (presetRow.removeFromRight (50));
    presetRow.removeFromRight (5);
    presetSelector.setBounds (presetRow);

    leftArea.removeFromTop (10);

    layoutCompressorSection (compA, leftArea);
    layoutCompressorSection (compB, leftArea);
    layoutCompressorSection (compC, leftArea);

    auto gainRow = leftArea.removeFromTop (28);
    makeupGainLabel.setBounds (gainRow.removeFromLeft (90));
    makeupGainSlider.setBounds (gainRow);

    // --- Left panel: Loudness Meter button ---
    constexpr int btnW = 180, btnH = 32;
    actionButton.setBounds ((kMeterWidth - btnW) / 2,
                             kPluginHeight - btnH - 14,
                             btnW, btnH);
}

//==============================================================================
void LoudnessMeterAudioProcessorEditor::paintLoudnessMeter (juce::Graphics& g,
                                                             juce::Rectangle<int> panel) const
{
    const int x = panel.getX();
    const int w = panel.getWidth();

    // Title
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("LOUDNESS METER", x, 0, w, 26, juce::Justification::centred);

    // Separator
    g.setColour (juce::Colour (0xFF3A3A3C));
    g.drawHorizontalLine (26, (float) (x + 16), (float) (x + w - 16));

    const auto contentArea = juce::Rectangle<int> (x, 30, w, kPluginHeight - 80);

    switch (currentState)
    {
        case State::Idle:      paintIdle      (g, contentArea); break;
        case State::Measuring: paintMeasuring (g, contentArea); break;
        case State::Done:      paintDone      (g, contentArea); break;
    }
}

void LoudnessMeterAudioProcessorEditor::paintIdle (juce::Graphics& g,
                                                    juce::Rectangle<int> area) const
{
    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Ready to measure.", area.withTrimmedBottom (40), juce::Justification::centred);

    g.setFont (juce::FontOptions (11.0f));
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawText ("Rewind to the start before pressing Start.",
                area.withTrimmedTop (area.getHeight() / 2),
                juce::Justification::centred);
}

void LoudnessMeterAudioProcessorEditor::paintMeasuring (juce::Graphics& g,
                                                         juce::Rectangle<int> area) const
{
    const juce::int64 elapsedMs = juce::Time::currentTimeMillis() - measureStartMs;
    const int ax = area.getX();
    const int aw = area.getWidth();

    g.setColour (juce::Colour (0xFFFF453A).withAlpha (0.85f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("● MEASURING", ax, area.getY() + 20, aw, 14, juce::Justification::centred);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (44.0f));
    g.drawText (formatElapsed (elapsedMs), ax, area.getY() + 40, aw, 56, juce::Justification::centred);

    const int barX = ax + 24;
    const int barY = area.getY() + 112;
    const int barW = aw - 48;
    const int barH = 10;

    g.setColour (juce::Colour (0xFF2C2C2E));
    g.fillRoundedRectangle ((float) barX, (float) barY, (float) barW, (float) barH, 5.0f);

    const float dbFS  = (displayLevel > 0.0f) ? 20.0f * std::log10 (displayLevel) : -100.0f;
    const float ratio = juce::jlimit (0.0f, 1.0f, (dbFS + 60.0f) / 60.0f);

    if (ratio > 0.0f)
    {
        const auto barColour = dbFS > -1.0f ? juce::Colour (0xFFFF453A)
                             : dbFS > -6.0f ? juce::Colour (0xFFFFD60A)
                                            : juce::Colour (0xFF30D158);
        g.setColour (barColour);
        g.fillRoundedRectangle ((float) barX, (float) barY,
                                ratio * (float) barW, (float) barH, 5.0f);
    }

    const bool hasSignal = displayLevel > 0.001f;
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (hasSignal ? juce::Colours::white.withAlpha (0.35f)
                           : juce::Colour (0xFFFF453A).withAlpha (0.7f));
    g.drawText (hasSignal ? "signal detected" : "no signal — check routing",
                barX, barY + barH + 5, barW, 13, juce::Justification::centred);
}

void LoudnessMeterAudioProcessorEditor::paintDone (juce::Graphics& g,
                                                    juce::Rectangle<int> area) const
{
    const int ax = area.getX();
    const int aw = area.getWidth();
    int y = area.getY() + 12;

    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("INTEGRATED LOUDNESS", ax, y, aw, 14, juce::Justification::centred);
    y += 16;

    const bool hasResult = ! std::isinf (integrated);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (52.0f));
    g.drawText (lufsToString (integrated), ax, y, aw, 60, juce::Justification::centred);
    y += 60;

    g.setColour (juce::Colours::white.withAlpha (0.4f));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("LUFS", ax, y, aw, 14, juce::Justification::centred);
    y += 24;

    if (hasResult)
    {
        const bool inRange = (integrated >= -24.0f && integrated <= -22.0f);
        g.setColour (inRange ? juce::Colour (0xFF30D158) : juce::Colour (0xFFFF453A));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (inRange ? "✓  EBU R128  (-23 LUFS ± 1)" : "✗  EBU R128  (-23 LUFS ± 1)",
                    ax, y, aw, 16, juce::Justification::centred);
    }
    y += 28;

    g.setColour (juce::Colour (0xFF3A3A3C));
    g.drawHorizontalLine (y, (float) (ax + 24), (float) (ax + aw - 24));
    y += 12;

    auto drawRow = [&](const juce::String& label, const juce::String& value,
                       juce::Colour valueColour = juce::Colours::white.withAlpha (0.6f))
    {
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (label, ax + 24, y, 130, 16, juce::Justification::centredLeft);

        g.setColour (valueColour);
        g.drawText (value, ax, y, aw - 24, 16, juce::Justification::centredRight);
        y += 18;
    };

    drawRow ("Momentary",  lufsToString (momentary)  + " LUFS");
    drawRow ("Short-term", lufsToString (shortTerm)  + " LUFS");

    const juce::String lraStr = (lra > 0.0f) ? juce::String (lra, 1) + " LU" : "---";
    drawRow ("LRA", lraStr);

    const bool tpHot = (! std::isinf (truePeakDB) && truePeakDB > -1.0f);
    const juce::String tpStr = (! std::isinf (truePeakDB))
                                    ? juce::String (truePeakDB, 1) + " dBTP"
                                    : "---";
    drawRow ("True Peak", tpStr,
             tpHot ? juce::Colour (0xFFFF453A) : juce::Colours::white.withAlpha (0.6f));

    drawRow ("Duration", formatElapsed (measureDurationMs));
}

//==============================================================================
// Compressor UI helpers
//==============================================================================
void LoudnessMeterAudioProcessorEditor::setupSlider (juce::Slider& slider, juce::Label& label,
                                                      const juce::String& labelText,
                                                      const juce::String& suffix)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 65, 20);
    slider.setTextValueSuffix (suffix);
    addAndMakeVisible (slider);

    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (label);
}

void LoudnessMeterAudioProcessorEditor::setupCompressorSection (CompressorControls& comp,
                                                                 const juce::String& prefix,
                                                                 const juce::String& sectionName)
{
    comp.sectionLabel.setText (sectionName, juce::dontSendNotification);
    comp.sectionLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    comp.sectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xFF00CCAA));
    comp.sectionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (comp.sectionLabel);

    setupSlider (comp.threshold, comp.thresholdLabel, "Thresh",  " dB");
    setupSlider (comp.ratio,     comp.ratioLabel,     "Ratio",   " :1");
    setupSlider (comp.attack,    comp.attackLabel,    "Attack",  " ms");
    setupSlider (comp.release,   comp.releaseLabel,   "Release", " ms");

    comp.thresholdAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, prefix + "Threshold", comp.threshold);
    comp.ratioAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, prefix + "Ratio", comp.ratio);
    comp.attackAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, prefix + "Attack", comp.attack);
    comp.releaseAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        audioProcessor.apvts, prefix + "Release", comp.release);
}

void LoudnessMeterAudioProcessorEditor::layoutCompressorSection (CompressorControls& comp,
                                                                  juce::Rectangle<int>& area)
{
    constexpr int rowHeight  = 28;
    constexpr int labelWidth = 60;
    constexpr int spacing    = 2;

    comp.sectionLabel.setBounds (area.removeFromTop (22));
    area.removeFromTop (spacing);

    auto placeRow = [&](juce::Label& label, juce::Slider& slider)
    {
        auto row = area.removeFromTop (rowHeight);
        label.setBounds (row.removeFromLeft (labelWidth));
        slider.setBounds (row);
        area.removeFromTop (spacing);
    };

    placeRow (comp.thresholdLabel, comp.threshold);
    placeRow (comp.ratioLabel,     comp.ratio);
    placeRow (comp.attackLabel,    comp.attack);
    placeRow (comp.releaseLabel,   comp.release);

    area.removeFromTop (6);
}

void LoudnessMeterAudioProcessorEditor::populatePresetSelector()
{
    presetSelector.clear (juce::dontSendNotification);
    const auto& presets = audioProcessor.getPresets();
    for (int i = 0; i < presets.size(); ++i)
        presetSelector.addItem (presets[i].name, i + 1);
}

//==============================================================================
juce::String LoudnessMeterAudioProcessorEditor::lufsToString (float lufs)
{
    if (std::isinf (lufs) || lufs < -200.0f)
        return "---";
    return juce::String (lufs, 1);
}

juce::String LoudnessMeterAudioProcessorEditor::formatElapsed (juce::int64 ms)
{
    const int totalSec = (int) (ms / 1000);
    const int h = totalSec / 3600;
    const int m = (totalSec % 3600) / 60;
    const int s = totalSec % 60;

    if (h > 0)
        return juce::String::formatted ("%d:%02d:%02d", h, m, s);

    return juce::String::formatted ("%02d:%02d", m, s);
}
