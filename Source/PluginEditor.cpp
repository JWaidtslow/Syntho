#include "PluginEditor.h"

using namespace juce;

Font neon::mono (float size, bool bold)
{
    return Font (FontOptions (Font::getDefaultMonospacedFontName(), size, bold ? Font::bold : Font::plain));
}

// ============================================================================
//  drawing helpers
// ============================================================================
static void glowText (Graphics& g, const String& s, Rectangle<float> r, Justification j, Font f, Colour c, float glow = 1.0f)
{
    g.setFont (f);
    for (int i = 3; i >= 1; --i)
    {
        g.setColour (c.withAlpha (0.10f * glow));
        const float o = (float) i * 1.2f;
        g.drawText (s, r.translated (-o, 0), j, false);
        g.drawText (s, r.translated ( o, 0), j, false);
        g.drawText (s, r.translated (0, -o), j, false);
        g.drawText (s, r.translated (0,  o), j, false);
    }
    g.setColour (c.withAlpha (0.35f * glow));
    g.drawText (s, r.expanded (1.0f, 0.0f), j, false);
    g.setColour (c.brighter (0.6f));
    g.drawText (s, r, j, false);
}

static void glowStroke (Graphics& g, const Path& p, Colour c, float width, float glow = 1.0f)
{
    g.setColour (c.withAlpha (0.07f * glow)); g.strokePath (p, PathStrokeType (width * 5.0f));
    g.setColour (c.withAlpha (0.16f * glow)); g.strokePath (p, PathStrokeType (width * 2.5f));
    g.setColour (c);                          g.strokePath (p, PathStrokeType (width));
}

static Path chamfer (Rectangle<float> r, float cut)
{
    Path p;
    p.startNewSubPath (r.getX() + cut, r.getY());
    p.lineTo (r.getRight(), r.getY());
    p.lineTo (r.getRight(), r.getBottom() - cut);
    p.lineTo (r.getRight() - cut, r.getBottom());
    p.lineTo (r.getX(), r.getBottom());
    p.lineTo (r.getX(), r.getY() + cut);
    p.closeSubPath();
    return p;
}

// ============================================================================
//  Look and feel
// ============================================================================
NeonLookAndFeel::NeonLookAndFeel()
{
    setColour (PopupMenu::backgroundColourId, neon::bg1);
    setColour (PopupMenu::textColourId, neon::text);
    setColour (PopupMenu::headerTextColourId, neon::amber);
    setColour (PopupMenu::highlightedBackgroundColourId, neon::cyan.withAlpha (0.25f));
    setColour (PopupMenu::highlightedTextColourId, Colours::white);
    setColour (ComboBox::textColourId, neon::amber);
    setColour (ComboBox::outlineColourId, neon::amber);
    setColour (ComboBox::backgroundColourId, neon::bg0);
    setColour (TextButton::buttonColourId, neon::cyan);
    setColour (AlertWindow::backgroundColourId, neon::bg1);
    setColour (AlertWindow::textColourId, neon::text);
    setColour (AlertWindow::outlineColourId, neon::cyan);
    setColour (TextEditor::backgroundColourId, neon::bg0);
    setColour (TextEditor::textColourId, neon::amber);
    setColour (TextEditor::outlineColourId, neon::dim);
    setColour (TextEditor::focusedOutlineColourId, neon::cyan);
}

void NeonLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos,
                                        float a0, float a1, Slider& s)
{
    const auto col = s.findColour (Slider::rotarySliderFillColourId);
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = a0 + pos * (a1 - a0);

    for (int i = 0; i <= 20; ++i)
    {
        const float a = a0 + (a1 - a0) * (float) i / 20.0f;
        const float r1 = r * 0.96f, r2 = r * (i % 5 == 0 ? 1.08f : 1.02f);
        g.setColour ((a <= angle + 0.001f ? col : neon::dim).withAlpha (i % 5 == 0 ? 0.9f : 0.5f));
        g.drawLine (c.x + r1 * std::sin (a), c.y - r1 * std::cos (a), c.x + r2 * std::sin (a), c.y - r2 * std::cos (a), 1.0f);
    }

    Path track; track.addCentredArc (c.x, c.y, r * 0.84f, r * 0.84f, 0, a0, a1, true);
    g.setColour (neon::dim.withAlpha (0.45f));
    g.strokePath (track, PathStrokeType (3.0f, PathStrokeType::curved, PathStrokeType::butt));

    // bipolar parameters (e.g. formant, LFO depth) draw their arc from the centre
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    Path arc; arc.addCentredArc (c.x, c.y, r * 0.84f, r * 0.84f, 0, jmin (from, angle), jmax (from, angle), true);
    g.setColour (col.withAlpha (0.10f)); g.strokePath (arc, PathStrokeType (12.0f, PathStrokeType::curved, PathStrokeType::butt));
    g.setColour (col.withAlpha (0.22f)); g.strokePath (arc, PathStrokeType (6.0f,  PathStrokeType::curved, PathStrokeType::butt));
    g.setColour (col);                   g.strokePath (arc, PathStrokeType (2.5f,  PathStrokeType::curved, PathStrokeType::butt));

    const float capR = r * 0.64f;
    g.setGradientFill (ColourGradient (Colour (0xff1a2a36), c.x - capR, c.y - capR, Colour (0xff05080c), c.x + capR, c.y + capR, false));
    g.fillEllipse (c.x - capR, c.y - capR, capR * 2, capR * 2);
    g.setColour (col.withAlpha (0.35f));
    g.drawEllipse (c.x - capR, c.y - capR, capR * 2, capR * 2, 1.0f);

    Path ptr;
    ptr.startNewSubPath (c.x + capR * 0.55f * std::sin (angle), c.y - capR * 0.55f * std::cos (angle));
    ptr.lineTo          (c.x + capR * 0.95f * std::sin (angle), c.y - capR * 0.95f * std::cos (angle));
    glowStroke (g, ptr, col, 2.0f);

    g.setColour (neon::text);
    g.setFont (neon::mono (jlimit (9.0f, 12.0f, capR * 0.42f), true));
    g.drawFittedText (s.getTextFromValue (s.getValue()), Rectangle<float> (c.x - capR, c.y - capR * 0.3f, capR * 2, capR * 0.9f).toNearestInt(),
                      Justification::centred, 1);
}

void NeonLookAndFeel::drawToggleButton (Graphics& g, ToggleButton& b, bool hi, bool)
{
    const auto col = b.findColour (ToggleButton::tickColourId);
    const bool isOn = b.getToggleState();
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const String label = b.getButtonText().isNotEmpty() ? b.getButtonText() : (isOn ? "ON" : "OFF");

    g.setColour (isOn ? col.withAlpha (0.18f) : neon::bg0.withAlpha (0.9f));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour ((isOn ? col : neon::dim).withAlpha (hi ? 1.0f : 0.85f));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);

    const float d = jmin (10.0f, r.getHeight() * 0.42f);
    const auto led = Rectangle<float> (r.getX() + 7.0f, r.getCentreY() - d * 0.5f, d, d);
    if (isOn) { g.setColour (col.withAlpha (0.3f)); g.fillEllipse (led.expanded (3.0f)); }
    g.setColour (isOn ? col : neon::dim);
    g.fillEllipse (led);

    g.setFont (neon::mono (11.0f, true));
    g.setColour (isOn ? col.brighter (0.3f) : neon::dim.brighter (0.3f));
    g.drawText (label, r.withTrimmedLeft (led.getRight() - r.getX() + 2.0f), Justification::centred);
}

void NeonLookAndFeel::drawButtonBackground (Graphics& g, Button& b, const Colour&, bool hi, bool down)
{
    const auto col = b.findColour (TextButton::buttonColourId);
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool lit = b.getToggleState() || down;
    g.setColour (lit ? col.withAlpha (0.25f) : neon::bg0.withAlpha (0.85f));
    g.fillPath (chamfer (r, 5.0f));
    g.setColour (col.withAlpha (lit ? 1.0f : (hi ? 0.9f : 0.55f)));
    g.strokePath (chamfer (r, 5.0f), PathStrokeType (1.0f));
}

void NeonLookAndFeel::drawButtonText (Graphics& g, TextButton& b, bool hi, bool)
{
    const auto col = b.findColour (TextButton::buttonColourId);
    g.setFont (neon::mono (12.0f, true));
    g.setColour ((b.getToggleState() ? col.brighter (0.5f) : col).withAlpha (hi || b.getToggleState() ? 1.0f : 0.8f));
    g.drawText (b.getButtonText(), b.getLocalBounds(), Justification::centred);
}

void NeonLookAndFeel::drawComboBox (Graphics& g, int w, int h, bool, int, int, int, int, ComboBox& box)
{
    const auto col = box.findColour (ComboBox::outlineColourId);
    auto r = Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (neon::bg0.withAlpha (0.9f));
    g.fillPath (chamfer (r, 6.0f));
    g.setColour (col.withAlpha (box.isMouseOver() ? 1.0f : 0.7f));
    g.strokePath (chamfer (r, 6.0f), PathStrokeType (1.0f));
    Path tri;
    const float ax = (float) w - 12.0f, ay = (float) h * 0.5f;
    tri.addTriangle (ax - 4, ay - 2, ax + 4, ay - 2, ax, ay + 3);
    g.setColour (col);
    g.fillPath (tri);
}

