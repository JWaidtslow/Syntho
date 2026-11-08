#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/PitchCorrector.h"
#include "DSP/Effects.h"

// ============================================================================
//  All parameters, modulation targets, Chaos macro mapping, randomiser ranges
//  and factory presets live here so they are easy to find and extend.
// ============================================================================
namespace params
{
    using APVTS = juce::AudioProcessorValueTreeState;

    // ---------------------------------------------------------------- chain
    // Re-orderable effect modules (the gate + autotune/harmony always come first).
    enum Module { mEQ, mComp, mDeEss, mTransmit, mDoubler, mChorus, mDelay, mReverb, mGlitch, numModules };
    inline juce::StringArray moduleNames() { return { "EQ", "COMP", "DE-ESS", "TRANSMIT", "DOUBLER", "CHORUS", "DELAY", "REVERB", "GLITCH" }; }
    inline juce::String defaultChain() { return "0,2,1,3,4,5,6,7,8"; }   // EQ, De-ess, Comp, Transmit, ...

    // ------------------------------------------------------ modulation targets
    struct ModTarget { const char* name; const char* id; };
    inline const std::vector<ModTarget>& modTargets()
    {
        static const std::vector<ModTarget> t {
            { "None", "" },
            { "Tune Speed", "tuneSpeed" },     { "Formant", "tuneFormant" },       { "Transpose", "tuneTranspose" },
            { "Harmony Mix", "harmMix" },      { "Harm Formant", "harmFormant" },  { "Sub Level", "harmSub" },
            { "Octave Up", "harmUp" },         { "EQ Mid Freq", "eqMidFreq" },     { "EQ Mid Gain", "eqMidGain" },
            { "Drive", "tmDrive" },            { "Crush", "tmCrush" },             { "Radio", "tmRadio" },
            { "Ring Freq", "tmRingFreq" },     { "Ring Mix", "tmRingMix" },        { "Doubler", "dblAmount" },
            { "Chorus Depth", "chorusDepth" }, { "Chorus Mix", "chorusMix" },      { "Delay Mix", "delayMix" },
            { "Delay Feedback", "delayFeedback" }, { "Reverb Size", "reverbSize" }, { "Reverb Mix", "reverbMix" },
            { "Glitch Chance", "glitchChance" }, { "Volume", "outGain" },
        };
        return t;
    }
    inline juce::StringArray modTargetNames() { juce::StringArray s; for (auto& t : modTargets()) s.add (t.name); return s; }

    // ------------------------------------------------------------ Chaos macro
    // Chaos pushes these parameters (normalised 0..1) toward a "wild" value.
    struct ChaosEntry { const char* id; float wild; };
    inline const std::vector<ChaosEntry>& chaosEntries()
    {
        static const std::vector<ChaosEntry> c {
            { "tmDrive", 0.9f }, { "tmCrush", 0.65f }, { "tmRingMix", 0.6f }, { "tmRadio", 0.6f },
            { "glitchChance", 0.85f }, { "glitchReverse", 0.8f }, { "delayFeedback", 0.85f },
            { "reverbSize", 1.0f }, { "harmDetune", 0.9f }, { "dblDetune", 0.9f }, { "chorusDepth", 0.9f },
        };
        return c;
    }

