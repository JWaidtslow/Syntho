#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include <vector>

// ============================================================================
//  PitchCorrector  -  "auto-tune" style pitch correction
//
//  1. Detects the sung pitch with the YIN algorithm (on a mono sum).
//  2. Snaps it to the nearest note of the chosen key/scale.
//  3. Glides toward that note at "retune speed" and shifts the audio with a
//     two-tap crossfading delay-line pitch shifter.
//  Latency is constant (half the shifter window) and is reported to the host.
// ============================================================================
class LegacyPitchCorrector
{
public:
    static constexpr int numScales = 6;
    static juce::StringArray scaleNames() { return { "Chromatic", "Major", "Minor", "Maj Penta", "Min Penta", "Blues" }; }
    static juce::StringArray keyNames()   { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;

        // --- pitch detector (70 Hz .. 1000 Hz)
        tauMax = (int) (sr / 70.0);
        tauMin = juce::jmax (2, (int) (sr / 1000.0));
        yinWindow = tauMax;
        frameSize = yinWindow + tauMax;
        analysisRing.assign ((size_t) frameSize, 0.0f);
        frame.assign ((size_t) frameSize, 0.0f);
        diff.assign ((size_t) tauMax + 1, 0.0f);
        hopSize = juce::jmax (128, (int) (sr * 0.008));
        ringPos = hopCounter = 0;

        // --- shifter
        window = (float) std::round (sr * 0.030);              // 30 ms grain
        int size = 1;
        while (size < (int) window * 2 + 8) size <<= 1;
        bufferMask = size - 1;
        buffers.assign ((size_t) numChannels, std::vector<float> ((size_t) size, 0.0f));
        writePos = 0;
        phase = 0.5f;

        currentShift = 0.0f;
        targetShift = 0.0f;
        lastTargetMidi = -1;
        voiced = false;
    }

    int getLatencySamples() const { return (int) std::round (1.0f + window * 0.5f); }

    void setParameters (int key, int scale, float speedMs, float amount01, float mix01)
    {
        keyIndex = key; scaleIndex = scale; retuneMs = speedMs; amount = amount01; mix = mix01;
    }

    // Processes the buffer in place. If 'enabled' is false the audio is only
    // delayed (so latency stays constant whether the module is on or off).
    void process (juce::AudioBuffer<float>& buffer, bool enabled)
    {
        const int numCh = juce::jmin (buffer.getNumChannels(), (int) buffers.size());
        const int n = buffer.getNumSamples();
        const float smoothCoeff = retuneMs <= 0.5f ? 0.0f
                                : std::exp (-1.0f / (retuneMs * 0.001f * (float) sr));
        const float halfWin = 1.0f + window * 0.5f;

        for (int i = 0; i < n; ++i)
        {
            // ---- analysis
            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c) mono += buffer.getSample (c, i);
            mono /= (float) juce::jmax (1, numCh);
            analysisRing[(size_t) ringPos] = mono;
            ringPos = (ringPos + 1) % frameSize;
            if (++hopCounter >= hopSize) { hopCounter = 0; analyse(); }

            // ---- glide toward the target
            const float goal = (enabled && voiced) ? targetShift * amount : 0.0f;
            currentShift = goal + (currentShift - goal) * smoothCoeff;
            const float ratio = std::pow (2.0f, currentShift / 12.0f);

            // ---- write input
            for (int c = 0; c < numCh; ++c)
                buffers[(size_t) c][(size_t) writePos] = buffer.getSample (c, i);

            if (! enabled)
            {
                for (int c = 0; c < numCh; ++c)
                    buffer.setSample (c, i, readDelayed (c, halfWin));
            }
            else
            {
                // advance the grain phase; when no shift is needed drift slowly
                // back to the centre so only one tap is heard (no comb filtering)
                float inc = (1.0f - ratio) / window;
                if (std::abs (ratio - 1.0f) < 0.0005f)
                {
                    const float off = phase - 0.5f;
                    if (std::abs (off) > 0.002f) inc = (off > 0 ? -1.0f : 1.0f) * 0.004f / window;
                }
                phase += inc;
                phase -= std::floor (phase);

                const float p2 = std::fmod (phase + 0.5f, 1.0f);
                const float g1 = std::sin (juce::MathConstants<float>::pi * phase);
                const float g2 = std::sin (juce::MathConstants<float>::pi * p2);
                const float w1 = g1 * g1, w2 = g2 * g2;
                const float d1 = 1.0f + phase * window, d2 = 1.0f + p2 * window;

                for (int c = 0; c < numCh; ++c)
                    buffer.setSample (c, i, mix * (readDelayed (c, d1) * w1 + readDelayed (c, d2) * w2)
                                            + (1.0f - mix) * readDelayed (c, halfWin));
            }

            writePos = (writePos + 1) & bufferMask;
        }
    }

    // --- read-outs for the UI (written on audio thread, read on UI thread)
    std::atomic<float> detectedHz { 0.0f };
    std::atomic<int>   targetNote { -1 };
    std::atomic<float> correctionCents { 0.0f };

