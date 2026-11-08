// Dev tool: renders every UI page to PNG, renders the test vocal through every
// factory preset to WAV, and stress-tests the randomiser.
// Usage: UISnapshot <outDir> <inputVocal.wav> [scale]
#include <juce_audio_utils/juce_audio_utils.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

static juce::AudioBuffer<float> loadWav (const juce::File& f, double& sr)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    juce::AudioBuffer<float> b;
    if (r == nullptr) return b;
    sr = r->sampleRate;
    b.setSize (2, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1) b.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
    return b;
}

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (b.getNumChannels()).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (os, opts)) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

// Run audio through the processor; returns peak and whether everything stayed finite
static std::pair<float, bool> run (NeonRuinAudioProcessor& p, juce::AudioBuffer<float>& b, std::vector<int> heldNotes = {})
{
    float peak = 0; bool finite = true;
    for (int pos = 0; pos < b.getNumSamples(); pos += 512)
    {
        const int len = std::min (512, b.getNumSamples() - pos);
        juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 2, pos, len);
        juce::MidiBuffer midi;
        if (pos == 0) for (int n : heldNotes) midi.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 100), 0);
        p.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i) { const float x = blk.getSample (c, i); finite &= std::isfinite (x); peak = std::max (peak, std::abs (x)); }
    }
    return { peak, finite };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File outDir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    outDir.createDirectory();
    double sr = 48000.0;
    juce::AudioBuffer<float> vocal;
    if (argc > 2) vocal = loadWav (juce::File (juce::String (argv[2])), sr);
    const float scale = argc > 3 ? (float) std::atof (argv[3]) : 1.0f;
    const int keyArg = argc > 4 ? std::atoi (argv[4]) : -1;      // optional key (0 = C ... 11 = B)
    const int scaleArg = argc > 5 ? std::atoi (argv[5]) : -1;    // optional scale index
    const bool skipExtras = juce::SystemStats::getEnvironmentVariable ("NR_ONLY_RENDER", "").isNotEmpty();
    auto setKey = [&] (NeonRuinAudioProcessor& p)
    {
        if (keyArg >= 0)   p.apvts.getParameter ("tuneKey")->setValueNotifyingHost (p.apvts.getParameter ("tuneKey")->convertTo0to1 ((float) keyArg));
        if (scaleArg >= 0) p.apvts.getParameter ("tuneScale")->setValueNotifyingHost (p.apvts.getParameter ("tuneScale")->convertTo0to1 ((float) scaleArg));
    };
    auto render = [&] (NeonRuinAudioProcessor& p, const juce::String& fileName, std::vector<int> held)
    {
        p.setPlayConfigDetails (2, 2, sr, 512);
        p.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> b; b.makeCopyOf (vocal);
        auto res = run (p, b, held);
        const int L = p.getLatencySamples();
        juce::AudioBuffer<float> aligned (2, b.getNumSamples()); aligned.clear();
        for (int c = 0; c < 2; ++c) aligned.copyFrom (c, 0, b, c, L, b.getNumSamples() - L);
        writeWav (outDir.getChildFile (fileName), aligned, sr);
        std::printf ("%-36s peak %5.2f  finite %s  limiter %.1f dB\n", fileName.toRawUTF8(), res.first, res.second ? "yes" : "NO!", p.limiterDb.load());
    };

    // ---------------- pitch-only renders (all other modules off) for measurement
    if (vocal.getNumSamples() > 0)
    {
        auto tuneOnly = [&] (const char* file, std::vector<std::pair<const char*, float>> vals)
        {
            NeonRuinAudioProcessor p;
            p.loadFactoryPreset (0);
            for (auto* off : { "eqOn", "compOn", "delayOn", "reverbOn" }) p.apvts.getParameter (off)->setValueNotifyingHost (0.0f);
            setKey (p);
            for (auto& [id, v] : vals) p.apvts.getParameter (id)->setValueNotifyingHost (p.apvts.getParameter (id)->convertTo0to1 (v));
            render (p, file, {});
        };
        tuneOnly ("tune_natural.wav", { { "tuneSpeed", 25 } });
        tuneOnly ("tune_robot.wav",   { { "tuneSpeed", 0 }, { "tuneVibrato", 0 } });
        tuneOnly ("tune_humanize_flex.wav", { { "tuneSpeed", 30 }, { "tuneHumanize", 60 }, { "tuneFlex", 40 } });
        tuneOnly ("tune_off_reference.wav", { { "tuneOn", 0 } });
        tuneOnly ("harmony_only_5th.wav", { { "tuneMix", 0 }, { "harmOn", 1 }, { "harmVoice1", 8 }, { "harmVoice2", 0 }, { "harmMix", 100 }, { "harmDetune", 0 } });
    }

    // ---------------- preset renders
    if (vocal.getNumSamples() > 0)
    {
        const auto& presets = params::factoryPresets();
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            NeonRuinAudioProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.loadFactoryPreset (i);
            setKey (p);
            p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> b; b.makeCopyOf (vocal);
            const bool midiPreset = juce::String (presets[(size_t) i].name).contains ("MIDI");
            // MIDI preset: hold a chord that fits the key (root, third, fifth around the singer's range)
            const int root = 48 + juce::jmax (0, keyArg);
            auto [peak, finite] = run (p, b, midiPreset ? std::vector<int> { root, root + (scaleArg == 2 ? 3 : 4), root + 7 } : std::vector<int> {});
            // remove plugin latency so the file lines up with the input
            const int L = p.getLatencySamples();
            juce::AudioBuffer<float> aligned (2, b.getNumSamples()); aligned.clear();
            for (int c = 0; c < 2; ++c) aligned.copyFrom (c, 0, b, c, L, b.getNumSamples() - L);
            const auto name = juce::String (i + 1).paddedLeft ('0', 2) + "_" + juce::String (presets[(size_t) i].name).replaceCharacters (" ()-", "____").removeCharacters ("_") ;
            writeWav (outDir.getChildFile ("preset_" + juce::String (i + 1).paddedLeft ('0', 2) + "_" +
                                           juce::File::createLegalFileName (presets[(size_t) i].name).replace (" ", "_") + ".wav"), aligned, sr);
            std::printf ("preset %-22s peak %5.2f  finite %s\n", presets[(size_t) i].name, peak, finite ? "yes" : "NO!");
        }

        if (skipExtras) return 0;
        // ---------------- randomiser stress test (fixed seeds so problems are reproducible)
        int bad = 0, hot = 0; float worst = 0.0f;
        for (int t = 1; t <= 200; ++t)
        {
            NeonRuinAudioProcessor p;
            p.setPlayConfigDetails (2, 2, sr, 512);
            p.randomise (t);
            if (auto* c = p.apvts.getParameter ("chaos")) c->setValueNotifyingHost ((float) (t % 5) / 4.0f);
            p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> b (2, (int) (sr * 4.0));
            for (int c = 0; c < 2; ++c) b.copyFrom (c, 0, vocal, c, 0, b.getNumSamples());
            float minLim = 0.0f; float peak = 0; bool finite = true;
            for (int pos = 0; pos < b.getNumSamples(); pos += 512)
            {
                juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 2, pos, std::min (512, b.getNumSamples() - pos));
                juce::MidiBuffer midi; p.processBlock (blk, midi);
                minLim = std::min (minLim, p.limiterDb.load());
                for (int c = 0; c < 2; ++c) for (int i = 0; i < blk.getNumSamples(); ++i)
                { const float x = blk.getSample (c, i); finite &= std::isfinite (x); peak = std::max (peak, std::abs (x)); }
            }
            worst = std::min (worst, minLim);
            if (minLim < -12.0f) ++hot;
            if (! finite || peak > 1.0f) ++bad;
        }
        std::printf ("randomiser stress test (200 seeds): %d unsafe outputs, %d needed >12 dB of safety limiting, worst %.1f dB\n", bad, hot, worst);
    }

    // ---------------- UI pages
    NeonRuinAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    proc.loadFactoryPreset (4);   // Ghost Choir: shows harmony on the graph
    proc.prepareToPlay (sr, 512);
    if (vocal.getNumSamples() > 0)
    {
        juce::AudioBuffer<float> b (2, juce::jmin (vocal.getNumSamples(), (int) (sr * 12.0)));
        for (int c = 0; c < 2; ++c) b.copyFrom (c, 0, vocal, c, 0, b.getNumSamples());
        run (proc, b);
    }
    std::unique_ptr<NeonRuinAudioProcessorEditor> ed (dynamic_cast<NeonRuinAudioProcessorEditor*> (proc.createEditor()));
    ed->setVisible (true);
    const char* names[] = { "ui_1_voice.png", "ui_2_fx.png", "ui_3_space.png", "ui_4_mod.png" };
    for (int pg = 0; pg < 4; ++pg)
    {
        ed->showPage (pg);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, scale);
        auto f = outDir.getChildFile (names[pg]);
        f.deleteFile();
        juce::FileOutputStream os (f);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }
    std::printf ("UI pages written\n");
    ed.reset();
    return 0;
}