    // ------------------------------------------------------------ randomiser
    struct RandRange { const char* id; float lo, hi; };   // normalised ranges that stay musical
    inline const std::vector<RandRange>& randomRanges()
    {
        static const std::vector<RandRange> r {
            { "tuneSpeed", 0.0f, 0.6f }, { "tuneHumanize", 0.0f, 0.7f }, { "tuneVibrato", 0.0f, 1.0f },
            { "tuneFormant", 0.3f, 0.7f },
            { "harmVoice1", 0.0f, 1.0f }, { "harmVoice2", 0.0f, 0.95f }, { "harmVoice3", 0.0f, 0.95f },
            { "harmMix", 0.2f, 0.7f }, { "harmWidth", 0.3f, 1.0f }, { "harmDetune", 0.0f, 0.6f }, { "harmFormant", 0.3f, 0.7f },
            { "harmSub", 0.0f, 0.6f }, { "harmUp", 0.0f, 0.5f },
            { "eqMidFreq", 0.2f, 0.8f }, { "eqMidGain", 0.35f, 0.65f }, { "eqHighGain", 0.45f, 0.75f },
            { "tmDrive", 0.1f, 0.8f }, { "tmCrush", 0.0f, 0.6f }, { "tmRadio", 0.0f, 0.8f }, { "tmRingFreq", 0.1f, 0.7f },
            { "tmRingMix", 0.0f, 0.5f }, { "tmMix", 0.5f, 1.0f },
            { "dblAmount", 0.3f, 0.8f }, { "dblWidth", 0.5f, 1.0f }, { "dblDetune", 0.1f, 0.6f }, { "dblDelay", 0.1f, 0.6f },
            { "chorusRate", 0.1f, 0.6f }, { "chorusDepth", 0.1f, 0.7f }, { "chorusMix", 0.15f, 0.5f },
            { "delayDiv", 0.0f, 1.0f }, { "delayFeedback", 0.1f, 0.7f }, { "delayMix", 0.1f, 0.4f },
            { "reverbSize", 0.3f, 1.0f }, { "reverbDamp", 0.1f, 0.8f }, { "reverbMix", 0.1f, 0.5f },
            { "glitchRate", 0.0f, 1.0f }, { "glitchChance", 0.1f, 0.6f }, { "glitchReverse", 0.0f, 0.7f },
            { "lfo1Target", 0.0f, 1.0f }, { "lfo1Shape", 0.0f, 1.0f }, { "lfo1Rate", 0.1f, 0.6f }, { "lfo1Depth", 0.2f, 0.8f },
        };
        return r;
    }
    struct RandModule { const char* id; float probability; };
    inline const std::vector<RandModule>& randomModules()
    {
        static const std::vector<RandModule> m {
            { "harmOn", 0.4f }, { "deessOn", 0.5f }, { "tmOn", 0.45f }, { "dblOn", 0.4f }, { "chorusOn", 0.3f },
            { "delayOn", 0.65f }, { "reverbOn", 0.85f }, { "glitchOn", 0.25f }, { "delaySync", 0.7f },
            { "delayPingPong", 0.4f }, { "delayReverse", 0.2f }, { "lfo1Sync", 0.5f },
        };
        return m;
    }