private:
    float readDelayed (int ch, float delay) const
    {
        const auto& b = buffers[(size_t) ch];
        float pos = (float) writePos - delay;
        while (pos < 0) pos += (float) (bufferMask + 1);
        const int i0 = (int) pos;
        const float f = pos - (float) i0;
        return b[(size_t) (i0 & bufferMask)] * (1.0f - f) + b[(size_t) ((i0 + 1) & bufferMask)] * f;
    }

    bool noteInScale (int midi) const
    {
        static const std::array<std::array<int, 12>, numScales> masks {{
            {{1,1,1,1,1,1,1,1,1,1,1,1}},   // chromatic
            {{1,0,1,0,1,1,0,1,0,1,0,1}},   // major
            {{1,0,1,1,0,1,0,1,1,0,1,0}},   // natural minor
            {{1,0,1,0,1,0,0,1,0,1,0,0}},   // major pentatonic
            {{1,0,0,1,0,1,0,1,0,0,1,0}},   // minor pentatonic
            {{1,0,0,1,0,1,1,1,0,0,1,0}},   // blues
        }};
        const int degree = ((midi - keyIndex) % 12 + 12) % 12;
        return masks[(size_t) juce::jlimit (0, numScales - 1, scaleIndex)][(size_t) degree] != 0;
    }

    int nearestScaleNote (float midi) const
    {
        const int base = (int) std::round (midi);
        for (int d = 0; d <= 6; ++d)
        {
            const int up = base + d, down = base - d;
            const bool okUp = noteInScale (up), okDown = noteInScale (down);
            if (okUp && okDown) return std::abs (up - midi) < std::abs (down - midi) ? up : down;
            if (okUp) return up;
            if (okDown) return down;
        }
        return base;
    }

    void analyse()
    {
        // unroll ring into a linear frame (oldest sample first)
        double energy = 0.0;
        for (int j = 0; j < frameSize; ++j)
        {
            frame[(size_t) j] = analysisRing[(size_t) ((ringPos + j) % frameSize)];
            energy += frame[(size_t) j] * frame[(size_t) j];
        }
        const float rms = (float) std::sqrt (energy / frameSize);

        float hz = 0.0f;
        if (rms > 0.003f)   // about -50 dBFS gate
            hz = yin();

        if (hz <= 0.0f)
        {
            voiced = false;
            detectedHz.store (0.0f);
            targetNote.store (-1);
            correctionCents.store (0.0f);
            return;
        }

        const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
        int target = nearestScaleNote (midi);

        // hysteresis: stick with the previous note unless clearly closer to a new one
        if (lastTargetMidi >= 0 && target != lastTargetMidi && noteInScale (lastTargetMidi)
            && std::abs (midi - (float) lastTargetMidi) < std::abs (midi - (float) target) + 0.15f)
            target = lastTargetMidi;

        lastTargetMidi = target;
        targetShift = (float) target - midi;
        voiced = true;

        detectedHz.store (hz);
        targetNote.store (target);
        correctionCents.store (targetShift * 100.0f);
    }

    float yin()
    {
        const float* x = frame.data();
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            float s = 0.0f;
            const float* y = x + tau;
            for (int j = 0; j < yinWindow; ++j) { const float d = x[j] - y[j]; s += d * d; }
            diff[(size_t) tau] = s;
        }

        // cumulative mean normalised difference
        diff[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += diff[(size_t) tau];
            diff[(size_t) tau] = running > 0.0f ? diff[(size_t) tau] * (float) tau / running : 1.0f;
        }

        constexpr float threshold = 0.15f;
        int tau = tauMin;
        for (; tau < tauMax; ++tau)
        {
            if (diff[(size_t) tau] < threshold)
            {
                while (tau + 1 < tauMax && diff[(size_t) tau + 1] < diff[(size_t) tau]) ++tau;
                break;
            }
        }
        if (tau >= tauMax) return 0.0f;

        // parabolic interpolation for sub-sample accuracy
        float better = (float) tau;
        if (tau > 1 && tau < tauMax)
        {
            const float a = diff[(size_t) tau - 1], b = diff[(size_t) tau], c = diff[(size_t) tau + 1];
            const float denom = a - 2.0f * b + c;
            if (std::abs (denom) > 1e-9f) better += 0.5f * (a - c) / denom;
        }
        return (float) sr / better;
    }

    double sr = 44100.0;
    int keyIndex = 0, scaleIndex = 0;
    float retuneMs = 20.0f, amount = 1.0f, mix = 1.0f;

    // detector
    int tauMin = 2, tauMax = 600, yinWindow = 600, frameSize = 1200, hopSize = 256;
    int ringPos = 0, hopCounter = 0;
    std::vector<float> analysisRing, frame, diff;
    bool voiced = false;
    int lastTargetMidi = -1;
    float targetShift = 0.0f, currentShift = 0.0f;

    // shifter
    std::vector<std::vector<float>> buffers;
    int writePos = 0, bufferMask = 0;
    float window = 1323.0f, phase = 0.5f;
};
