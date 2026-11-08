#include "PluginProcessor.h"
#include "PluginEditor.h"

static float blockPeakDb (const juce::AudioBuffer<float>& b)
{
    float peak = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        peak = juce::jmax (peak, b.getMagnitude (c, 0, b.getNumSamples()));
    return juce::Decibels::gainToDecibels (peak, -100.0f);
}

NeonRuinAudioProcessor::NeonRuinAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", params::createLayout())
{
    // every parameter that an LFO or the Chaos macro can move gets a modulation slot
    for (auto& t : params::modTargets()) if (*t.id) modSlots.push_back ({ t.id, 0.0f });
    for (auto& c : params::chaosEntries())
    {
        bool found = false;
        for (auto& s : modSlots) found |= std::strcmp (s.id, c.id) == 0;
        if (! found) modSlots.push_back ({ c.id, 0.0f });
    }

    apvts.state.setProperty ("chain", params::defaultChain(), nullptr);
    readChainFromState();
    startTimerHz (2);
}

bool NeonRuinAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void NeonRuinAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    const auto numCh = juce::jmax (1, getTotalNumOutputChannels());
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) numCh };

    tuner.prepare (sampleRate, numCh);
    setLatencySamples (tuner.getLatencySamples());

    gate.prepare (sampleRate);
    for (auto* f : { &lowCut, &lowShelf, &midPeak, &highShelf }) { f->prepare (spec); f->reset(); }
    for (auto& c : eqCache) c = -1.0f;
    updateEq();
    comp.prepare (spec);
    deesser.prepare (sampleRate, numCh);
    transmit.prepare (sampleRate, numCh);
    doubler.prepare (sampleRate);
    chorus.prepare (spec);
    chorus.setCentreDelay (12.0f);
    chorus.setFeedback (0.1f);
    delay.prepare (sampleRate);
    reverb.prepare (spec);
    reverb.reset();
    glitch.prepare (sampleRate, numCh);
    limiter.prepare (sampleRate);
    bpmDetector.prepare (sampleRate);
}

// ============================================================================
//  Modulation: LFOs and the Chaos macro add normalised offsets to parameters
// ============================================================================
float NeonRuinAudioProcessor::pm (const char* id) const
{
    const float base = p (id);
    for (auto& s : modSlots)
    {
        if (std::strcmp (s.id, id) != 0) continue;
        if (s.offset == 0.0f) return base;
        auto* prm = apvts.getParameter (id);
        const float norm = juce::jlimit (0.0f, 1.0f, prm->convertTo0to1 (base) + s.offset);
        return prm->convertFrom0to1 (norm);
    }
    return base;
}

void NeonRuinAudioProcessor::updateModulation (const fx::Tempo& tempo, int numSamples)
{
    for (auto& s : modSlots) s.offset = 0.0f;
    const float chaos = p ("chaos") * 0.01f;

    static const char* ids[2][6] = { { "lfo1Shape", "lfo1Rate", "lfo1Sync", "lfo1Div", "lfo1Target", "lfo1Depth" },
                                     { "lfo2Shape", "lfo2Rate", "lfo2Sync", "lfo2Div", "lfo2Target", "lfo2Depth" } };
    for (int n = 0; n < 2; ++n)
    {
        const float value = lfos[n].next ((int) p (ids[n][0]), p (ids[n][1]), on (ids[n][2]), (int) p (ids[n][3]),
                                          tempo, numSamples, sr);
        lfoOut[n].store (value);
        const int target = (int) p (ids[n][4]);
        if (target <= 0) continue;
        const char* id = params::modTargets()[(size_t) target].id;
        const float depth = p (ids[n][5]) * 0.01f * 0.5f * (1.0f + 2.0f * chaos);
        for (auto& s : modSlots) if (std::strcmp (s.id, id) == 0) s.offset += depth * value;
    }

    if (chaos > 0.0f)
        for (auto& c : params::chaosEntries())
        {
            auto* prm = apvts.getParameter (c.id);
            const float baseNorm = prm->convertTo0to1 (p (c.id));
            for (auto& s : modSlots) if (std::strcmp (s.id, c.id) == 0) s.offset += chaos * (c.wild - baseNorm);
        }
}

