#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>

// ============================================================================
//  Analysers: key + scale detection (from sung pitch) and tempo (BPM) detection
//  (from the rhythm of the vocal). Both run on the audio thread, cheaply.
// ============================================================================
namespace analysis
{
    // Scale indices match PitchCorrector::scaleNames():
    // 0 Chromatic, 1 Major, 2 Minor, 3 Maj Penta, 4 Min Penta, 5 Blues
    struct KeyResult
    {
        int key = -1, scale = 1;            // key -1 = not enough data yet
        int altKey = -1, altScale = 1;      // second-best candidate
        float confidence = 0.0f;            // 0..1
        float seconds = 0.0f;               // seconds of sung pitch analysed
    };

    // ------------------------------------------------------------------------
    // Key / scale: a pitch-class histogram of the sung notes (weighted by time)
    // is correlated with Krumhansl-Kessler major/minor key profiles; the result
    // is then refined to pentatonic / blues when the "missing" notes are absent.
    // ------------------------------------------------------------------------
    class KeyDetector
    {
    public:
        void reset() { hist.fill (0.0); total = 0.0; }

        // midi = note centre (vibrato removed), seconds = how long it was sung
        void add (float midi, float seconds)
        {
            if (! std::isfinite (midi) || midi < 20.0f || midi > 110.0f) return;
            // spread the weight between the two nearest semitones (robust to out-of-tune singing)
            const float m = midi - std::floor (midi);
            const int lo = ((int) std::floor (midi) % 12 + 12) % 12, hi = (lo + 1) % 12;
            hist[(size_t) lo] += seconds * (1.0 - m) * (1.0 - m) / ((1.0 - m) * (1.0 - m) + m * m);
            hist[(size_t) hi] += seconds * m * m / ((1.0 - m) * (1.0 - m) + m * m);
            total += seconds;
        }

        KeyResult analyse() const
        {
            KeyResult r;
            r.seconds = (float) total;
            if (total < 3.0) return r;   // need a few seconds of singing

            static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
            static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
            struct Cand { double score; int key, mode; };
            std::array<Cand, 24> c {};
            for (int k = 0; k < 12; ++k)
                for (int mode = 0; mode < 2; ++mode)
                    c[(size_t) (k * 2 + mode)] = { correlate (mode == 0 ? maj : mnr, k), k, mode };
            std::sort (c.begin(), c.end(), [] (const Cand& a, const Cand& b) { return a.score > b.score; });

            r.key = c[0].key;
            r.scale = refine (c[0].key, c[0].mode);
            r.altKey = c[1].key;
            r.altScale = refine (c[1].key, c[1].mode);
            // confidence: how clearly the best key beats the runner-up, and how much we heard
            const double margin = juce::jlimit (0.0, 1.0, (c[0].score - c[1].score) / 0.15);
            const double fit = juce::jlimit (0.0, 1.0, (c[0].score - 0.3) / 0.5);
            const double amount = juce::jlimit (0.0, 1.0, total / 15.0);
            r.confidence = (float) (0.5 * fit + 0.5 * margin) * (float) (0.4 + 0.6 * amount);
            return r;
        }

        const std::array<double, 12>& histogram() const { return hist; }

    private:
        double correlate (const double* profile, int key) const
        {
            double mx = 0, my = 0;
            for (int i = 0; i < 12; ++i) { mx += hist[(size_t) i]; my += profile[(i - key + 12) % 12]; }
            mx /= 12; my /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i)
            {
                const double x = hist[(size_t) i] - mx, y = profile[(i - key + 12) % 12] - my;
                sxy += x * y; sxx += x * x; syy += y * y;
            }
            return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
        }

        double share (int key, std::initializer_list<int> degrees) const
        {
            double s = 0;
            for (int d : degrees) s += hist[(size_t) ((key + d) % 12)];
            return total > 0 ? s / total : 0.0;
        }

        int refine (int key, int mode) const
        {
            if (mode == 0)   // major: pentatonic if the 4th and 7th are (almost) never sung
                return share (key, { 5, 11 }) < 0.06 ? 3 : 1;
            // minor: pentatonic if the 2nd and b6 are missing; blues if the b5 is also there
            if (share (key, { 2, 8 }) < 0.06)
                return share (key, { 6 }) > 0.08 ? 5 : 4;
            return 2;
        }