void NeonLookAndFeel::positionComboBoxText (ComboBox& box, Label& l)
{
    l.setBounds (6, 1, box.getWidth() - 22, box.getHeight() - 2);
    l.setFont (getComboBoxFont (box));
}

void NeonLookAndFeel::drawTooltip (Graphics& g, const String& text, int w, int h)
{
    g.fillAll (neon::bg0.withAlpha (0.95f));
    g.setColour (neon::cyan.withAlpha (0.7f));
    g.drawRect (0, 0, w, h, 1);
    g.setColour (neon::text);
    g.setFont (neon::mono (12.0f));
    g.drawFittedText (text, Rectangle<int> (0, 0, w, h).reduced (6, 4), Justification::centredLeft, 6);
}

// ============================================================================
//  Controls
// ============================================================================
NeonKnob::NeonKnob (AudioProcessorValueTreeState& s, const String& id, const String& name, Colour c, const String& help)
    : label (name)
{
    slider.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    slider.setColour (Slider::rotarySliderFillColourId, c);
    slider.setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    addAndMakeVisible (slider);
    attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (s, id, slider);

    const String unit = s.getParameter (id)->getLabel();
    slider.textFromValueFunction = [unit] (double v)
    {
        if (unit == "Hz" && v >= 1000.0) return String (v / 1000.0, 1) + "k";
        if (unit == "%") return String ((int) std::round (v)) + "%";
        if (unit == "st") return (v > 0.04 ? "+" : "") + String (v, std::abs (v - std::round (v)) < 0.01 ? 0 : 1) + "st";
        if (unit == "c") return String ((int) std::round (v)) + "c";
        const int dp = std::abs (v) >= 100.0 ? 0 : (std::abs (v) >= 10.0 ? 1 : 2);
        return String (v, dp) + (unit == ":1" ? ":1" : "");
    };
    slider.setDoubleClickReturnValue (true, s.getParameterRange (id).convertFrom0to1 (s.getParameter (id)->getDefaultValue()));
    slider.setPopupDisplayEnabled (true, true, nullptr);
    slider.setTooltip (help.isNotEmpty() ? help : name);
}

void NeonKnob::resized() { slider.setBounds (getLocalBounds().withTrimmedTop (16)); }

void NeonKnob::paint (Graphics& g)
{
    g.setFont (neon::mono (11.0f, true));
    g.setColour (neon::text.withAlpha (0.85f));
    g.drawText (label.toUpperCase(), getLocalBounds().removeFromTop (16), Justification::centred);
}

NeonCombo::NeonCombo (AudioProcessorValueTreeState& s, const String& id, const String& name, Colour c, const String& help)
    : label (name)
{
    box.setColour (ComboBox::outlineColourId, c);
    box.setColour (ComboBox::textColourId, c);
    if (auto* choice = dynamic_cast<AudioParameterChoice*> (s.getParameter (id)))
        box.addItemList (choice->choices, 1);
    box.setTooltip (help.isNotEmpty() ? help : name);
    addAndMakeVisible (box);
    attachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (s, id, box);
}

void NeonCombo::resized() { box.setBounds (getLocalBounds().withTrimmedTop (15)); }

void NeonCombo::paint (Graphics& g)
{
    g.setFont (neon::mono (10.0f, true));
    g.setColour (neon::text.withAlpha (0.75f));
    g.drawText (label.toUpperCase(), getLocalBounds().removeFromTop (14), Justification::left);
}

NeonToggle::NeonToggle (AudioProcessorValueTreeState& s, const String& id, const String& text, Colour c, const String& help)
{
    button.setButtonText (text);
    button.setColour (ToggleButton::tickColourId, c);
    button.setTooltip (help.isNotEmpty() ? help : text);
    addAndMakeVisible (button);
    attachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, id, button);
}

// ============================================================================
//  Module panel
// ============================================================================
ModulePanel::ModulePanel (AudioProcessorValueTreeState& s, const String& powerParam,
                          const String& c, const String& t, Colour col, const String& help)
    : colour (col), code (c), title (t)
{
    hasPower = powerParam.isNotEmpty();
    if (hasPower)
    {
        power.setColour (ToggleButton::tickColourId, col);
        power.onStateChange = [this] { repaint(); };
        power.setTooltip ("Switch the " + t + " module on/off");
        addAndMakeVisible (power);
        powerAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (s, powerParam, power);
    }
    setTooltip (help);
}

Rectangle<int> ModulePanel::getContentArea() const { return getLocalBounds().reduced (14, 10).withTrimmedTop (34); }

void ModulePanel::resized() { power.setBounds (getWidth() - 72, 10, 58, 20); }

void ModulePanel::paint (Graphics& g)
{
    const bool isOn = ! hasPower || power.getToggleState();
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const auto shape = chamfer (r, 14.0f);

    g.setGradientFill (ColourGradient (Colour (0xe00d1923), 0, r.getY(), Colour (0xf005090e), 0, r.getBottom(), false));
    g.fillPath (shape);
    g.saveState();
    g.reduceClipRegion (shape);
    g.setColour (colour.withAlpha (0.035f));
    for (float gx = r.getX(); gx < r.getRight(); gx += 12.0f) g.drawVerticalLine ((int) gx, r.getY(), r.getBottom());
    for (float gy = r.getY(); gy < r.getBottom(); gy += 12.0f) g.drawHorizontalLine ((int) gy, r.getX(), r.getRight());
    g.restoreState();

    glowStroke (g, shape, colour.withAlpha (isOn ? 0.9f : 0.3f), 1.2f, isOn ? 1.0f : 0.3f);
    g.setColour (colour.withAlpha (isOn ? 0.55f : 0.2f));
    g.drawHorizontalLine (38, r.getX() + 12.0f, r.getRight() - 12.0f);

    g.setFont (neon::mono (10.0f));
    g.setColour (neon::dim.brighter (0.4f));
    g.drawText (code, Rectangle<float> (r.getX() + 20.0f, r.getY() + 6.0f, 260.0f, 12.0f), Justification::left);
    glowText (g, title, Rectangle<float> (r.getX() + 20.0f, r.getY() + 16.0f, 320.0f, 18.0f), Justification::left,
              neon::mono (15.0f, true).withExtraKerningFactor (0.15f), isOn ? colour : neon::dim, isOn ? 1.0f : 0.2f);

    auto hz = Rectangle<float> (r.getX() + 10.0f, r.getBottom() - 9.0f, 46.0f, 4.0f);
    g.saveState();
    g.reduceClipRegion (hz.toNearestInt());
    g.setColour (neon::amber.withAlpha (isOn ? 0.55f : 0.2f));
    for (float sx = hz.getX() - 8.0f; sx < hz.getRight(); sx += 8.0f)
    {
        Path st; st.addQuadrilateral (sx, hz.getBottom(), sx + 4, hz.getY(), sx + 8, hz.getY(), sx + 4, hz.getBottom());
        g.fillPath (st);
    }
    g.restoreState();
}

void ModulePanel::paintOverChildren (Graphics& g)
{
    // a switched-off module is dimmed so it is obvious it is not in the signal
    if (hasPower && ! power.getToggleState())
    {
        g.setColour (neon::bg0.withAlpha (0.45f));
        g.fillRect (getContentArea());
    }
}