void NeonRuinAudioProcessor::updateEq()
{
    const float v[5] { pm ("eqLowCut"), p ("eqLowGain"), pm ("eqMidFreq"), pm ("eqMidGain"), p ("eqHighGain") };
    using C = juce::dsp::IIR::Coefficients<float>;
    if (v[0] != eqCache[0]) *lowCut.state = *C::makeHighPass (sr, v[0], 0.707f);
    if (v[1] != eqCache[1]) *lowShelf.state = *C::makeLowShelf (sr, 200.0f, 0.707f, juce::Decibels::decibelsToGain (v[1]));
    if (v[2] != eqCache[2] || v[3] != eqCache[3])
        *midPeak.state = *C::makePeakFilter (sr, v[2], 1.0f, juce::Decibels::decibelsToGain (v[3]));
    if (v[4] != eqCache[4])
        *highShelf.state = *C::makeHighShelf (sr, juce::jmin (10000.0, sr * 0.4), 0.707f, juce::Decibels::decibelsToGain (v[4]));
    std::copy (v, v + 5, eqCache);
}

// ============================================================================
void NeonRuinAudioProcessor::processModule (int module, juce::AudioBuffer<float>& buffer, const fx::Tempo& tempo)
{
    juce::dsp::AudioBlock<float> block (buffer);
    juce::dsp::ProcessContextReplacing<float> ctx (block);

    switch (module)
    {
        case params::mEQ:
            if (! on ("eqOn")) return;
            updateEq();
            lowCut.process (ctx); lowShelf.process (ctx); midPeak.process (ctx); highShelf.process (ctx);
            return;

        case params::mComp:
        {
            if (! on ("compOn")) { gainReductionDb.store (0.0f); return; }
            comp.setThreshold (p ("compThresh"));
            comp.setRatio (p ("compRatio"));
            comp.setAttack (p ("compAttack"));
            comp.setRelease (p ("compRelease"));
            const float before = blockPeakDb (buffer);
            comp.process (ctx);
            const float after = blockPeakDb (buffer);
            const float gr = before > -80.0f ? juce::jmin (0.0f, after - before) : 0.0f;
            gainReductionDb.store (gainReductionDb.load() * 0.7f + gr * 0.3f);
            buffer.applyGain (juce::Decibels::decibelsToGain (p ("compMakeup")));
            return;
        }

        case params::mDeEss:
            if (! on ("deessOn")) { deessReductionDb.store (0.0f); return; }
            deesser.process (buffer, p ("deessFreq"), p ("deessThresh"), p ("deessRange"));
            deessReductionDb.store (deesser.gainReductionDb);
            return;

        case params::mTransmit:
            if (! on ("tmOn")) return;
            transmit.process (buffer, pm ("tmDrive") * 0.01f, pm ("tmCrush") * 0.01f, pm ("tmRadio") * 0.01f,
                              pm ("tmRingFreq"), pm ("tmRingMix") * 0.01f, p ("tmMix") * 0.01f);
            return;

        case params::mDoubler:
            if (! on ("dblOn")) return;
            doubler.process (buffer, pm ("dblAmount") * 0.01f, p ("dblWidth") * 0.01f, pm ("dblDetune"), p ("dblDelay"));
            return;

        case params::mChorus:
            if (! on ("chorusOn")) return;
            chorus.setRate (p ("chorusRate"));
            chorus.setDepth (pm ("chorusDepth") * 0.01f);
            chorus.setMix (pm ("chorusMix") * 0.01f);
            chorus.process (ctx);
            return;

        case params::mDelay:
        {
            if (! on ("delayOn")) return;
            const float samples = on ("delaySync")
                ? (float) (fx::divisionBeats ((int) p ("delayDiv")) * 60.0 / tempo.bpm * sr)
                : p ("delayTime") * 0.001f * (float) sr;
            delay.process (buffer, samples, pm ("delayFeedback") * 0.01f, pm ("delayMix") * 0.01f,
                           on ("delayPingPong"), on ("delayReverse"));
            return;
        }

        case params::mReverb:
        {
            if (! on ("reverbOn")) return;
            juce::Reverb::Parameters rp;
            rp.roomSize   = pm ("reverbSize") * 0.01f;
            rp.damping    = p ("reverbDamp") * 0.01f;
            rp.width      = p ("reverbWidth") * 0.01f;
            rp.wetLevel   = pm ("reverbMix") * 0.01f;
            rp.dryLevel   = 1.0f - 0.5f * rp.wetLevel;
            rp.freezeMode = on ("reverbFreeze") ? 1.0f : 0.0f;
            reverb.setParameters (rp);
            reverb.process (ctx);
            return;
        }

        case params::mGlitch:
        {
            if (! on ("glitchOn")) { glitchActive.store (false); return; }
            static const int rateToDiv[] = { 2, 3, 4, 5, 10, 11 };   // glitchRate choice -> division index
            glitch.process (buffer, tempo, rateToDiv[juce::jlimit (0, 5, (int) p ("glitchRate"))],
                            pm ("glitchChance") * 0.01f, pm ("glitchReverse") * 0.01f, p ("glitchMix") * 0.01f);
            glitchActive.store (glitch.isActive);
            return;
        }
        default: return;
    }
}

void NeonRuinAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    for (auto c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, numSamples);

    // ---- MIDI: track held notes (used by MIDI target mode and MIDI harmony voices)
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn()) heldNotes[(size_t) m.getNoteNumber()] = true;
        else if (m.isNoteOff()) heldNotes[(size_t) m.getNoteNumber()] = false;
        else if (m.isAllNotesOff() || m.isAllSoundOff()) heldNotes.fill (false);
    }
    int held = 0; for (bool b : heldNotes) held += b ? 1 : 0;
    midiNotesHeld.store (held);
    tuner.setHeldNotes (heldNotes);

    // ---- tempo from the host (falls back to 120 BPM, free running)
    fx::Tempo tempo;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) tempo.bpm = juce::jlimit (20.0, 400.0, *b);
            if (auto q = pos->getPpqPosition()) tempo.ppq = *q;
            tempo.playing = pos->getIsPlaying();
        }

    // ---- IN
    buffer.applyGain (juce::Decibels::decibelsToGain (p ("inGain")));
    inLevelDb.store (blockPeakDb (buffer));

    // ---- tempo detection on the incoming vocal
    if (resetBpmRequest.exchange (false)) bpmDetector.reset();
    for (int pos = 0; pos < numSamples; pos += (int) monoScratch.size())
    {
        const int n = juce::jmin ((int) monoScratch.size(), numSamples - pos);
        const int nCh = buffer.getNumChannels();
        for (int i = 0; i < n; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < nCh; ++c) m += buffer.getSample (c, pos + i);
            monoScratch[(size_t) i] = m / (float) juce::jmax (1, nCh);
        }
        bpmDetector.process (monoScratch.data(), n);
    }
    detBpm.store (bpmDetector.bpm.load());
    detBpmConfidence.store (bpmDetector.confidence.load());

    // detected tempo replaces the host tempo when chosen (free-running, not bar-locked)
    const int tempoSource = (int) p ("tempoSource");
    const float det = bpmDetector.bpm.load();
    usingDetectedTempo.store (tempoSource > 0 && det > 0.0f);
    if (tempoSource > 0 && det > 0.0f)
    {
        static const double factor[] = { 1.0, 1.0, 0.5, 2.0 };
        tempo.bpm = juce::jlimit (20.0, 400.0, det * factor[tempoSource]);
        tempo.playing = false;
    }
    bpm.store ((float) tempo.bpm);

    updateModulation (tempo, numSamples);

    // ---- GATE
    if (on ("gateOn")) gate.process (buffer, p ("gateThresh"), p ("gateRelease"));

    // ---- AUTOTUNE + HARMONY (always runs so latency stays constant)
    PitchCorrector::Settings ts;
    ts.key = (int) p ("tuneKey");
    ts.scale = (int) p ("tuneScale");
    ts.retuneMs = pm ("tuneSpeed");
    ts.humanize = p ("tuneHumanize") * 0.01f;
    ts.flex = p ("tuneFlex") * 0.01f;
    ts.vibrato = p ("tuneVibrato") * 0.01f;
    ts.formantSemis = pm ("tuneFormant");
    ts.transpose = std::round (pm ("tuneTranspose"));
    ts.mix = p ("tuneMix") * 0.01f;
    ts.midiTarget = on ("tuneMidi");
    ts.harmVoice = { (int) p ("harmVoice1"), (int) p ("harmVoice2"), (int) p ("harmVoice3") };
    ts.harmMix = pm ("harmMix") * 0.01f;
    ts.harmWidth = p ("harmWidth") * 0.01f;
    ts.harmDetune = pm ("harmDetune");
    ts.harmFormant = pm ("harmFormant");
    ts.subLevel = pm ("harmSub") * 0.01f;
    ts.upLevel = pm ("harmUp") * 0.01f;
    tuner.setSettings (ts);
    if (resetKeyRequest.exchange (false)) tuner.keyDetector.reset();
    tuner.process (buffer, on ("tuneOn"), on ("harmOn"));

    // key / scale read-out (cheap; refreshed a few times per second)
    if (++keyAnalysisCounter * numSamples >= (int) (sr * 0.25))
    {
        keyAnalysisCounter = 0;
        const auto k = tuner.keyDetector.analyse();
        detKey.store (k.key); detScale.store (k.scale); detAltKey.store (k.altKey); detAltScale.store (k.altScale);
        detKeyConfidence.store (k.confidence); detKeySeconds.store (k.seconds);
    }

    // ---- FX chain in the user's order
    for (auto& m : chain)
        processModule (m.load(), buffer, tempo);

    // ---- OUT
    buffer.applyGain (juce::Decibels::decibelsToGain (pm ("outGain")));
    limiter.process (buffer);
    limiterDb.store (limiter.reductionDb);
    outLevelDb.store (blockPeakDb (buffer));
}

