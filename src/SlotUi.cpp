#include "SlotUi.h"
#include "SmartTrigger.h"

namespace
{
juce::String slotTitle (const OpenTriggerProcessor::SlotInfo& i)
{
    if (i.data != nullptr) return i.data->meta.name.isNotEmpty() ? i.data->meta.name : i.file.getFileNameWithoutExtension();
    return {};
}

juce::String artName (const OpenTriggerProcessor::SlotInfo& i, int art)
{
    if (i.data == nullptr || art < 0 || art >= (int) i.data->meta.articulations.size()) return {};
    const auto n = ArticulationName::parse (i.data->meta.articulations[(size_t) art].name);
    return n.group.isEmpty() || n.group == "Hit" ? n.label : n.label + "  -  " + n.group;
}

void drawCover (juce::Graphics& g, const juce::Image& img, juce::Rectangle<int> r, bool fill)
{
    g.setColour (ui::bg);
    g.fillRoundedRectangle (r.toFloat(), 5.0f);
    if (img.isValid())
    {
        g.saveState();
        juce::Path clip;
        clip.addRoundedRectangle (r.toFloat(), 5.0f);
        g.reduceClipRegion (clip);
        g.drawImage (img, r.toFloat(), fill ? juce::RectanglePlacement::fillDestination : juce::RectanglePlacement::centred);
        g.restoreState();
    }
    g.setColour (ui::line);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);
}
} // namespace

// ============================================================================================ SlotCard
SlotCard::SlotCard (OpenTriggerProcessor& p, int index) : proc (p), idx (index)
{
    for (auto* b : { &lBtn, &eBtn, &xBtn, &mBtn, &sBtn, &pBtn })
    {
        addAndMakeVisible (*b);
        b->setWantsKeyboardFocus (false);
    }
    for (auto* b : { &mBtn, &sBtn, &pBtn }) b->setClickingTogglesState (true);
    mBtn.setColour (juce::TextButton::buttonOnColourId, ui::orange);
    sBtn.setColour (juce::TextButton::buttonOnColourId, ui::green);
    pBtn.setColour (juce::TextButton::buttonOnColourId, ui::accent);
    lBtn.onClick = [this] { if (onBrowse) onBrowse (idx); };
    eBtn.onClick = [this] { if (onEdit) onEdit (idx); };
    xBtn.onClick = [this] { if (onClear) onClear (idx); };
    mAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, OpenTriggerProcessor::slotParam (idx, "mute"), mBtn);
    sAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, OpenTriggerProcessor::slotParam (idx, "solo"), sBtn);
    pAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, OpenTriggerProcessor::slotParam (idx, "phase"), pBtn);
    vol.setSliderStyle (juce::Slider::LinearHorizontal);
    vol.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    vol.setDoubleClickReturnValue (true, 0.0);
    vol.setPopupMenuEnabled (false);
    addAndMakeVisible (vol);
    vAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (p.apvts, OpenTriggerProcessor::slotParam (idx, "vol"), vol);
    lastHits = proc.telemetry().hits[idx].load();
    startTimerHz (30);
    refresh();
}

void SlotCard::refresh()
{
    info = proc.slotInfo (idx);
    const bool has = info.status == OpenTriggerProcessor::SlotStatus::ready;
    eBtn.setEnabled (has);
    xBtn.setEnabled (info.status != OpenTriggerProcessor::SlotStatus::empty);
    for (auto* c : { (juce::Component*) &mBtn, (juce::Component*) &sBtn, (juce::Component*) &pBtn, (juce::Component*) &vol }) c->setEnabled (true);
    repaint();
}

void SlotCard::timerCallback()
{
    const unsigned h = proc.telemetry().hits[idx].load (std::memory_order_relaxed);
    if (h != lastHits) { lastHits = h; flash = 1.0f; }
    if (flash > 0.0f || info.status == OpenTriggerProcessor::SlotStatus::loading)
    {
        flash = std::max (0.0f, flash - 0.12f);
        repaint();
    }
}

void SlotCard::resized()
{
    lBtn.setBounds (78, 38, 28, 18);
    eBtn.setBounds (108, 38, 28, 18);
    xBtn.setBounds (138, 38, 28, 18);
    mBtn.setBounds (176, 38, 28, 18);
    sBtn.setBounds (206, 38, 28, 18);
    pBtn.setBounds (236, 38, 28, 18);
    vol.setBounds (78, 58, 144, 16);
}

void SlotCard::mouseDown (const juce::MouseEvent& e)
{
    if (onSelect) onSelect (idx);
    if (! thumbRect().contains (e.getPosition())) return;
    if (info.status == OpenTriggerProcessor::SlotStatus::ready)
    {
        // click height = how hard: bottom of the picture is soft, top is hard
        const float t = 1.0f - (float) (e.y - thumbRect().getY()) / (float) thumbRect().getHeight();
        proc.auditionSlot (idx, 40 + (int) (juce::jlimit (0.0f, 1.0f, t) * 87.0f));
    }
    else if (info.status == OpenTriggerProcessor::SlotStatus::failed && info.error.startsWith ("password needed") && onUnlock) onUnlock (idx);
    else if (info.status != OpenTriggerProcessor::SlotStatus::loading && onBrowse) onBrowse (idx);
}

