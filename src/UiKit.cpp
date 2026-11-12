#include "UiKit.h"

namespace ui
{
juce::Font font (float h, bool bold)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

void drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title)
{
    g.setColour (panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    if (title.isNotEmpty())
    {
        g.setColour (dim);
        g.setFont (font (11.0f, true));
        g.drawText (title.toUpperCase(), r.withHeight (20.0f).toNearestInt(), juce::Justification::centred);
    }
}
} // namespace ui

// ------------------------------------------------------------------------------------------------ look
OtLook::OtLook()
{
    using namespace ui;
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, bg);
    setColour (juce::Slider::textBoxOutlineColourId, line);
    setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
    setColour (juce::Label::textColourId, dim);
    setColour (juce::ComboBox::backgroundColourId, panelHi);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, line);
    setColour (juce::ComboBox::arrowColourId, dim);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));
    setColour (juce::TextButton::buttonColourId, panelHi);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, bg);
    setColour (juce::TextEditor::backgroundColourId, bg);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, line);
}

void OtLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
{
    using namespace ui;
    const auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
    const float d = std::min (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    const float rad = d * 0.5f;
    const float ang = start + pos * (end - start);
    const bool bipolar = (bool) s.getProperties().getWithDefault ("bipolar", false);

    // knob body
    g.setGradientFill (juce::ColourGradient (panelHi.brighter (0.15f), c.x, c.y - rad, bg.brighter (0.05f), c.x, c.y + rad, false));
    g.fillEllipse (c.x - rad * 0.72f, c.y - rad * 0.72f, rad * 1.44f, rad * 1.44f);
    g.setColour (line.brighter (0.2f));
    g.drawEllipse (c.x - rad * 0.72f, c.y - rad * 0.72f, rad * 1.44f, rad * 1.44f, 1.0f);
    // value arc
    const float ar = rad * 0.9f;
    juce::Path track, val;
    track.addCentredArc (c.x, c.y, ar, ar, 0.0f, start, end, true);
    g.setColour (line);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float zero = bipolar ? (start + end) * 0.5f : start;
    val.addCentredArc (c.x, c.y, ar, ar, 0.0f, std::min (zero, ang), std::max (zero, ang), true);
    g.setColour (s.isEnabled() ? accent : dim);
    g.strokePath (val, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // pointer
    juce::Path ptr;
    ptr.addRoundedRectangle (-1.5f, -rad * 0.68f, 3.0f, rad * 0.34f, 1.5f);
    g.setColour (text);
    g.fillPath (ptr, juce::AffineTransform::rotation (ang).translated (c.x, c.y));
}

void OtLook::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider& s)
{
    using namespace ui;
    const float cy = y + h * 0.5f;
    g.setColour (bg);
    g.fillRoundedRectangle ((float) x, cy - 2.5f, (float) w, 5.0f, 2.5f);
    g.setColour (accent.withAlpha (s.isEnabled() ? 0.9f : 0.3f));
    g.fillRoundedRectangle ((float) x, cy - 2.5f, std::max (0.0f, pos - x), 5.0f, 2.5f);
    g.setColour (text);
    g.fillRoundedRectangle (pos - 4.0f, cy - 8.0f, 8.0f, 16.0f, 2.5f);
}

juce::Slider::SliderLayout OtLook::getSliderLayout (juce::Slider& s)
{
    auto l = LookAndFeel_V4::getSliderLayout (s);
    if (s.getSliderStyle() == juce::Slider::LinearHorizontal) l.sliderBounds = s.getLocalBounds().reduced (2, 0);
    return l;
}

void OtLook::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down)
{
    using namespace ui;
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();
    auto fill = on ? (juce::Colour) b.findColour (juce::TextButton::buttonOnColourId) : panelHi;
    if (hover) fill = fill.brighter (0.12f);
    if (down) fill = fill.brighter (0.25f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (on ? fill.brighter (0.3f) : line);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void OtLook::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                     .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.drawText (b.getButtonText(), b.getLocalBounds().reduced (2, 0), juce::Justification::centred);
}

void OtLook::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    using namespace ui;
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (panelHi);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path arrow;
    const float cx = w - 14.0f, cy = h * 0.5f;
    arrow.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
    g.setColour (box.isEnabled() ? dim : dim.withAlpha (0.4f));
    g.fillPath (arrow);
}

// ------------------------------------------------------------------------------------------------ knob
ParamKnob::ParamKnob (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& title, bool bipolar)
{
    caption.setText (title.toUpperCase(), juce::dontSendNotification);
    caption.setJustificationType (juce::Justification::centred);
    caption.setFont (ui::font (11.0f, true));
    caption.setColour (juce::Label::textColourId, ui::text.withAlpha (0.85f));
    addAndMakeVisible (caption);
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 18);
    slider.setDoubleClickReturnValue (true, apvts.getParameter (id)->convertFrom0to1 (apvts.getParameter (id)->getDefaultValue()));
    slider.getProperties().set ("bipolar", bipolar);
    slider.setPopupMenuEnabled (false);
    addAndMakeVisible (slider);
    att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, slider);
}

