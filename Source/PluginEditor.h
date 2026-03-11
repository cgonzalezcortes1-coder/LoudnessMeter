#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class LoudnessMeterAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit LoudnessMeterAudioProcessorEditor (LoudnessMeterAudioProcessor&);
    ~LoudnessMeterAudioProcessorEditor() override;

    void paint   (juce::Graphics&) override;
    void resized () override;

private:
    void timerCallback() override;

    //==========================================================================
    // Loudness Meter UI
    //==========================================================================
    void paintLoudnessMeter (juce::Graphics& g, juce::Rectangle<int> panel) const;
    void paintIdle          (juce::Graphics& g, juce::Rectangle<int> area)  const;
    void paintMeasuring     (juce::Graphics& g, juce::Rectangle<int> area)  const;
    void paintDone          (juce::Graphics& g, juce::Rectangle<int> area)  const;

    static juce::String lufsToString  (float lufs);
    static juce::String formatElapsed (juce::int64 ms);

    using State = LoudnessMeterAudioProcessor::MeasurementState;
    State currentState = State::Idle;
    float momentary    = -std::numeric_limits<float>::infinity();
    float shortTerm    = -std::numeric_limits<float>::infinity();
    float integrated   = -std::numeric_limits<float>::infinity();
    float lra          = 0.0f;
    float truePeakDB   = -std::numeric_limits<float>::infinity();
    float displayLevel = 0.0f;

    juce::int64 measureStartMs    = 0;
    juce::int64 measureDurationMs = 0;

    juce::TextButton actionButton { "Start Measurement" };

    //==========================================================================
    // Compressor UI
    //==========================================================================
    juce::ComboBox   presetSelector;
    juce::TextButton savePresetButton { "Save" };
    juce::Label      presetLabel;

    struct CompressorControls
    {
        juce::Slider threshold, ratio, attack, release;
        juce::Label  thresholdLabel, ratioLabel, attackLabel, releaseLabel;
        juce::Label  sectionLabel;

        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
            thresholdAtt, ratioAtt, attackAtt, releaseAtt;
    };

    CompressorControls compA, compB, compC;

    juce::Slider makeupGainSlider;
    juce::Label  makeupGainLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> makeupGainAttachment;

    void setupCompressorSection (CompressorControls& comp,
                                 const juce::String& prefix,
                                 const juce::String& sectionName);
    void setupSlider (juce::Slider& slider, juce::Label& label,
                      const juce::String& labelText, const juce::String& suffix);
    void layoutCompressorSection (CompressorControls& comp, juce::Rectangle<int>& area);
    void populatePresetSelector();

    //==========================================================================
    LoudnessMeterAudioProcessor& audioProcessor;

    static constexpr int kMeterWidth      = 300;
    static constexpr int kCompressorWidth = 450;
    static constexpr int kPluginHeight    = 660;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterAudioProcessorEditor)
};