void SlotCard::paint (juce::Graphics& g)
{
    using namespace ui;
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (selected ? panelHi : panel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (selected ? accent : line);
    g.drawRoundedRectangle (r, 6.0f, selected ? 1.5f : 1.0f);

    const auto tr = thumbRect();
    const bool ready = info.status == OpenTriggerProcessor::SlotStatus::ready;
    drawCover (g, info.data != nullptr ? info.data->cover : juce::Image(), flash > 0.0f ? tr.reduced ((int) std::lround (flash * 3.0f)) : tr, false); // a small squash on every hit
    if (info.status == OpenTriggerProcessor::SlotStatus::empty)
    {
        g.setColour (dim);
        g.setFont (font (11.0f, true));
        g.drawText ("EMPTY", tr.withTrimmedTop (36), juce::Justification::centred);
        g.setFont (font (28.0f));
        g.drawText ("+", tr.withTrimmedBottom (14), juce::Justification::centred);
    }
    else if (info.status == OpenTriggerProcessor::SlotStatus::loading)
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (tr.toFloat(), 5.0f);
        g.setColour (text);
        g.setFont (font (11.0f, true));
        const int dots = (int) (juce::Time::getMillisecondCounter() / 300) % 4;
        g.drawText ("LOADING" + juce::String::repeatedString (".", dots), tr, juce::Justification::centred);
    }
    else if (ready && hover)
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (tr.toFloat(), 5.0f);
        juce::Path tri;
        const auto c = tr.getCentre().toFloat();
        tri.addTriangle (c.x - 8, c.y - 11, c.x - 8, c.y + 11, c.x + 12, c.y);
        g.setColour (text);
        g.fillPath (tri);
    }
    if (flash > 0.0f)
    {
        g.setColour (orange.withAlpha (flash));
        g.drawRoundedRectangle (tr.toFloat().reduced (0.5f), 5.0f, 2.0f);
        g.fillEllipse ((float) tr.getX() + 4.0f, (float) tr.getY() + 4.0f, 7.0f, 7.0f);
    }

    g.setColour (ready ? text : dim);
    g.setFont (font (13.0f, true));
    juce::String title = info.status == OpenTriggerProcessor::SlotStatus::empty ? "Slot " + juce::String (idx + 1) : slotTitle (info);
    if (info.status == OpenTriggerProcessor::SlotStatus::loading) title = info.file.getFileNameWithoutExtension();
    const bool locked = info.status == OpenTriggerProcessor::SlotStatus::failed && info.error.startsWith ("password needed");
    if (info.status == OpenTriggerProcessor::SlotStatus::failed) title = locked ? "Locked" : "Could not load";
    g.drawText (title, 78, 4, 188, 17, juce::Justification::centredLeft);
    g.setFont (font (11.0f));
    g.setColour (info.status == OpenTriggerProcessor::SlotStatus::failed ? red : dim);
    juce::String sub = ready ? artName (info, info.contentArticulation) : (locked ? juce::String ("click the picture to enter the pass phrase") : info.status == OpenTriggerProcessor::SlotStatus::failed ? info.error : juce::String());
    g.drawText (sub, 78, 20, 188, 14, juce::Justification::centredLeft);

    const float db = proc.apvts.getRawParameterValue (OpenTriggerProcessor::slotParam (idx, "vol"))->load();
    g.setColour (dim);
    g.setFont (font (10.0f));
    g.drawText (juce::String (db, 1) + " dB", 224, 58, 44, 16, juce::Justification::centredRight);
}

// ============================================================================================ SlotDetail
SlotDetail::SlotDetail (OpenTriggerProcessor& p) : proc (p)
{
    for (int s = 0; s < kSlots; ++s)
    {
        pan[s] = std::make_unique<ParamKnob> (p.apvts, OpenTriggerProcessor::slotParam (s, "pan"), "Pan", true);
        tune[s] = std::make_unique<ParamKnob> (p.apvts, OpenTriggerProcessor::slotParam (s, "tune"), "Tune", true);
        addChildComponent (*pan[s]);
        addChildComponent (*tune[s]);
        // Tune reads like a tuner: the pitch the piece rings at now ("187.3 Hz  F#3 +12c"); type a Hz value to tune to it.
        auto& sl = tune[s]->slider;
        sl.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 126, 18);
        sl.textFromValueFunction = [this] (double semis)
        {
            const float base = info.fundamentalHz;
            if (base <= 0.0f) return juce::String (semis, 2) + " st";
            const float hz = tunedHz (base, (float) semis);
            return juce::String (hz, 1) + " Hz  " + noteWithCents (hz);
        };
        sl.valueFromTextFunction = [this] (const juce::String& text)
        {
            const auto t = text.trim();
            const float v = t.getFloatValue(), base = info.fundamentalHz;
            if (base <= 0.0f || t.containsIgnoreCase ("st")) return (double) v;
            return (double) juce::jlimit (-12.0f, 12.0f, semisForHz (base, v));
        };
    }
    setSlot (0);
    startTimerHz (15);
}

void SlotDetail::setSlot (int s)
{
    slot = juce::jlimit (0, kSlots - 1, s);
    activeGroup.clear();
    for (int i = 0; i < kSlots; ++i)
    {
        pan[i]->setVisible (i == slot && viewMode == View::mics);
        tune[i]->setVisible (i == slot && viewMode == View::mics);
    }
    refresh();
}

void SlotDetail::refresh()
{
    const auto prevData = info.data;
    info = proc.slotInfo (slot);
    if (prevData != info.data) activeGroup.clear();
    layoutChips();
    repaint();
}

void SlotDetail::timerCallback()
{
    const auto now = proc.slotInfo (slot);
    const bool changedData = now.data != info.data || now.status != info.status;
    if (now.data != info.data) activeGroup.clear();
    const bool pitchChanged = now.fundamentalHz != info.fundamentalHz;
    info = now;
    if (pitchChanged) tune[slot]->slider.updateText();
    if (changedData) layoutChips();
    const int art = proc.slotArticulation (slot);
    if (art != shownArt || changedData)
    {
        shownArt = art;
        if (strokeMode() && info.data != nullptr && ! info.data->meta.articulations.empty())
        {
            const auto g = ArticulationName::parse (info.data->meta.articulations[(size_t) juce::jlimit (0, (int) info.data->meta.articulations.size() - 1, art)].name).group;
            if (g != activeGroup) { activeGroup = g; layoutChips(); }
        }
        repaint();
    }
}

void SlotDetail::resized()
{
    const int w = getWidth();
    for (int i = 0; i < kSlots; ++i)
    {
        pan[i]->setBounds (w - 292, 56, 130, 120);
        tune[i]->setBounds (w - 148, 56, 130, 120);
    }
    layoutChips();
}