void ParamKnob::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromTop (14));
    slider.setBounds (r);
}

// ------------------------------------------------------------------------------------------------ meter
MeterView::MeterView (std::atomic<float>* l, std::atomic<float>* r) : peak { l, r }
{
    startTimerHz (30);
}

void MeterView::timerCallback()
{
    for (int c = 0; c < 2; ++c)
    {
        const float p = peak[c]->exchange (0.0f);
        level[c] = std::max (p, level[c] * 0.82f);
        hold[c] = std::max (p, hold[c] * 0.985f);
    }
    repaint();
}

void MeterView::paint (juce::Graphics& g)
{
    using namespace ui;
    auto r = getLocalBounds().toFloat();
    g.setColour (bg);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
    auto area = r.reduced (4.0f, 6.0f);
    const float labelW = 24.0f;
    auto bars = area.withTrimmedLeft (labelW);
    const float bw = (bars.getWidth() - 4.0f) / 2.0f;
    auto yFor = [&] (float db) { return area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };
    g.setFont (font (9.0f));
    float lastLabelY = -100.0f;
    for (float db : { 0.0f, -3.0f, -6.0f, -12.0f, -24.0f, -48.0f })
    {
        const float y = yFor (db);
        if (std::abs (y - lastLabelY) < 11.0f) continue; // short meters: skip labels that would overlap
        lastLabelY = y;
        g.setColour (dim);
        g.drawText (juce::String ((int) db == 0 ? "-0" : juce::String ((int) db)), (int) area.getX(), (int) y - 5, (int) labelW - 2, 10, juce::Justification::centredRight);
        g.setColour (line);
        g.drawHorizontalLine ((int) y, bars.getX(), bars.getRight());
    }
    for (int c = 0; c < 2; ++c)
    {
        auto bar = juce::Rectangle<float> (bars.getX() + c * (bw + 4.0f), area.getY(), bw, area.getHeight());
        const float db = juce::Decibels::gainToDecibels (level[c], -80.0f);
        const float top = yFor (db);
        g.saveState();
        g.reduceClipRegion (bar.withTop (top).toNearestInt());
        juce::ColourGradient gr (red, 0, yFor (0.0f), green, 0, yFor (-18.0f), false);
        gr.addColour (0.12, orange);
        g.setGradientFill (gr);
        g.fillRect (bar);
        g.restoreState();
        const float hy = yFor (juce::Decibels::gainToDecibels (hold[c], -80.0f));
        g.setColour (text.withAlpha (0.8f));
        g.fillRect (bar.getX(), hy, bar.getWidth(), 1.5f);
    }
}

// ------------------------------------------------------------------------------------------------ scope
ScopeView::ScopeView (juce::AudioProcessorValueTreeState& a, const EngineTelemetry& t)
    : apvts (a), tel (t), thr (a.getParameter ("threshold"))
{
    startTimerHz (30);
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ScopeView::timerCallback()
{
    const int h = tel.head.load (std::memory_order_acquire);
    const float t = apvts.getRawParameterValue ("threshold")->load();
    if (h != lastHead || dragging || ! juce::approximatelyEqual (t, lastThr)) { lastHead = h; lastThr = t; repaint(); } // knob moves the lines even when nothing plays
}

float ScopeView::thresholdY() const
{
    const float amp = juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("threshold")->load());
    const float half = getHeight() * 0.5f;
    return half - shape (amp) * (half - 4.0f);
}

bool ScopeView::nearThreshold (float y) const
{
    const float up = thresholdY(), dn = getHeight() - up;
    return std::abs (y - up) < 9.0f || std::abs (y - dn) < 9.0f;
}

void ScopeView::setThresholdFromY (float y)
{
    const float half = getHeight() * 0.5f;
    const float v = juce::jlimit (0.0f, 1.0f, std::abs (y - half) / (half - 4.0f));
    const float db = juce::jlimit (-60.0f, 0.0f, juce::Decibels::gainToDecibels (unshape (v), -80.0f));
    thr->setValueNotifyingHost (thr->convertTo0to1 (db));
}

void ScopeView::mouseMove (const juce::MouseEvent& e)
{
    const bool near = nearThreshold ((float) e.y);
    if (near != hover) { hover = near; setMouseCursor (near ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor); repaint(); }
}

void ScopeView::mouseDown (const juce::MouseEvent& e)
{
    if (! nearThreshold ((float) e.y)) return; // only the white lines are grabbable
    dragging = true;
    thr->beginChangeGesture();
    setThresholdFromY ((float) e.y);
}
void ScopeView::mouseDrag (const juce::MouseEvent& e) { if (dragging) setThresholdFromY ((float) e.y); }
void ScopeView::mouseUp (const juce::MouseEvent&) { if (dragging) { dragging = false; thr->endChangeGesture(); repaint(); } }
void ScopeView::mouseDoubleClick (const juce::MouseEvent&) { thr->beginChangeGesture(); thr->setValueNotifyingHost (thr->getDefaultValue()); thr->endChangeGesture(); }

