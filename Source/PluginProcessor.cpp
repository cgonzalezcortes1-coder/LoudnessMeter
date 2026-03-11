#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
LoudnessMeterAudioProcessor::LoudnessMeterAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout())
{
    initFactoryPresets();
}

LoudnessMeterAudioProcessor::~LoudnessMeterAudioProcessor() {}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
LoudnessMeterAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    auto addCompParams = [&](const juce::String& prefix, const juce::String& label,
                             float defThresh, float defRatio, float defAttack, float defRelease)
    {
        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { prefix + "Threshold", 1 },
            label + " Threshold",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f),
            defThresh,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { prefix + "Ratio", 1 },
            label + " Ratio",
            juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.5f),
            defRatio,
            juce::AudioParameterFloatAttributes().withLabel (":1")));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { prefix + "Attack", 1 },
            label + " Attack",
            juce::NormalisableRange<float> (0.1f, 200.0f, 0.1f, 0.4f),
            defAttack,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        params.push_back (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { prefix + "Release", 1 },
            label + " Release",
            juce::NormalisableRange<float> (5.0f, 1000.0f, 1.0f, 0.4f),
            defRelease,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
    };

    addCompParams ("compA_", "Comp A", -30.0f, 2.0f, 20.0f, 150.0f);
    addCompParams ("compB_", "Comp B", -20.0f, 4.0f, 10.0f, 100.0f);
    addCompParams ("compC_", "Comp C", -15.0f, 6.0f,  5.0f,  80.0f);

    params.push_back (std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { "makeupGain", 1 },
        "Makeup Gain",
        juce::NormalisableRange<float> (0.0f, 30.0f, 0.1f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    return { params.begin(), params.end() };
}

//==============================================================================
void LoudnessMeterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    // --- Loudness Meter DSP ---
    const int numChannels = juce::jmin (getTotalNumInputChannels(), kMaxChannels);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        computePreFilterCoeffs (preFilter[ch], sampleRate);
        computeRLBCoeffs       (rlbFilter[ch], sampleRate);
        preFilter[ch].reset();
        rlbFilter[ch].reset();
    }

    samplesPerBlock100ms = juce::roundToInt (sampleRate * 0.1);
    integratedBlocks.reserve (36000);
    shortTermLoudnessBlocks.reserve (3600);

    tpOversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        (size_t) getTotalNumInputChannels(),
        2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
        true);
    tpOversampler->initProcessing ((size_t) samplesPerBlock);

    measuring = false;
    pendingCommand.store ((int) Command::None);
    measurementState.store ((int) MeasurementState::Idle);
    doReset();

    // --- Compressor DSP ---
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels      = static_cast<juce::uint32> (getTotalNumOutputChannels());

    highPassFilter.reset();
    *highPassFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
        sampleRate, 70.0f, 0.7071f);
    highPassFilter.prepare (spec);

    compressorA.reset(); compressorA.prepare (spec);
    compressorB.reset(); compressorB.prepare (spec);
    compressorC.reset(); compressorC.prepare (spec);

    makeupGain.reset(); makeupGain.prepare (spec);

    limiter.reset(); limiter.prepare (spec);
    limiter.setThreshold (-2.0f);
    limiter.setRelease (50.0f);

    updateDSPFromParameters();
}

void LoudnessMeterAudioProcessor::releaseResources() {}

bool LoudnessMeterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
    return true;
}