// ============================================================================
//  Key detection -> autotune
// ============================================================================
void NeonRuinAudioProcessor::applyDetectedKey()
{
    const int k = detKey.load();
    if (k < 0) return;
    applyValues ({ { "tuneKey", (float) k }, { "tuneScale", (float) detScale.load() } });
}

void NeonRuinAudioProcessor::timerCallback()
{
    // Auto Key: follow the detected key once it is reasonably certain
    if (! on ("keyAuto")) return;
    if (detKey.load() < 0 || detKeyConfidence.load() < 0.6f || detKeySeconds.load() < 8.0f) return;
    if ((int) p ("tuneKey") != detKey.load() || (int) p ("tuneScale") != detScale.load())
        applyDetectedKey();
}

// ============================================================================
//  Chain order (stored as "chain" property in the state, e.g. "0,2,1,3,...")
// ============================================================================
juce::Array<int> NeonRuinAudioProcessor::getChainOrder() const
{
    juce::Array<int> a;
    for (auto& m : chain) a.add (m.load());
    return a;
}

void NeonRuinAudioProcessor::setChainOrder (const juce::Array<int>& order)
{
    juce::StringArray s;
    for (int m : order) s.add (juce::String (m));
    apvts.state.setProperty ("chain", s.joinIntoString (","), nullptr);
    readChainFromState();
}

void NeonRuinAudioProcessor::readChainFromState()
{
    auto items = juce::StringArray::fromTokens (apvts.state.getProperty ("chain", params::defaultChain()).toString(), ",", "");
    juce::Array<int> order;
    for (auto& t : items)
    {
        const int m = t.getIntValue();
        if (m >= 0 && m < params::numModules && ! order.contains (m)) order.add (m);
    }
    for (int m = 0; m < params::numModules; ++m) if (! order.contains (m)) order.add (m);   // repair
    for (int i = 0; i < params::numModules; ++i) chain[(size_t) i].store (order[i]);
}

