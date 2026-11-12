#include "SmartUi.h"

SmartTriggerPanel::SmartTriggerPanel (OpenTriggerProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    for (auto* b : { &startBtn, &stopBtn, &applyBtn, &againBtn, &closeBtn }) addChildComponent (*b);
    startBtn.onClick = [this] { startListening(); };
    stopBtn.onClick = [this] { stopAndAnalyze(); };
    applyBtn.onClick = [this] { apply(); };
    againBtn.onClick = [this] { open(); };
    closeBtn.onClick = [this] { proc.stopLearn(); stopTimer(); setVisible (false); if (onClose) onClose(); };
    applyBtn.setColour (juce::TextButton::buttonColourId, ui::accent.darker (0.3f));
    startBtn.setColour (juce::TextButton::buttonColourId, ui::accent.darker (0.3f));
}

void SmartTriggerPanel::open()
{
    proc.stopLearn();
    stopTimer();
    st = State::intro;
    res = {};
    buf.reset();
    liveHits = 0;
    setVisible (true);
    toFront (true);
    grabKeyboardFocus();
    resized();
    repaint();
}

void SmartTriggerPanel::startListening()
{
    proc.startLearn (30.0);
    buf = proc.learnBuffer();
    liveHits = 0; tick = 0; stopAt = -1;
    st = State::listening;
    startTimerHz (20);
    resized();
    repaint();
}

void SmartTriggerPanel::stopAndAnalyze()
{
    if (st != State::listening) return;
    proc.stopLearn();
    stopAt = 3; // let the audio thread leave the buffer (a few timer ticks) before reading it
    stopBtn.setEnabled (false);
}

void SmartTriggerPanel::finishAnalysis()
{
    stopTimer();
    const int n = buf ? buf->pos.load (std::memory_order_acquire) : 0;
    res = analyzeLearn (buf ? buf->data.data() : nullptr, n, proc.currentSampleRate());
    if (res.ok) proc.estimateOutputDb (res); // needs the loaded kit piece
    st = res.ok ? State::result : State::failed;
    stopBtn.setEnabled (true);
    resized();
    repaint();
}

void SmartTriggerPanel::apply()
{
    if (st != State::result) return;
    proc.applyLearn (res);
    if (onApplied) onApplied (res);
    setVisible (false);
    if (onClose) onClose();
}

void SmartTriggerPanel::timerCallback()
{
    if (st != State::listening || buf == nullptr) return;
    if (stopAt >= 0)
    {
        if (--stopAt < 0) finishAnalysis();
        return;
    }
    ++tick;
    const int pos = buf->pos.load (std::memory_order_acquire);
    // every second, count what has been heard so far; stop by itself after enough hits or 30 s
    if (tick % 20 == 0 && pos > (int) (3.0 * buf->sampleRate))
    {
        auto partial = analyzeLearn (buf->data.data(), pos, buf->sampleRate);
        liveHits = partial.ok ? partial.hits : 0;
        if (liveHits >= 24) { stopAndAnalyze(); return; }
    }
    if (pos >= (int) buf->data.size()) stopAndAnalyze();
    repaint();
}

bool SmartTriggerPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { closeBtn.onClick(); return true; }
    return false;
}

void SmartTriggerPanel::resized()
{
    card = getLocalBounds().withSizeKeepingCentre (820, 560);
    auto row = juce::Rectangle<int> (card.getX() + 24, card.getBottom() - 58, card.getWidth() - 48, 38);
    for (auto* b : { &startBtn, &stopBtn, &applyBtn, &againBtn, &closeBtn }) b->setVisible (false);
    closeBtn.setBounds (row.removeFromRight (120));
    row.removeFromRight (10);
    switch (st)
    {
        case State::intro:     startBtn.setBounds (row.removeFromRight (160)); startBtn.setVisible (true); break;
        case State::listening: stopBtn.setBounds (row.removeFromRight (200)); stopBtn.setVisible (true); break;
        case State::result:    applyBtn.setBounds (row.removeFromRight (160)); row.removeFromRight (10);
                               againBtn.setBounds (row.removeFromRight (120)); applyBtn.setVisible (true); againBtn.setVisible (true); break;
        case State::failed:    againBtn.setBounds (row.removeFromRight (140)); againBtn.setVisible (true); break;
    }
    closeBtn.setVisible (true);
}

