#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// ============================================================================
//  PitchCorrector  -  "auto-tune" style real-time pitch correction
//
//  DETECT   YIN pitch detector (FFT-accelerated) on a mono sum, with octave-
//           error protection, median smoothing and voiced/unvoiced decision.
//  DECIDE   Snaps the *note centre* (pitch with vibrato filtered out) to the
//           key/scale, then applies Retune Speed, Humanize, Flex-Tune and
//           Natural-Vibrato rules to get a shift curve.
//  SHIFT    TD-PSOLA: the voice is cut into grains centred on each vocal-cord
//           pulse ("pitch marks") and re-spaced at the target period. This
//           keeps the singer's formants (no chipmunk effect). A separate
//           Formant control resamples each grain to move formants on purpose.
//           Unvoiced sounds (s, t, breaths) pass through unshifted.
//
//  HARMONY Up to 3 extra voices (scale-aware intervals or notes held on a MIDI
//           keyboard) plus sub-octave and octave-up layers, all synthesised
//           from the same pitch marks and mixed onto a stereo bus.
//  MIDI     Optional: notes held on a MIDI keyboard become the correction targets.
//
//  Latency is constant (getLatencySamples) and reported to the host.
// ============================================================================
class PitchCorrector
{
public:
    static constexpr int numScales = 6;
    static juce::StringArray scaleNames() { return { "Chromatic", "Major", "Minor", "Maj Penta", "Min Penta", "Blues" }; }
    static juce::StringArray keyNames()   { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

    struct Settings
    {
        int   key = 0, scale = 0;
        float retuneMs = 20.0f;     // 0 = instant ("robot")
        float humanize = 0.0f;      // 0..1  slower correction on sustained notes
        float flex = 0.0f;          // 0..1  only correct notes already close to target
        float vibrato = 1.0f;       // 0..1  how much natural vibrato to keep
        float formantSemis = 0.0f;  // -5..+5
        float transpose = 0.0f;     // -12..+12 semitones (applied on top of correction)
        float mix = 1.0f;           // 0..1
        bool  midiTarget = false;   // held MIDI notes are the targets

        // harmony
        std::array<int, 3> harmVoice { 0, 0, 0 };   // index into harmonyChoices()
        float harmMix = 0.6f;       // 0..1 level of the 3 harmony voices
        float harmWidth = 0.7f;     // 0..1 stereo spread
        float harmDetune = 8.0f;    // cents of random-ish drift between voices
        float harmFormant = 0.0f;   // semitones, applied to all extra voices
        float subLevel = 0.0f;      // 0..1 octave-down layer
        float upLevel = 0.0f;       // 0..1 octave-up layer
    };

    static juce::StringArray harmonyChoices()
    {
        return { "Off", "-Oct", "-6th", "-5th", "-4th", "-3rd", "+3rd", "+4th", "+5th", "+6th", "+Oct", "MIDI" };
    }
    static constexpr int numExtra = 5;   // harm 1..3, sub, up

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate;

        tauMax = (int) std::ceil (sr / minF0);
        tauMin = juce::jmax (2, (int) std::floor (sr / maxF0));
        yinW   = tauMax;
        frameF = yinW + tauMax;
        hop    = juce::jmax (64, (int) std::round (sr * 0.004));
        uvSpacing = juce::jmax (32, (int) std::round (sr * 0.005));
        latency = (int) std::ceil (3.6 * tauMax) + 32;

        int fftOrder = 1;
        while ((1 << fftOrder) < frameF + yinW) ++fftOrder;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
        fftN = 1 << fftOrder;
        fa.assign ((size_t) fftN * 2, 0.0f);
        fb.assign ((size_t) fftN * 2, 0.0f);
        frame.assign ((size_t) frameF, 0.0f);
        prefix.assign ((size_t) frameF + 1, 0.0);
        cmnd.assign ((size_t) tauMax + 2, 1.0f);

        int size = 1;
        while (size < 4 * (latency + frameF + 4 * tauMax)) size <<= 1;
        mask = size - 1;
        mono.assign ((size_t) size, 0.0f);
        lowp.assign ((size_t) size, 0.0f);
        inRing.assign ((size_t) numChannels, std::vector<float> ((size_t) size, 0.0f));
        outRing.assign ((size_t) numChannels, std::vector<float> ((size_t) size, 0.0f));
        harmRing.assign (2, std::vector<float> ((size_t) size, 0.0f));

        reset();
    }

