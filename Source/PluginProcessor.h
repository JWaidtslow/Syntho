#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "DSP/PitchCorrector.h"
#include "DSP/Effects.h"
#include "Params.h"

// Signal chain:  IN -> GATE -> AUTOTUNE + HARMONY -> [re-orderable FX chain] -> OUT
class NeonRuinAudioProcessor : public juce::AudioProcessor
{
public:
    NeonRuinAudioProcessor();
    ~NeonRuinAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Neon Ruin"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 6.0; }

    // factory presets are exposed to the host as programs
    int getNumPrograms() override { return (int) params::factoryPresets().size(); }
    int getCurrentProgram() override { return juce::jmax (0, currentFactoryPreset); }
    void setCurrentProgram (int i) override { loadFactoryPreset (i); }
    const juce::String getProgramName (int i) override { return params::factoryPresets()[(size_t) juce::jlimit (0, getNumPrograms() - 1, i)].name; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // ---------------------------------------------------- presets / A-B / random
    void loadFactoryPreset (int index);
    juce::File getUserPresetFolder() const;
    juce::Array<juce::File> getUserPresets() const;
    bool saveUserPreset (const juce::String& name);
    void loadUserPreset (const juce::File& file);
    juce::String getCurrentPresetName() const { return currentPresetName; }
    void randomise (juce::int64 seed = 0);   // 0 = truly random
    void selectAB (int slot);              // 0 = A, 1 = B
    void copyABToOther();
    int  getABSlot() const { return abSlot; }

    // ------------------------------------------------------------- chain order
    juce::Array<int> getChainOrder() const;
    void setChainOrder (const juce::Array<int>& order);

    // --------------------------------------------------------- UI read-outs
    PitchCorrector tuner;
    std::atomic<float> inLevelDb { -100.0f }, outLevelDb { -100.0f }, gainReductionDb { 0.0f }, deessReductionDb { 0.0f }, limiterDb { 0.0f };
    std::atomic<float> lfoOut[2] { 0.0f, 0.0f };
    std::atomic<float> bpm { 120.0f };
    std::atomic<bool>  glitchActive { false };
    std::atomic<int>   midiNotesHeld { 0 };

private:
    float p (const char* id) const { return apvts.getRawParameterValue (id)->load(); }
    bool  on (const char* id) const { return p (id) > 0.5f; }
    float pm (const char* id) const;                // modulated value (LFOs + Chaos)
    void  updateModulation (const fx::Tempo&, int numSamples);
    void  updateEq();
    void  processModule (int module, juce::AudioBuffer<float>&, const fx::Tempo&);
    void  applyValues (const std::vector<std::pair<const char*, float>>& values);
    void  resetToDefaults (bool keepKeyAndLevels);
    void  readChainFromState();

    double sr = 44100.0;

    // modulation offsets (normalised) for every modulatable parameter
    struct ModSlot { const char* id; float offset; };
    std::vector<ModSlot> modSlots;
    fx::LFO lfos[2];

    std::array<bool, 128> heldNotes {};

    fx::Gate gate;
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    Filter lowCut, lowShelf, midPeak, highShelf;
    float eqCache[5] { -1, -1, -1, -1, -1 };
    juce::dsp::Compressor<float> comp;
    fx::DeEsser deesser;
    fx::Transmission transmit;
    fx::Doubler doubler;
    juce::dsp::Chorus<float> chorus;
    fx::Delay delay;
    juce::dsp::Reverb reverb;
    fx::Glitch glitch;
    fx::SafetyLimiter limiter;

    std::array<std::atomic<int>, params::numModules> chain;

    int currentFactoryPreset = 0;
    juce::String currentPresetName { "Init" };
    juce::ValueTree abStates[2];
    int abSlot = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonRuinAudioProcessor)
};
