#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <vector>
#include <array>

// ============================================================================
//  Effect modules for NEON RUIN. Each one processes a stereo/mono buffer in
//  place. Parameters are set once per block by the processor.
// ============================================================================
namespace fx
{
    constexpr float pi = juce::MathConstants<float>::pi;

    // Tempo divisions shared by delay, glitch and LFO sync (in beats)
    inline juce::StringArray divisionNames() { return { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 D", "1/8 D", "1/16 D", "1/4 T", "1/8 T", "1/16 T" }; }
    inline double divisionBeats (int i)
    {
        static const double b[] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1.5, 0.75, 0.375, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0 };
        return b[juce::jlimit (0, 11, i)];
    }

    struct Tempo { double bpm = 120.0, ppq = 0.0; bool playing = false; };

    // ------------------------------------------------------------------ Gate
    class Gate
    {
    public:
        void prepare (double sampleRate) { sr = sampleRate; env = 0; gain = 1; hold = 0; }
        void process (juce::AudioBuffer<float>& b, float threshDb, float releaseMs)
        {
            const float open = juce::Decibels::decibelsToGain (threshDb);
            const float close = open * 0.7f;                         // 3 dB hysteresis
            const float envRel = std::exp (-1.0f / (0.05f * (float) sr));
            const float att = std::exp (-1.0f / (0.001f * (float) sr));
            const float rel = std::exp (-1.0f / (releaseMs * 0.001f * (float) sr));
            const int holdSamples = (int) (0.03 * sr);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float pk = 0.0f;
                for (int c = 0; c < b.getNumChannels(); ++c) pk = std::max (pk, std::abs (b.getSample (c, i)));
                env = pk > env ? pk : pk + (env - pk) * envRel;
                if (env > open) { isOpen = true; hold = holdSamples; }
                else if (env < close && --hold <= 0) isOpen = false;
                const float target = isOpen ? 1.0f : 0.0f;
                gain = target + (gain - target) * (target > gain ? att : rel);
                for (int c = 0; c < b.getNumChannels(); ++c) b.setSample (c, i, b.getSample (c, i) * gain);
            }
        }
    private:
        double sr = 44100; float env = 0, gain = 1; int hold = 0; bool isOpen = true;
    };