// ============================================================================
//  Pitch display (live input vs corrected pitch, Antares-style graph)
// ============================================================================
void PitchDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    const bool tuneOn = proc.apvts.getRawParameterValue ("tuneOn")->load() > 0.5f;
    const bool harmOn = proc.apvts.getRawParameterValue ("harmOn")->load() > 0.5f;
    const auto col = tuneOn ? neon::amber : neon::dim;
    auto& tu = proc.tuner;

    g.setColour (Colour (0xf0030507));
    g.fillPath (chamfer (r, 10.0f));
    glowStroke (g, chamfer (r.reduced (1.0f), 10.0f), col.withAlpha (0.6f), 1.0f, 0.5f);

    const float hz = tu.detectedHz.load();
    const int note = tu.targetNote.load();
    shownCents += (tu.correctionCents.load() - shownCents) * 0.35f;

    auto top = r.reduced (12.0f, 8.0f).removeFromTop (54.0f);
    const auto names = PitchCorrector::keyNames();
    const String noteStr = note >= 0 ? names[note % 12] + String (note / 12 - 1) : "--";
    glowText (g, noteStr, top.removeFromLeft (120.0f), Justification::centredLeft, neon::mono (44.0f, true), col, tuneOn ? 1.2f : 0.2f);

    const int held = proc.midiNotesHeld.load();
    g.setFont (neon::mono (10.0f));
    g.setColour (neon::text.withAlpha (0.6f));
    g.drawText (proc.apvts.getRawParameterValue ("tuneMidi")->load() > 0.5f ? "TARGET: MIDI" : "TARGET NOTE", top.removeFromTop (14.0f), Justification::right);
    g.setFont (neon::mono (13.0f, true));
    g.setColour (neon::text);
    g.drawText (hz > 0 ? String (hz, 1) + " Hz" : ((frame / 15) % 2 ? "NO SIGNAL" : ""), top.removeFromTop (18.0f), Justification::right);
    g.setColour (neon::magenta);
    String corr = hz > 0 ? "CORR " + String (shownCents >= 0 ? "+" : "") + String ((int) std::round (shownCents)) + " c" : "";
    if (held > 0) corr = "MIDI x" + String (held) + "   " + corr;
    g.drawText (corr, top, Justification::right);

    auto gr = r.reduced (10.0f).withTrimmedTop (60.0f).withTrimmedBottom (2.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRect (gr);

    const int N = PitchCorrector::historySize;
    const int w0 = tu.histWrite.load();
    float latestT = -1.0f;
    for (int i = N - 1; i >= N - 40 && latestT < 0; --i) latestT = tu.histTarget[(size_t) ((w0 + i) % N)].load();
    if (latestT > 0) viewCentre += (latestT - viewCentre) * 0.12f;
    const float span = 6.0f;
    auto yOf = [&] (float midi) { return gr.getCentreY() - (midi - viewCentre) / span * gr.getHeight() * 0.5f; };

    g.saveState();
    g.reduceClipRegion (gr.toNearestInt());
    const int keyIdx = (int) proc.apvts.getRawParameterValue ("tuneKey")->load();
    for (int m = (int) std::floor (viewCentre - span) - 1; m <= (int) std::ceil (viewCentre + span) + 1; ++m)
    {
        const bool tonic = ((m - keyIdx) % 12 + 12) % 12 == 0;
        g.setColour ((tonic ? neon::amber : neon::cyan).withAlpha (m == (int) latestT ? 0.45f : 0.10f));
        g.drawHorizontalLine ((int) yOf ((float) m), gr.getX(), gr.getRight());
        g.setFont (neon::mono (9.0f));
        g.setColour (neon::text.withAlpha (0.35f));
        g.drawText (names[(m % 12 + 12) % 12] + String (m / 12 - 1), Rectangle<float> (gr.getX() + 3, yOf ((float) m) - 11.0f, 40.0f, 10.0f), Justification::left);
    }

    auto trace = [&] (std::array<std::atomic<float>, PitchCorrector::historySize>& h, Colour c, float width, bool glow, float offset)
    {
        Path p; bool pen = false;
        for (int i = 0; i < N; ++i)
        {
            const float v = h[(size_t) ((w0 + i) % N)].load();
            const float x = gr.getX() + gr.getWidth() * (float) i / (float) (N - 1);
            if (v < 0) { pen = false; continue; }
            if (! pen) p.startNewSubPath (x, yOf (v + offset)); else p.lineTo (x, yOf (v + offset));
            pen = true;
        }
        if (glow) glowStroke (g, p, c, width, 0.8f);
        else { g.setColour (c); g.strokePath (p, PathStrokeType (width)); }
    };
    trace (tu.histIn, neon::text.withAlpha (0.45f), 1.2f, false, 0.0f);
    if (tuneOn) trace (tu.histOut, neon::amber, 1.8f, true, 0.0f);
    if (harmOn)   // show the first harmony voice (approximate interval) as a thin magenta trace
    {
        const int v1 = (int) proc.apvts.getRawParameterValue ("harmVoice1")->load();
        static const float approxSemis[] = { 0, -12, -9, -7, -5, -3, 4, 5, 7, 9, 12, 0 };
        if (v1 > 0 && v1 < 11) trace (tu.histOut, neon::magenta.withAlpha (0.6f), 1.0f, false, approxSemis[v1]);
    }
    g.restoreState();

    g.setFont (neon::mono (9.0f));
    auto legend = gr.reduced (4.0f).removeFromBottom (11.0f);
    g.setColour (neon::text.withAlpha (0.5f)); g.drawText ("IN", legend, Justification::right);
    g.setColour (col);                         g.drawText ("OUT     ", legend, Justification::right);
    if (harmOn) { g.setColour (neon::magenta); g.drawText ("HARM          ", legend, Justification::right); }
}

void GRMeter::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto bar = r.withTrimmedTop (14.0f).withTrimmedBottom (14.0f);
    g.setColour (neon::bg0.withAlpha (0.9f)); g.fillRect (bar);
    g.setColour (colour.withAlpha (0.4f));    g.drawRect (bar, 1.0f);
    const float norm = jlimit (0.0f, 1.0f, -shown / 24.0f);
    auto fill = bar.reduced (3.0f);
    g.setColour (colour);
    g.fillRect (fill.withHeight (fill.getHeight() * norm));
    g.setFont (neon::mono (9.0f, true));
    g.setColour (neon::text.withAlpha (0.8f));
    g.drawText ("GR", r.removeFromTop (12.0f), Justification::centred);
    g.drawText (String ((int) std::round (-shown)), r.removeFromBottom (12.0f), Justification::centred);
}

void LfoScope::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Colour (0xf0030507)); g.fillPath (chamfer (r, 8.0f));
    g.setColour (colour.withAlpha (0.4f)); g.strokePath (chamfer (r.reduced (0.5f), 8.0f), PathStrokeType (1.0f));
    g.setColour (neon::dim.withAlpha (0.5f)); g.drawHorizontalLine ((int) r.getCentreY(), r.getX() + 6, r.getRight() - 6);
    auto a = r.reduced (8.0f, 10.0f);
    Path p;
    const int N = (int) values.size();
    for (int i = 0; i < N; ++i)
    {
        const float v = values[(size_t) ((write + i) % N)];
        const float x = a.getX() + a.getWidth() * (float) i / (float) (N - 1), y = a.getCentreY() - v * a.getHeight() * 0.5f;
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    glowStroke (g, p, colour, 1.6f, 0.8f);
}

// ============================================================================
//  Detect view: key / scale and tempo read-outs
// ============================================================================
void DetectView::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto left = r.removeFromLeft (r.getWidth() * 0.55f).reduced (4.0f, 0.0f);
    auto right = r.reduced (4.0f, 0.0f);
    const auto keys = PitchCorrector::keyNames();
    const auto scales = PitchCorrector::scaleNames();

    auto confBar = [&] (Rectangle<float> b, float conf, Colour c)
    {
        g.setColour (neon::bg0.withAlpha (0.9f)); g.fillRect (b);
        g.setColour (c.withAlpha (0.4f)); g.drawRect (b, 1.0f);
        g.setColour (conf > 0.6f ? c : (conf > 0.35f ? neon::amber : neon::magenta));
        g.fillRect (b.reduced (2.0f).withWidth ((b.getWidth() - 4.0f) * jlimit (0.0f, 1.0f, conf)));
    };

    // ---- key / scale
    g.setFont (neon::mono (10.0f, true));
    g.setColour (neon::text.withAlpha (0.75f));
    g.drawText ("DETECTED KEY", left.removeFromTop (14.0f), Justification::left);
    const int k = proc.detKey.load();
    const float secs = proc.detKeySeconds.load(), conf = proc.detKeyConfidence.load();
    if (k >= 0)
    {
        glowText (g, keys[k] + " " + scales[proc.detScale.load()].toUpperCase(), left.removeFromTop (34.0f), Justification::left,
                  neon::mono (22.0f, true), neon::green, 1.0f);
        confBar (left.removeFromTop (10.0f), conf, neon::green);
        left.removeFromTop (4.0f);
        g.setFont (neon::mono (10.0f));
        g.setColour (neon::text.withAlpha (0.6f));
        const int ak = proc.detAltKey.load();
        g.drawText ("conf " + String ((int) std::round (conf * 100)) + "%   alt: " + (ak >= 0 ? keys[ak] + " " + scales[proc.detAltScale.load()] : String ("-")),
                    left.removeFromTop (13.0f), Justification::left);
        g.drawText ("heard " + String (secs, 1) + " s of singing", left.removeFromTop (13.0f), Justification::left);
    }
    else
    {
        glowText (g, "LISTENING...", left.removeFromTop (34.0f), Justification::left, neon::mono (18.0f, true), neon::dim.brighter (0.5f), 0.4f);
        g.setFont (neon::mono (10.0f));
        g.setColour (neon::text.withAlpha (0.6f));
        g.drawText ("sing ~3 s to detect (" + String (secs, 1) + " s so far)", left.removeFromTop (13.0f), Justification::left);
    }

    // ---- tempo
    g.setFont (neon::mono (10.0f, true));
    g.setColour (neon::text.withAlpha (0.75f));
    g.drawText ("DETECTED TEMPO", right.removeFromTop (14.0f), Justification::left);
    const float bpm = proc.detBpm.load(), bconf = proc.detBpmConfidence.load();
    glowText (g, bpm > 0 ? String (bpm, 1) : String ("---"), right.removeFromTop (34.0f), Justification::left, neon::mono (22.0f, true),
              bpm > 0 ? neon::amber : neon::dim, bpm > 0 ? 1.0f : 0.3f);
    confBar (right.removeFromTop (10.0f), bconf, neon::amber);
    right.removeFromTop (4.0f);
    g.setFont (neon::mono (10.0f));
    g.setColour (neon::text.withAlpha (0.6f));
    g.drawText ("conf " + String ((int) std::round (bconf * 100)) + "%   BPM", right.removeFromTop (13.0f), Justification::left);
    g.drawText ((proc.usingDetectedTempo.load() ? "using: detected " : "using: host ") + String (proc.bpm.load(), 1),
                right.removeFromTop (13.0f), Justification::left);
}