        std::array<double, 12> hist {};
        double total = 0.0;
    };

    // ------------------------------------------------------------------------
    // Tempo: an onset-strength envelope (rises in loudness and in high-frequency
    // energy = syllable starts / consonants) at 100 frames per second, over the
    // last 8 seconds, is autocorrelated. The best periodicity between 60 and
    // 200 BPM (with a mild preference for 80-160) is the tempo.
    // ------------------------------------------------------------------------
    class BpmDetector
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            hop = juce::jmax (32, (int) std::round (sr / framesPerSecond));
            env.assign ((size_t) envSize, 0.0f);
            r.assign ((size_t) (2 * maxLagFrames + 2), 0.0);
            reset();
        }
        void reset()
        {
            std::fill (env.begin(), env.end(), 0.0f);
            write = 0; count = 0; acc = accHf = 0.0; prevLog = prevLogHf = 0.0f; prevX = 0.0f;
            sinceAnalysis = 0; numHistory = 0; bpm.store (0.0f); confidence.store (0.0f);
        }

        void process (const float* mono, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const float x = mono[i];
                const float d = x - prevX;  prevX = x;           // crude high-pass (emphasises consonants)
                acc += (double) x * x; accHf += (double) d * d;
                if (++count < hop) continue;
                count = 0;
                const float lg = std::log10 ((float) (acc / hop) + 1e-9f), lh = std::log10 ((float) (accHf / hop) + 1e-10f);
                const float on = juce::jmax (0.0f, lg - prevLog) + 0.7f * juce::jmax (0.0f, lh - prevLogHf);
                prevLog = lg; prevLogHf = lh;
                // ignore silence: only count onsets when there is some signal
                env[(size_t) write] = lg > -5.5f ? on : 0.0f;
                write = (write + 1) % envSize;
                acc = accHf = 0.0;
                if (++sinceAnalysis >= framesPerSecond / 2) { sinceAnalysis = 0; analyse(); }
            }
        }

        std::atomic<float> bpm { 0.0f }, confidence { 0.0f };
        int dbgActive = 0; float dbgBest = 0, dbgRatio = 0;   // diagnostics for the test bench

    private:
        void analyse()
        {
            // linearise the ring (oldest first), remove the mean
            std::array<float, envSize> e {};
            double mean = 0; int onsets = 0;
            for (int i = 0; i < envSize; ++i)
            {
                // light smoothing (3 frames) so slightly uneven onsets still line up
                const float a = env[(size_t) ((write + i - 1 + envSize) % envSize)], b = env[(size_t) ((write + i) % envSize)],
                            c = env[(size_t) ((write + i + 1) % envSize)];
                e[(size_t) i] = 0.25f * a + 0.5f * b + 0.25f * c;
                mean += e[(size_t) i];
                if (b > 0.08f && b >= a && b >= c) ++onsets;      // count distinct onset peaks
            }
            dbgActive = onsets;
            if (onsets < 8) return;                               // not enough rhythmic material
            mean /= envSize;
            for (auto& v : e) v -= (float) mean;

            auto ac = [&] (int lag)
            {
                double s = 0;
                for (int i = lag; i < envSize; ++i) s += (double) e[(size_t) i] * e[(size_t) (i - lag)];
                return s / (envSize - lag);
            };
            const double r0 = ac (0);
            if (r0 <= 0) return;

            const int minLag = (int) std::floor (60.0 * framesPerSecond / 200.0);   // 200 BPM
            const int maxLag = maxLagFrames;                                         // 60 BPM
            std::fill (r.begin(), r.end(), 0.0);
            for (int L = 1; L <= 2 * maxLag + 1 && L < envSize / 2; ++L) r[(size_t) L] = ac (L);

            int best = -1; double bestScore = -1e9;
            for (int L = minLag; L <= maxLag; ++L)
            {
                const double bpmL = 60.0 * framesPerSecond / L;
                // preference for common song tempi (centre 115 BPM, +-half an octave)
                const double prior = std::exp (-0.5 * std::pow (std::log2 (bpmL / 115.0) / 0.5, 2.0));
                // a real beat also repeats at 2x the lag, and usually has subdivisions at half the lag
                const double s = (r[(size_t) L] + 0.5 * r[(size_t) juce::jmin (2 * L, (int) r.size() - 1)]
                                  + 0.3 * juce::jmax (0.0, r[(size_t) (L / 2)])) * prior;
                if (s > bestScore) { bestScore = s; best = L; }
            }
            if (best < 0) return;
            double lag = best;
            if (best > minLag && best < maxLag)
            {
                const double a = r[(size_t) best - 1], b = r[(size_t) best], c = r[(size_t) best + 1];
                const double den = a - 2 * b + c;
                if (std::abs (den) > 1e-12) lag += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / den);
            }
            const float estimate = (float) (60.0 * framesPerSecond / lag);
            const float conf = (float) juce::jlimit (0.0, 1.0, r[(size_t) best] / r0 * 2.5);
            dbgBest = (float) (60.0 * framesPerSecond / lag); dbgRatio = (float) (r[(size_t) best] / r0);
            if (conf < 0.06f) return;

            // median of the last estimates (octave-folded to the first one) = stable read-out
            if (numHistory < (int) history.size()) history[(size_t) numHistory++] = estimate;
            else { std::rotate (history.begin(), history.begin() + 1, history.end()); history.back() = estimate; }
            auto sorted = history;
            std::sort (sorted.begin(), sorted.begin() + numHistory);
            bpm.store (sorted[(size_t) (numHistory / 2)]);
            confidence.store (conf);
        }

        static constexpr int framesPerSecond = 100, envSize = 800;   // 8 s window
        static constexpr int maxLagFrames = 100;                       // 60 BPM
        double sr = 44100;
        int hop = 441, write = 0, count = 0, sinceAnalysis = 0;
        std::vector<float> env;
        double acc = 0, accHf = 0;
        float prevLog = 0, prevLogHf = 0, prevX = 0;
        std::array<float, 7> history {}; int numHistory = 0;
        std::vector<double> r;
    };
}