void SlotDetail::layoutChips()
{
    chips.clear();
    headings.clear();
    groups.clear();
    strokeBox = {};
    chipArea = { 16, 90, getWidth() - 340, getHeight() - 100 };
    const int tabRight = viewMode == View::mixer ? getWidth() - 16 : chipArea.getRight();
    tabOuts = { tabRight - 112, 22, 112, 24 };
    tabMics = { tabOuts.getX() - 118, 22, 112, 24 };
    mixedBtn = {}; resetBtn = {};
    strips.clear();
    if (info.data == nullptr) return;
    if (viewMode == View::mixer)
    {
        const auto& arts0 = info.data->meta.articulations;
        chips.assign (arts0.size(), {});
        for (auto& a : arts0)
        {
            const auto g = ArticulationName::parse (a.name).group;
            if (std::find (groups.begin(), groups.end(), g) == groups.end()) groups.push_back (g);
        }
        if (std::find (groups.begin(), groups.end(), activeGroup) == groups.end())
            activeGroup = ArticulationName::parse (arts0[(size_t) juce::jlimit (0, (int) arts0.size() - 1, proc.slotArticulation (slot))].name).group;
        strokeBox = groups.size() > 1 ? juce::Rectangle<int> { 352, 22, 190, 26 } : juce::Rectangle<int>();
        const int bx = groups.size() > 1 ? 550 : 352;
        mixedBtn = { bx, 22, 86, 26 };
        resetBtn = { bx + 92, 22, 86, 26 };
        std::vector<int> mine;
        for (int i = 0; i < (int) arts0.size(); ++i)
            if (ArticulationName::parse (arts0[(size_t) i].name).group == activeGroup) mine.push_back (i);
        const int W = getWidth() - 32, n = std::max (1, (int) mine.size());
        const int sw = std::min (118, W / n);
        const int y0 = 62, y1 = getHeight() - 26;
        int x = 16;
        for (int art : mine)
        {
            Strip st;
            st.art = art;
            st.area = { x, y0, sw - 6, y1 - y0 };
            const int cx = st.area.getCentreX();
            st.pan = { cx - 17, y0 + 4, 34, 34 };
            st.mute = { cx - 30, y0 + 42, 28, 18 };
            st.solo = { cx + 2, y0 + 42, 28, 18 };
            st.phase = { cx - 30, y0 + 63, 60, 18 };
            st.fader = { cx - 14, y0 + 88, 28, st.area.getHeight() - 88 - 64 };
            st.name = { x + 2, y1 - 46, sw - 10, 16 };
            st.out = { x + 6, y1 - 24, sw - 18, 20 };
            strips.push_back (st);
            x += sw;
        }
        return;
    }
    const auto& arts = info.data->meta.articulations;
    const int cw = 146, ch = 28, gap = 8;
    const int perRow = std::max (1, (chipArea.getWidth() + gap) / (cw + gap));

    // stroke types in order of first appearance; every articulation of one sits together whatever its order in the file
    for (auto& a : arts)
    {
        const auto g = ArticulationName::parse (a.name).group;
        if (std::find (groups.begin(), groups.end(), g) == groups.end()) groups.push_back (g);
    }
    chips.assign (arts.size(), {});
    if (groups.size() <= 1)   // one style: just the mic chips
    {
        int x = 0, y = 0;
        for (size_t i = 0; i < arts.size(); ++i)
        {
            chips[i] = { chipArea.getX() + x * (cw + gap), chipArea.getY() + y, cw, ch };
            if (++x >= perRow) { x = 0; y += ch + gap; }
        }
        return;
    }

    // Stroke Type dropdown, then only the mics of the chosen type
    const int cur = juce::jlimit (0, (int) arts.size() - 1, proc.slotArticulation (slot));
    if (std::find (groups.begin(), groups.end(), activeGroup) == groups.end()) activeGroup = ArticulationName::parse (arts[(size_t) cur].name).group;
    strokeBox = { chipArea.getX(), chipArea.getY(), 300, 28 };
    chipArea.setY (chipArea.getY() + 58);
    chipArea.setHeight (std::max (ch, chipArea.getHeight() - 58));
    int x = 0, y = 0;
    for (size_t i = 0; i < arts.size(); ++i)
    {
        if (ArticulationName::parse (arts[i].name).group != activeGroup) continue;
        chips[i] = { chipArea.getX() + x * (cw + gap), chipArea.getY() + y, cw, ch };
        if (++x >= perRow) { x = 0; y += ch + gap; }
    }
}

void SlotDetail::selectStroke (const juce::String& group)
{
    if (info.data == nullptr || std::find (groups.begin(), groups.end(), group) == groups.end()) return;
    if (viewMode == View::mixer) { activeGroup = group; layoutChips(); repaint(); return; }   // the mixer only shows that stroke type's mics
    const auto& arts = info.data->meta.articulations;
    const int cur = juce::jlimit (0, (int) arts.size() - 1, proc.slotArticulation (slot));
    const auto wanted = ArticulationName::parse (arts[(size_t) cur].name).label;
    int pick = -1;
    for (int i = 0; i < (int) arts.size(); ++i)
    {
        const auto n = ArticulationName::parse (arts[(size_t) i].name);
        if (n.group != group) continue;
        if (pick < 0) pick = i;
        if (n.label == wanted) { pick = i; break; }
    }
    activeGroup = group;
    layoutChips();
    if (pick >= 0) { proc.setSlotArticulation (slot, pick, true); shownArt = -1; }
    repaint();
}

void SlotDetail::setView (View v)
{
    viewMode = v;
    for (int i = 0; i < kSlots; ++i)
    {
        pan[i]->setVisible (i == slot && v == View::mics);
        tune[i]->setVisible (i == slot && v == View::mics);
    }
    layoutChips();
    repaint();
}