void SmartTriggerPanel::drawWave (juce::Graphics& g, juce::Rectangle<int> r, bool markers)
{
    using namespace ui;
    g.setColour (juce::Colour (0xff0b0d11));
    g.fillRoundedRectangle (r.toFloat(), 6.0f);
    g.setColour (line);
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);
    if (buf == nullptr) return;
    const int n = st == State::listening ? buf->pos.load (std::memory_order_acquire) : res.length;
    if (n < 2) return;
    const double total = st == State::listening ? (double) buf->data.size() : (double) n; // while listening, the bar fills up to 30 s
    const float half = r.getHeight() * 0.5f, cy = (float) r.getCentreY();
    g.setColour (accent);
    for (int px = 0; px < r.getWidth(); ++px)
    {
        const int a = (int) (px / (double) r.getWidth() * total), b = std::min (n, (int) ((px + 1) / (double) r.getWidth() * total));
        if (a >= n) break;
        float lo = 0.0f, hi = 0.0f;
        const int step = std::max (1, (b - a) / 64);
        for (int i = a; i < b; i += step) { lo = std::min (lo, buf->data[(size_t) i]); hi = std::max (hi, buf->data[(size_t) i]); }
        const float yh = cy - ScopeView::shape (hi) * (half - 3.0f), yl = cy - ScopeView::shape (lo) * (half - 3.0f);
        g.fillRect ((float) (r.getX() + px), yh, 1.0f, std::max (1.0f, yl - yh));
    }
    if (! markers || st != State::result) return;
    const float tY = cy - ScopeView::shape (juce::Decibels::decibelsToGain (res.thresholdDb - res.inputGainDb)) * (half - 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.drawHorizontalLine ((int) tY, (float) r.getX(), (float) r.getRight());
    g.drawHorizontalLine ((int) (2.0f * cy - tY), (float) r.getX(), (float) r.getRight());
    auto mark = [&] (int pos, juce::Colour c)
    {
        const float x = (float) r.getX() + (float) (pos / total) * (float) r.getWidth();
        juce::Path t;
        t.addTriangle (x - 4.0f, (float) r.getY() + 2.0f, x + 4.0f, (float) r.getY() + 2.0f, x, (float) r.getY() + 10.0f);
        g.setColour (c);
        g.fillPath (t);
    };
    for (int p : res.hitPos) mark (p, orange);
    for (int p : res.bleedPos) mark (p, dim);
}

