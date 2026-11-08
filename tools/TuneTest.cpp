// Offline test bench for the pitch corrector.
// Synthesises a "singer" (glottal source + vowel formants, out-of-tune notes,
// vibrato, scoops, a legato slide, sibilants and breaths), runs it through the
// legacy and new engines with several settings and writes WAV files + a CSV of
// the true pitch so the results can be measured (tools/analyse.py).
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "../Source/DSP/PitchCorrector.h"
#include "LegacyPitchCorrector.h"
#include <chrono>
#include <functional>

static constexpr double SR = 48000.0;

struct Note { int midi; float cents; int vowel; float dur; float slideFrom; float hold; float slideTime; float gapType; };

static const float formants[5][4] = {
    { 730, 1090, 2440, 3400 },   // a
    { 530, 1840, 2480, 3400 },   // e
    { 270, 2290, 3010, 3400 },   // i
    { 570,  840, 2410, 3400 },   // o
    { 300,  870, 2240, 3400 },   // u
};
static const float bws[4] = { 80, 100, 120, 150 };

struct Resonator
{
    double a1 = 0, a2 = 0, g = 1, y1 = 0, y2 = 0;
    void set (double f, double bw)
    {
        const double r = std::exp (-juce::MathConstants<double>::pi * bw / SR);
        a1 = 2 * r * std::cos (2 * juce::MathConstants<double>::pi * f / SR);
        a2 = -r * r;
        g = 1 - r;
    }
    double process (double x) { const double y = g * x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; }
};

static void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (SR).withNumChannels (b.getNumChannels()).withBitsPerSample (24);
    if (auto w = wav.createWriterFor (os, opts))
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