float SlotDetail::yToDb (const juce::Rectangle<int>& f, int y)
{
    const float t = juce::jlimit (0.0f, 1.0f, (float) (f.getBottom() - y) / (float) std::max (1, f.getHeight()));
    float db = 66.0f * std::sqrt (t) - 60.0f;
    if (db < -58.0f) return -60.0f;
    if (std::abs (db) < 0.8f) return 0.0f;
    return std::round (db * 2.0f) / 2.0f;
}

int SlotDetail::dbToY (const juce::Rectangle<int>& f, float db)
{
    const float t = std::pow (juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f), 2.0f);
    return f.getBottom() - (int) std::lround (t * (float) f.getHeight());
}

void SlotDetail::showBusMenu (int lane)
{
    juce::PopupMenu menu;
    const int cur = proc.laneInfo (slot, lane).bus;
    const juce::String off = "  (not enabled in the host: plays on Main)";
    menu.addSectionHeader ("Stereo");
    for (int b = 0; b < kBuses; ++b)
        menu.addItem (b + 1, OpenTriggerProcessor::busName (b) + (proc.busEnabled (b) ? "" : off), true, b == cur);
    menu.addSeparator();
    menu.addSectionHeader ("Mono (sum of the mic on one channel)");
    for (int m = 0; m < kMonoOuts; ++m)
        menu.addItem (kBuses + m + 1, OpenTriggerProcessor::busName (kBuses + m) + (proc.busEnabled (kBuses + m) ? "" : off), true, kBuses + m == cur);
    juce::Component::SafePointer<SlotDetail> self (this);
    const int sl = slot;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [self, lane, sl] (int r) { if (self != nullptr && r > 0) { self->proc.setLaneBus (sl, lane, r - 1); self->repaint(); } });
}

void SlotDetail::mouseDrag (const juce::MouseEvent& e)
{
    if (viewMode != View::mixer || dragStrip < 0 || dragStrip >= (int) strips.size()) return;
    const auto& st = strips[(size_t) dragStrip];
    if (dragKind == 2) proc.setMicLevel (slot, st.art, yToDb (st.fader, e.getPosition().y));
    else if (dragKind == 1)
    {
        const int lane = proc.laneOfArticulation (slot, st.art);
        if (lane >= 0)
        {
            float p = juce::jlimit (-1.0f, 1.0f, dragStartValue + (float) (dragStartY - e.getPosition().y) / 70.0f);
            if (std::abs (p) < 0.04f) p = 0.0f;
            proc.setLanePan (slot, lane, std::round (p * 100.0f) / 100.0f);
        }
    }
    repaint();
}

void SlotDetail::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (viewMode != View::mixer) return;
    for (auto& st : strips)
    {
        if (st.fader.expanded (10, 4).contains (e.getPosition())) { proc.setMicLevel (slot, st.art, 0.0f); repaint(); return; }
        if (st.pan.contains (e.getPosition())) { const int lane = proc.laneOfArticulation (slot, st.art); if (lane >= 0) proc.setLanePan (slot, lane, 0.0f); repaint(); return; }
    }
}

void SlotDetail::mouseDown (const juce::MouseEvent& e)
{
    if (tabMics.contains (e.getPosition())) { setView (View::mics); return; }
    if (tabOuts.contains (e.getPosition())) { setView (View::mixer); return; }
    if (viewMode == View::mixer)
    {
        dragStrip = -1;
        if (info.data == nullptr) return;
        if (strokeMode() && strokeBox.contains (e.getPosition())) { /* handled below */ }
        else if (mixedBtn.contains (e.getPosition())) { proc.calibrateMixed (slot, activeGroup); repaint(); return; }
        else if (resetBtn.contains (e.getPosition())) { proc.resetMixer (slot); repaint(); return; }
        else
        {
            for (int i = 0; i < (int) strips.size(); ++i)
            {
                const auto& st = strips[(size_t) i];
                if (! st.area.contains (e.getPosition())) continue;
                const int lane = proc.laneOfArticulation (slot, st.art);
                if (st.fader.expanded (12, 4).contains (e.getPosition())) { dragStrip = i; dragKind = 2; proc.setMicLevel (slot, st.art, yToDb (st.fader, e.getPosition().y)); }
                else if (lane >= 0 && st.pan.contains (e.getPosition())) { dragStrip = i; dragKind = 1; dragStartY = e.getPosition().y; dragStartValue = proc.laneInfo (slot, lane).pan; }
                else if (lane >= 0 && st.mute.contains (e.getPosition())) proc.setLaneMute (slot, lane, ! proc.laneInfo (slot, lane).mute);
                else if (lane >= 0 && st.solo.contains (e.getPosition())) proc.setLaneSolo (slot, lane, ! proc.laneInfo (slot, lane).solo);
                else if (lane >= 0 && st.phase.contains (e.getPosition())) proc.setLanePhase (slot, lane, ! proc.laneInfo (slot, lane).phase);
                else if (lane >= 0 && st.out.contains (e.getPosition())) showBusMenu (lane);
                repaint();
                return;
            }
            return;
        }
    }
    if (strokeMode() && strokeBox.contains (e.getPosition()))
    {
        juce::PopupMenu menu;
        int id = 1;
        bool sep = false;
        for (auto& g : groups)
        {
            if (! sep && g.startsWith ("VeloSplit")) { menu.addSeparator(); sep = true; }
            menu.addItem (id++, g, true, g == activeGroup);
        }
        juce::Component::SafePointer<SlotDetail> self (this);
        const auto names = groups;
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (strokeBox)).withMinimumWidth (strokeBox.getWidth()),
                            [self, names] (int r) { if (self != nullptr && r > 0 && r <= (int) names.size()) self->selectStroke (names[(size_t) r - 1]); });
        return;
    }
    for (int i = 0; i < (int) chips.size(); ++i)
        if (! chips[(size_t) i].isEmpty() && chips[(size_t) i].contains (e.getPosition()))
        {
            proc.setSlotArticulation (slot, i, true); // switches, and plays as soon as the mic is decoded
            shownArt = -1;
            return;
        }
}