    // ------------------------------------------------------------- De-esser
    // Splits off the band above 'freq', and turns only that band down when it gets loud.
    class DeEsser
    {
    public:
        void prepare (double sampleRate, int ch)
        {
            sr = sampleRate;
            numCh = juce::jlimit (1, 2, ch);
            for (auto& f : hp) f.reset();
            env = 0; g = 1; lastFreq = -1;
        }
        void process (juce::AudioBuffer<float>& b, float freq, float threshDb, float rangeDb)
        {
            if (freq != lastFreq)
            {
                lastFreq = freq;
                auto coef = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, juce::jmin ((double) freq, sr * 0.45), 0.707f);
                for (auto& f : hp) f.coefficients = coef;
            }
            const float thr = juce::Decibels::decibelsToGain (threshDb);
            const float att = std::exp (-1.0f / (0.0005f * (float) sr)), rel = std::exp (-1.0f / (0.06f * (float) sr));
            const int nCh = juce::jmin (b.getNumChannels(), numCh);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float hi[2] {}, lvl = 0.0f;
                for (int c = 0; c < nCh; ++c)
                {
                    hi[c] = hp[(size_t) c].processSample (b.getSample (c, i));
                    lvl = std::max (lvl, std::abs (hi[c]));
                }
                env = lvl > env ? lvl + (env - lvl) * att : lvl + (env - lvl) * rel;
                float red = 0.0f;
                if (env > thr) red = juce::jmin (rangeDb, juce::Decibels::gainToDecibels (env / thr) * 0.8f);
                const float target = juce::Decibels::decibelsToGain (-red);
                g = target + (g - target) * 0.995f;
                gainReductionDb = -red;
                for (int c = 0; c < nCh; ++c)
                    b.setSample (c, i, b.getSample (c, i) - hi[c] * (1.0f - g));   // low + high*g
            }
        }
        float gainReductionDb = 0.0f;
    private:
        double sr = 44100; std::array<juce::dsp::IIR::Filter<float>, 2> hp; int numCh = 2; float env = 0, g = 1, lastFreq = -1;
    };

    // --------------------------------------------------------- Transmission
    // Drive (tape/tube-style saturation), bit-crush + sample-rate reduction,
    // "radio" band-limiting and a ring modulator.
    class Transmission
    {
    public:
        void prepare (double sampleRate, int ch)
        {
            sr = sampleRate;
            numCh = juce::jlimit (1, 2, ch);
            for (auto& f : hp) f.reset();
            for (auto& f : lp) f.reset();
            hold.assign ((size_t) ch, 0.0f); holdCount = 0; ringPhase = 0; lastRadio = -1;
        }
        void process (juce::AudioBuffer<float>& b, float drive, float crush, float radio, float ringHz, float ringMix, float mix)
        {
            const int nCh = juce::jmin (b.getNumChannels(), numCh);
            if (radio > 0.001f && std::abs (radio - lastRadio) > 0.002f)
            {
                lastRadio = radio;
                const double hpF = 20.0 * std::pow (600.0 / 20.0, radio), lpF = 20000.0 * std::pow (2200.0 / 20000.0, radio);
                const float q = 0.7f + radio * 2.2f;
                auto h = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, hpF, q);
                auto l = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, juce::jmin (lpF, sr * 0.45), q);
                for (int c = 0; c < 2; ++c) { hp[(size_t) c].coefficients = h; lp[(size_t) c].coefficients = l; }
            }
            const float pre = 1.0f + 30.0f * drive * drive;
            const float comp = 1.0f / std::pow (pre, 0.7f);
            const float bits = 16.0f - crush * 13.0f;
            const float step = std::pow (2.0f, 1.0f - bits);
            const int dsFactor = 1 + (int) (crush * crush * 24.0f);
            const float ringInc = 2.0f * pi * ringHz / (float) sr;

            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const bool takeNew = (holdCount++ % dsFactor) == 0;
                const float ring = std::sin (ringPhase);
                ringPhase += ringInc; if (ringPhase > 2.0f * pi) ringPhase -= 2.0f * pi;
                for (int c = 0; c < nCh; ++c)
                {
                    const float x = b.getSample (c, i);
                    float y = x;
                    if (radio > 0.001f) y = lp[(size_t) c].processSample (hp[(size_t) c].processSample (y)) * (1.0f + radio);
                    if (drive > 0.001f) y = std::tanh (y * pre) * comp * (1.0f + drive);
                    if (crush > 0.001f)
                    {
                        if (takeNew) hold[(size_t) c] = std::round (y / step) * step;
                        y = hold[(size_t) c];
                    }
                    y = y * (1.0f - ringMix) + y * ring * ringMix * 1.4f;
                    b.setSample (c, i, x * (1.0f - mix) + y * mix);
                }
            }
        }
    private:
        double sr = 44100;
        std::array<juce::dsp::IIR::Filter<float>, 2> hp, lp; int numCh = 2;
        std::vector<float> hold; juce::int64 holdCount = 0; float ringPhase = 0, lastRadio = -1;
    };

    // ---------------------------------------------------------- Ring buffer
    struct Ring
    {
        std::vector<float> d; int mask = 0, w = 0;
        void prepare (int minSize) { int s = 1; while (s < minSize) s <<= 1; d.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; }
        void push (float x) { d[(size_t) w] = x; w = (w + 1) & mask; }
        float read (float delay) const   // delay in samples (>= 1), linear interpolation
        {
            float p = (float) w - delay;
            while (p < 0) p += (float) (mask + 1);
            const int i0 = (int) p; const float f = p - (float) i0;
            return d[(size_t) (i0 & mask)] * (1.0f - f) + d[(size_t) ((i0 + 1) & mask)] * f;
        }
    };

    // -------------------------------------------------------------- Doubler
    // Two slightly delayed, slowly detuned copies panned left/right (ADT).
    class Doubler
    {
    public:
        void prepare (double sampleRate) { sr = sampleRate; ring.prepare ((int) (sr * 0.2)); ph1 = 0; ph2 = 1.7f; }
        void process (juce::AudioBuffer<float>& b, float amount, float width, float detuneCents, float delayMs)
        {
            const int numCh = b.getNumChannels();
            const float f1 = 0.35f, f2 = 0.27f;
            auto ampFor = [&] (float f) { return detuneCents / (1731.0f * 2.0f * pi * f) * (float) sr; };   // samples
            const float a1 = ampFor (f1), a2 = ampFor (f2);
            const float base = delayMs * 0.001f * (float) sr;
            const float d1 = juce::jmax (base, a1 + 48.0f), d2 = juce::jmax (base * 1.37f, a2 + 48.0f);
            const float pan = 0.5f + 0.5f * width;              // 0.5 = centre, 1 = hard
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float m = 0.0f;
                for (int c = 0; c < numCh; ++c) m += b.getSample (c, i);
                m /= (float) numCh;
                ring.push (m);
                ph1 += 2.0f * pi * f1 / (float) sr; ph2 += 2.0f * pi * f2 / (float) sr;
                if (ph1 > 2 * pi) ph1 -= 2 * pi;
                if (ph2 > 2 * pi) ph2 -= 2 * pi;
                const float v1 = ring.read (d1 + a1 * std::sin (ph1));
                const float v2 = ring.read (d2 + a2 * std::sin (ph2));
                if (numCh == 1)
                    b.setSample (0, i, b.getSample (0, i) + amount * 0.5f * (v1 + v2));
                else
                {
                    b.setSample (0, i, b.getSample (0, i) + amount * 0.7f * (v1 * pan + v2 * (1.0f - pan)));
                    b.setSample (1, i, b.getSample (1, i) + amount * 0.7f * (v2 * pan + v1 * (1.0f - pan)));
                }
            }
        }
    private:
        double sr = 44100; Ring ring; float ph1 = 0, ph2 = 0;
    };

    // ---------------------------------------------------------------- Delay
    // Free or tempo-synced, stereo or ping-pong, forward or reversed echoes.
    class Delay
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            for (auto& r : rings) r.prepare ((int) (sr * 2.0 * 2.2) + 8);   // reverse needs 2x the delay time
            time.reset (sampleRate, 0.12); time.setCurrentAndTargetValue ((float) (sr * 0.3));
            lpState[0] = lpState[1] = 0; revPhase = 0;
        }
        void process (juce::AudioBuffer<float>& b, float delaySamples, float feedback, float mix, bool pingPong, bool reverse)
        {
            delaySamples = juce::jlimit (16.0f, (float) (sr * 2.0), delaySamples);
            time.setTargetValue (delaySamples);
            const float lpCoeff = std::exp (-2.0f * pi * 5500.0f / (float) sr);
            const int numCh = juce::jmin (2, b.getNumChannels());
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const float D = time.getNextValue();
                float wet[2] {};
                for (int c = 0; c < numCh; ++c)
                {
                    if (! reverse) wet[c] = rings[c].read (c == 1 && ! pingPong ? D * 1.03f : D);
                    else
                    {
                        // two read heads moving backwards through the last 2*D samples, cross-faded
                        const float p2 = std::fmod (revPhase + 0.5f, 1.0f);
                        const float g1 = std::sin (pi * revPhase), g2 = std::sin (pi * p2);
                        wet[c] = rings[c].read (1.0f + 2.0f * revPhase * D) * g1 * g1 + rings[c].read (1.0f + 2.0f * p2 * D) * g2 * g2;
                    }
                    lpState[c] = wet[c] + (lpState[c] - wet[c]) * lpCoeff;
                }
                if (reverse) { revPhase += 1.0f / D; if (revPhase >= 1.0f) revPhase -= 1.0f; }

                if (numCh == 2 && pingPong)
                {
                    const float inMono = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
                    rings[0].push (inMono + lpState[1] * feedback);
                    rings[1].push (lpState[0] * feedback);
                }
                else
                    for (int c = 0; c < numCh; ++c) rings[c].push (b.getSample (c, i) + lpState[c] * feedback);

                for (int c = 0; c < numCh; ++c) b.setSample (c, i, b.getSample (c, i) + wet[c] * mix);
            }
        }
    private:
        double sr = 44100; Ring rings[2]; juce::SmoothedValue<float> time; float lpState[2] {}; float revPhase = 0;
    };

    // --------------------------------------------------------------- Glitch
    // Beat-repeat: on each beat, with probability 'chance', the beat is replaced
    // by repeats of its first slice (optionally reversed). Tempo-synced.
    class Glitch
    {
    public:
        void prepare (double sampleRate, int ch)
        {
            sr = sampleRate;
            buf.assign ((size_t) juce::jmax (1, ch), std::vector<float> ((size_t) (sr * 6.0), 0.0f));
            pos = 0; active = false; lastBeat = -1; freePpq = 0; act = 0;
        }
        void process (juce::AudioBuffer<float>& b, const Tempo& tempo, int division, float chance, float reverseChance, float mix)
        {
            const int numCh = juce::jmin (b.getNumChannels(), (int) buf.size());
            const int size = (int) buf[0].size();
            const double beatsPerSample = tempo.bpm / 60.0 / sr;
            const int sliceLen = juce::jmax (64, (int) (divisionBeats (division) / beatsPerSample));
            const int eventLen = juce::jmax (sliceLen * 2, (int) (1.0 / beatsPerSample));   // one beat
            const int fade = (int) (0.0015 * sr);
            double ppq = tempo.playing ? tempo.ppq : freePpq;

            for (int i = 0; i < b.getNumSamples(); ++i, ppq += beatsPerSample)
            {
                for (int c = 0; c < numCh; ++c) buf[(size_t) c][(size_t) pos] = b.getSample (c, i);

                const juce::int64 beat = (juce::int64) std::floor (ppq);
                if (beat != lastBeat)
                {
                    lastBeat = beat;
                    active = rng.nextFloat() < chance;
                    reversed = rng.nextFloat() < reverseChance;
                    start = pos; offset = 0;
                }
                const float target = active ? 1.0f : 0.0f;
                act += (target - act) * 0.01f;

                if (active || act > 0.001f)
                {
                    const int o = offset++;
                    const int slice = o / sliceLen, inSlice = o % sliceLen;
                    float edge = 1.0f;
                    if (slice > 0) edge = juce::jmin (1.0f, juce::jmin ((float) inSlice / fade, (float) (sliceLen - 1 - inSlice) / fade));
                    const int readIdx = (slice > 0 && reversed) ? sliceLen - 1 - inSlice : inSlice;
                    const int rp = (start + readIdx) % size;
                    for (int c = 0; c < numCh; ++c)
                    {
                        const float g = buf[(size_t) c][(size_t) rp] * edge;
                        const float dry = b.getSample (c, i);
                        const float w = (o < eventLen ? act : 0.0f) * mix;
                        b.setSample (c, i, dry * (1.0f - w) + g * w);
                    }
                }
                pos = (pos + 1) % size;
            }
            freePpq = tempo.playing ? 0.0 : ppq;
            isActive = active;
        }
        bool isActive = false;
    private:
        double sr = 44100;
        std::vector<std::vector<float>> buf;
        int pos = 0, start = 0, offset = 0;
        bool active = false, reversed = false;
        juce::int64 lastBeat = -1;
        double freePpq = 0;
        float act = 0;
        juce::Random rng { 99 };
    };

    // ------------------------------------------------------- Safety limiter
    // Always on at the very end: catches level explosions from extreme settings
    // (stacked harmonies + drive + resonance ...). Instant attack, smooth release.
    class SafetyLimiter
    {
    public:
        void prepare (double sampleRate) { rel = std::exp (-1.0f / (0.15f * (float) sampleRate)); g = 1.0f; }
        void process (juce::AudioBuffer<float>& b)
        {
            const float ceiling = 0.944f;   // -0.5 dBFS
            float minG = 1.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float pk = 0.0f;
                for (int c = 0; c < b.getNumChannels(); ++c) pk = std::max (pk, std::abs (b.getSample (c, i)));
                const float target = pk > ceiling ? ceiling / pk : 1.0f;
                g = target < g ? target : target + (g - target) * rel;
                minG = std::min (minG, g);
                for (int c = 0; c < b.getNumChannels(); ++c)
                {
                    float y = b.getSample (c, i) * g;
                    if (! std::isfinite (y)) y = 0.0f;
                    b.setSample (c, i, juce::jlimit (-1.0f, 1.0f, y));
                }
            }
            reductionDb = juce::Decibels::gainToDecibels (minG);
        }
        float reductionDb = 0.0f;
    private:
        float rel = 0.999f, g = 1.0f;
    };

    // ------------------------------------------------------------------ LFO
    class LFO
    {
    public:
        static juce::StringArray shapeNames() { return { "Sine", "Triangle", "Saw", "Square", "S&H", "Drift" }; }
        static juce::StringArray syncNames()  { return { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/4 T", "1/8 T" }; }
        static double syncBeats (int i) { static const double b[] = { 16, 8, 4, 2, 1, 0.5, 0.25, 2.0 / 3.0, 1.0 / 3.0 }; return b[juce::jlimit (0, 8, i)]; }

        // advance by one block and return a value in -1..1
        float next (int shape, double rateHz, bool sync, int syncIdx, const Tempo& t, int numSamples, double sr)
        {
            double newPhase;
            if (sync && t.playing) newPhase = std::fmod (t.ppq / syncBeats (syncIdx), 1.0);
            else
            {
                const double hz = sync ? t.bpm / 60.0 / syncBeats (syncIdx) : rateHz;
                newPhase = phase + hz * numSamples / sr;
            }
            if (newPhase >= 1.0 || newPhase < phase) { prevRnd = rnd; rnd = rng.nextFloat() * 2.0f - 1.0f; }
            phase = newPhase - std::floor (newPhase);
            const float p = (float) phase;
            switch (shape)
            {
                case 0:  return std::sin (2.0f * pi * p);
                case 1:  return 1.0f - 4.0f * std::abs (p - 0.5f);
                case 2:  return 2.0f * p - 1.0f;
                case 3:  return p < 0.5f ? 1.0f : -1.0f;
                case 4:  return rnd;
                default: { const float s = p * p * (3 - 2 * p); return prevRnd + (rnd - prevRnd) * s; }
            }
        }
        float lastPhase() const { return (float) phase; }
    private:
        double phase = 0; float rnd = 0, prevRnd = 0; juce::Random rng { 7 };
    };
}