// ============================================================================
//  Chain view: drag modules to reorder the effect chain
// ============================================================================
static const char* modulePower[] = { "eqOn", "compOn", "deessOn", "tmOn", "dblOn", "chorusOn", "delayOn", "reverbOn", "glitchOn" };
static Colour moduleColour (int m)
{
    static const Colour c[] = { neon::cyan, neon::magenta, neon::green, neon::smog, neon::cyan, neon::cyan, neon::amber, neon::magenta, neon::green };
    return c[jlimit (0, 8, m)];
}

Rectangle<float> ChainView::chipBounds (int slot) const
{
    const float pre = 70.0f, post = 60.0f;
    const float w = ((float) getWidth() - pre - post) / (float) params::numModules;
    return { pre + w * (float) slot + 3.0f, 8.0f, w - 6.0f, 40.0f };
}

int ChainView::slotAt (float x) const
{
    const float pre = 70.0f, post = 60.0f;
    const float w = ((float) getWidth() - pre - post) / (float) params::numModules;
    return jlimit (0, params::numModules - 1, (int) ((x - pre) / w));
}

void ChainView::paint (Graphics& g)
{
    const auto order = proc.getChainOrder();
    const auto names = params::moduleNames();
    auto fixedChip = [&] (Rectangle<float> r, const String& s)
    {
        g.setColour (neon::amber.withAlpha (0.15f)); g.fillPath (chamfer (r, 5.0f));
        g.setColour (neon::amber.withAlpha (0.8f));  g.strokePath (chamfer (r, 5.0f), PathStrokeType (1.0f));
        g.setFont (neon::mono (10.0f, true)); g.drawText (s, r, Justification::centred);
    };
    fixedChip ({ 2.0f, 8.0f, 60.0f, 40.0f }, "TUNE");
    fixedChip ({ (float) getWidth() - 54.0f, 8.0f, 52.0f, 40.0f }, "OUT");

    for (int s = 0; s < order.size(); ++s)
    {
        auto r = chipBounds (s);
        g.setColour (neon::dim);
        g.drawLine (r.getX() - 6.0f, r.getCentreY(), r.getX(), r.getCentreY(), 1.0f);
        if (s == dragging) continue;
        const int m = order[s];
        const bool isOn = proc.apvts.getRawParameterValue (modulePower[m])->load() > 0.5f;
        const auto c = moduleColour (m);
        g.setColour (c.withAlpha (isOn ? 0.18f : 0.04f)); g.fillPath (chamfer (r, 5.0f));
        glowStroke (g, chamfer (r, 5.0f), c.withAlpha (isOn ? 0.9f : 0.3f), 1.0f, isOn ? 0.6f : 0.0f);
        g.setFont (neon::mono (10.0f, true));
        g.setColour (isOn ? c.brighter (0.4f) : neon::dim.brighter (0.2f));
        g.drawText (names[m], r, Justification::centred);
        g.setFont (neon::mono (8.0f));
        g.setColour (neon::text.withAlpha (0.4f));
        g.drawText (String (s + 1), r.reduced (4.0f, 2.0f), Justification::topLeft);
    }
    if (dragging >= 0)
    {
        auto r = chipBounds (dragging).withX (dragX - chipBounds (0).getWidth() * 0.5f).translated (0, -3.0f);
        const auto c = moduleColour (order[dragging]);
        g.setColour (c.withAlpha (0.35f)); g.fillPath (chamfer (r, 5.0f));
        glowStroke (g, chamfer (r, 5.0f), c, 1.5f, 1.0f);
        g.setFont (neon::mono (10.0f, true)); g.setColour (Colours::white);
        g.drawText (names[order[dragging]], r, Justification::centred);
        const float ix = chipBounds (slotAt (dragX)).getX() - 3.0f;
        g.setColour (c); g.fillRect (ix - 1.0f, 4.0f, 2.0f, 48.0f);
    }
    g.setFont (neon::mono (10.0f));
    g.setColour (neon::text.withAlpha (0.55f));
    g.drawText ("Drag a module to change the order of the effect chain. Dim = switched off.",
                getLocalBounds().toFloat().withTrimmedTop (56.0f).withHeight (16.0f), Justification::centredLeft);
}

void ChainView::mouseDown (const MouseEvent& e)
{
    for (int s = 0; s < params::numModules; ++s)
        if (chipBounds (s).contains (e.position)) { dragging = s; dragX = e.position.x; repaint(); return; }
}

void ChainView::mouseDrag (const MouseEvent& e) { if (dragging >= 0) { dragX = e.position.x; repaint(); } }

void ChainView::mouseUp (const MouseEvent&)
{
    if (dragging < 0) return;
    auto order = proc.getChainOrder();
    order.move (dragging, slotAt (dragX));
    proc.setChainOrder (order);
    dragging = -1;
    repaint();
}

// ============================================================================
//  Editor
// ============================================================================
ModulePanel* NeonRuinAudioProcessorEditor::panel (int page, const String& power, const String& code, const String& title, Colour c, const String& help)
{
    auto* p = new ModulePanel (proc.apvts, power, code, title, c, help);
    owned.add (p);
    pages[page].addAndMakeVisible (p);
    return p;
}

NeonKnob* NeonRuinAudioProcessorEditor::knob (ModulePanel* p, const String& id, const String& label, const String& help)
{
    auto* k = new NeonKnob (proc.apvts, id, label, p->colour, help);
    owned.add (k);
    p->addAndMakeVisible (k);
    return k;
}

NeonCombo* NeonRuinAudioProcessorEditor::combo (ModulePanel* p, const String& id, const String& label, const String& help)
{
    auto* c = new NeonCombo (proc.apvts, id, label, p->colour, help);
    owned.add (c);
    p->addAndMakeVisible (c);
    return c;
}

NeonToggle* NeonRuinAudioProcessorEditor::toggle (ModulePanel* p, const String& id, const String& text, const String& help)
{
    auto* t = new NeonToggle (proc.apvts, id, text, p->colour, help);
    owned.add (t);
    p->addAndMakeVisible (t);
    return t;
}