void SlotDetail::paint (juce::Graphics& g)
{
    using namespace ui;
    drawPanel (g, getLocalBounds().toFloat());
    g.setColour (dim);
    g.setFont (font (11.0f, true));
    g.drawText ("SLOT " + juce::String (slot + 1), 16, 10, 120, 14, juce::Justification::left);
    if (info.data == nullptr)
    {
        g.setColour (dim);
        g.setFont (font (14.0f));
        g.drawText (info.status == OpenTriggerProcessor::SlotStatus::loading ? "Loading..." : "Empty slot. Press L on the slot (or click its picture) to choose a kit piece.",
                    16, 30, getWidth() - 32, 20, juce::Justification::left);
        return;
    }
    const auto& m = info.data->meta;
    g.setColour (text);
    g.setFont (font (20.0f, true));
    g.drawText (slotTitle (info), 16, 26, viewMode == View::mixer ? 330 : getWidth() - 360 - 240, 26, juce::Justification::left);
    for (auto v : { View::mics, View::mixer })
    {
        const auto r = viewTabRect (v);
        const bool on = v == viewMode;
        g.setColour (on ? orange : panelHi);
        g.fillRoundedRectangle (r.toFloat(), 12.0f);
        g.setColour (on ? orange.brighter (0.2f) : line);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 12.0f, 1.0f);
        g.setColour (on ? bg : text);
        g.setFont (font (12.0f, true));
        g.drawText (v == View::mics ? "MICS" : "MIXER", r, juce::Justification::centred);
    }
    g.setColour (dim);
    g.setFont (font (12.0f));
    const int art = juce::jlimit (0, (int) m.articulations.size() - 1, proc.slotArticulation (slot));
    const auto& a = m.articulations[(size_t) art];
    const int rr = a.layers.empty() ? 0 : (int) a.layers[0].roundRobins.size();
    if (viewMode == View::mics) g.drawText (m.library + "   |   " + juce::String ((int) a.layers.size()) + " velocity layers x " + juce::String (rr) + " round robins",
                16, 54, getWidth() - 360, 16, juce::Justification::left);
    g.setColour (dim);
    g.setFont (font (11.0f, true));
    if (viewMode == View::mixer)
    {
        auto button = [&] (juce::Rectangle<int> r, const juce::String& t, bool on, juce::Colour onCol)
        {
            g.setColour (on ? onCol : panelHi);
            g.fillRoundedRectangle (r.toFloat(), 4.0f);
            g.setColour (on ? onCol.brighter (0.2f) : line);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setColour (on ? bg : text);
            g.setFont (font (10.0f, true));
            g.drawText (t, r, juce::Justification::centred);
        };
        if (strokeMode())
        {
            auto sb = strokeBox.toFloat();
            g.setColour (panelHi);
            g.fillRoundedRectangle (sb, 5.0f);
            g.setColour (orange);
            g.drawRoundedRectangle (sb.reduced (0.5f), 5.0f, 1.0f);
            g.setColour (text);
            g.setFont (font (12.0f, true));
            g.drawText (activeGroup, strokeBox.reduced (10, 0).withTrimmedRight (20), juce::Justification::centredLeft);
        }
        button (mixedBtn, "MIXED", false, orange);
        button (resetBtn, "FULL MIX", false, orange);
        const auto note = proc.mixerMessage();
        if (note.isNotEmpty())
        {
            g.setColour (dim);
            g.setFont (font (11.0f));
            g.drawText (note, 16, getHeight() - 20, getWidth() - 32, 14, juce::Justification::left);
        }
        for (auto& st : strips)
        {
            const int lane = proc.laneOfArticulation (slot, st.art);
            const auto li = proc.laneInfo (slot, lane < 0 ? 0 : lane);
            const bool live = lane >= 0 && li.gainDb > kLaneOffDb;
            g.setColour (panelHi.darker (0.25f));
            g.fillRoundedRectangle (st.area.toFloat(), 6.0f);
            g.setColour (lane >= 0 ? line.brighter (0.2f) : line);
            g.drawRoundedRectangle (st.area.toFloat().reduced (0.5f), 6.0f, 1.0f);
            // pan knob
            const auto kc = st.pan.toFloat().getCentre();
            const float kr = 14.0f, ang = (lane >= 0 ? li.pan : 0.0f) * 2.35f;
            g.setColour (panelHi);
            g.fillEllipse (kc.x - kr, kc.y - kr, kr * 2, kr * 2);
            g.setColour (live ? orange : line);
            g.drawEllipse (kc.x - kr, kc.y - kr, kr * 2, kr * 2, 1.6f);
            g.setColour (live ? text : dim);
            g.drawLine (kc.x, kc.y, kc.x + std::sin (ang) * (kr - 3.0f), kc.y - std::cos (ang) * (kr - 3.0f), 2.2f);
            button (st.mute, "M", lane >= 0 && li.mute, orange);
            button (st.solo, "S", lane >= 0 && li.solo, accent);
            button (st.phase, "PHASE", lane >= 0 && li.phase, orange);
            // fader
            const auto& f = st.fader;
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.fillRoundedRectangle ((float) f.getCentreX() - 3.0f, (float) f.getY(), 6.0f, (float) f.getHeight(), 3.0f);
            g.setColour (line);
            g.drawHorizontalLine (dbToY (f, 0.0f), (float) f.getX() + 2.0f, (float) f.getRight() - 2.0f);
            const int cy = dbToY (f, lane >= 0 ? li.gainDb : -60.0f);
            auto cap = juce::Rectangle<float> ((float) f.getX() - 1.0f, (float) cy - 8.0f, (float) f.getWidth() + 2.0f, 16.0f);
            g.setColour (live ? orange : panelHi.brighter (0.15f));
            g.fillRoundedRectangle (cap, 3.0f);
            g.setColour (live ? orange.brighter (0.35f) : line);
            g.drawRoundedRectangle (cap.reduced (0.5f), 3.0f, 1.0f);
            g.setColour (live ? bg : dim);
            g.drawHorizontalLine (cy, cap.getX() + 4.0f, cap.getRight() - 4.0f);
            g.setColour (live ? text : dim);
            g.setFont (font (10.0f));
            g.drawText (live ? juce::String (li.gainDb, 1) + " dB" : juce::String ("off"), st.fader.getX() - 14, st.fader.getBottom() + 2, st.fader.getWidth() + 28, 12, juce::Justification::centred);
            // name and output
            g.setColour (live ? text : dim);
            g.setFont (font (10.5f, live));
            g.drawFittedText (ArticulationName::parse (m.articulations[(size_t) st.art].name).label, st.name, juce::Justification::centred, 1, 0.75f);
            const bool missing = lane >= 0 && li.bus > 0 && ! proc.busEnabled (li.bus);
            button (st.out, (lane >= 0 ? OpenTriggerProcessor::busName (li.bus).toUpperCase() : juce::String ("MAIN")) + (missing ? " (OFF)" : ""), false, orange);
        }
        return;
    }
    if (strokeMode())
    {
        g.drawText ("STROKE TYPE  -  how the piece is hit", 16, 72, 400, 14, juce::Justification::left);
        auto sb = strokeBox.toFloat();
        g.setColour (panelHi);
        g.fillRoundedRectangle (sb, 5.0f);
        g.setColour (orange);
        g.drawRoundedRectangle (sb.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (text);
        g.setFont (font (13.0f, true));
        g.drawText (activeGroup, strokeBox.reduced (10, 0).withTrimmedRight (22), juce::Justification::centredLeft);
        juce::Path chev;
        const auto c = juce::Point<float> (sb.getRight() - 16.0f, sb.getCentreY());
        chev.startNewSubPath (c.x - 5.0f, c.y - 2.5f); chev.lineTo (c.x, c.y + 2.5f); chev.lineTo (c.x + 5.0f, c.y - 2.5f);
        g.setColour (orange);
        g.strokePath (chev, juce::PathStrokeType (2.0f));
        g.setColour (dim);
        g.setFont (font (11.0f, true));
        g.drawText ("MIC POSITION  -  click to select it and hear it", 16, chipArea.getY() - 16, 400, 14, juce::Justification::left);
    }
    else
        g.drawText ("MIC POSITION  -  click to select it and hear it", 16, 72, 400, 14, juce::Justification::left);
    const int cur = proc.slotArticulation (slot);
    for (int i = 0; i < (int) chips.size(); ++i)
    {
        if (chips[(size_t) i].isEmpty()) continue; // not in the style shown
        const bool on = i == juce::jlimit (0, (int) chips.size() - 1, cur);
        auto cr = chips[(size_t) i].toFloat();
        g.setColour (on ? accent : panelHi);
        g.fillRoundedRectangle (cr, 5.0f);
        g.setColour (on ? accent.brighter (0.3f) : line);
        g.drawRoundedRectangle (cr.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (on ? bg : text);
        g.setFont (font (12.0f, on));
        g.drawText (ArticulationName::parse (m.articulations[(size_t) i].name).label, chips[(size_t) i].reduced (4, 0), juce::Justification::centred);
    }
    g.setFont (font (11.0f, true));
    for (auto& h : headings)
    {
        g.setColour (orange);
        g.drawText (h.second.toUpperCase(), h.first.withTrimmedLeft (2), juce::Justification::centredLeft);
        g.setColour (line);
        juce::GlyphArrangement ga;
        ga.addLineOfText (g.getCurrentFont(), h.second.toUpperCase(), 0.0f, 0.0f);
        g.drawHorizontalLine (h.first.getCentreY(), (float) h.first.getX() + 14.0f + ga.getBoundingBox (0, -1, true).getWidth(), (float) h.first.getRight());
    }
    g.setColour (line);
    g.drawVerticalLine (getWidth() - 310, 40.0f, (float) getHeight() - 16.0f);
}

// ============================================================================================ BrowserOverlay
BrowserOverlay::BrowserOverlay()
{
    setWantsKeyboardFocus (true);
    for (auto* b : { &okBtn, &cancelBtn, &rescanBtn, &folderBtn, &importBtn, &exportBtn }) addAndMakeVisible (*b);
    importBtn.onClick = [this] { if (onImport) onImport(); };
    exportBtn.onClick = [this]
    {
        if (! onExport) return;
        juce::Array<juce::File> files;
        if (lib != nullptr) for (int i : filtered()) files.add (lib->entries()[(size_t) i].file);
        const auto name = filter.getSelectedId() <= 1 ? juce::String ("OpenTrigger Library") : filter.getText();
        onExport (files, name);
    };
    addAndMakeVisible (filter);
    okBtn.onClick = [this] { choose(); };
    cancelBtn.onClick = [this] { if (onCancel) onCancel(); };
    rescanBtn.onClick = [this] { if (onRescan) onRescan(); };
    folderBtn.onClick = [this] { if (onOpenFolder) onOpenFolder(); };
    filter.onChange = [this] { page = 0; sel = -1; layoutCards(); repaint(); };
}

void BrowserOverlay::open (const Library& l, int targetSlot, juce::File current)
{
    lib = &l;
    target = targetSlot;
    const auto prevFilter = filter.getText();
    filter.clear (juce::dontSendNotification);
    filter.addItem ("All Libraries", 1);
    juce::StringArray libs;
    for (auto& e : lib->entries()) libs.addIfNotAlreadyThere (e.library);
    libs.sort (true);
    for (int i = 0; i < libs.size(); ++i) filter.addItem (libs[i], i + 2);
    filter.setSelectedId (1, juce::dontSendNotification);
    page = 0;
    sel = -1;
    const auto idx = filtered();
    for (int i = 0; i < (int) idx.size(); ++i)
        if (lib->entries()[(size_t) idx[(size_t) i]].file == current) { sel = i; page = i / kPerPage; }
    layoutCards();
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    repaint();
}

std::vector<int> BrowserOverlay::filtered() const
{
    std::vector<int> out;
    if (lib == nullptr) return out;
    const bool all = filter.getSelectedId() <= 1;
    const auto name = filter.getText();
    for (int i = 0; i < (int) lib->entries().size(); ++i)
        if (all || lib->entries()[(size_t) i].library == name) out.push_back (i);
    return out;
}

void BrowserOverlay::resized()
{
    panelRect = getLocalBounds().reduced (70, 44);
    auto r = panelRect.reduced (18, 0);
    r.removeFromTop (46);
    infoRect = r.removeFromBottom (190).withTrimmedRight (330);
    auto right = juce::Rectangle<int> (panelRect.getRight() - 18 - 310, panelRect.getBottom() - 190, 310, 180);
    filter.setBounds (right.removeFromTop (30));
    right.removeFromTop (8);
    auto row = right.removeFromTop (30);
    rescanBtn.setBounds (row.removeFromLeft (100));
    folderBtn.setBounds (row.removeFromRight (140));
    right.removeFromTop (8);
    row = right.removeFromTop (30);
    importBtn.setBounds (row.removeFromLeft (150));
    exportBtn.setBounds (row.removeFromRight (150));
    right.removeFromTop (12);
    row = right.removeFromTop (34);
    okBtn.setBounds (row.removeFromLeft (150));
    cancelBtn.setBounds (row.removeFromRight (150));
    r.removeFromBottom (36); // page dots
    gridRect = r;
    layoutCards();
}

void BrowserOverlay::layoutCards()
{
    cardRects.clear();
    dotRects.clear();
    if (gridRect.isEmpty()) return;
    const auto idx = filtered();
    const int pages = std::max (1, (int) std::ceil (idx.size() / (double) kPerPage));
    page = juce::jlimit (0, pages - 1, page);
    const int cols = 4, gap = 14;
    const int cw = (gridRect.getWidth() - gap * (cols - 1)) / cols, chh = (gridRect.getHeight() - gap) / 2;
    const int first = page * kPerPage, count = std::min ((int) idx.size() - first, kPerPage);
    for (int i = 0; i < count; ++i)
        cardRects.push_back ({ gridRect.getX() + (i % cols) * (cw + gap), gridRect.getY() + (i / cols) * (chh + gap), cw, chh });
    for (int p = 0; p < pages; ++p)
        dotRects.push_back ({ panelRect.getCentreX() - pages * 11 + p * 22, gridRect.getBottom() + 14, 14, 14 });
}

int BrowserOverlay::cardAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) cardRects.size(); ++i)
        if (cardRects[(size_t) i].contains (p)) return page * kPerPage + i;
    return -1;
}