//==============================================================================
void LoudnessMeterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                 juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalIn    = getTotalNumInputChannels();
    const int totalOut   = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = totalIn; i < totalOut; ++i)
        buffer.clear (i, 0, numSamples);

    // =========================================================================
    // 1. LOUDNESS MEASUREMENT — on the dry input signal
    // =========================================================================
    const int cmd = pendingCommand.exchange ((int) Command::None);

    if (cmd == (int) Command::Start)
    {
        doReset();
        measuring = true;
        measurementState.store ((int) MeasurementState::Measuring);
    }
    else if (cmd == (int) Command::Stop && measuring)
    {
        commitFinalValues (juce::jmin (totalIn, kMaxChannels));
        measuring = false;
        measurementState.store ((int) MeasurementState::Done);
    }

    if (measuring && samplesPerBlock100ms > 0)
    {
        // True Peak — 4x oversampling on the original (uncompressed) signal
        if (tpOversampler != nullptr)
        {
            juce::dsp::AudioBlock<float> inputBlock (buffer);
            auto upBlock = tpOversampler->processSamplesUp (inputBlock);

            for (size_t ch = 0; ch < upBlock.getNumChannels(); ++ch)
            {
                const float* data = upBlock.getChannelPointer (ch);
                for (size_t i = 0; i < upBlock.getNumSamples(); ++i)
                    runningTruePeak = std::max (runningTruePeak, std::abs (data[i]));
            }
        }

        // Level indicator
        {
            float blockPeak = 0.0f;
            for (int ch = 0; ch < juce::jmin (totalIn, kMaxChannels); ++ch)
                blockPeak = std::max (blockPeak, buffer.getMagnitude (ch, 0, numSamples));
            currentPeakLevel.store (blockPeak);
        }

        // K-weighting + 100ms block accumulation
        const int numChannels = juce::jmin (totalIn, kMaxChannels);
        int offset = 0;

        while (offset < numSamples)
        {
            const int toProcess = juce::jmin (numSamples - offset,
                                              samplesPerBlock100ms - currentBlock.count);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* src = buffer.getReadPointer (ch) + offset;
                for (int i = 0; i < toProcess; ++i)
                {
                    float s = preFilter[ch].processSample (src[i]);
                    s       = rlbFilter[ch].processSample (s);
                    currentBlock.sumSq[ch] += static_cast<double> (s) * s;
                }
            }

            currentBlock.count += toProcess;
            offset             += toProcess;

            if (currentBlock.count >= samplesPerBlock100ms)
                onBlockComplete (numChannels);
        }
    }

    // =========================================================================
    // 2. COMPRESSOR CHAIN — processes the signal after measurement
    // =========================================================================
    updateDSPFromParameters();

    juce::dsp::AudioBlock<float>            block   (buffer);
    juce::dsp::ProcessContextReplacing<float> context (block);

    highPassFilter.process (context);
    compressorA.process    (context);
    compressorB.process    (context);
    compressorC.process    (context);
    makeupGain.process     (context);
    limiter.process        (context);
}