NeonRuinAudioProcessorEditor::NeonRuinAudioProcessorEditor (NeonRuinAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    tooltips.setLookAndFeel (&lnf);

    // ------------------------------------------------------------ header
    addAndMakeVisible (presetBox);
    presetBox.setColour (ComboBox::outlineColourId, neon::cyan);
    presetBox.setColour (ComboBox::textColourId, neon::cyan);
    presetBox.setTooltip ("Presets: factory sounds and your own saved presets");
    presetBox.onChange = [this]
    {
        const int id = presetBox.getSelectedId();
        if (id <= 0) return;
        if (id < 1000) proc.loadFactoryPreset (id - 1);
        else if (id - 1000 < userPresets.size()) proc.loadUserPreset (userPresets[id - 1000]);
    };
    prevBtn.onClick = [this] { const int n = (int) params::factoryPresets().size(); proc.loadFactoryPreset ((proc.getCurrentProgram() + n - 1) % n); };
    nextBtn.onClick = [this] { const int n = (int) params::factoryPresets().size(); proc.loadFactoryPreset ((proc.getCurrentProgram() + 1) % n); };
    saveBtn.onClick = [this] { savePresetDialog(); };
    randomBtn.onClick = [this] { proc.randomise(); };
    aBtn.onClick = [this] { proc.selectAB (0); };
    bBtn.onClick = [this] { proc.selectAB (1); };
    copyBtn.onClick = [this] { proc.copyABToOther(); };
    prevBtn.setTooltip ("Previous factory preset");
    nextBtn.setTooltip ("Next factory preset");
    saveBtn.setTooltip ("Save the current sound as your own preset");
    randomBtn.setTooltip ("Roll the dice: a random but musical new sound");
    aBtn.setTooltip ("Compare: slot A");
    bBtn.setTooltip ("Compare: slot B");
    copyBtn.setTooltip ("Copy the current slot into the other slot");
    randomBtn.setColour (TextButton::buttonColourId, neon::magenta);
    for (auto* b : { &aBtn, &bBtn, &copyBtn }) b->setColour (TextButton::buttonColourId, neon::amber);
    for (auto* b : { &prevBtn, &nextBtn, &saveBtn, &randomBtn, &aBtn, &bBtn, &copyBtn }) addAndMakeVisible (b);

    chaosKnob = std::make_unique<NeonKnob> (proc.apvts, "chaos", "Chaos", neon::magenta,
        "CHAOS: pushes drive, crush, ring mod, glitch, feedback, reverb size, detune and LFO depth toward extremes (in modules that are on)");
    inKnob  = std::make_unique<NeonKnob> (proc.apvts, "inGain",  "Input",  neon::cyan, "Input gain");
    outKnob = std::make_unique<NeonKnob> (proc.apvts, "outGain", "Output", neon::magenta, "Output gain");
    for (auto* k : { chaosKnob.get(), inKnob.get(), outKnob.get() }) addAndMakeVisible (k);

    // ------------------------------------------------------------ tabs
    const char* tabNames[] = { "VOICE", "FX", "SPACE", "MOD" };
    const char* tabHelp[] = { "Autotune, harmony and gate", "EQ, compressor, de-esser, Transmission and doubler",
                              "Delay, reverb, chorus and glitch", "LFOs, Chaos macro and effect chain order" };
    for (int i = 0; i < 4; ++i)
    {
        tabs[i].setButtonText (tabNames[i]);
        tabs[i].setTooltip (tabHelp[i]);
        tabs[i].onClick = [this, i] { showPage (i); };
        addAndMakeVisible (tabs[i]);
        addChildComponent (pages[i]);
    }

    // ------------------------------------------------------------ VOICE page
    tune.panel = panel (0, "tuneOn", "MOD-01 // PITCH", "AUTOTUNE", neon::amber);
    pitchDisplay = new PitchDisplay (proc); owned.add (pitchDisplay); tune.panel->addAndMakeVisible (pitchDisplay);
    tune.combos = { combo (tune.panel, "tuneKey", "Key", "Key of the song"),
                    combo (tune.panel, "tuneScale", "Scale", "Which notes the voice is pulled to") };
    tune.toggles = { toggle (tune.panel, "tuneMidi", "MIDI TARGET", "Play notes on a MIDI keyboard: the voice is tuned to the notes you hold") };
    tune.knobs = { knob (tune.panel, "tuneSpeed", "Speed", "How fast notes are corrected. 0 = hard robot effect, 20-50 ms = natural"),
                   knob (tune.panel, "tuneHumanize", "Humanize", "Long, held notes get a gentler correction"),
                   knob (tune.panel, "tuneFlex", "Flex-Tune", "Only correct notes already close to the target; leaves slides and bends alone"),
                   knob (tune.panel, "tuneVibrato", "Vibrato", "How much of the singer's own vibrato is kept"),
                   knob (tune.panel, "tuneFormant", "Formant", "Voice character: smaller/bigger throat, without changing pitch"),
                   knob (tune.panel, "tuneTranspose", "Transpose", "Shift the whole vocal in semitones (keeps the voice character)"),
                   knob (tune.panel, "tuneMix", "Mix", "Blend corrected and original voice") };

    harm.panel = panel (0, "harmOn", "MOD-02 // CHOIR", "HARMONY", neon::magenta, "Extra voices generated from your voice");
    harm.combos = { combo (harm.panel, "harmVoice1", "Voice 1", "Interval in the scale, or MIDI = sings the lowest held MIDI note"),
                    combo (harm.panel, "harmVoice2", "Voice 2", "Interval in the scale, or MIDI = sings the 2nd held MIDI note"),
                    combo (harm.panel, "harmVoice3", "Voice 3", "Interval in the scale, or MIDI = sings the 3rd held MIDI note") };
    harm.knobs = { knob (harm.panel, "harmMix", "Mix", "Level of the harmony voices"),
                   knob (harm.panel, "harmWidth", "Width", "Stereo spread of the voices"),
                   knob (harm.panel, "harmDetune", "Detune", "Small pitch drift between voices - more = bigger choir"),
                   knob (harm.panel, "harmFormant", "Formant", "Voice character of the extra voices (e.g. + for female, - for male)"),
                   knob (harm.panel, "harmSub", "Sub Oct", "Octave-down layer"),
                   knob (harm.panel, "harmUp", "Oct Up", "Octave-up layer") };

    detect.panel = panel (0, {}, "SYS // LISTEN", "DETECT", neon::green,
                          "Listens to the vocal and detects key, scale and tempo");
    detectView = new DetectView (proc); owned.add (detectView); detect.panel->addAndMakeVisible (detectView);
    detect.toggles = { toggle (detect.panel, "keyAuto", "AUTO", "Automatically set the autotune key/scale when the detection is confident") };
    detect.combos = { combo (detect.panel, "tempoSource", "Tempo", "Which tempo the synced delay, glitch and LFOs follow: the DAW (host) or the detected tempo (with x1/2 and x2 to fix half/double-tempo mistakes)") };
    for (auto* b : { &applyKeyBtn, &resetKeyBtn, &resetBpmBtn }) { b->setColour (TextButton::buttonColourId, neon::green); detect.panel->addAndMakeVisible (b); }
    applyKeyBtn.setTooltip ("Use the detected key and scale in the autotune");
    resetKeyBtn.setTooltip ("Forget what was heard and start detecting the key again");
    resetBpmBtn.setTooltip ("Start detecting the tempo again");
    applyKeyBtn.onClick = [this] { proc.applyDetectedKey(); };
    resetKeyBtn.onClick = [this] { proc.resetKeyDetection(); };
    resetBpmBtn.onClick = [this] { proc.resetBpmDetection(); };

    gateP.panel = panel (0, "gateOn", "MOD-00 // CLEAN", "GATE", neon::green, "Silences background noise between phrases");
    gateP.knobs = { knob (gateP.panel, "gateThresh", "Thresh", "Below this level the gate closes"),
                    knob (gateP.panel, "gateRelease", "Release", "How fast the gate closes") };

    // ------------------------------------------------------------ FX page
    eq.panel = panel (1, "eqOn", "MOD-03 // SPECTRA", "EQUALIZER", neon::cyan);
    eq.knobs = { knob (eq.panel, "eqLowCut", "Low Cut", "Removes rumble below this frequency"),
                 knob (eq.panel, "eqLowGain", "Low", "Body / warmth (200 Hz shelf)"),
                 knob (eq.panel, "eqMidFreq", "Mid Freq", "Frequency of the mid band"),
                 knob (eq.panel, "eqMidGain", "Mid", "Boost/cut the mid band"),
                 knob (eq.panel, "eqHighGain", "Air", "Brightness / air (high shelf)") };
    comp.panel = panel (1, "compOn", "MOD-04 // DYN", "COMPRESSOR", neon::magenta);
    compMeter = new GRMeter ([this] { return proc.gainReductionDb.load(); }, neon::magenta);
    owned.add (compMeter); comp.panel->addAndMakeVisible (compMeter);
    comp.knobs = { knob (comp.panel, "compThresh", "Thresh"), knob (comp.panel, "compRatio", "Ratio"),
                   knob (comp.panel, "compAttack", "Attack"), knob (comp.panel, "compRelease", "Release"),
                   knob (comp.panel, "compMakeup", "Makeup") };
    deess.panel = panel (1, "deessOn", "MOD-05 // SIBIL", "DE-ESSER", neon::green, "Tames sharp S sounds");
    deessMeter = new GRMeter ([this] { return proc.deessReductionDb.load(); }, neon::green);
    owned.add (deessMeter); deess.panel->addAndMakeVisible (deessMeter);
    deess.knobs = { knob (deess.panel, "deessFreq", "Freq", "S sounds above this frequency are reduced"),
                    knob (deess.panel, "deessThresh", "Thresh", "Lower = more de-essing"),
                    knob (deess.panel, "deessRange", "Range", "Maximum reduction") };
    trans.panel = panel (1, "tmOn", "MOD-06 // SIGNAL", "TRANSMISSION", neon::smog, "Distortion, bit-crushing, radio filter and ring modulator");
    trans.knobs = { knob (trans.panel, "tmDrive", "Drive", "Saturation / distortion"),
                    knob (trans.panel, "tmCrush", "Crush", "Bit-crusher and sample-rate reduction (lo-fi digital)"),
                    knob (trans.panel, "tmRadio", "Radio", "Band-limits the voice like an old radio or telephone"),
                    knob (trans.panel, "tmRingFreq", "Ring Hz", "Ring modulator frequency (metallic / robot tones)"),
                    knob (trans.panel, "tmRingMix", "Ring Mix", "Amount of ring modulation"),
                    knob (trans.panel, "tmMix", "Mix", "Blend processed and clean") };
    dbl.panel = panel (1, "dblOn", "MOD-07 // TWIN", "DOUBLER", neon::cyan, "Simulates double-tracking: thick and wide");
    dbl.knobs = { knob (dbl.panel, "dblAmount", "Amount"), knob (dbl.panel, "dblWidth", "Width"),
                  knob (dbl.panel, "dblDetune", "Detune", "Pitch difference between the copies"),
                  knob (dbl.panel, "dblDelay", "Delay", "Timing difference between the copies") };

    // ------------------------------------------------------------ SPACE page
    delayP.panel = panel (2, "delayOn", "MOD-08 // ECHO", "DELAY", neon::amber);
    delayP.toggles = { toggle (delayP.panel, "delaySync", "SYNC", "Lock the delay time to the song tempo"),
                       toggle (delayP.panel, "delayPingPong", "PING-PONG", "Echoes bounce between left and right"),
                       toggle (delayP.panel, "delayReverse", "REVERSE", "Echoes play backwards") };
    delayP.combos = { combo (delayP.panel, "delayDiv", "Division", "Delay time in note values (when SYNC is on)") };
    delayP.knobs = { knob (delayP.panel, "delayTime", "Time", "Delay time in ms (when SYNC is off)"),
                     knob (delayP.panel, "delayFeedback", "Feedback", "Number of repeats"),
                     knob (delayP.panel, "delayMix", "Mix") };
    reverbP.panel = panel (2, "reverbOn", "MOD-09 // SPACE", "REVERB", neon::magenta);
    reverbP.toggles = { toggle (reverbP.panel, "reverbFreeze", "FREEZE", "Hold the reverb tail forever - sing a chord into it") };
    reverbP.knobs = { knob (reverbP.panel, "reverbSize", "Size"), knob (reverbP.panel, "reverbDamp", "Damp"),
                      knob (reverbP.panel, "reverbWidth", "Width"), knob (reverbP.panel, "reverbMix", "Mix") };
    chorusP.panel = panel (2, "chorusOn", "MOD-10 // MOD", "CHORUS", neon::cyan);
    chorusP.knobs = { knob (chorusP.panel, "chorusRate", "Rate"), knob (chorusP.panel, "chorusDepth", "Depth"), knob (chorusP.panel, "chorusMix", "Mix") };
    glitchP.panel = panel (2, "glitchOn", "MOD-11 // FRACTURE", "GLITCH", neon::green, "Tempo-synced stutter: beats get repeated and reversed at random");
    glitchP.combos = { combo (glitchP.panel, "glitchRate", "Slice", "Length of the repeated slice") };
    glitchP.knobs = { knob (glitchP.panel, "glitchChance", "Chance", "Probability that a beat gets glitched"),
                      knob (glitchP.panel, "glitchReverse", "Reverse", "Probability that the repeats play backwards"),
                      knob (glitchP.panel, "glitchMix", "Mix") };

    // ------------------------------------------------------------ MOD page
    Parts* lfoParts[] = { &lfo1, &lfo2 };
    for (int n = 0; n < 2; ++n)
    {
        const String pre = "lfo" + String (n + 1);
        auto& L = *lfoParts[n];
        L.panel = panel (3, {}, "MOD-1" + String (2 + n) + " // MOTION", "LFO " + String (n + 1), n == 0 ? neon::cyan : neon::amber,
                         "An LFO moves a parameter up and down by itself");
        L.combos = { combo (L.panel, pre + "Target", "Target", "Which parameter this LFO moves"),
                     combo (L.panel, pre + "Shape", "Shape", "Waveform: smooth, stepped random (S&H) or drifting"),
                     combo (L.panel, pre + "Div", "Sync Rate", "LFO speed in note values (when SYNC is on)") };
        L.toggles = { toggle (L.panel, pre + "Sync", "SYNC", "Lock the LFO to the song tempo") };
        L.knobs = { knob (L.panel, pre + "Rate", "Rate", "LFO speed in Hz (when SYNC is off)"),
                    knob (L.panel, pre + "Depth", "Depth", "How far the LFO moves the target (negative = inverted)") };
        scopes[n] = new LfoScope (proc.lfoOut[n], L.panel->colour);
        owned.add (scopes[n]);
        L.panel->addAndMakeVisible (scopes[n]);
    }
    routing.panel = panel (3, {}, "SYS // ROUTING", "SIGNAL CHAIN", neon::green, "Order of the effect modules");
    chainView = new ChainView (proc); owned.add (chainView); routing.panel->addAndMakeVisible (chainView);
    macro.panel = panel (3, {}, "SYS // ENTROPY", "CHAOS MACRO", neon::magenta);
    bigChaos = std::make_unique<NeonKnob> (proc.apvts, "chaos", "Chaos", neon::magenta,
        "CHAOS: pushes drive, crush, ring mod, glitch, feedback, reverb size, detune and LFO depth toward extremes");
    macro.panel->addAndMakeVisible (bigChaos.get());
    chaosInfo.setText ("One knob, many parameters. CHAOS pushes drive, crush, ring mod, glitch chance, delay feedback, "
                       "reverb size and detune toward the extremes, and makes both LFOs move harder.\n\n"
                       "Only modules that are switched ON are affected.", dontSendNotification);
    chaosInfo.setFont (neon::mono (11.0f));
    chaosInfo.setColour (Label::textColourId, neon::text.withAlpha (0.75f));
    chaosInfo.setJustificationType (Justification::centredLeft);
    macro.panel->addAndMakeVisible (chaosInfo);

    for (int i = 0; i < 160; ++i)
        rain.push_back ({ rng.nextFloat() * 1280.0f, rng.nextFloat() * 800.0f, 9.0f + rng.nextFloat() * 10.0f, 6.0f + rng.nextFloat() * 14.0f });

    refreshPresetBox();
    setSize (1280, 790);
    showPage (0);
    startTimerHz (30);
}