void BrowserOverlay::select (int fi)
{
    const bool changed = fi != sel;
    sel = fi;
    if (changed) { wantPlay = false; prefetchFor = {}; soft = {}; hard = {}; prefetch (false); prefetch (true); }
    repaint();
}

// ---- audition without loading: decode one take of the selected piece in the background
void BrowserOverlay::prefetch (bool hardOne)
{
    const auto idx = filtered();
    if (sel < 0 || sel >= (int) idx.size() || lib == nullptr) return;
    const auto file = lib->entries()[(size_t) idx[(size_t) sel]].file;
    auto& slot = hardOne ? hard : soft;
    if (slot.loading) return;
    slot.loading = true;
    prefetchFor = file.getFullPathName();
    const int vel = hardOne ? 118 : 55;
    const auto cached = meta.find (file.getFullPathName());
    auto data = cached != meta.end() ? cached->second : nullptr;
    const juce::int64 seed = rng.nextInt64();
    juce::Component::SafePointer<BrowserOverlay> self (this);
    pool.addJob ([self, file, data, vel, hardOne, seed]
    {
        juce::String err;
        auto d = data ? data : readInstrumentData (file, err);
        PreviewSound ps;
        if (d) { juce::Random r (seed); ps = buildPreview (d, vel, r, err); }
        juce::MessageManager::callAsync ([self, file, d, ps, hardOne]
        {
            if (self == nullptr) return;
            auto& s2 = hardOne ? self->hard : self->soft;
            s2.loading = false;
            if (d) self->meta[file.getFullPathName()] = d;
            if (self->prefetchFor != file.getFullPathName() || ps.audio == nullptr) return; // selection moved on, or failed
            s2.buf = ps.audio;
            s2.rate = ps.rate;
            if (self->wantPlay && self->wantHard == hardOne) self->play (hardOne);
        });
    });
}