//==============================================================================
// Loudness Meter helpers (unchanged from original)
//==============================================================================
void LoudnessMeterAudioProcessor::computePreFilterCoeffs (BiquadFilter& f, double sampleRate)
{
    constexpr double G  = 3.999843853973347;
    constexpr double f0 = 1681.974450955533;
    constexpr double Q  = 0.7071752369554196;

    const double A     = std::pow (10.0, G / 40.0);
    const double w0    = juce::MathConstants<double>::twoPi * f0 / sampleRate;
    const double cosw0 = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * Q);
    const double sqrtA = std::sqrt (A);

    const double b0 =  A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
    const double b2 =  A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha);
    const double a0 =       (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    const double a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
    const double a2 =       (A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha;

    f.b0 = b0 / a0;  f.b1 = b1 / a0;  f.b2 = b2 / a0;
    f.a1 = a1 / a0;  f.a2 = a2 / a0;
}

void LoudnessMeterAudioProcessor::computeRLBCoeffs (BiquadFilter& f, double sampleRate)
{
    constexpr double f0 = 38.13547087602444;
    constexpr double Q  = 0.5003270373238773;

    const double K    = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
    const double K2   = K * K;
    const double norm = 1.0 / (1.0 + K / Q + K2);

    f.b0 =  norm;  f.b1 = -2.0 * norm;  f.b2 = norm;
    f.a1 = 2.0 * (K2 - 1.0) * norm;
    f.a2 = (1.0 - K / Q + K2) * norm;
}

float LoudnessMeterAudioProcessor::computeLUFS (double weightedMeanSqSum) noexcept
{
    if (weightedMeanSqSum <= 0.0)
        return -std::numeric_limits<float>::infinity();
    return static_cast<float> (-0.691 + 10.0 * std::log10 (weightedMeanSqSum));
}

float LoudnessMeterAudioProcessor::computeWindowLUFS (const MeasBlock* ring,
                                                       int filled,
                                                       int numChannels) noexcept
{
    double totalSumSq[kMaxChannels] {};
    int    totalCount = 0;

    for (int i = 0; i < filled; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            totalSumSq[ch] += ring[i].sumSq[ch];
        totalCount += ring[i].count;
    }

    if (totalCount == 0)
        return -std::numeric_limits<float>::infinity();

    double weighted = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
        weighted += kChannelWeights[ch] * totalSumSq[ch] / totalCount;

    return computeLUFS (weighted);
}

void LoudnessMeterAudioProcessor::onBlockComplete (int numChannels)
{
    momentaryRing[momentaryIdx] = currentBlock;
    momentaryIdx    = (momentaryIdx + 1) % kMomentaryBlocks;
    momentaryFilled = juce::jmin (momentaryFilled + 1, kMomentaryBlocks);

    shortTermRing[shortTermIdx] = currentBlock;
    shortTermIdx    = (shortTermIdx + 1) % kShortTermBlocks;
    shortTermFilled = juce::jmin (shortTermFilled + 1, kShortTermBlocks);

    if (currentBlock.count > 0)
    {
        const int nCh = juce::jmin (numChannels, kMaxChannels);
        double blockWeighted = 0.0;
        for (int ch = 0; ch < nCh; ++ch)
            blockWeighted += kChannelWeights[ch] * currentBlock.sumSq[ch] / currentBlock.count;

        if (blockWeighted >= kAbsGateThresh)
            integratedBlocks.push_back (blockWeighted);
    }

    if (++lraBlockCounter >= kLRAUpdateBlocks)
    {
        lraBlockCounter = 0;
        const float stLUFS = computeWindowLUFS (shortTermRing, shortTermFilled,
                                                juce::jmin (numChannels, kMaxChannels));
        if (! std::isinf (stLUFS) && stLUFS > -70.0f)
            shortTermLoudnessBlocks.push_back (stLUFS);
    }

    currentBlock = {};
}

void LoudnessMeterAudioProcessor::commitFinalValues (int numChannels)
{
    const int nCh = juce::jmin (numChannels, kMaxChannels);

    loudnessMomentary.store (computeWindowLUFS (momentaryRing, momentaryFilled, nCh));
    loudnessShortTerm.store (computeWindowLUFS (shortTermRing, shortTermFilled, nCh));

    if (integratedBlocks.empty())
    {
        loudnessIntegrated.store (-std::numeric_limits<float>::infinity());
        return;
    }

    double absSum = 0.0;
    for (double v : integratedBlocks) absSum += v;
    const double relGateThresh = (absSum / (double) integratedBlocks.size()) * 0.1;

    double relSum  = 0.0;
    int    relCount = 0;
    for (double v : integratedBlocks)
        if (v >= relGateThresh) { relSum += v; ++relCount; }

    loudnessIntegrated.store (relCount > 0 ? computeLUFS (relSum / relCount)
                                           : -std::numeric_limits<float>::infinity());

    truePeak.store (runningTruePeak > 0.0f
                    ? 20.0f * std::log10 (runningTruePeak)
                    : -std::numeric_limits<float>::infinity());

    if (shortTermLoudnessBlocks.size() >= 2)
    {
        std::sort (shortTermLoudnessBlocks.begin(), shortTermLoudnessBlocks.end());
        const int n   = (int) shortTermLoudnessBlocks.size();
        const int i10 = juce::jlimit (0, n - 1, (int) (0.10 * (n - 1)));
        const int i95 = juce::jlimit (0, n - 1, (int) (0.95 * (n - 1)));
        loudnessRange.store (shortTermLoudnessBlocks[i95] - shortTermLoudnessBlocks[i10]);
    }
    else
    {
        loudnessRange.store (0.0f);
    }
}

void LoudnessMeterAudioProcessor::doReset()
{
    currentBlock = {};

    for (auto& b : momentaryRing) b = {};
    momentaryIdx = momentaryFilled = 0;

    for (auto& b : shortTermRing) b = {};
    shortTermIdx = shortTermFilled = 0;

    integratedBlocks.clear();
    shortTermLoudnessBlocks.clear();
    lraBlockCounter = 0;

    runningTruePeak = 0.0f;
    if (tpOversampler != nullptr)
        tpOversampler->reset();

    loudnessMomentary .store (-std::numeric_limits<float>::infinity());
    loudnessShortTerm .store (-std::numeric_limits<float>::infinity());
    loudnessIntegrated.store (-std::numeric_limits<float>::infinity());
    loudnessRange     .store (0.0f);
    truePeak          .store (-std::numeric_limits<float>::infinity());
    currentPeakLevel  .store (0.0f);
}

void LoudnessMeterAudioProcessor::startMeasurement() { pendingCommand.store ((int) Command::Start); }
void LoudnessMeterAudioProcessor::stopMeasurement()  { pendingCommand.store ((int) Command::Stop);  }

//==============================================================================
// Compressor helpers
//==============================================================================
void LoudnessMeterAudioProcessor::updateDSPFromParameters()
{
    compressorA.setThreshold (apvts.getRawParameterValue ("compA_Threshold")->load());
    compressorA.setRatio     (apvts.getRawParameterValue ("compA_Ratio")->load());
    compressorA.setAttack    (apvts.getRawParameterValue ("compA_Attack")->load());
    compressorA.setRelease   (apvts.getRawParameterValue ("compA_Release")->load());

    compressorB.setThreshold (apvts.getRawParameterValue ("compB_Threshold")->load());
    compressorB.setRatio     (apvts.getRawParameterValue ("compB_Ratio")->load());
    compressorB.setAttack    (apvts.getRawParameterValue ("compB_Attack")->load());
    compressorB.setRelease   (apvts.getRawParameterValue ("compB_Release")->load());

    compressorC.setThreshold (apvts.getRawParameterValue ("compC_Threshold")->load());
    compressorC.setRatio     (apvts.getRawParameterValue ("compC_Ratio")->load());
    compressorC.setAttack    (apvts.getRawParameterValue ("compC_Attack")->load());
    compressorC.setRelease   (apvts.getRawParameterValue ("compC_Release")->load());

    makeupGain.setGainDecibels (apvts.getRawParameterValue ("makeupGain")->load());
}

void LoudnessMeterAudioProcessor::initFactoryPresets()
{
    presets.add ({ "Default",               0.0f, 1.0f, 20.0f, 150.0f,   0.0f, 1.0f, 10.0f, 100.0f,   0.0f,  1.0f,  5.0f,  80.0f,  0.0f });
    presets.add ({ "Dialog - Natural",     -30.0f, 1.5f, 25.0f, 200.0f, -22.0f, 2.5f, 15.0f, 150.0f, -15.0f,  4.0f, 10.0f, 100.0f,  3.0f });
    presets.add ({ "Dialog - Controlled",  -28.0f, 2.0f, 20.0f, 150.0f, -20.0f, 3.5f, 10.0f, 100.0f, -14.0f,  5.0f,  5.0f,  80.0f,  5.0f });
    presets.add ({ "Dialog - Broadcast",   -26.0f, 2.5f, 15.0f, 120.0f, -18.0f, 4.0f,  8.0f,  90.0f, -12.0f,  6.0f,  3.0f,  60.0f,  7.0f });
    presets.add ({ "Voiceover - Smooth",   -28.0f, 2.0f, 20.0f, 180.0f, -20.0f, 3.0f, 12.0f, 120.0f, -14.0f,  5.0f,  8.0f,  90.0f,  4.0f });
    presets.add ({ "SFX - Punch",          -24.0f, 2.0f, 10.0f, 100.0f, -16.0f, 4.0f,  3.0f,  60.0f, -10.0f,  8.0f,  1.0f,  40.0f,  5.0f });
    presets.add ({ "SFX - Sustain",        -32.0f, 1.5f, 30.0f, 300.0f, -24.0f, 2.5f, 25.0f, 250.0f, -18.0f,  3.5f, 20.0f, 200.0f,  6.0f });
    presets.add ({ "Music - Stereo Glue",  -26.0f, 1.5f, 30.0f, 250.0f, -20.0f, 2.0f, 25.0f, 200.0f, -16.0f,  3.0f, 20.0f, 150.0f,  3.0f });
    presets.add ({ "Music - Aggressive",   -22.0f, 2.5f, 10.0f, 100.0f, -15.0f, 5.0f,  5.0f,  60.0f, -10.0f, 10.0f,  2.0f,  40.0f,  9.0f });
    presets.add ({ "Ambience - Room Tone", -36.0f, 1.5f, 50.0f, 400.0f, -28.0f, 2.0f, 40.0f, 350.0f, -22.0f,  2.5f, 30.0f, 300.0f,  8.0f });
    presets.add ({ "Mix Bus - Final",      -26.0f, 1.8f, 25.0f, 200.0f, -20.0f, 2.5f, 18.0f, 150.0f, -15.0f,  3.5f, 12.0f, 100.0f,  4.0f });
}

void LoudnessMeterAudioProcessor::loadPreset (int index)
{
    if (index < 0 || index >= presets.size()) return;

    currentPresetIndex = index;
    const auto& p = presets[index];

    auto setParam = [this](const juce::String& id, float value)
    {
        apvts.getParameter (id)->setValueNotifyingHost (
            apvts.getParameterRange (id).convertTo0to1 (value));
    };

    setParam ("compA_Threshold", p.threshA);  setParam ("compA_Ratio",    p.ratioA);
    setParam ("compA_Attack",    p.attackA);  setParam ("compA_Release",  p.releaseA);
    setParam ("compB_Threshold", p.threshB);  setParam ("compB_Ratio",    p.ratioB);
    setParam ("compB_Attack",    p.attackB);  setParam ("compB_Release",  p.releaseB);
    setParam ("compC_Threshold", p.threshC);  setParam ("compC_Ratio",    p.ratioC);
    setParam ("compC_Attack",    p.attackC);  setParam ("compC_Release",  p.releaseC);
    setParam ("makeupGain",      p.makeupGainDb);
}

void LoudnessMeterAudioProcessor::saveCurrentAsPreset (const juce::String& name)
{
    Preset p;
    p.name = name;

    p.threshA  = apvts.getRawParameterValue ("compA_Threshold")->load();
    p.ratioA   = apvts.getRawParameterValue ("compA_Ratio")->load();
    p.attackA  = apvts.getRawParameterValue ("compA_Attack")->load();
    p.releaseA = apvts.getRawParameterValue ("compA_Release")->load();

    p.threshB  = apvts.getRawParameterValue ("compB_Threshold")->load();
    p.ratioB   = apvts.getRawParameterValue ("compB_Ratio")->load();
    p.attackB  = apvts.getRawParameterValue ("compB_Attack")->load();
    p.releaseB = apvts.getRawParameterValue ("compB_Release")->load();

    p.threshC  = apvts.getRawParameterValue ("compC_Threshold")->load();
    p.ratioC   = apvts.getRawParameterValue ("compC_Ratio")->load();
    p.attackC  = apvts.getRawParameterValue ("compC_Attack")->load();
    p.releaseC = apvts.getRawParameterValue ("compC_Release")->load();

    p.makeupGainDb = apvts.getRawParameterValue ("makeupGain")->load();

    presets.add (p);
    currentPresetIndex = presets.size() - 1;
}

//==============================================================================
// Boilerplate
//==============================================================================
bool LoudnessMeterAudioProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* LoudnessMeterAudioProcessor::createEditor()
{
    return new LoudnessMeterAudioProcessorEditor (*this);
}

const juce::String LoudnessMeterAudioProcessor::getName() const  { return JucePlugin_Name; }
bool LoudnessMeterAudioProcessor::acceptsMidi() const            { return false; }
bool LoudnessMeterAudioProcessor::producesMidi() const           { return false; }
bool LoudnessMeterAudioProcessor::isMidiEffect() const           { return false; }
double LoudnessMeterAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int LoudnessMeterAudioProcessor::getNumPrograms()                           { return 1; }
int LoudnessMeterAudioProcessor::getCurrentProgram()                        { return 0; }
void LoudnessMeterAudioProcessor::setCurrentProgram (int)                   {}
const juce::String LoudnessMeterAudioProcessor::getProgramName (int)        { return {}; }
void LoudnessMeterAudioProcessor::changeProgramName (int, const juce::String&) {}

void LoudnessMeterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void LoudnessMeterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoudnessMeterAudioProcessor();
}