// ============================================================================
//  Presets, A/B, randomiser
// ============================================================================
void NeonRuinAudioProcessor::resetToDefaults (bool keepKeyAndLevels)
{
    for (auto* prm : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
        {
            const auto id = r->getParameterID();
            if (keepKeyAndLevels && (id == "tuneKey" || id == "tuneScale" || id == "inGain" || id == "outGain")) continue;
            r->setValueNotifyingHost (r->getDefaultValue());
        }
}

void NeonRuinAudioProcessor::applyValues (const std::vector<std::pair<const char*, float>>& values)
{
    for (auto& [id, value] : values)
        if (auto* r = apvts.getParameter (id))
            r->setValueNotifyingHost (r->convertTo0to1 (value));
}

void NeonRuinAudioProcessor::loadFactoryPreset (int index)
{
    const auto& presets = params::factoryPresets();
    index = juce::jlimit (0, (int) presets.size() - 1, index);
    resetToDefaults (true);
    applyValues (presets[(size_t) index].values);
    setChainOrder (juce::Array<int> { 0, 2, 1, 3, 4, 5, 6, 7, 8 });
    currentFactoryPreset = index;
    currentPresetName = presets[(size_t) index].name;
}

juce::File NeonRuinAudioProcessor::getUserPresetFolder() const
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Neon Ruin").getChildFile ("Presets");
}

juce::Array<juce::File> NeonRuinAudioProcessor::getUserPresets() const
{
    auto files = getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.nrpreset");
    files.sort();
    return files;
}

bool NeonRuinAudioProcessor::saveUserPreset (const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName (name.trim());
    if (clean.isEmpty()) return false;
    getUserPresetFolder().createDirectory();
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return false;
    const bool ok = xml->writeTo (getUserPresetFolder().getChildFile (clean + ".nrpreset"));
    if (ok) { currentPresetName = clean; currentFactoryPreset = -1; }
    return ok;
}

void NeonRuinAudioProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    const float key = p ("tuneKey"), scale = p ("tuneScale");
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    applyValues ({ { "tuneKey", key }, { "tuneScale", scale } });
    readChainFromState();
    currentPresetName = file.getFileNameWithoutExtension();
    currentFactoryPreset = -1;
}

void NeonRuinAudioProcessor::randomise (juce::int64 seed)
{
    juce::Random r;
    if (seed != 0) r.setSeed (seed);
    for (auto& m : params::randomModules())
        if (auto* prm = apvts.getParameter (m.id)) prm->setValueNotifyingHost (r.nextFloat() < m.probability ? 1.0f : 0.0f);
    for (auto& rr : params::randomRanges())
        if (auto* prm = apvts.getParameter (rr.id)) prm->setValueNotifyingHost (rr.lo + r.nextFloat() * (rr.hi - rr.lo));
    if (auto* prm = apvts.getParameter ("tuneOn")) prm->setValueNotifyingHost (1.0f);
    if (auto* prm = apvts.getParameter ("lfo2Target")) prm->setValueNotifyingHost (0.0f);

    // shuffle the middle of the chain a little (keep EQ first, reverb near the end)
    auto order = getChainOrder();
    for (int i = 1; i < order.size() - 2; ++i)
        if (r.nextFloat() < 0.3f) order.swap (i, i + 1);
    setChainOrder (order);
    currentPresetName = "Random #" + juce::String (r.nextInt (9999));
    currentFactoryPreset = -1;
}

void NeonRuinAudioProcessor::selectAB (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    if (slot == abSlot) return;
    abStates[abSlot] = apvts.copyState();
    abSlot = slot;
    if (abStates[slot].isValid())
    {
        apvts.replaceState (abStates[slot].createCopy());
        readChainFromState();
    }
    else
        abStates[slot] = apvts.copyState();   // first visit: B starts as a copy of A
}

void NeonRuinAudioProcessor::copyABToOther()
{
    abStates[1 - abSlot] = apvts.copyState();
}

// ============================================================================
void NeonRuinAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("presetName", currentPresetName, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void NeonRuinAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            currentPresetName = apvts.state.getProperty ("presetName", "Init").toString();
            readChainFromState();
        }
}

juce::AudioProcessorEditor* NeonRuinAudioProcessor::createEditor() { return new NeonRuinAudioProcessorEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NeonRuinAudioProcessor(); }