void BrowserOverlay::play (bool hardOne)
{
    auto& p = hardOne ? hard : soft;
    if (p.buf == nullptr) return;
    wantPlay = false;
    if (onPreview) onPreview (p.buf, p.rate);
    ++played;
    // keep a fresh take ready for the next hit (a different round robin)
    p.buf = nullptr;
    prefetch (hardOne);
}

void BrowserOverlay::hitCard (int visibleIndex, float yFrac)
{
    if (visibleIndex < 0 || visibleIndex >= (int) cardRects.size()) return;
    const int fi = page * kPerPage + visibleIndex;
    if (fi != sel) { select (fi); return; }   // first click chooses the piece, the next ones play it
    const bool hardOne = yFrac < 0.5f;
    animCard = visibleIndex;
    animStart = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
    auto& p = hardOne ? hard : soft;
    if (p.buf != nullptr) play (hardOne);
    else { wantPlay = true; wantHard = hardOne; prefetch (hardOne); } // plays the moment the take is decoded
    repaint();
}

void BrowserOverlay::timerCallback()
{
    if (juce::Time::getMillisecondCounterHiRes() - animStart > 650.0) { animCard = -1; stopTimer(); }
    repaint();
}

void BrowserOverlay::choose()
{
    const auto idx = filtered();
    if (sel >= 0 && sel < (int) idx.size() && onChoose) onChoose (lib->entries()[(size_t) idx[(size_t) sel]].file, target);
}

void BrowserOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (const int c = cardAt (e.getPosition()); c >= 0)
    {
        const auto cr = cardRects[(size_t) (c - page * kPerPage)];
        const auto img = cr.reduced (8).withTrimmedBottom (28);
        hitCard (c - page * kPerPage, juce::jlimit (0.0f, 1.0f, (float) (e.y - img.getY()) / (float) std::max (1, img.getHeight())));
        return;
    }
    for (int i = 0; i < (int) dotRects.size(); ++i)
        if (dotRects[(size_t) i].expanded (4).contains (e.getPosition())) { page = i; layoutCards(); repaint(); return; }
}

void BrowserOverlay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (cardAt (e.getPosition()) >= 0) choose();
}

bool BrowserOverlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (onCancel) onCancel(); return true; }
    if (k == juce::KeyPress::returnKey) { choose(); return true; }
    return false;
}

void BrowserOverlay::paint (juce::Graphics& g)
{
    using namespace ui;
    g.fillAll (juce::Colours::black.withAlpha (0.62f));
    g.setColour (juce::Colour (0xff161a21));
    g.fillRoundedRectangle (panelRect.toFloat(), 10.0f);
    g.setColour (line.brighter (0.15f));
    g.drawRoundedRectangle (panelRect.toFloat().reduced (0.5f), 10.0f, 1.0f);
    g.setColour (text);
    g.setFont (font (15.0f, true));
    g.drawText ("KITPIECE BROWSER   -   SLOT " + juce::String (target + 1), panelRect.withHeight (44), juce::Justification::centred);

    const auto idx = filtered();
    if (idx.empty())
    {
        g.setColour (dim);
        g.setFont (font (14.0f));
        g.drawText ("No instruments found. Put .otci files in the library folder and press Rescan.", gridRect, juce::Justification::centred);
    }
    for (int i = 0; i < (int) cardRects.size(); ++i)
    {
        const int fi = page * kPerPage + i;
        const auto& e = lib->entries()[(size_t) idx[(size_t) fi]];
        const auto cr = cardRects[(size_t) i];
        const bool on = fi == sel;
        g.setColour (on ? panelHi : panel);
        g.fillRoundedRectangle (cr.toFloat(), 8.0f);
        g.setColour (on ? accent : line);
        g.drawRoundedRectangle (cr.toFloat().reduced (0.5f), 8.0f, on ? 2.0f : 1.0f);
        auto img = cr.reduced (8).withTrimmedBottom (28);
        if (i == animCard)
        {
            // the picture takes the hit: a quick squash, a small jolt, a flash and a ripple, then settles
            const double t = juce::Time::getMillisecondCounterHiRes() - animStart;
            const float squash = (float) (0.075 * std::exp (-t / 120.0) * std::cos (t * 0.034));
            const float jolt = (float) (3.0 * std::exp (-t / 140.0) * std::sin (t * 0.05));
            auto r = img.toFloat().withSizeKeepingCentre (img.getWidth() * (1.0f - squash), img.getHeight() * (1.0f - squash)).translated (0.0f, jolt);
            g.saveState();
            g.reduceClipRegion (img);
            g.setColour (ui::bg);
            g.fillRoundedRectangle (img.toFloat(), 5.0f);
            if (e.cover.isValid()) g.drawImage (e.cover, r, juce::RectanglePlacement::centred);
            g.setColour (juce::Colours::white.withAlpha ((float) (0.14 * std::exp (-t / 80.0))));
            g.fillRect (img);
            if (t < 520.0)
            {
                const float k = (float) (t / 520.0);
                g.setColour (ui::accent.withAlpha ((1.0f - k) * 0.55f));
                const float rad = img.getHeight() * (0.15f + 0.55f * k);
                g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (img.getCentre().toFloat()), 2.5f * (1.0f - k) + 0.5f);
            }
            g.restoreState();
            g.setColour (ui::line);
            g.drawRoundedRectangle (img.toFloat().reduced (0.5f), 5.0f, 1.0f);
        }
        else drawCover (g, e.cover, img, false);
        g.setColour (on ? text : text.withAlpha (0.9f));
        g.setFont (font (13.0f, true));
        g.drawText (e.name, cr.withTrimmedTop (cr.getHeight() - 28).reduced (8, 0), juce::Justification::centred);
    }
    for (int p = 0; p < (int) dotRects.size(); ++p)
    {
        g.setColour (p == page ? accent : line.brighter (0.3f));
        g.fillEllipse (dotRects[(size_t) p].toFloat().reduced (3.0f));
    }

    // info box
    g.setColour (panel);
    g.fillRoundedRectangle (infoRect.toFloat(), 8.0f);
    g.setColour (line);
    g.drawRoundedRectangle (infoRect.toFloat().reduced (0.5f), 8.0f, 1.0f);
    auto ir = infoRect.reduced (14, 10);
    if (sel >= 0 && sel < (int) idx.size())
    {
        const auto& e = lib->entries()[(size_t) idx[(size_t) sel]];
        drawCover (g, e.cover, ir.removeFromLeft (110), false);
        ir.removeFromLeft (14);
        g.setColour (text);
        g.setFont (font (18.0f, true));
        g.drawText (e.name, ir.removeFromTop (24), juce::Justification::left);
        g.setColour (dim);
        g.setFont (font (12.0f));
        g.drawText (e.library, ir.removeFromTop (18), juce::Justification::left);
        g.drawText (juce::String (e.articulations) + " mic positions / articulations   |   " + juce::String (e.layers) + " velocity layers   |   " + juce::String (e.waves) + " samples",
                    ir.removeFromTop (18), juce::Justification::left);
        g.setColour (ui::orange);
        g.drawText ("Click the selected piece again to hear it (Full Mix). Load it to hear every mic.", ir.removeFromTop (20), juce::Justification::left);
    }
    else
    {
        g.setColour (dim);
        g.setFont (font (14.0f));
        g.drawText ("Empty (nothing selected)", ir, juce::Justification::centredLeft);
    }
}