    // ---------------------------------------------------------------- layout
    inline APVTS::ParameterLayout createLayout()
    {
        using namespace juce;
        APVTS::ParameterLayout l;
        auto fl = [&] (const char* id, const char* name, float lo, float hi, float def, float skew = 1.0f, const char* unit = "")
        {
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, 0.0f, skew), def,
                                                          AudioParameterFloatAttributes().withLabel (unit)));
        };
        auto sw = [&] (const char* id, const char* name, bool def) { l.add (std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def)); };
        auto ch = [&] (const char* id, const char* name, const StringArray& items, int def)
        {
            l.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, def));
        };

        fl ("inGain",  "Input",  -24.0f, 24.0f, 0.0f, 1.0f, "dB");
        fl ("outGain", "Output", -24.0f, 24.0f, 0.0f, 1.0f, "dB");
        fl ("chaos",   "Chaos",    0.0f, 100.0f, 0.0f, 1.0f, "%");

        sw ("gateOn", "Gate On", false);
        fl ("gateThresh",  "Gate Threshold", -80.0f, -20.0f, -50.0f, 1.0f, "dB");
        fl ("gateRelease", "Gate Release",    10.0f, 500.0f, 120.0f, 0.5f, "ms");

        sw ("tuneOn", "Tune On", true);
        ch ("tuneKey", "Key", PitchCorrector::keyNames(), 0);
        ch ("tuneScale", "Scale", PitchCorrector::scaleNames(), 1);
        fl ("tuneSpeed",    "Retune Speed", 0.0f, 400.0f, 25.0f, 0.4f, "ms");
        fl ("tuneHumanize", "Humanize",   0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("tuneFlex",     "Flex-Tune",  0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("tuneVibrato",  "Natural Vibrato", 0.0f, 100.0f, 50.0f, 1.0f, "%");
        fl ("tuneFormant",  "Formant",   -5.0f, 5.0f, 0.0f, 1.0f, "st");
        l.add (std::make_unique<AudioParameterInt> (ParameterID { "tuneTranspose", 1 }, "Transpose", -12, 12, 0,
                                                    AudioParameterIntAttributes().withLabel ("st")));
        fl ("tuneMix",      "Tune Mix",   0.0f, 100.0f, 100.0f, 1.0f, "%");
        sw ("tuneMidi", "MIDI Target", false);

        sw ("harmOn", "Harmony On", false);
        ch ("harmVoice1", "Voice 1", PitchCorrector::harmonyChoices(), 6);
        ch ("harmVoice2", "Voice 2", PitchCorrector::harmonyChoices(), 8);
        ch ("harmVoice3", "Voice 3", PitchCorrector::harmonyChoices(), 0);
        fl ("harmMix",     "Harmony Mix",     0.0f, 100.0f, 50.0f, 1.0f, "%");
        fl ("harmWidth",   "Harmony Width",   0.0f, 100.0f, 70.0f, 1.0f, "%");
        fl ("harmDetune",  "Harmony Detune",  0.0f, 50.0f, 8.0f, 1.0f, "c");
        fl ("harmFormant", "Harmony Formant", -6.0f, 6.0f, 0.0f, 1.0f, "st");
        fl ("harmSub",     "Sub Octave",      0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("harmUp",      "Octave Up",       0.0f, 100.0f, 0.0f, 1.0f, "%");

        sw ("eqOn", "EQ On", true);
        fl ("eqLowCut",   "Low Cut",   20.0f, 500.0f, 80.0f, 0.4f, "Hz");
        fl ("eqLowGain",  "Low",      -12.0f, 12.0f, 0.0f, 1.0f, "dB");
        fl ("eqMidFreq",  "Mid Freq", 200.0f, 6000.0f, 2500.0f, 0.4f, "Hz");
        fl ("eqMidGain",  "Mid",      -12.0f, 12.0f, 0.0f, 1.0f, "dB");
        fl ("eqHighGain", "Air",      -12.0f, 12.0f, 2.0f, 1.0f, "dB");

        sw ("compOn", "Comp On", true);
        fl ("compThresh",  "Threshold", -60.0f, 0.0f, -18.0f, 1.0f, "dB");
        fl ("compRatio",   "Ratio",       1.0f, 20.0f, 4.0f, 0.5f, ":1");
        fl ("compAttack",  "Attack",      0.5f, 100.0f, 5.0f, 0.4f, "ms");
        fl ("compRelease", "Release",    10.0f, 1000.0f, 120.0f, 0.4f, "ms");
        fl ("compMakeup",  "Makeup",      0.0f, 24.0f, 4.0f, 1.0f, "dB");

        sw ("deessOn", "De-ess On", false);
        fl ("deessFreq",   "De-ess Freq",   3000.0f, 10000.0f, 6000.0f, 0.6f, "Hz");
        fl ("deessThresh", "De-ess Thresh", -50.0f, 0.0f, -24.0f, 1.0f, "dB");
        fl ("deessRange",  "De-ess Range",    0.0f, 24.0f, 10.0f, 1.0f, "dB");

        sw ("tmOn", "Transmit On", false);
        fl ("tmDrive",    "Drive",     0.0f, 100.0f, 30.0f, 1.0f, "%");
        fl ("tmCrush",    "Crush",     0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("tmRadio",    "Radio",     0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("tmRingFreq", "Ring Freq", 20.0f, 2000.0f, 220.0f, 0.35f, "Hz");
        fl ("tmRingMix",  "Ring Mix",  0.0f, 100.0f, 0.0f, 1.0f, "%");
        fl ("tmMix",      "Transmit Mix", 0.0f, 100.0f, 100.0f, 1.0f, "%");

        sw ("dblOn", "Doubler On", false);
        fl ("dblAmount", "Doubler",        0.0f, 100.0f, 50.0f, 1.0f, "%");
        fl ("dblWidth",  "Doubler Width",  0.0f, 100.0f, 80.0f, 1.0f, "%");
        fl ("dblDetune", "Doubler Detune", 0.0f, 30.0f, 10.0f, 1.0f, "c");
        fl ("dblDelay",  "Doubler Delay",  5.0f, 60.0f, 18.0f, 1.0f, "ms");

        sw ("chorusOn", "Chorus On", false);
        fl ("chorusRate",  "Rate",  0.05f, 5.0f, 0.8f, 0.5f, "Hz");
        fl ("chorusDepth", "Depth", 0.0f, 100.0f, 30.0f, 1.0f, "%");
        fl ("chorusMix",   "Chorus Mix", 0.0f, 100.0f, 30.0f, 1.0f, "%");

        sw ("delayOn", "Delay On", true);
        fl ("delayTime",     "Time",     20.0f, 1500.0f, 350.0f, 0.5f, "ms");
        fl ("delayFeedback", "Feedback",  0.0f, 95.0f, 35.0f, 1.0f, "%");
        fl ("delayMix",      "Delay Mix", 0.0f, 100.0f, 18.0f, 1.0f, "%");
        sw ("delaySync",     "Delay Sync", false);
        ch ("delayDiv",      "Delay Division", fx::divisionNames(), 3);
        sw ("delayPingPong", "Ping-Pong", false);
        sw ("delayReverse",  "Reverse Delay", false);

        sw ("reverbOn", "Reverb On", true);
        fl ("reverbSize",  "Size",    0.0f, 100.0f, 65.0f, 1.0f, "%");
        fl ("reverbDamp",  "Damping", 0.0f, 100.0f, 45.0f, 1.0f, "%");
        fl ("reverbWidth", "Width",   0.0f, 100.0f, 100.0f, 1.0f, "%");
        fl ("reverbMix",   "Reverb Mix", 0.0f, 100.0f, 22.0f, 1.0f, "%");
        sw ("reverbFreeze", "Freeze", false);

        sw ("glitchOn", "Glitch On", false);
        ch ("glitchRate",    "Glitch Rate", juce::StringArray { "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T" }, 2);
        fl ("glitchChance",  "Glitch Chance",  0.0f, 100.0f, 30.0f, 1.0f, "%");
        fl ("glitchReverse", "Glitch Reverse", 0.0f, 100.0f, 25.0f, 1.0f, "%");
        fl ("glitchMix",     "Glitch Mix",     0.0f, 100.0f, 100.0f, 1.0f, "%");

        for (int n = 1; n <= 2; ++n)
        {
            const String p = "lfo" + String (n);
            l.add (std::make_unique<AudioParameterChoice> (ParameterID { p + "Target", 1 }, "LFO " + String (n) + " Target", modTargetNames(), 0));
            l.add (std::make_unique<AudioParameterChoice> (ParameterID { p + "Shape", 1 }, "LFO " + String (n) + " Shape", fx::LFO::shapeNames(), 0));
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { p + "Rate", 1 }, "LFO " + String (n) + " Rate",
                                                          NormalisableRange<float> (0.02f, 20.0f, 0.0f, 0.3f), 0.5f,
                                                          AudioParameterFloatAttributes().withLabel ("Hz")));
            l.add (std::make_unique<AudioParameterFloat> (ParameterID { p + "Depth", 1 }, "LFO " + String (n) + " Depth",
                                                          NormalisableRange<float> (-100.0f, 100.0f), 50.0f,
                                                          AudioParameterFloatAttributes().withLabel ("%")));
            l.add (std::make_unique<AudioParameterBool> (ParameterID { p + "Sync", 1 }, "LFO " + String (n) + " Sync", false));
            l.add (std::make_unique<AudioParameterChoice> (ParameterID { p + "Div", 1 }, "LFO " + String (n) + " Division", fx::LFO::syncNames(), 4));
        }
        return l;
    }

    // --------------------------------------------------------------- presets
    // Values are in real units (dB, %, ms ...). Choice/bool params use index / 0-1.
    // Everything not listed is reset to its default.
    struct Preset { const char* name; const char* description; std::vector<std::pair<const char*, float>> values; };
    inline const std::vector<Preset>& factoryPresets()
    {
        static const std::vector<Preset> p {
            { "Init", "Clean starting point", {} },
            { "Natural Pop", "Transparent correction, polished vocal",
              { { "tuneSpeed", 35 }, { "tuneHumanize", 40 }, { "tuneVibrato", 80 }, { "eqLowCut", 90 }, { "eqHighGain", 3 }, { "compThresh", -20 }, { "compRatio", 3 },
                { "deessOn", 1 }, { "reverbMix", 15 }, { "delaySync", 1 }, { "delayDiv", 3 }, { "delayMix", 10 } } },
            { "Hard-Tune Robot", "Instant note snapping, flat pitch",
              { { "tuneSpeed", 0 }, { "tuneVibrato", 0 }, { "compThresh", -22 }, { "delaySync", 1 }, { "delayMix", 12 }, { "reverbMix", 15 } } },
            { "Android Radio", "Robotic voice through a broken transmitter",
              { { "tuneSpeed", 0 }, { "tuneVibrato", 0 }, { "tmOn", 1 }, { "tmDrive", 45 }, { "tmRadio", 70 }, { "tmCrush", 25 },
                { "tmRingFreq", 90 }, { "tmRingMix", 25 }, { "delaySync", 1 }, { "delayDiv", 7 }, { "delayMix", 20 }, { "reverbMix", 10 } } },
            { "Ghost Choir", "Three-part choir drifting in a huge space",
              { { "harmOn", 1 }, { "harmVoice1", 6 }, { "harmVoice2", 8 }, { "harmVoice3", 1 }, { "harmMix", 55 }, { "harmWidth", 100 },
                { "harmDetune", 18 }, { "harmFormant", 2 }, { "chorusOn", 1 }, { "chorusMix", 35 }, { "reverbSize", 90 }, { "reverbMix", 45 }, { "delayMix", 15 } } },
            { "Vocal Chords (MIDI)", "Play chords on a MIDI keyboard - the voice sings them",
              { { "tuneMidi", 1 }, { "tuneSpeed", 0 }, { "tuneVibrato", 0 }, { "harmOn", 1 }, { "harmVoice1", 11 }, { "harmVoice2", 11 },
                { "harmVoice3", 11 }, { "harmMix", 70 }, { "harmWidth", 90 }, { "harmDetune", 6 }, { "dblOn", 1 }, { "dblAmount", 40 }, { "reverbMix", 30 } } },
            { "Monster", "Sub-octave beast with a giant throat",
              { { "harmOn", 1 }, { "harmVoice1", 0 }, { "harmVoice2", 0 }, { "harmSub", 100 }, { "harmFormant", -5 }, { "tuneFormant", -3 },
                { "tmOn", 1 }, { "tmDrive", 55 }, { "reverbMix", 20 } } },
            { "Fairy", "Octave-up shimmer with bouncing echoes",
              { { "harmOn", 1 }, { "harmVoice1", 0 }, { "harmVoice2", 0 }, { "harmUp", 80 }, { "harmFormant", 4 }, { "chorusOn", 1 },
                { "chorusMix", 40 }, { "delaySync", 1 }, { "delayPingPong", 1 }, { "delayDiv", 4 }, { "delayMix", 25 }, { "reverbSize", 85 }, { "reverbMix", 40 } } },
            { "Ruins Transmission", "Crushed, distorted broadcast from a dead city",
              { { "tmOn", 1 }, { "tmDrive", 70 }, { "tmCrush", 55 }, { "tmRadio", 50 }, { "tmMix", 80 }, { "glitchOn", 1 }, { "glitchChance", 20 },
                { "delaySync", 1 }, { "delayDiv", 3 }, { "delayFeedback", 55 }, { "delayMix", 25 }, { "reverbMix", 25 } } },
            { "Glitch Prophet", "Stuttering, reversing hard-tuned vocal",
              { { "tuneSpeed", 0 }, { "tuneVibrato", 0 }, { "glitchOn", 1 }, { "glitchRate", 2 }, { "glitchChance", 55 }, { "glitchReverse", 50 },
                { "delayOn", 0 }, { "reverbMix", 15 } } },
            { "Frozen Cathedral", "Endless space - press FREEZE to hold the room",
              { { "harmOn", 1 }, { "harmVoice1", 10 }, { "harmVoice2", 0 }, { "harmMix", 35 }, { "harmFormant", 3 }, { "chorusOn", 1 },
                { "delayOn", 0 }, { "reverbSize", 100 }, { "reverbDamp", 30 }, { "reverbMix", 60 } } },
            { "Reverse Dreams", "Backwards echoes swelling into reverb",
              { { "delayReverse", 1 }, { "delaySync", 1 }, { "delayDiv", 2 }, { "delayFeedback", 45 }, { "delayMix", 45 }, { "chorusOn", 1 },
                { "reverbSize", 85 }, { "reverbMix", 40 } } },
            { "Wide Double", "Thick, wide double-tracked vocal",
              { { "dblOn", 1 }, { "dblAmount", 70 }, { "dblWidth", 100 }, { "dblDetune", 12 }, { "dblDelay", 22 }, { "tuneSpeed", 30 }, { "reverbMix", 15 } } },
            { "Breathing Formant", "Slowly morphing voice character",
              { { "lfo1Target", 2 }, { "lfo1Shape", 0 }, { "lfo1Rate", 0.25f }, { "lfo1Depth", 60 }, { "lfo2Target", 21 }, { "lfo2Shape", 5 },
                { "lfo2Rate", 0.1f }, { "lfo2Depth", 40 }, { "harmOn", 1 }, { "harmVoice2", 0 }, { "harmMix", 35 } } },
            { "Chaos Engine", "Everything moving. Turn CHAOS up or down",
              { { "chaos", 70 }, { "tmOn", 1 }, { "tmRingMix", 30 }, { "glitchOn", 1 }, { "delaySync", 1 }, { "harmOn", 1 }, { "harmVoice2", 0 },
                { "lfo1Target", 3 }, { "lfo1Shape", 4 }, { "lfo1Sync", 1 }, { "lfo1Div", 5 }, { "lfo1Depth", 15 },
                { "lfo2Target", 13 }, { "lfo2Shape", 5 }, { "lfo2Rate", 1.5f }, { "lfo2Depth", 70 } } },
        };
        return p;
    }
}