NeonRuinAudioProcessorEditor::~NeonRuinAudioProcessorEditor()
{
    tooltips.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void NeonRuinAudioProcessorEditor::showPage (int index)
{
    currentPage = jlimit (0, 3, index);
    for (int i = 0; i < 4; ++i)
    {
        pages[i].setVisible (i == currentPage);
        tabs[i].setToggleState (i == currentPage, dontSendNotification);
    }
    repaint();
}

void NeonRuinAudioProcessorEditor::refreshPresetBox()
{
    presetBox.clear (dontSendNotification);
    presetBox.addSectionHeading ("FACTORY");
    const auto& f = params::factoryPresets();
    for (int i = 0; i < (int) f.size(); ++i) presetBox.addItem (f[(size_t) i].name, i + 1);
    userPresets = proc.getUserPresets();
    if (! userPresets.isEmpty())
    {
        presetBox.addSeparator();
        presetBox.addSectionHeading ("USER");
        for (int i = 0; i < userPresets.size(); ++i) presetBox.addItem (userPresets[i].getFileNameWithoutExtension(), 1000 + i);
    }
    presetBox.setText (proc.getCurrentPresetName(), dontSendNotification);
    lastPresetName = proc.getCurrentPresetName();
}

void NeonRuinAudioProcessorEditor::savePresetDialog()
{
    auto* w = new AlertWindow ("SAVE PRESET", "Name your sound:", MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel (&lnf);
    w->addTextEditor ("name", proc.getCurrentPresetName());
    w->addButton ("SAVE", 1, KeyPress (KeyPress::returnKey));
    w->addButton ("CANCEL", 0, KeyPress (KeyPress::escapeKey));
    Component::SafePointer<NeonRuinAudioProcessorEditor> safe (this);
    w->enterModalState (true, ModalCallbackFunction::create ([safe, w] (int result)
    {
        if (safe == nullptr || result != 1) return;
        if (safe->proc.saveUserPreset (w->getTextEditorContents ("name")))
            safe->refreshPresetBox();
    }), true);
}

// ------------------------------------------------------------ layout helpers
static void rowOfKnobs (const std::vector<NeonKnob*>& ks, Rectangle<int> area, int maxSize = 120)
{
    if (ks.empty()) return;
    const int w = area.getWidth() / (int) ks.size();
    const int size = jmin (maxSize, w);
    for (auto* k : ks)
    {
        auto cell = area.removeFromLeft (w);
        k->setBounds (cell.withSizeKeepingCentre (size - 4, jmin (cell.getHeight(), size + 16)));
    }
}

static void gridOfKnobs (const std::vector<NeonKnob*>& ks, Rectangle<int> area, int cols)
{
    const int rows = ((int) ks.size() + cols - 1) / cols;
    const int w = area.getWidth() / cols, h = area.getHeight() / rows;
    for (int i = 0; i < (int) ks.size(); ++i)
    {
        const int r = i / cols, c = i % cols;
        const int inRow = jmin (cols, (int) ks.size() - r * cols);
        const int xOff = (cols - inRow) * w / 2;
        ks[(size_t) i]->setBounds (Rectangle<int> (area.getX() + xOff + c * w, area.getY() + r * h, w, h)
                                     .withSizeKeepingCentre (jmin (w, h + 4) - 4, jmin (h, w + 16)));
    }
}

static Rectangle<int> column (Rectangle<int>& area, int width, int gapAfter = 12)
{
    auto c = area.removeFromLeft (width);
    area.removeFromLeft (gapAfter);
    return c;
}

void NeonRuinAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    headerArea = area.removeFromTop (96).toFloat();
    tabArea = area.removeFromTop (40).toFloat();
    footerArea = area.removeFromBottom (122).toFloat();
    auto pageArea = area.reduced (16, 4);

    // ---------------- header
    {
        auto h = headerArea.toNearestInt().reduced (16, 8);
        h.removeFromLeft (318);                                       // title
        auto right = h.removeFromRight (300);
        auto outA = right.removeFromRight (140);
        outMeterArea = outA.removeFromRight (64).withSizeKeepingCentre (64, 12).translated (0, 10).toFloat();
        outKnob->setBounds (outA.withSizeKeepingCentre (72, 80));
        auto inA = right.removeFromRight (140);
        inMeterArea = inA.removeFromRight (64).withSizeKeepingCentre (64, 12).translated (0, 10).toFloat();
        inKnob->setBounds (inA.withSizeKeepingCentre (72, 80));
        chaosKnob->setBounds (h.removeFromRight (84).withSizeKeepingCentre (80, 80));
        h.removeFromRight (14);

        auto bar = h.withSizeKeepingCentre (h.getWidth(), 30).translated (0, -10);
        prevBtn.setBounds (bar.removeFromLeft (30)); bar.removeFromLeft (4);
        auto bTail = bar.removeFromRight (240);
        nextBtn.setBounds (bar.removeFromRight (30)); bar.removeFromRight (4);
        presetBox.setBounds (bar);
        bTail.removeFromLeft (8);
        saveBtn.setBounds (bTail.removeFromLeft (54)); bTail.removeFromLeft (6);
        randomBtn.setBounds (bTail.removeFromLeft (72)); bTail.removeFromLeft (10);
        aBtn.setBounds (bTail.removeFromLeft (26)); bTail.removeFromLeft (2);
        bBtn.setBounds (bTail.removeFromLeft (26)); bTail.removeFromLeft (4);
        copyBtn.setBounds (bTail);
    }

    // ---------------- tabs
    {
        auto t = tabArea.toNearestInt().reduced (16, 4);
        for (int i = 0; i < 4; ++i) { tabs[i].setBounds (t.removeFromLeft (120)); t.removeFromLeft (6); }
    }

    for (auto& pg : pages) pg.setBounds (pageArea);
    const int W = pageArea.getWidth(), gap = 10;
    const int r1h = 282, r2h = pageArea.getHeight() - r1h - gap;
    const Rectangle<int> row1 (0, 0, W, r1h), row2 (0, r1h + gap, W, r2h);
    auto split = [gap] (Rectangle<int> row, std::initializer_list<float> fractions, std::initializer_list<ModulePanel*> panels)
    {
        const int total = row.getWidth() - gap * ((int) panels.size() - 1);
        auto f = fractions.begin();
        int i = 0;
        for (auto* p : panels)
        {
            const bool last = ++i == (int) panels.size();
            p->setBounds (last ? row : row.removeFromLeft ((int) (*f++ * (float) total)));
            if (! last) row.removeFromLeft (gap);
        }
    };

    // ---------------- VOICE
    tune.panel->setBounds (row1);
    {
        auto c = tune.panel->getContentArea();
        pitchDisplay->setBounds (column (c, 430, 16));
        auto col = column (c, 132, 10).withSizeKeepingCentre (132, 200);
        tune.combos[0]->setBounds (col.removeFromTop (44)); col.removeFromTop (14);
        tune.combos[1]->setBounds (col.removeFromTop (44)); col.removeFromTop (22);
        tune.toggles[0]->setBounds (col.removeFromTop (28));
        gridOfKnobs (tune.knobs, c, 4);
    }
    split (row2, { 0.52f, 0.31f }, { harm.panel, detect.panel, gateP.panel });
    {
        auto c = detect.panel->getContentArea();
        auto bottom = c.removeFromBottom (44);
        detectView->setBounds (c);
        const int half = (int) (bottom.getWidth() * 0.55f);
        auto l = bottom.removeFromLeft (half).withTrimmedTop (16).withTrimmedRight (8);
        applyKeyBtn.setBounds (l.removeFromLeft (62)); l.removeFromLeft (4);
        resetKeyBtn.setBounds (l.removeFromLeft (56)); l.removeFromLeft (4);
        detect.toggles[0]->setBounds (l);
        auto rgt = bottom;
        resetBpmBtn.setBounds (rgt.removeFromRight (56).withTrimmedTop (16)); rgt.removeFromRight (4);
        detect.combos[0]->setBounds (rgt);
    }
    {
        auto c = harm.panel->getContentArea();
        auto col = column (c, 120, 14);
        const int ch = col.getHeight() / 3;
        for (auto* cb : harm.combos) cb->setBounds (col.removeFromTop (ch).withSizeKeepingCentre (120, 42));
        rowOfKnobs (harm.knobs, c);
        rowOfKnobs (gateP.knobs, gateP.panel->getContentArea());
    }

    // ---------------- FX
    split (row1, { 0.5f }, { eq.panel, comp.panel });
    rowOfKnobs (eq.knobs, eq.panel->getContentArea());
    { auto c = comp.panel->getContentArea(); compMeter->setBounds (column (c, 26, 10)); rowOfKnobs (comp.knobs, c); }
    split (row2, { 0.27f, 0.45f }, { deess.panel, trans.panel, dbl.panel });
    { auto c = deess.panel->getContentArea(); deessMeter->setBounds (column (c, 26, 10)); rowOfKnobs (deess.knobs, c); }
    rowOfKnobs (trans.knobs, trans.panel->getContentArea());
    rowOfKnobs (dbl.knobs, dbl.panel->getContentArea());

    // ---------------- SPACE
    split (row1, { 0.52f }, { delayP.panel, reverbP.panel });
    {
        auto c = delayP.panel->getContentArea();
        auto col = column (c, 132, 10).withSizeKeepingCentre (132, 196);
        delayP.toggles[0]->setBounds (col.removeFromTop (28)); col.removeFromTop (8);
        delayP.combos[0]->setBounds (col.removeFromTop (44)); col.removeFromTop (16);
        delayP.toggles[1]->setBounds (col.removeFromTop (28)); col.removeFromTop (10);
        delayP.toggles[2]->setBounds (col.removeFromTop (28));
        rowOfKnobs (delayP.knobs, c);
    }
    {
        auto c = reverbP.panel->getContentArea();
        auto right = c.removeFromRight (110);
        reverbP.toggles[0]->setBounds (right.withSizeKeepingCentre (104, 44));
        rowOfKnobs (reverbP.knobs, c);
    }
    split (row2, { 0.36f }, { chorusP.panel, glitchP.panel });
    rowOfKnobs (chorusP.knobs, chorusP.panel->getContentArea());
    {
        auto c = glitchP.panel->getContentArea();
        glitchP.combos[0]->setBounds (column (c, 120, 14).withSizeKeepingCentre (120, 44));
        rowOfKnobs (glitchP.knobs, c);
    }

    // ---------------- MOD
    split (row1, { 0.5f }, { lfo1.panel, lfo2.panel });
    Parts* lfoParts[] = { &lfo1, &lfo2 };
    for (int n = 0; n < 2; ++n)
    {
        auto& L = *lfoParts[n];
        auto c = L.panel->getContentArea();
        auto col = column (c, 140, 12);
        L.combos[0]->setBounds (col.removeFromTop (44)); col.removeFromTop (8);
        L.combos[1]->setBounds (col.removeFromTop (44)); col.removeFromTop (12);
        L.toggles[0]->setBounds (col.removeFromTop (28)); col.removeFromTop (8);
        L.combos[2]->setBounds (col.removeFromTop (44));
        auto scope = c.removeFromBottom (70);
        scopes[n]->setBounds (scope.reduced (4, 4));
        rowOfKnobs (L.knobs, c, 110);
    }
    split (row2, { 0.74f }, { routing.panel, macro.panel });
    chainView->setBounds (routing.panel->getContentArea().withSizeKeepingCentre (routing.panel->getContentArea().getWidth(), 76));
    {
        auto c = macro.panel->getContentArea();
        bigChaos->setBounds (c.removeFromLeft (130).withSizeKeepingCentre (126, 140));
        chaosInfo.setBounds (c.reduced (8, 0));
    }

    // ---------------- skyline cache
    skylineCache = Image (Image::ARGB, getWidth(), (int) footerArea.getHeight() + 40, true);
    Graphics sg (skylineCache);
    drawSkyline (sg, skylineCache.getBounds().toFloat());
}

// ============================================================================
//  Painting
// ============================================================================
void NeonRuinAudioProcessorEditor::drawSkyline (Graphics& g, Rectangle<float> a)
{
    Random r (77);
    const float base = a.getBottom();
    for (float x = a.getX() - 10.0f; x < a.getRight();)
    {
        const float w = 18.0f + r.nextFloat() * 50.0f, h = 40.0f + r.nextFloat() * (a.getHeight() - 30.0f);
        g.setColour (Colour (0xff0c1722));
        g.fillRect (x, base - h, w, h);
        for (float wy = base - h + 6; wy < base - 4; wy += 7)
            for (float wx = x + 3; wx < x + w - 3; wx += 6)
                if (r.nextFloat() < 0.12f)
                {
                    g.setColour ((r.nextBool() ? neon::amber : neon::cyan).withAlpha (0.15f + r.nextFloat() * 0.3f));
                    g.fillRect (wx, wy, 2.0f, 3.0f);
                }
        x += w + r.nextFloat() * 4.0f;
    }
    for (float x = a.getX() - 20.0f; x < a.getRight();)
    {
        const float w = 30.0f + r.nextFloat() * 80.0f, h = 20.0f + r.nextFloat() * (a.getHeight() * 0.65f);
        g.setColour (Colour (0xff05080d));
        g.fillRect (x, base - h, w, h);
        g.setColour (neon::cyan.withAlpha (0.12f));
        g.drawVerticalLine ((int) x, base - h, base);
        for (float wy = base - h + 5; wy < base - 3; wy += 6)
            for (float wx = x + 4; wx < x + w - 4; wx += 7)
                if (r.nextFloat() < 0.07f)
                {
                    g.setColour ((r.nextFloat() < 0.7f ? neon::amber : neon::magenta).withAlpha (0.4f + r.nextFloat() * 0.5f));
                    g.fillRect (wx, wy, 3.0f, 2.0f);
                }
        if (r.nextFloat() < 0.18f && h > 40.0f)
        {
            auto sign = Rectangle<float> (x + w * 0.5f - 4.0f, base - h + 8.0f, 8.0f, jmin (50.0f, h - 16.0f));
            const auto sc = r.nextBool() ? neon::magenta : neon::cyan;
            g.setColour (sc.withAlpha (0.15f)); g.fillRect (sign.expanded (6.0f, 4.0f));
            g.setColour (sc.withAlpha (0.8f));  g.fillRect (sign);
        }
        x += w;
    }
}

void NeonRuinAudioProcessorEditor::drawMeter (Graphics& g, Rectangle<float> r, float db, Colour c)
{
    g.setColour (neon::bg0.withAlpha (0.85f)); g.fillRect (r);
    g.setColour (c.withAlpha (0.4f)); g.drawRect (r, 1.0f);
    const float norm = jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    auto fill = r.reduced (2.0f);
    const int segs = 14;
    const float sw = fill.getWidth() / (float) segs;
    for (int i = 0; i < segs; ++i)
    {
        const bool lit = (float) i / (float) segs < norm;
        const auto sc = i > segs - 3 ? Colours::red : c;
        g.setColour (lit ? sc : sc.withAlpha (0.08f));
        g.fillRect (fill.getX() + (float) i * sw + 0.5f, fill.getY(), sw - 1.5f, fill.getHeight());
    }
    g.setFont (neon::mono (9.0f));
    g.setColour (neon::text.withAlpha (0.8f));
    g.drawText (db <= -99.0f ? "-inf" : String (db, 1), r.translated (0, -14.0f), Justification::centred);
}

void NeonRuinAudioProcessorEditor::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (ColourGradient (neon::bg1, 0, 0, Colour (0xff1c0d08), 0, b.getBottom(), false));
    g.fillAll();
    g.setGradientFill (ColourGradient (neon::smog.withAlpha (0.22f), b.getCentreX(), b.getBottom(),
                                       Colours::transparentBlack, b.getCentreX(), b.getBottom() - 420.0f, true));
    g.fillRect (b);
    g.setGradientFill (ColourGradient (neon::magenta.withAlpha (0.10f), b.getRight(), 0, Colours::transparentBlack, b.getRight() - 500.0f, 200.0f, true));
    g.fillRect (b);

    g.setColour (neon::cyan.withAlpha (0.05f));
    const float horizon = tabArea.getBottom();
    for (int i = -20; i <= 20; ++i) g.drawLine (b.getCentreX(), horizon, b.getCentreX() + (float) i * 90.0f, b.getBottom(), 1.0f);
    for (float y = horizon, step = 4.0f; y < b.getBottom(); y += step, step *= 1.25f) g.drawHorizontalLine ((int) y, 0, b.getRight());

    // header
    g.setOpacity (0.3f);
    g.drawImageAt (skylineCache, 0, (int) headerArea.getBottom() - skylineCache.getHeight());
    g.setOpacity (1.0f);
    g.setGradientFill (ColourGradient (neon::bg0.withAlpha (0.92f), 0, 0, neon::bg0.withAlpha (0.25f), 0, headerArea.getBottom(), false));
    g.fillRect (headerArea);
    glowText (g, "NEON RUIN", { 24.0f, 14.0f, 320.0f, 44.0f }, Justification::left,
              neon::mono (38.0f, true).withExtraKerningFactor (0.16f), neon::cyan, flicker);
    g.setFont (neon::mono (10.5f));
    g.setColour (neon::magenta.withAlpha (0.9f));
    g.drawText ("VOCAL PROCESSING UNIT  //  NR-7", Rectangle<float> (27.0f, 58.0f, 300.0f, 14.0f), Justification::left);
    g.setColour (neon::amber.withAlpha (0.8f)); g.fillRect (27.0f, 76.0f, 60.0f, 2.0f);
    g.setColour (neon::dim);                    g.fillRect (91.0f, 76.0f, 200.0f, 1.0f);

    // preset description under the preset box
    {
        const int idx = proc.getCurrentProgram();
        const auto& f = params::factoryPresets();
        String desc = (proc.getCurrentPresetName() == f[(size_t) idx].name) ? String (f[(size_t) idx].description) : String ("User / modified sound");
        g.setFont (neon::mono (10.5f));
        g.setColour (neon::text.withAlpha (0.65f));
        g.drawText (desc, presetBox.getBounds().translated (0, 34).withHeight (14).withRight (copyBtn.getRight()).toFloat(), Justification::left);
        g.setColour (neon::amber.withAlpha (0.8f));
        g.setFont (neon::mono (9.5f, true));
        g.drawText ("SLOT " + String (proc.getABSlot() == 0 ? "A" : "B"), copyBtn.getBounds().translated (0, 34).withHeight (14).toFloat(), Justification::right);
    }

    shownIn  = jmax (proc.inLevelDb.load(),  shownIn  - 1.5f);
    shownOut = jmax (proc.outLevelDb.load(), shownOut - 1.5f);
    drawMeter (g, inMeterArea,  shownIn,  neon::cyan);
    drawMeter (g, outMeterArea, shownOut, neon::magenta);

    // tab bar status
    {
        auto t = tabArea.reduced (16.0f, 6.0f);
        g.setColour (neon::cyan.withAlpha (0.25f));
        g.drawHorizontalLine ((int) tabArea.getBottom() - 2, 16.0f, b.getRight() - 16.0f);
        g.setFont (neon::mono (11.0f, true));
        auto status = t.removeFromRight (640.0f);
        const int held = proc.midiNotesHeld.load();
        const bool glitchOn = proc.apvts.getRawParameterValue ("glitchOn")->load() > 0.5f;
        g.setColour (neon::text.withAlpha (0.7f));
        g.drawText ((proc.usingDetectedTempo.load() ? "DET " : "") + String (proc.bpm.load(), 1) + " BPM", status.removeFromRight (110.0f), Justification::right);
        auto led = [&] (const String& label, bool lit, Colour c)
        {
            auto r = status.removeFromRight (110.0f);
            g.setColour (lit ? c : neon::dim);
            g.fillEllipse (r.getRight() - 12.0f, r.getCentreY() - 4.0f, 8.0f, 8.0f);
            if (lit) { g.setColour (c.withAlpha (0.3f)); g.fillEllipse (r.getRight() - 15.0f, r.getCentreY() - 7.0f, 14.0f, 14.0f); }
            g.setColour (lit ? c : neon::dim.brighter (0.3f));
            g.drawText (label, r.withTrimmedRight (18.0f), Justification::right);
        };
        led ("MIDI " + String (held), held > 0, neon::green);
        led ("GLITCH", glitchOn && proc.glitchActive.load(), neon::green);
        led ("CHAOS", proc.apvts.getRawParameterValue ("chaos")->load() > 0.5f, neon::magenta);
        led ("LIMIT", proc.limiterDb.load() < -0.5f, Colours::red);
    }

    // footer
    g.drawImageAt (skylineCache, 0, (int) footerArea.getBottom() - skylineCache.getHeight());
    g.setGradientFill (ColourGradient (Colours::transparentBlack, 0, footerArea.getY(), neon::bg0.withAlpha (0.8f), 0, footerArea.getBottom(), false));
    g.fillRect (footerArea);
    if ((frameCount / 15) % 2 == 0)
    {
        g.setColour (Colours::red.withAlpha (0.9f));
        g.fillEllipse (212.0f, footerArea.getY() + 40.0f, 3.0f, 3.0f);
        g.fillEllipse (857.0f, footerArea.getY() + 28.0f, 3.0f, 3.0f);
    }
    g.setFont (neon::mono (11.0f));
    g.setColour (neon::text.withAlpha (0.75f));
    const double lat = 1000.0 * proc.getLatencySamples() / jmax (1.0, proc.getSampleRate());
    g.drawText ("SYS://NEON-RUIN v0.3   SR " + String (proc.getSampleRate() / 1000.0, 1) + "k   LATENCY " + String (lat, 1) + " ms",
                footerArea.reduced (20, 8).removeFromBottom (14), Justification::left);
    g.setColour (neon::amber.withAlpha (0.75f));
    g.drawText (String ("ACID RAIN ADVISORY // STAY INDOORS") + ((frameCount / 12) % 2 ? "_" : " "),
                footerArea.reduced (20, 8).removeFromBottom (14), Justification::right);
}