    void reset()
    {
        for (auto& b : inRing)  std::fill (b.begin(), b.end(), 0.0f);
        for (auto& b : outRing) std::fill (b.begin(), b.end(), 0.0f);
        for (auto& b : harmRing) std::fill (b.begin(), b.end(), 0.0f);
        std::fill (mono.begin(), mono.end(), 0.0f);
        std::fill (lowp.begin(), lowp.end(), 0.0f);
        n = 0; hopCounter = 0; lp1 = lp2 = 0.0f;
        estCount = 0;
        markCount = 0;
        addMark (0, (float) uvSpacing, false);
        v = 0.0;
        vx.fill (0.0);
        prevPeriod = 0.0f; voicedState = false; octavePending = 0; uvTime = 1.0f;
        centre1 = centre2 = centreAge = 0.0f; jumpCount = pendingCount = 0;
        lastTarget = -1; noteAge = 0.0f;
        corrSm = 0.0f; flatW = 0.0f;
        wetGain = 0.0f;
        lateSamples = 0;
        for (int k = 0; k < historySize; ++k) { histIn[(size_t) k] = -1.0f; histOut[(size_t) k] = -1.0f; histTarget[(size_t) k] = -1.0f; }
    }

    int getLatencySamples() const { return latency; }
    void setSettings (const Settings& s) { set = s; }

    // Note-centre tracker tuning (exposed for the test bench)
    struct Tracker { float jumpSemis = 0.6f; int jumpHops = 3; float baseHz = 1.2f; };
    inline static Tracker tracker { 0.6f, 3, 1.2f };

    // Called once per block from the audio thread with the currently held MIDI notes.
    void setHeldNotes (const std::array<bool, 128>& held)
    {
        numHeld = 0;
        for (int i = 0; i < 128; ++i)
            if (held[(size_t) i] && numHeld < 8) heldNotes[(size_t) numHeld++] = i;
    }

    void process (juce::AudioBuffer<float>& buffer, bool enabled, bool harmonyEnabled = false)
    {
        harmOn = harmonyEnabled;
        const int numCh = juce::jmin (buffer.getNumChannels(), (int) inRing.size());
        const int len = buffer.getNumSamples();
        const float lpA = std::exp (-2.0f * juce::MathConstants<float>::pi * 900.0f / (float) sr);
        const float wetTarget = enabled ? set.mix : 0.0f;
        const float wetStep = 1.0f / (0.02f * (float) sr);

        for (int i = 0; i < len; ++i)
        {
            // ---- write input
            float m = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                const float x = buffer.getSample (c, i);
                inRing[(size_t) c][(size_t) (n & mask)] = x;
                m += x;
            }
            m /= (float) juce::jmax (1, numCh);
            mono[(size_t) (n & mask)] = m;
            lp1 = m + (lp1 - m) * lpA;
            lp2 = lp1 + (lp2 - lp1) * lpA;
            lowp[(size_t) (n & mask)] = lp2;
            ++n;   // samples [0, n) are now available

            if (n >= frameF && ++hopCounter >= hop) { hopCounter = 0; detect(); }

            placeMarks();
            synthesise (numCh);
            for (int k = 0; k < numExtra; ++k) synthesiseExtra (k);

            // ---- read output (delayed by 'latency')
            const juce::int64 t = n - 1 - latency;
            wetGain += juce::jlimit (-wetStep, wetStep, wetTarget - wetGain);
            harmGain += juce::jlimit (-wetStep, wetStep, (harmOn ? 1.0f : 0.0f) - harmGain);
            float hl = 0.0f, hr = 0.0f;
            if (t >= 0)
            {
                auto& l = harmRing[0][(size_t) (t & mask)];
                auto& r = harmRing[1][(size_t) (t & mask)];
                hl = l * harmGain; hr = r * harmGain; l = r = 0.0f;
            }
            for (int c = 0; c < numCh; ++c)
            {
                float y = 0.0f, dry = 0.0f;
                if (t >= 0)
                {
                    auto& o = outRing[(size_t) c][(size_t) (t & mask)];
                    y = o; o = 0.0f;
                    dry = inRing[(size_t) c][(size_t) (t & mask)];
                }
                const float h = numCh == 1 ? 0.5f * (hl + hr) : (c == 0 ? hl : hr);
                buffer.setSample (c, i, wetGain * y + (1.0f - wetGain) * dry + h);
            }
        }
    }

    // ---------------- read-outs for the UI (audio thread writes, UI reads)
    std::atomic<float> detectedHz { 0.0f };
    std::atomic<int>   targetNote { -1 };
    std::atomic<float> correctionCents { 0.0f };

    static constexpr int historySize = 256;
    std::array<std::atomic<float>, historySize> histIn {}, histOut {}, histTarget {};   // MIDI notes, <0 = silence
    std::atomic<int> histWrite { 0 };

    // diagnostics (used by the test tool)
    juce::int64 lateSamples = 0;

