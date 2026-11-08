#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

namespace neon
{
    const juce::Colour bg0     { 0xff04060a };
    const juce::Colour bg1     { 0xff0a1520 };
    const juce::Colour cyan    { 0xff00e5ff };
    const juce::Colour magenta { 0xffff2a6d };
    const juce::Colour amber   { 0xffffa21f };
    const juce::Colour green   { 0xff39ff9c };
    const juce::Colour smog    { 0xffff5a1f };
    const juce::Colour text    { 0xffa9d6dd };
    const juce::Colour dim     { 0xff3a5560 };

    juce::Font mono (float size, bool bold = false);
}

// ----------------------------------------------------------------------------
class NeonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    NeonLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return neon::mono (13.0f, true); }
    juce::Font getPopupMenuFont() override { return neon::mono (14.0f); }
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
};

// ----------------------------------------------------------------------------
// A labelled neon knob bound to a parameter
class NeonKnob : public juce::Component
{
public:
    NeonKnob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label, juce::Colour, const juce::String& help = {});
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::Slider slider;
private:
    juce::String label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// A labelled combo box bound to a choice parameter
class NeonCombo : public juce::Component
{
public:
    NeonCombo (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& label, juce::Colour, const juce::String& help = {});
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::ComboBox box;
private:
    juce::String label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

// A labelled pill switch bound to a bool parameter
class NeonToggle : public juce::Component
{
public:
    NeonToggle (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& text, juce::Colour, const juce::String& help = {});
    void resized() override { button.setBounds (getLocalBounds()); }
    juce::ToggleButton button;
private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

// ----------------------------------------------------------------------------
// A chamfered "hardware module" panel with a title and optional power switch
class ModulePanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    ModulePanel (juce::AudioProcessorValueTreeState&, const juce::String& powerParam,
                 const juce::String& code, const juce::String& title, juce::Colour, const juce::String& help = {});
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    juce::Rectangle<int> getContentArea() const;
    juce::Colour colour;
private:
    juce::String code, title;
    juce::ToggleButton power;
    bool hasPower = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAttachment;
};

// ----------------------------------------------------------------------------
class PitchDisplay : public juce::Component
{
public:
    explicit PitchDisplay (NeonRuinAudioProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    int frame = 0;
private:
    NeonRuinAudioProcessor& proc;
    float viewCentre = 60.0f, shownCents = 0.0f;
};

class GRMeter : public juce::Component
{
public:
    GRMeter (std::function<float()> src, juce::Colour c) : source (std::move (src)), colour (c) {}
    void paint (juce::Graphics&) override;
    void tick() { shown += (source() - shown) * 0.3f; repaint(); }
private:
    std::function<float()> source; juce::Colour colour; float shown = 0.0f;
};

class LfoScope : public juce::Component
{
public:
    LfoScope (std::atomic<float>& src, juce::Colour c) : source (src), colour (c) { values.fill (0.0f); }
    void paint (juce::Graphics&) override;
    void tick() { values[(size_t) write] = source.load(); write = (write + 1) % (int) values.size(); repaint(); }
private:
    std::atomic<float>& source; juce::Colour colour;
    std::array<float, 120> values; int write = 0;
};

// Drag-and-drop view of the effect chain order
class ChainView : public juce::Component
{
public:
    explicit ChainView (NeonRuinAudioProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    juce::Rectangle<float> chipBounds (int slot) const;
    int slotAt (float x) const;
    NeonRuinAudioProcessor& proc;
    int dragging = -1;   // slot index being dragged
    float dragX = 0.0f;
};

// ----------------------------------------------------------------------------
class NeonRuinAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NeonRuinAudioProcessorEditor (NeonRuinAudioProcessor&);
    ~NeonRuinAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    void showPage (int index);   // public so the snapshot tool can render every page

private:
    void timerCallback() override;
    void drawSkyline (juce::Graphics&, juce::Rectangle<float> area);
    void drawMeter (juce::Graphics&, juce::Rectangle<float>, float db, juce::Colour);
    void refreshPresetBox();
    void savePresetDialog();

    // builders
    ModulePanel* panel (int page, const juce::String& power, const juce::String& code, const juce::String& title, juce::Colour, const juce::String& help = {});
    NeonKnob*   knob   (ModulePanel*, const juce::String& id, const juce::String& label, const juce::String& help = {});
    NeonCombo*  combo  (ModulePanel*, const juce::String& id, const juce::String& label, const juce::String& help = {});
    NeonToggle* toggle (ModulePanel*, const juce::String& id, const juce::String& text, const juce::String& help = {});

    NeonRuinAudioProcessor& proc;
    NeonLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 500 };

    // header
    juce::ComboBox presetBox;
    juce::TextButton prevBtn { "<" }, nextBtn { ">" }, saveBtn { "SAVE" }, randomBtn { "RANDOM" },
                     aBtn { "A" }, bBtn { "B" }, copyBtn { "COPY" };
    std::unique_ptr<NeonKnob> chaosKnob, inKnob, outKnob;
    juce::Array<juce::File> userPresets;

    // pages
    juce::TextButton tabs[4];
    juce::Component pages[4];
    int currentPage = 0;

    juce::OwnedArray<juce::Component> owned;
    // named parts for layout
    struct Parts { ModulePanel* panel = nullptr; std::vector<NeonKnob*> knobs; std::vector<NeonCombo*> combos; std::vector<NeonToggle*> toggles; };
    Parts tune, harm, gateP, eq, comp, deess, trans, dbl, delayP, reverbP, chorusP, glitchP, lfo1, lfo2, routing, macro;
    PitchDisplay* pitchDisplay = nullptr;
    GRMeter* compMeter = nullptr;
    GRMeter* deessMeter = nullptr;
    LfoScope* scopes[2] {};
    ChainView* chainView = nullptr;
    std::unique_ptr<NeonKnob> bigChaos;
    juce::Label chaosInfo;

    juce::Rectangle<float> inMeterArea, outMeterArea, footerArea, headerArea, tabArea;

    struct Drop { float x, y, speed, len; };
    std::vector<Drop> rain;
    juce::Random rng { 1987 };
    juce::Image skylineCache;
    float flicker = 1.0f;
    int frameCount = 0;
    float shownIn = -100.0f, shownOut = -100.0f;
    juce::String lastPresetName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NeonRuinAudioProcessorEditor)
};