void NeonRuinAudioProcessorEditor::paintOverChildren (Graphics& g)
{
    for (auto& d : rain)
    {
        const bool inHeader = d.y < headerArea.getBottom() - 10.0f;
        const bool inFooter = d.y > footerArea.getY();
        if (! (inHeader || inFooter)) continue;
        g.setColour (neon::text.withAlpha (inFooter ? 0.22f : 0.10f));
        g.drawLine (d.x, d.y, d.x - d.len * 0.18f, d.y + d.len, 1.0f);
    }
    g.setColour (Colours::black.withAlpha (0.10f));
    for (int y = 0; y < getHeight(); y += 3) g.drawHorizontalLine (y, 0.0f, (float) getWidth());
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (ColourGradient (Colours::transparentBlack, b.getCentreX(), b.getCentreY(), Colours::black.withAlpha (0.45f), 0, 0, true));
    g.fillRect (b);
}

void NeonRuinAudioProcessorEditor::timerCallback()
{
    ++frameCount;
    for (auto& d : rain)
    {
        d.y += d.speed;
        d.x -= d.speed * 0.18f;
        if (d.y > (float) getHeight()) { d.y = -20.0f; d.x = rng.nextFloat() * (float) getWidth() + 60.0f; }
    }
    flicker = rng.nextFloat() < 0.03f ? 0.3f : 1.0f;

    if (proc.getCurrentPresetName() != lastPresetName) refreshPresetBox();
    aBtn.setToggleState (proc.getABSlot() == 0, dontSendNotification);
    bBtn.setToggleState (proc.getABSlot() == 1, dontSendNotification);

    repaint (headerArea.toNearestInt());
    repaint (tabArea.toNearestInt());
    repaint (footerArea.toNearestInt());
    switch (currentPage)
    {
        case 0: pitchDisplay->frame = frameCount; pitchDisplay->repaint(); if (frameCount % 6 == 0) detectView->repaint(); break;
        case 1: compMeter->tick(); deessMeter->tick(); break;
        case 3: scopes[0]->tick(); scopes[1]->tick(); chainView->repaint(); break;
        default: break;
    }
}