void SmartTriggerPanel::paint (juce::Graphics& g)
{
    using namespace ui;
    g.fillAll (juce::Colours::black.withAlpha (0.62f));
    g.setColour (juce::Colour (0xff161a21));
    g.fillRoundedRectangle (card.toFloat(), 10.0f);
    g.setColour (line.brighter (0.15f));
    g.drawRoundedRectangle (card.toFloat().reduced (0.5f), 10.0f, 1.0f);
    auto r = card.reduced (28, 20);
    g.setColour (text);
    g.setFont (font (18.0f, true));
    g.drawText ("SMART TRIGGER", r.removeFromTop (28), juce::Justification::left);
    r.removeFromTop (6);
    g.setFont (font (13.0f));

    auto body = [&] (const juce::String& s, juce::Colour c = juce::Colour (0xffc7cfdc)) { g.setColour (c); g.drawFittedText (s, r.removeFromTop (20), juce::Justification::topLeft, 1); };
    if (st == State::intro)
    {
        g.setColour (dim);
        g.drawFittedText ("Teaches the plug-in the best starting settings for THIS track: threshold, retrigger, filters, sensitivity.", r.removeFromTop (36), juce::Justification::topLeft, 2);
        r.removeFromTop (8);
        g.setColour (orange);
        g.setFont (font (12.0f, true));
        g.drawText ("HOW TO USE", r.removeFromTop (20), juce::Justification::left);
        g.setFont (font (14.0f));
        body ("1.  Put the playhead where this instrument plays clearly: 10 to 20 hits, some soft and some loud.");
        body ("2.  Press Start here, then press play in your DAW and let it run for 10 to 20 seconds.");
        body ("3.  It stops by itself after about 24 hits (or press Stop). Then check the result and press Apply.");
        r.removeFromTop (14);
        g.setColour (orange);
        g.setFont (font (12.0f, true));
        g.drawText ("TIPS", r.removeFromTop (20), juce::Justification::left);
        g.setFont (font (14.0f));
        body ("-  Choose a part where the other drums are quiet. A few of them are fine: it uses them to learn what to ignore.");
        body ("-  Use the same take and the same mic you will trigger from. Playing the part twice is not needed.");
        body ("-  You can fine tune everything afterwards. Think of this as a good first guess, not the final word.");
    }
    else if (st == State::listening)
    {
        const int n = buf ? buf->pos.load() : 0;
        const double secs = buf ? n / buf->sampleRate : 0.0;
        g.setColour (green);
        g.drawText (juce::String::formatted ("Listening...  %.1f s   -   %d hits heard", secs, liveHits), r.removeFromTop (22), juce::Justification::left);
        r.removeFromTop (6);
        drawWave (g, r.removeFromTop (230), false);
        r.removeFromTop (10);
        g.setColour (dim);
        g.setFont (font (12.0f));
        g.drawText ("Press play in your DAW now. Press Stop when you have heard enough hits, or wait: it ends by itself.", r.removeFromTop (20), juce::Justification::left);
    }
    else if (st == State::result)
    {
        g.setColour (green);
        g.drawFittedText (res.message, r.removeFromTop (22), juce::Justification::topLeft, 1);
        r.removeFromTop (6);
        drawWave (g, r.removeFromTop (120), true);
        g.setColour (dim);
        g.setFont (font (11.0f));
        g.drawText ("orange = hits it will trigger     grey = other sounds it will ignore     white lines = proposed threshold", r.removeFromTop (18), juce::Justification::left);
        r.removeFromTop (6);
        auto val = [&] (const juce::String& name, const juce::String& now, const juce::String& next, juce::Rectangle<int> c)
        {
            g.setColour (dim); g.setFont (font (11.0f, true));
            g.drawText (name.toUpperCase(), c.removeFromTop (14), juce::Justification::left);
            g.setColour (text); g.setFont (font (16.0f, true));
            g.drawText (next, c.removeFromTop (22), juce::Justification::left);
            g.setColour (dim); g.setFont (font (11.0f));
            g.drawText ("now " + now, c.removeFromTop (14), juce::Justification::left);
        };
        auto cur = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        auto grid = r.removeFromTop (230);
        const int cw = grid.getWidth() / 4;
        auto cell = [&] (int i, int row2) { return juce::Rectangle<int> (grid.getX() + i * cw, grid.getY() + row2 * 56, cw - 10, 54); };
        val ("Threshold", juce::String (cur ("threshold"), 1) + " dB", juce::String (res.thresholdDb, 1) + " dB", cell (0, 0));
        val ("Retrigger", juce::String ((int) cur ("holdoff")) + " ms", juce::String ((int) res.retriggerMs) + " ms", cell (1, 0));
        val ("Sensitivity", juce::String ((int) cur ("sensitivity")), juce::String ((int) res.sensitivity), cell (2, 0));
        val ("Detail", juce::String ((int) cur ("detail")), juce::String ((int) res.detail), cell (3, 0));
        val ("Low cut", juce::String ((int) cur ("lowcut")) + " hz", juce::String ((int) res.lowcutHz) + " hz", cell (0, 1));
        val ("High cut", juce::String ((int) cur ("highcut")) + " hz", juce::String ((int) res.highcutHz) + " hz", cell (1, 1));
        val ("Suppress", juce::String ((int) cur ("suppress")) + "%", juce::String ((int) res.suppress) + "%", cell (2, 1));
        val ("Input volume", juce::String (cur ("inGain"), 1) + " dB", juce::String (res.inputGainDb, 1) + " dB", cell (3, 1));
        const bool gateNow = cur ("gateOn") > 0.5f;
        val ("Gate", gateNow ? "on" : "off", res.gateOn ? "on" : "off (not needed)", cell (0, 2));
        val ("Gate attack", juce::String (cur ("gateAttack"), 1) + " ms", res.gateOn ? juce::String (res.gateAttackMs, 1) + " ms" : juce::String ("-"), cell (1, 2));
        val ("Gate hold", juce::String ((int) cur ("gateHold")) + " ms", res.gateOn ? juce::String ((int) res.gateHoldMs) + " ms" : juce::String ("-"), cell (2, 2));
        val ("Gate release", juce::String ((int) cur ("gateRelease")) + " ms", res.gateOn ? juce::String ((int) res.gateReleaseMs) + " ms" : juce::String ("-"), cell (3, 2));
        val ("Output volume", juce::String (cur ("gain"), 1) + " dB", res.hasOutput ? juce::String (res.outputGainDb, 1) + " dB" : juce::String ("unchanged (load a kit piece)"), cell (0, 3));
        g.setColour (dim); g.setFont (font (11.0f));
        g.drawFittedText ("Mix, trigger source and each slot's volume, pan and tune are yours to choose: the track does not say what you want there.",
                          juce::Rectangle<int> (grid.getX() + cw, grid.getY() + 3 * 56 + 4, cw * 3 - 10, 40), juce::Justification::topLeft, 2);
    }
    else
    {
        g.setColour (red);
        g.setFont (font (15.0f, true));
        g.drawText ("Could not learn from this part", r.removeFromTop (26), juce::Justification::left);
        g.setColour (text);
        g.setFont (font (14.0f));
        g.drawFittedText (res.message, r.removeFromTop (60), juce::Justification::topLeft, 3);
        r.removeFromTop (10);
        g.setColour (dim);
        g.drawFittedText ("Nothing was changed. Try again on another stretch of the track.", r.removeFromTop (24), juce::Justification::topLeft, 1);
    }
}
