#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

class LoudnessMeterAudioProcessor : public juce::AudioProcessor
{
public:
    LoudnessMeterAudioProcessor();
    ~LoudnessMeterAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Compressor — Parameter Tree
    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Preset system
    struct Preset
    {
        juce::String name;
        float threshA, ratioA, attackA, releaseA;
        float threshB, ratioB, attackB, releaseB;
        float threshC, ratioC, attackC, releaseC;
        float makeupGainDb;
    };

    const juce::Array<Preset>& getPresets() const { return presets; }
    int getCurrentPresetIndex() const { return currentPresetIndex; }
    void loadPreset (int index);
    void saveCurrentAsPreset (const juce::String& name);

    //==========================================================================
    // Loudness Meter — public atomics (written by audio thread, read by UI)
    //==========================================================================
    enum class MeasurementState : int { Idle = 0, Measuring = 1, Done = 2 };

    std::atomic<int>   measurementState   { (int)MeasurementState::Idle };
    std::atomic<float> loudnessMomentary  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> loudnessShortTerm  { -std::numeric_limits<float>::infinity() };
    std::atomic<float> loudnessIntegrated { -std::numeric_limits<float>::infinity() };
    std::atomic<float> loudnessRange      { 0.0f };
    std::atomic<float> truePeak           { -std::numeric_limits<float>::infinity() };
    std::atomic<float> currentPeakLevel   { 0.0f };

    void startMeasurement();
    void stopMeasurement();

private:
    //==========================================================================
    // Compressor DSP
    //==========================================================================
    juce::dsp::ProcessorDuplicator<
        juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>> highPassFilter;

    juce::dsp::Compressor<float> compressorA;
    juce::dsp::Compressor<float> compressorB;
    juce::dsp::Compressor<float> compressorC;

    juce::dsp::Gain<float>    makeupGain;
    juce::dsp::Limiter<float> limiter;

    double currentSampleRate = 44100.0;

    juce::Array<Preset> presets;
    int currentPresetIndex = -1;
    void initFactoryPresets();
    void updateDSPFromParameters();

    //==========================================================================
    // Loudness Meter DSP (ITU-R BS.1770-4)
    //==========================================================================
    struct BiquadFilter
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        float processSample (float x) noexcept
        {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float> (y);
        }

        void reset() noexcept { z1 = z2 = 0.0; }
    };

    static constexpr int kMaxChannels = 8;
    BiquadFilter preFilter[kMaxChannels];
    BiquadFilter rlbFilter[kMaxChannels];

    static void computePreFilterCoeffs (BiquadFilter& f, double sampleRate);
    static void computeRLBCoeffs       (BiquadFilter& f, double sampleRate);

    static constexpr int kMomentaryBlocks = 4;
    static constexpr int kShortTermBlocks = 30;

    static constexpr double kChannelWeights[8] = {
        1.0, 1.0, 1.0, 0.0, 1.41254, 1.41254, 1.41254, 1.41254
    };

    static constexpr double kAbsGateThresh = 1.1724e-7;

    struct MeasBlock
    {
        double sumSq[kMaxChannels] {};
        int    count = 0;
    };

    MeasBlock currentBlock;
    int       samplesPerBlock100ms = 4800;

    MeasBlock momentaryRing[kMomentaryBlocks] {};
    int       momentaryIdx    = 0;
    int       momentaryFilled = 0;

    MeasBlock shortTermRing[kShortTermBlocks] {};
    int       shortTermIdx    = 0;
    int       shortTermFilled = 0;

    std::vector<double> integratedBlocks;

    enum class Command : int { None = 0, Start = 1, Stop = 2 };
    std::atomic<int> pendingCommand { (int)Command::None };
    bool measuring = false;

    std::unique_ptr<juce::dsp::Oversampling<float>> tpOversampler;
    float runningTruePeak = 0.0f;

    std::vector<float> shortTermLoudnessBlocks;
    int lraBlockCounter = 0;
    static constexpr int kLRAUpdateBlocks = 10;

    static float computeWindowLUFS (const MeasBlock* ring, int filled, int numChannels) noexcept;
    static float computeLUFS       (double weightedMeanSqSum) noexcept;
    void         onBlockComplete   (int numChannels);
    void         commitFinalValues (int numChannels);
    void         doReset();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessMeterAudioProcessor)
};