int main (int argc, char** argv)
{
    const juce::File dir (argc > 1 ? juce::String (argv[1]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    dir.createDirectory();

    const std::vector<Note> notes {
        { 57, -35, 0, 1.3f, -0.7f, 0.0f, 0.10f, 1 },   // A3 flat, "a"
        { 60,  40, 1, 1.0f, -0.7f, 0.0f, 0.10f, 2 },   // C4 sharp, "e"
        { 64, -30, 2, 1.0f, -0.7f, 0.0f, 0.10f, 1 },   // E4 flat, "i"
        { 62,  42, 4, 1.0f, -0.7f, 0.0f, 0.10f, 2 },   // D4 sharp, "u"
        { 45, -25, 0, 1.3f, -0.7f, 0.0f, 0.10f, 1 },   // A2 (low male) flat
        { 72,  30, 3, 1.2f, -0.7f, 0.0f, 0.10f, 2 },   // C5 (high) sharp, "o"
        { 67, -20, 0, 1.6f, -3.0f, 0.45f, 0.25f, 0 },  // E4 -> G4 legato slide
    };

    // ---- null tests: with no correction, output must equal the delayed input
    {
        auto nullTest = [] (const char* label, std::function<float (int)> gen)
        {
            PitchCorrector pc; pc.prepare (SR, 1);
            PitchCorrector::Settings s; s.scale = 0; s.flex = 1.0f; s.retuneMs = 20;
            pc.setSettings (s);
            const int N = (int) (3 * SR);
            juce::AudioBuffer<float> b (1, N), ref (1, N);
            for (int i = 0; i < N; ++i) { const float g = gen (i); b.setSample (0, i, g); ref.setSample (0, i, g); }
            for (int pos = 0; pos < N; pos += 512)
            {
                juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 1, pos, std::min (512, N - pos));
                pc.process (blk, true);
            }
            const int L = pc.getLatencySamples();
            double e = 0, r = 0, so = 0;
            for (int i = (int) SR; i < N; ++i)
            {
                const double d = b.getSample (0, i) - ref.getSample (0, i - L);
                e += d * d; r += (double) ref.getSample (0, i - L) * ref.getSample (0, i - L);
                so += (double) b.getSample (0, i) * b.getSample (0, i);
            }
            int bestLag = 0; double bestC = -1e9;
            for (int lag = 0; lag < 6000; ++lag)
            {
                double c = 0;
                for (int i = (int) SR; i < (int) SR + 20000; ++i) c += (double) b.getSample (0, i) * ref.getSample (0, i - lag);
                if (c > bestC) { bestC = c; bestLag = lag; }
            }
            std::printf ("   measured delay %d (expected %d)\n", bestLag, L);
            std::printf ("null test %-22s level ratio %.3f   error %.1f dB below signal\n", label, std::sqrt (so / r), 10 * std::log10 (r / (e + 1e-20)));
        };
        juce::Random nr (1);
        nullTest ("white noise", [&] (int) { return 0.3f * (nr.nextFloat() * 2 - 1); });
        nullTest ("220 Hz in-tune saw", [] (int i) { double p = std::fmod (i * 220.0 / SR, 1.0); return (float) (0.3 * (2 * p - 1)); });
        nullTest ("silence->tone", [] (int i) { return i < 60000 ? 0.0f : (float) (0.3 * std::sin (i * 2 * 3.14159265 * 330.0 / SR)); });
    }

    // ---- robustness: sample rates, odd block sizes, stereo; output must stay finite and aligned
    for (double rate : { 44100.0, 88200.0, 96000.0, 192000.0 })
        for (int bs : { 1, 7, 64, 1000, 4096 })
        {
            PitchCorrector pc; pc.prepare (rate, 2);
            PitchCorrector::Settings st; st.scale = 1; st.retuneMs = 0; st.formantSemis = 2; st.transpose = -3;
            pc.setSettings (st);
            const int N = (int) (rate * 1.5);
            juce::AudioBuffer<float> b (2, N);
            double ph = 0;
            for (int i = 0; i < N; ++i)
            {
                ph += 2 * juce::MathConstants<double>::pi * 300.0 * (1 + 0.02 * std::sin (i / rate * 30)) / rate;
                b.setSample (0, i, (float) (0.4 * std::sin (ph))); b.setSample (1, i, (float) (0.3 * std::sin (ph)));
            }
            for (int pos = 0; pos < N; pos += bs)
            {
                juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 2, pos, std::min (bs, N - pos));
                pc.process (blk, true);
            }
            bool finite = true; float pk = 0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < N; ++i) { const float x = b.getSample (c, i); finite &= std::isfinite (x); pk = std::max (pk, std::abs (x)); }
            if (! finite || pk > 1.0f || pc.lateSamples > 0)
                std::printf ("ROBUSTNESS FAIL sr %.0f block %d finite %d peak %.2f late %lld\n", rate, bs, (int) finite, pk, (long long) pc.lateSamples);
        }
    std::printf ("robustness test done (4 sample rates x 5 block sizes, stereo)\n");

    juce::Random rng (42);
    const juce::String sweepFile = juce::SystemStats::getEnvironmentVariable ("NR_SWEEP_REAL", "");
    std::vector<float> sig, truth;
    std::vector<float> gapMarker;   // 0 voice, 1 sibilant, 2 breath, -1 silence

    for (const auto& nt : notes)
    {
        // ---- the sung note
        Resonator res[4];
        for (int k = 0; k < 4; ++k) res[k].set (formants[nt.vowel][k], bws[k]);
        const int len = (int) (nt.dur * SR);
        std::vector<double> phase (200, 0.0);
        double jitter = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double t = i / SR;
            double slide = 0.0;
            if (t < nt.hold) slide = nt.slideFrom;
            else
            {
                const double x = juce::jlimit (0.0, 1.0, (t - nt.hold) / nt.slideTime);
                slide = nt.slideFrom * (1.0 - x * x * (3 - 2 * x));
            }
            const double vibDepth = 0.35 * juce::jlimit (0.0, 1.0, (t - 0.35) / 0.25);
            const double vib = vibDepth * std::sin (2 * juce::MathConstants<double>::pi * 5.6 * t);
            const double drift = 0.10 * t / nt.dur;
            jitter = jitter * 0.999 + 0.0004 * (rng.nextDouble() - 0.5);
            const double midi = nt.midi + nt.cents / 100.0 + slide + vib + drift + jitter;
            const double f0 = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);

            double src = 0.0;
            const int nh = juce::jmin (199, (int) (SR * 0.45 / f0));
            for (int h = 1; h <= nh; ++h)
            {
                phase[(size_t) h] += 2 * juce::MathConstants<double>::pi * f0 * h / SR;
                src += std::sin (phase[(size_t) h]) / h;
            }
            src += 0.02 * (rng.nextDouble() - 0.5);   // aspiration
            double y = src;
            for (auto& r : res) y = r.process (y);

            const double env = juce::jlimit (0.0, 1.0, t / 0.03) * juce::jlimit (0.0, 1.0, (nt.dur - t) / 0.06);
            sig.push_back ((float) y);
            truth.push_back (env > 0.3 ? (float) midi : -1.0f);
            gapMarker.push_back (0);
            sig.back() *= (float) env;
        }

        // ---- gap: sibilant "s", breath, or nothing
        const int gapLen = (int) (0.22 * SR);
        double hp = 0, lp = 0, prev = 0;
        for (int i = 0; i < gapLen; ++i)
        {
            const double t = i / SR;
            const double env = std::sin (juce::MathConstants<double>::pi * juce::jlimit (0.0, 1.0, t / 0.18));
            const double w = rng.nextDouble() * 2 - 1;
            double y = 0;
            if (nt.gapType == 1) { hp = 0.92 * (hp + w - prev); prev = w; y = 0.10 * hp * env; }   // "s"
            else if (nt.gapType == 2) { lp += 0.05 * (w - lp); y = 0.08 * lp * env; }             // breath
            sig.push_back ((float) y);
            truth.push_back (-1.0f);
            gapMarker.push_back (nt.gapType == 0 ? -1.0f : nt.gapType);
        }
    }

    // normalise to -6 dBFS peak
    float pk = 0; for (auto s : sig) pk = std::max (pk, std::abs (s));
    for (auto& s : sig) s *= 0.5f / pk;

    const int N = (int) sig.size();
    juce::AudioBuffer<float> input (1, N);
    for (int i = 0; i < N; ++i) input.setSample (0, i, sig[(size_t) i]);
    writeWav (dir.getChildFile ("01_input_untuned.wav"), input);

    // ---- tracker sweep: renders synthetic + a real vocal (chromatic, natural settings) per combination
    if (sweepFile.isNotEmpty())
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (sweepFile)));
        juce::AudioBuffer<float> real (1, (int) rd->lengthInSamples);
        rd->read (&real, 0, (int) rd->lengthInSamples, 0, true, false);
        struct Combo { float jump; int hops; float base; };
        const std::vector<Combo> combos { { 0.6f, 3, 1.2f }, { 0.6f, 3, 1.2f }, { 0.6f, 3, 1.2f }, { 0.6f, 3, 1.2f } };
        const float vibs[] = { 1.0f, 0.5f, 0.0f, 0.0f };
        const float speeds[] = { 25.0f, 25.0f, 25.0f, 60.0f };
        for (size_t ci = 0; ci < combos.size(); ++ci)
        {
            PitchCorrector::tracker = { combos[ci].jump, combos[ci].hops, combos[ci].base };
            for (int which = 0; which < 2; ++which)
            {
                auto& src = which == 0 ? input : real;
                PitchCorrector pc; pc.prepare (SR, 1);
                PitchCorrector::Settings st; st.scale = which == 0 ? 1 : 0; st.retuneMs = speeds[ci]; st.vibrato = vibs[ci];
                const auto realKey = juce::SystemStats::getEnvironmentVariable ("NR_REAL_KEY", "");
                if (which == 1 && realKey.isNotEmpty()) { st.key = realKey.getIntValue(); st.scale = 1; }
                pc.setSettings (st);
                juce::AudioBuffer<float> b; b.makeCopyOf (src);
                juce::String dbg = "pos,inst,out,target\n"; int lastH = 0;
                for (int pos = 0; pos < b.getNumSamples(); pos += 128)
                {
                    juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 1, pos, std::min (128, b.getNumSamples() - pos));
                    pc.process (blk, true);
                    for (int h = pc.histWrite.load(); lastH != h; lastH = (lastH + 1) % PitchCorrector::historySize)
                        dbg << pos << "," << pc.histIn[(size_t) lastH].load() << "," << pc.histOut[(size_t) lastH].load() << "," << pc.histTarget[(size_t) lastH].load() << "\n";
                }
                if (which == 1) dir.getChildFile ("sweep_" + juce::String ((int) ci) + "_real_debug.csv").replaceWithText (dbg);
                juce::AudioBuffer<float> al (1, b.getNumSamples()); al.clear();
                const int L = pc.getLatencySamples();
                al.copyFrom (0, 0, b, 0, L, b.getNumSamples() - L);
                writeWav (dir.getChildFile ("sweep_" + juce::String ((int) ci) + (which == 0 ? "_synth.wav" : "_real.wav")), al);
            }
            std::printf ("sweep %d: jump %.2f st, %d hops, base %.1f Hz\n", (int) ci, combos[ci].jump, combos[ci].hops, combos[ci].base);
        }
        return 0;
    }

    // ground truth
    {
        juce::FileOutputStream csv (dir.getChildFile ("truth.csv"));
        csv.setPosition (0); csv.truncate();
        csv << "sample,midi,gap\n";
        for (int i = 0; i < N; i += 48)
            csv << i << "," << truth[(size_t) i] << "," << gapMarker[(size_t) i] << "\n";
    }

    auto renderRaw = [&] (PitchCorrector::Settings s, bool harm, std::vector<int> held) -> juce::AudioBuffer<float>
    {
        PitchCorrector pc; pc.prepare (SR, 1); pc.setSettings (s);
        std::array<bool, 128> h {}; for (int x : held) h[(size_t) x] = true;
        pc.setHeldNotes (h);
        juce::AudioBuffer<float> b; b.makeCopyOf (input);
        for (int pos = 0; pos < N; pos += 256)
        {
            juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 1, pos, std::min (256, N - pos));
            pc.process (blk, true, harm);
        }
        juce::AudioBuffer<float> aligned (1, N); aligned.clear();
        const int L = pc.getLatencySamples();
        aligned.copyFrom (0, 0, b, 0, L, N - L);
        return aligned;
    };
    auto runHarmonyOnly = [&] (const char* name, PitchCorrector::Settings s, std::vector<int> held)
    {
        auto withH = renderRaw (s, true, held);
        auto without = renderRaw (s, false, held);
        withH.addFrom (0, 0, without, 0, 0, N, -1.0f);   // leaves only the harmony bus
        writeWav (dir.getChildFile (name), withH);
        std::printf ("%-34s (harmony bus isolated)\n", name);
    };
    auto runFull = [&] (const char* name, PitchCorrector::Settings s, std::vector<int> held)
    {
        writeWav (dir.getChildFile (name), renderRaw (s, true, held));
        std::printf ("%-34s rendered\n", name);
    };

    auto runNew = [&] (const char* name, PitchCorrector::Settings s, std::vector<int> held = {})
    {
        PitchCorrector pc;
        pc.prepare (SR, 1);
        pc.setSettings (s);
        { std::array<bool, 128> h {}; for (int x : held) h[(size_t) x] = true; pc.setHeldNotes (h); }
        juce::AudioBuffer<float> b;
        b.makeCopyOf (input);
        const auto t0 = std::chrono::steady_clock::now();
        juce::String dbg = "pos,inst,out,target\n";
        int lastH = 0;
        for (int pos = 0; pos < N; pos += 128)
        {
            const int len = std::min (128, N - pos);
            juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 1, pos, len);
            pc.process (blk, true);
            for (int h = pc.histWrite.load(); lastH != h; lastH = (lastH + 1) % PitchCorrector::historySize)
                dbg << pos << "," << pc.histIn[(size_t) lastH].load() << "," << pc.histOut[(size_t) lastH].load() << "," << pc.histTarget[(size_t) lastH].load() << "\n";
        }
        dir.getChildFile (juce::String (name) + ".debug.csv").replaceWithText (dbg);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        // remove latency so files line up with the input
        juce::AudioBuffer<float> aligned (1, N);
        aligned.clear();
        const int L = pc.getLatencySamples();
        aligned.copyFrom (0, 0, b, 0, L, N - L);
        writeWav (dir.getChildFile (name), aligned);
        std::printf ("%-34s latency %4d smp (%.1f ms)  CPU %.2f%% of real time  late samples %lld\n",
                     name, L, 1000.0 * L / SR, 100.0 * secs / (N / SR), (long long) pc.lateSamples);
    };

    auto runLegacy = [&] (const char* name, float speed)
    {
        LegacyPitchCorrector pc;
        pc.prepare (SR, 1);
        pc.setParameters (0, 1, speed, 1.0f, 1.0f);
        juce::AudioBuffer<float> b;
        b.makeCopyOf (input);
        for (int pos = 0; pos < N; pos += 512)
        {
            const int len = std::min (512, N - pos);
            juce::AudioBuffer<float> blk (b.getArrayOfWritePointers(), 1, pos, len);
            pc.process (blk, true);
        }
        juce::AudioBuffer<float> aligned (1, N);
        aligned.clear();
        const int L = pc.getLatencySamples();
        aligned.copyFrom (0, 0, b, 0, L, N - L);
        writeWav (dir.getChildFile (name), aligned);
        std::printf ("%-34s latency %4d smp (%.1f ms)\n", name, L, 1000.0 * L / SR);
    };

    PitchCorrector::Settings natural;  natural.key = 0; natural.scale = 1; natural.retuneMs = 20; natural.vibrato = 1.0f;
    PitchCorrector::Settings robot = natural;  robot.retuneMs = 0; robot.vibrato = 0.0f;
    PitchCorrector::Settings human = natural;  human.humanize = 0.8f; human.flex = 0.4f;
    PitchCorrector::Settings formUp = natural; formUp.formantSemis = 4.0f;
    PitchCorrector::Settings trans5 = natural; trans5.transpose = 5.0f;
    PitchCorrector::Settings bypassish = natural; bypassish.scale = 0; bypassish.flex = 1.0f;   // almost no correction

    runLegacy ("02_old_engine.wav", 20.0f);
    runNew ("03_new_natural.wav", natural);
    runNew ("04_new_robot.wav", robot);
    runNew ("05_new_humanize_flex.wav", human);
    runNew ("06_new_formant_plus4.wav", formUp);
    runNew ("07_new_transparency.wav", bypassish);
    runNew ("08_new_transpose_plus5.wav", trans5);

    PitchCorrector::Settings fifth = natural; fifth.harmVoice = { 8, 0, 0 }; fifth.harmMix = 1.0f; fifth.harmDetune = 0.0f;
    runHarmonyOnly ("09_harmony_fifth_only.wav", fifth, {});
    PitchCorrector::Settings midiT = natural; midiT.midiTarget = true;
    runNew ("10_midi_target_D.wav", midiT, { 62 });
    PitchCorrector::Settings choir = natural; choir.harmVoice = { 6, 8, 1 }; choir.harmMix = 0.5f; choir.harmWidth = 0.9f;
    runFull ("11_choir_3rd_5th_oct.wav", choir, {});
    PitchCorrector::Settings chord = natural; chord.retuneMs = 0; chord.vibrato = 0; chord.harmVoice = { 11, 11, 11 }; chord.harmMix = 0.6f;
    runFull ("12_midi_chord_C_E_G.wav", chord, { 48, 52, 55 });
    PitchCorrector::Settings monster = natural; monster.subLevel = 1.0f; monster.harmFormant = -4.0f;
    runFull ("13_monster_sub.wav", monster, {});
    return 0;
}