private:
    // ======================================================================
    //  DETECTION
    // ======================================================================
    struct Estimate
    {
        juce::int64 t = 0; float period = 0; bool voiced = false; float shift = 0;
        std::array<float, numExtra> xShift {};   // semitones relative to the input, per extra voice
        std::array<bool, numExtra>  xActive {};
    };

    void detect()
    {
        const juce::int64 start = n - frameF;
        double e = 0.0;
        int zc = 0;
        prefix[0] = 0.0;
        for (int j = 0; j < frameF; ++j)
        {
            const float x = mono[(size_t) ((start + j) & mask)];
            frame[(size_t) j] = x;
            prefix[(size_t) j + 1] = prefix[(size_t) j] + (double) x * x;
            if (j > 0 && ((x >= 0) != (frame[(size_t) j - 1] >= 0))) ++zc;
        }
        e = prefix[(size_t) frameF];
        const float rms = (float) std::sqrt (e / frameF);
        const float zcRate = (float) zc * (float) sr / (float) frameF;

        float period = 0.0f, quality = 1.0f;
        if (rms > 0.001f)                  // ~ -60 dBFS gate
            yin (period, quality);

        // voiced / unvoiced decision with hysteresis
        const float qLimit = voicedState ? 0.30f : 0.18f;
        bool voiced = period > 0.0f && quality < qLimit && zcRate < 6000.0f;

        if (voiced)
        {
            // octave-error protection: an octave jump relative to the previous period is
            // only accepted once it has lasted ~24 ms (real octave leaps do, detector errors don't)
            if (prevPeriod > 0.0f)
            {
                const float r = period / prevPeriod;
                const bool octUp = std::abs (r - 0.5f) < 0.04f, octDown = std::abs (r - 2.0f) < 0.12f;
                if ((octUp || octDown) && ++octavePending < 6)
                    period = octUp ? period * 2.0f : period * 0.5f;
                else if (! (octUp || octDown))
                    octavePending = 0;
            }
            prevPeriod = period;
        }
        voicedState = voiced;

        Estimate est;
        // YIN compares x[j] with x[j+T] for j < W, so the measurement is centred here:
        est.t = n - frameF + (juce::int64) ((yinW + (voiced ? period : (float) yinW)) * 0.5f);
        est.voiced = voiced;
        est.period = voiced ? period : (float) uvSpacing;
        est.shift  = decide (voiced, period, est);

        estimates[(size_t) (estCount % estRing)] = est;
        ++estCount;
    }

    float cmndAt (float tau) const
    {
        const int t = juce::jlimit (1, tauMax, (int) std::round (tau));
        float best = cmnd[(size_t) t];
        for (int d = -2; d <= 2; ++d)
            best = std::min (best, cmnd[(size_t) juce::jlimit (1, tauMax, t + d)]);
        return best;
    }

    void yin (float& periodOut, float& qualityOut)
    {
        // cross-correlation c(tau) = sum_{j<W} x[j] x[j+tau] via FFT
        std::fill (fa.begin(), fa.end(), 0.0f);
        std::fill (fb.begin(), fb.end(), 0.0f);
        for (int j = 0; j < yinW; ++j)   fa[(size_t) j] = frame[(size_t) j];
        for (int j = 0; j < frameF; ++j) fb[(size_t) j] = frame[(size_t) j];
        fft->performRealOnlyForwardTransform (fa.data());
        fft->performRealOnlyForwardTransform (fb.data());
        for (int k = 0; k <= fftN / 2; ++k)
        {
            const float ar = fa[(size_t) 2 * k], ai = fa[(size_t) 2 * k + 1];
            const float br = fb[(size_t) 2 * k], bi = fb[(size_t) 2 * k + 1];
            fb[(size_t) 2 * k]     = ar * br + ai * bi;   // conj(A) * B
            fb[(size_t) 2 * k + 1] = ar * bi - ai * br;
        }
        fft->performRealOnlyInverseTransform (fb.data());

        const double ea = prefix[(size_t) yinW];
        if (ea <= 1e-12 || std::abs (fb[0]) < 1e-20f) { periodOut = 0; return; }
        const double scale = ea / fb[0];   // makes the result independent of the FFT's scaling convention

        // difference function -> cumulative mean normalised difference
        double running = 0.0;
        cmnd[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            const double eb = prefix[(size_t) (tau + yinW)] - prefix[(size_t) tau];
            const double d = juce::jmax (0.0, ea + eb - 2.0 * scale * fb[(size_t) tau]);
            running += d;
            cmnd[(size_t) tau] = running > 0.0 ? (float) (d * tau / running) : 1.0f;
        }

        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)   // no clear dip: take global minimum (may still be judged unvoiced)
        {
            float mn = 10.0f;
            for (int tau = tauMin; tau < tauMax; ++tau)
                if (cmnd[(size_t) tau] < mn) { mn = cmnd[(size_t) tau]; best = tau; }
        }
        if (best <= tauMin || best >= tauMax) { periodOut = 0; return; }

        const float a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
        const float den = a - 2.0f * b + c;
        const float off = std::abs (den) > 1e-9f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / den) : 0.0f;
        periodOut = (float) best + off;
        qualityOut = b;
    }

    // ======================================================================
    //  CORRECTION DECISION (runs once per hop)
    // ======================================================================
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
        const int degree = ((midi - set.key) % 12 + 12) % 12;
        return masks[(size_t) juce::jlimit (0, numScales - 1, set.scale)][(size_t) degree] != 0;
    }

    int nearestScaleNote (float midi) const
    {
        const int base = (int) std::round (midi);
        for (int d = 0; d <= 6; ++d)
        {
            const int up = base + d, down = base - d;
            const bool okUp = noteInScale (up), okDown = noteInScale (down);
            if (okUp && okDown) return std::abs ((float) up - midi) < std::abs ((float) down - midi) ? up : down;
            if (okUp) return up;
            if (okDown) return down;
        }
        return base;
    }

    int nearestHeld (float centre) const
    {
        int best = heldNotes[0]; float bestD = 1e9f;
        for (int i = 0; i < numHeld; ++i)
        {
            // any octave of a held note counts, so you can sing in your own range
            const int pc = heldNotes[(size_t) i] % 12;
            int cand = (int) std::round ((centre - (float) pc) / 12.0f) * 12 + pc;
            const float d = std::abs ((float) cand - centre);
            if (d < bestD) { bestD = d; best = cand; }
        }
        return best;
    }

    bool isHeldPitchClass (int midi) const
    {
        for (int i = 0; i < numHeld; ++i) if (heldNotes[(size_t) i] % 12 == ((midi % 12) + 12) % 12) return true;
        return false;
    }

    // scale-aware interval above/below 'note'
    int harmonyNote (int note, int choice) const
    {
        if (choice == 1)  return note - 12;
        if (choice == 10) return note + 12;
        static const int steps[] = { 0, 0, -5, -4, -3, -2, 2, 3, 4, 5 };   // scale steps
        static const int semis[] = { 0, 0, -8, -7, -5, -3, 4, 5, 7, 9 };   // used for the chromatic scale
        if (set.scale == 0) return note + semis[choice];
        int st = steps[choice], cur = note;
        const int dir = st > 0 ? 1 : -1;
        for (int guard = 0; st != 0 && guard < 48; ++guard)
        {
            cur += dir;
            if (noteInScale (cur)) st -= dir;
        }
        return cur;
    }

    float decide (bool voiced, float period, Estimate& est)
    {
        est.xActive.fill (false);
        const float hopSec = (float) hop / (float) sr;
        int h = histWrite.load();

        if (! voiced)
        {
            // short dropouts (consonants, breathy moments) keep the correction state, so a
            // note does not restart its correction every time detection blinks
            uvTime += hopSec;
            if (uvTime < 0.08f && lastTarget >= 0)
            {
                histIn[(size_t) h].store (-1.0f); histOut[(size_t) h].store (-1.0f); histTarget[(size_t) h].store (-1.0f);
                histWrite.store ((h + 1) % historySize);
                return 0.0f;
            }
            corrSm = 0.0f; flatW = 0.0f; lastTarget = -1; noteAge = 0.0f; jumpCount = 0;
            detectedHz.store (0.0f); targetNote.store (-1); correctionCents.store (0.0f);
            histIn[(size_t) h].store (-1.0f); histOut[(size_t) h].store (-1.0f); histTarget[(size_t) h].store (-1.0f);
            histWrite.store ((h + 1) % historySize);
            return 0.0f;
        }

        uvTime = 0.0f;
        const float hz = (float) sr / period;
        const float inst = 69.0f + 12.0f * std::log2 (hz / 440.0f);

        // Note centre = pitch with vibrato filtered out. A 2-pole low-pass that is
        // fast at the start of a note (follows scoops) and settles to 1.2 Hz once
        // vibrato typically begins (removes 5-7 Hz vibrato).
        if (lastTarget < 0) { centre1 = centre2 = inst; centreAge = 0.0f; voicedAge = 0.0f; }
        voicedAge += hopSec;
        jumpCount = std::abs (inst - centre2) > tracker.jumpSemis ? jumpCount + 1 : 0;   // real note change, not vibrato
        if (jumpCount >= tracker.jumpHops) { centre1 = centre2 = inst; centreAge = 0.0f; jumpCount = 0; }
        const float fc = tracker.baseHz + 10.0f * std::exp (-centreAge / 0.10f);
        centreAge += hopSec;
        const float a = std::exp (-2.0f * juce::MathConstants<float>::pi * fc * hopSec);
        centre1 = inst + (centre1 - inst) * a;
        centre2 = centre1 + (centre2 - centre1) * a;
        const float centre = centre2;

        // target note: hysteresis + must persist ~12 ms before switching
        const bool useMidi = set.midiTarget && numHeld > 0;
        int target = useMidi ? nearestHeld (centre) : nearestScaleNote (centre);
        if (lastTarget >= 0 && target != lastTarget)
        {
            const bool clearlyCloser = (useMidi ? ! isHeldPitchClass (lastTarget) : ! noteInScale (lastTarget))
                || std::abs (centre - (float) lastTarget) > std::abs (centre - (float) target) + (noteAge > 0.1f ? 0.35f : 0.15f);
            pendingCount = clearlyCloser ? pendingCount + 1 : 0;
            if (pendingCount < 3) target = lastTarget;
            else pendingCount = 0;
        }
        else
        {
            pendingCount = 0;
        }
        if (target != lastTarget) noteAge = 0.0f; else noteAge += hopSec;
        lastTarget = target;

        // Flex-Tune: only correct when the note centre is already near the target
        float weight = 1.0f;
        if (set.flex > 0.001f)
        {
            const float zone = 100.0f - 92.0f * set.flex;                      // cents
            const float dev = std::abs (centre - (float) target) * 100.0f;
            weight = juce::jlimit (0.0f, 1.0f, (zone + 10.0f - dev) / 10.0f);
        }

        // Onset: singers scoop into notes; fade correction in over the first ~70 ms of a
        // phrase so the scoop isn't snapped to the wrong note first
        weight *= juce::jlimit (0.0f, 1.0f, (voicedAge - 0.02f) / 0.05f);

        // Retune speed (+ Humanize: sustained notes get a slower, gentler correction)
        const float sustain = juce::jlimit (0.0f, 1.0f, (noteAge - 0.15f) / 0.3f);
        const float tauMs = set.retuneMs + set.humanize * sustain * 250.0f;
        const float k = tauMs < 0.5f ? 0.0f : std::exp (-hopSec * 1000.0f / tauMs);
        const float wantCorr = weight * ((float) target - centre);
        corrSm = wantCorr + (corrSm - wantCorr) * k;
        const float kf = std::exp (-hopSec / 0.02f);
        flatW = weight + (flatW - weight) * kf;

        // Natural vibrato: keep (vibrato) of the deviation around the centre
        // (clamped so a detector glitch can never throw the voice more than 1.5 semitones)
        const float shift = corrSm + flatW * (1.0f - set.vibrato) * juce::jlimit (-1.5f, 1.5f, centre - inst);

        // ---- extra voices: harmony intervals / MIDI chord notes / octave layers
        {
            const float expr = inst + shift - (float) target;   // the singer's own movement around the note
            int midiIdx = 0;
            for (int k = 0; k < 3; ++k)
            {
                const int choice = set.harmVoice[(size_t) k];
                int note = -1;
                if (choice == 11) { if (midiIdx < numHeld) note = heldNotes[(size_t) midiIdx]; ++midiIdx; }
                else if (choice > 0) note = harmonyNote (target, choice);
                if (note >= 0)
                {
                    est.xActive[(size_t) k] = true;
                    est.xShift[(size_t) k] = (float) note + expr + set.transpose - inst;
                }
            }
            const float mainOut = inst + shift + set.transpose;
            est.xActive[3] = set.subLevel > 0.001f; est.xShift[3] = mainOut - 12.0f - inst;
            est.xActive[4] = set.upLevel  > 0.001f; est.xShift[4] = mainOut + 12.0f - inst;
        }

        detectedHz.store (hz);
        targetNote.store (target);
        correctionCents.store (shift * 100.0f);
        histIn[(size_t) h].store (inst);
        histOut[(size_t) h].store (inst + shift);
        histTarget[(size_t) h].store ((float) target);
        histWrite.store ((h + 1) % historySize);
        return shift;
    }

    Estimate estimateAt (juce::int64 t) const
    {
        if (estCount == 0) return {};
        juce::int64 lo = juce::jmax ((juce::int64) 0, estCount - estRing + 8), hi = estCount - 1;
        const auto& at = [this] (juce::int64 k) -> const Estimate& { return estimates[(size_t) (k % estRing)]; };
        if (t <= at (lo).t) return at (lo);
        if (t >= at (hi).t) return at (hi);
        while (hi - lo > 1) { const auto mid = (lo + hi) / 2; (at (mid).t <= t ? lo : hi) = mid; }
        const Estimate& e0 = at (lo);
        const Estimate& e1 = at (hi);
        if (e0.voiced != e1.voiced) return (t - e0.t < e1.t - t) ? e0 : e1;
        Estimate r = e0;
        const float x = (float) (t - e0.t) / (float) juce::jmax ((juce::int64) 1, e1.t - e0.t);
        r.shift  = e0.shift  + x * (e1.shift  - e0.shift);
        r.period = e0.period + x * (e1.period - e0.period);
        for (size_t k = 0; k < (size_t) numExtra; ++k)
        {
            if (e0.xActive[k] && e1.xActive[k])        r.xShift[k] = e0.xShift[k] + x * (e1.xShift[k] - e0.xShift[k]);
            else if (x >= 0.5f)                       { r.xShift[k] = e1.xShift[k]; r.xActive[k] = e1.xActive[k]; }
        }
        return r;
    }

    // ======================================================================
    //  PITCH MARKS (one per glottal pulse when voiced, fixed grid otherwise)
    // ======================================================================
    struct Mark { juce::int64 t; float period; bool voiced; };

    void addMark (juce::int64 t, float period, bool voiced)
    {
        marks[(size_t) (markCount % markRing)] = { t, period, voiced };
        ++markCount;
    }
    const Mark& mark (juce::int64 idx) const { return marks[(size_t) (idx % markRing)]; }

    void placeMarks()
    {
        for (int guard = 0; guard < 8; ++guard)
        {
            const Mark last = mark (markCount - 1);
            const Estimate est = estimateAt (last.t);

            if (! est.voiced)
            {
                const juce::int64 cand = last.t + uvSpacing;
                if (cand > n - 1) return;
                addMark (cand, (float) uvSpacing, false);
                continue;
            }

            const float T = est.period;
            if (last.voiced)
            {
                // next pulse = position whose preceding period best matches the previous one
                const int lo = (int) (0.75f * T), hi = (int) std::ceil (1.25f * T);
                if (last.t + hi > n - 1) return;
                const int win = juce::jmax (8, (int) std::round (T));
                double bestScore = -1e9; int bestOff = (int) std::round (T);
                double e0 = 0.0;
                for (int j = 0; j < win; ++j) { const float a = mono[(size_t) ((last.t - j) & mask)]; e0 += (double) a * a; }
                for (int off = lo; off <= hi; ++off)
                {
                    double xy = 0.0, e1 = 0.0;
                    const juce::int64 c = last.t + off;
                    for (int j = 0; j < win; ++j)
                    {
                        const float a = mono[(size_t) ((last.t - j) & mask)];
                        const float b = mono[(size_t) ((c - j) & mask)];
                        xy += (double) a * b; e1 += (double) b * b;
                    }
                    const double score = xy / std::sqrt (e0 * e1 + 1e-12);
                    if (score > bestScore) { bestScore = score; bestOff = off; }
                }
                addMark (last.t + bestOff, (float) bestOff, true);
            }
            else
            {
                // entering a voiced region: start on the strongest peak within one period
                const int lo = juce::jmax (1, uvSpacing / 2), hi = juce::jmax (lo + 1, (int) std::ceil (T));
                if (last.t + hi > n - 1) return;
                juce::int64 bestT = last.t + lo; float bestV = -1e9f;
                for (int off = lo; off <= hi; ++off)
                {
                    const float x = lowp[(size_t) ((last.t + off) & mask)];
                    if (x > bestV) { bestV = x; bestT = last.t + off; }
                }
                addMark (bestT, (float) (bestT - last.t), true);
            }
        }
    }

    // ======================================================================
    //  SYNTHESIS (PSOLA overlap-add)
    // ======================================================================
    void synthesise (int numCh)
    {
        const float fRatio = std::pow (2.0f, juce::jlimit (-5.0f, 5.0f, set.formantSemis) / 12.0f);
        const juce::int64 outReadPos = n - 1 - latency;

        for (int guard = 0; guard < 16; ++guard)
        {
            const juce::int64 fl = (juce::int64) std::floor (v);
            const juce::int64 j  = (juce::int64) std::llround (v);
            const juce::int64 needIdx = juce::jmax (fl + 1, j + 1);
            if (needIdx >= markCount) return;                      // wait for more marks

            const Mark& m0 = mark (fl);
            const Mark& m1 = mark (fl + 1);
            const double s = (double) m0.t + (v - (double) fl) * (double) (m1.t - m0.t);

            // never run ahead of the pitch analysis (would apply stale corrections)
            if (estCount == 0 || s > (double) estimates[(size_t) ((estCount - 1) % estRing)].t) return;

            const Mark& src = mark (j);
            const juce::int64 right = juce::jmax ((juce::int64) 1, mark (j + 1).t - src.t);
            const juce::int64 left  = j > 0 ? juce::jmax ((juce::int64) 1, src.t - mark (j - 1).t) : right;
            const float wl = (float) juce::jmin (left,  (juce::int64) (1.5f * tauMax));
            const float wr = (float) juce::jmin (right, (juce::int64) (1.5f * tauMax));

            const bool voicedGrain = src.voiced;
            const float f = voicedGrain ? fRatio : 1.0f;
            if ((double) src.t + wr * f + 2.0 > (double) (n - 1)) return;   // source not recorded yet

            // pitch ratio at this grain
            float ratio = 1.0f;
            if (voicedGrain)
            {
                const Estimate est = estimateAt ((juce::int64) s);
                if (est.voiced) ratio = std::pow (2.0f, juce::jlimit (-24.0f, 24.0f, est.shift + set.transpose) / 12.0f);
            }

            // overlap-add the grain centred at s, taken from source mark src.t
            const juce::int64 t0 = (juce::int64) std::ceil (s - wl);
            const juce::int64 t1 = (juce::int64) std::floor (s + wr);
            for (juce::int64 t = t0; t <= t1; ++t)
            {
                const float d = (float) ((double) t - s);
                const float w = d < 0 ? 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * d / wl)
                                      : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * d / wr);
                if (t <= outReadPos) { ++lateSamples; continue; }
                const double sp = (double) src.t + (double) d * f;
                const juce::int64 i0 = (juce::int64) std::floor (sp);
                const float fr = (float) (sp - (double) i0);
                for (int c = 0; c < numCh; ++c)
                {
                    const auto& in = inRing[(size_t) c];
                    const float x = in[(size_t) (i0 & mask)] * (1.0f - fr) + in[(size_t) ((i0 + 1) & mask)] * fr;
                    outRing[(size_t) c][(size_t) (t & mask)] += w * x;
                }
            }

            // advance. When no shift is needed, slowly pull grains back onto their own
            // source pulse so the output becomes sample-identical to the input.
            double step = 1.0 / ratio;
            if (! voicedGrain)
                step = std::round (v + 1.0) - v;                  // unvoiced: snap to the grid
            else if (std::abs (ratio - 1.0f) < 0.003f)
                step += juce::jlimit (-0.01, 0.01, std::round (v) - v);
            v += step;
        }
    }

    // Extra voices (harmony 1-3, sub, up): mono source, panned onto the stereo harmony bus
    void synthesiseExtra (int k)
    {
        double& vv = vx[(size_t) k];
        const juce::int64 outReadPos = n - 1 - latency;
        const float fRatio = std::pow (2.0f, juce::jlimit (-12.0f, 12.0f, set.formantSemis + set.harmFormant) / 12.0f);

        // level, pan and a fixed detune "personality" per voice
        static const float detuneSign[] = { 0.6f, -0.8f, 0.35f, -0.4f, 0.5f };
        static const float panPos[]     = { -1.0f, 1.0f, 0.35f, 0.0f, -0.25f };
        static const float wobbleRate[] = { 0.31f, 0.47f, 0.23f, 0.19f, 0.41f };
        const float level = k < 3 ? set.harmMix : (k == 3 ? set.subLevel : set.upLevel);
        const float pan = juce::jlimit (-1.0f, 1.0f, panPos[k] * set.harmWidth);
        const float gl = level * std::cos ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);
        const float gr = level * std::sin ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f);

        for (int guard = 0; guard < 16; ++guard)
        {
            const juce::int64 fl = (juce::int64) std::floor (vv);
            const juce::int64 j  = (juce::int64) std::llround (vv);
            if (juce::jmax (fl + 1, j + 1) >= markCount) return;
            if (fl < markCount - markRing + 4) { vv = (double) (markCount - 8); continue; }   // fell too far behind

            const Mark& m0 = mark (fl);
            const Mark& m1 = mark (fl + 1);
            const double s = (double) m0.t + (vv - (double) fl) * (double) (m1.t - m0.t);
            if (estCount == 0 || s > (double) estimates[(size_t) ((estCount - 1) % estRing)].t) return;

            const Mark& src = mark (j);
            const juce::int64 right = juce::jmax ((juce::int64) 1, mark (j + 1).t - src.t);
            const juce::int64 left  = j > 0 ? juce::jmax ((juce::int64) 1, src.t - mark (j - 1).t) : right;
            const float wl = (float) juce::jmin (left,  (juce::int64) (1.5f * tauMax));
            const float wr = (float) juce::jmin (right, (juce::int64) (1.5f * tauMax));
            if ((double) src.t + wr * fRatio + 2.0 > (double) (n - 1)) return;

            const Estimate est = estimateAt ((juce::int64) s);
            const bool sounding = src.voiced && est.voiced && est.xActive[(size_t) k] && level > 0.0005f && harmOn;
            float ratio = 1.0f;
            if (sounding)
            {
                const float wob = set.harmDetune * (detuneSign[k] + 0.5f * std::sin ((float) (s / sr) * 6.2831853f * wobbleRate[k]));
                ratio = std::pow (2.0f, juce::jlimit (-36.0f, 36.0f, est.xShift[(size_t) k] + wob * 0.01f) / 12.0f);

                const juce::int64 t0 = (juce::int64) std::ceil (s - wl);
                const juce::int64 t1 = (juce::int64) std::floor (s + wr);
                for (juce::int64 t = t0; t <= t1; ++t)
                {
                    if (t <= outReadPos) continue;
                    const float d = (float) ((double) t - s);
                    const float w = d < 0 ? 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * d / wl)
                                          : 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * d / wr);
                    const double sp = (double) src.t + (double) d * fRatio;
                    const juce::int64 i0 = (juce::int64) std::floor (sp);
                    const float fr = (float) (sp - (double) i0);
                    const float x = mono[(size_t) (i0 & mask)] * (1.0f - fr) + mono[(size_t) ((i0 + 1) & mask)] * fr;
                    harmRing[0][(size_t) (t & mask)] += w * x * gl;
                    harmRing[1][(size_t) (t & mask)] += w * x * gr;
                }
            }
            vv += sounding ? 1.0 / ratio : std::round (vv + 1.0) - vv;
        }
    }

    // ---------------------------------------------------------------- state
    static constexpr double minF0 = 75.0, maxF0 = 1100.0;
    double sr = 44100.0;
    Settings set;

    int tauMin = 2, tauMax = 640, yinW = 640, frameF = 1280, hop = 192, uvSpacing = 240, latency = 2400;
    int mask = 0;
    juce::int64 n = 0;
    int hopCounter = 0;
    float lp1 = 0, lp2 = 0;

    std::vector<float> mono, lowp, frame, cmnd, fa, fb;
    std::vector<double> prefix;
    std::vector<std::vector<float>> inRing, outRing, harmRing;
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftN = 2048;

    static constexpr juce::int64 estRing = 512;
    std::array<Estimate, (size_t) estRing> estimates {};
    juce::int64 estCount = 0;

    static constexpr juce::int64 markRing = 4096;
    std::array<Mark, (size_t) markRing> marks {};
    juce::int64 markCount = 0;
    double v = 0.0;
    std::array<double, numExtra> vx {};
    bool harmOn = false; float harmGain = 0.0f;
    std::array<int, 8> heldNotes {}; int numHeld = 0;

    float prevPeriod = 0; bool voicedState = false; int octavePending = 0; float uvTime = 1.0f;
    float centre1 = 0, centre2 = 0, centreAge = 0; int jumpCount = 0, pendingCount = 0;
    int lastTarget = -1; float noteAge = 0, voicedAge = 0;
    float corrSm = 0, flatW = 0, wetGain = 0;
};