void ScopeView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const float db = juce::jlimit (-60.0f, 0.0f, apvts.getRawParameterValue ("threshold")->load() + (w.deltaY > 0 ? 1.0f : -1.0f) * std::max (0.5f, std::abs (w.deltaY) * 6.0f));
    thr->beginChangeGesture();
    thr->setValueNotifyingHost (thr->convertTo0to1 (db));
    thr->endChangeGesture();
}

void ScopeView::paint (juce::Graphics& g)
{
    using namespace ui;
    auto r = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0b0d11), 0, 0, juce::Colour (0xff10141b), 0, r.getHeight(), false));
    g.fillRoundedRectangle (r, 6.0f);
    const float w = r.getWidth(), half = r.getHeight() * 0.5f, ampH = half - 4.0f;

    // grid
    g.setColour (juce::Colour (0xff1b212b));
    for (float a : { 0.01f, 0.05f, 0.2f, 0.5f })
        for (int sgn = -1; sgn <= 1; sgn += 2)
            g.drawHorizontalLine ((int) (half - sgn * shape (a) * ampH), 0.0f, w);
    g.setColour (juce::Colour (0xff262d39));
    g.drawHorizontalLine ((int) half, 0.0f, w);

    // waveform: last kShownColumns columns, drawn per pixel as a min/max bar
    const int head = tel.head.load (std::memory_order_acquire);
    const int px = std::max (1, (int) w);
    bool any = false;
    const float colsPerPx = (float) kShownColumns / (float) px;
    for (int x = 0; x < px; ++x)
    {
        int c0 = head - kShownColumns + (int) std::floor (x * colsPerPx);
        int c1 = head - kShownColumns + (int) std::floor ((x + 1) * colsPerPx);
        c1 = std::max (c1, c0 + 1);
        float lo = 0.0f, hi = 0.0f;
        for (int c = c0; c < c1; ++c)
        {
            if (c < 0 || c < head - EngineTelemetry::kColumns) continue;
            const auto& col = tel.cols[(size_t) (c % EngineTelemetry::kColumns)];
            lo = std::min (lo, col.lo.load (std::memory_order_relaxed));
            hi = std::max (hi, col.hi.load (std::memory_order_relaxed));
        }
        const float yHi = half - shape (hi) * ampH, yLo = half - shape (lo) * ampH;
        if (hi > 1e-5f || lo < -1e-5f) any = true;
        g.setColour (accent);
        g.fillRect ((float) x, std::min (yHi, half - 0.5f), 1.0f, std::max (1.0f, std::max (yLo, half + 0.5f) - std::min (yHi, half - 0.5f)));
    }

    // trigger markers
    for (int x = 0; x < px; ++x)
    {
        const int c0 = head - kShownColumns + (int) std::floor (x * colsPerPx);
        const int c1 = std::max (head - kShownColumns + (int) std::floor ((x + 1) * colsPerPx), c0 + 1);
        int vel = 0;
        for (int c = c0; c < c1; ++c)
            if (c >= 0 && c >= head - EngineTelemetry::kColumns)
                vel = std::max (vel, tel.cols[(size_t) (c % EngineTelemetry::kColumns)].trigVel.load (std::memory_order_relaxed));
        if (vel <= 0) continue;
        g.setColour (orange.withAlpha (0.35f));
        g.drawVerticalLine (x, 0.0f, r.getHeight());
        juce::Path t;
        t.addTriangle ((float) x - 4.0f, 2.0f, (float) x + 4.0f, 2.0f, (float) x, 10.0f);
        t.addTriangle ((float) x - 4.0f, r.getHeight() - 2.0f, (float) x + 4.0f, r.getHeight() - 2.0f, (float) x, r.getHeight() - 10.0f);
        g.setColour (orange);
        g.fillPath (t);
    }

    // threshold lines
    const float ty = thresholdY(), by = r.getHeight() - ty;
    const auto lineCol = (dragging || hover) ? juce::Colours::white : juce::Colours::white.withAlpha (0.75f);
    g.setColour (lineCol);
    g.drawHorizontalLine ((int) ty, 0.0f, w);
    g.drawHorizontalLine ((int) by, 0.0f, w);
    g.setColour (lineCol.withAlpha (0.18f));
    g.fillRect (0.0f, ty, w, by - ty); // dead zone between the lines: nothing in here triggers
    const float db = apvts.getRawParameterValue ("threshold")->load();
    g.setColour (lineCol);
    g.setFont (font (11.0f, true));
    g.drawText (juce::String (db, 1) + " dB", juce::Rectangle<int> (6, (int) ty - 16, 80, 14), juce::Justification::left);

    if (! any)
    {
        g.setColour (dim.withAlpha (0.8f));
        g.setFont (font (12.0f));
        g.drawText ("Play audio into this track.  Drag the white lines: anything above them triggers.", getLocalBounds().withTrimmedTop ((int) half + 24).reduced (10, 0), juce::Justification::centredTop);
    }
    g.setColour (line);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
}
