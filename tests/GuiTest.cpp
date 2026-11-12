#include <set>
// End-to-end UI flow, driven through the same handlers JUCE calls (no window is shown).
#include "PluginEditor.h"
#include "TestSignals.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace
{
juce::File writeWav (const juce::File& f, float v, int frames = 3000)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), 44100.0, 1, 16, {}, 0));
    juce::AudioBuffer<float> b (1, frames);
    for (int i = 0; i < frames; ++i) b.setSample (0, i, v);
    w->writeFromAudioSampleBuffer (b, 0, frames);
    return f;
}

otci::Wave constWave (float v, int frames = 4000)
{
    juce::AudioBuffer<float> b (1, frames);
    for (int i = 0; i < frames; ++i) b.setSample (0, i, v);
    otci::Wave w;
    w.flac = encodeFlac (b, 44100.0, 24);
    w.frames = frames; w.channels = 1; w.rate = 44100.0;
    return w;
}

// Two articulations ("Top" = 0.2, "Bottom" = 0.4), one velocity layer, one take each; a cover so the browser has a picture.
juce::File writeKit (const juce::File& dir, const juce::String& name, const juce::String& library, float base)
{
    otci::Instrument k;
    k.name = name; k.library = library;
    k.waves = { constWave (base), constWave (base * 2.0f) };
    otci::Articulation top, bottom;
    top.name = "Top"; top.layers = { { 0, 1.0f, 1.0f, { { 0 } } } };
    bottom.name = "Bottom"; bottom.layers = { { 0, 1.0f, 1.0f, { { 1 } } } };
    k.articulations = { top, bottom };
    juce::Image img (juce::Image::ARGB, 64, 64, true);
    { juce::Graphics g (img); g.fillAll (juce::Colours::orange); }
    juce::MemoryOutputStream mo;
    juce::PNGImageFormat().writeImageToStream (img, mo);
    k.image = mo.getMemoryBlock();
    auto f = dir.getChildFile (name + ".otci");
    juce::String err;
    otci::write (f, k, err);
    return f;
}


template <class T> T* findChild (juce::Component& c, std::function<bool (T&)> pred = [] (T&) { return true; })
{
    for (auto* ch : c.getChildren())
        if (auto* t = dynamic_cast<T*> (ch); t != nullptr && pred (*t)) return t;
    return nullptr;
}

juce::MouseEvent mouseAt (juce::Component& c, float x, float y, bool popup = false)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    juce::Point<float> p (x, y);
    return { src, p, popup ? juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier) : juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &c, &c, juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false };
}

struct GuiTests : juce::UnitTest
{
    GuiTests() : juce::UnitTest ("GUI flow", "OpenTrigger") {}

    float hit (OpenTriggerProcessor& proc, int note, int vel, int ch = 0)
    {
        proc.prepareToPlay (44100.0, 512); // silences ringing voices and any preview from earlier steps
        const int L = proc.getLatencySamples();       // MIDI hits are delayed by the look-ahead, like the dry signal
        juce::AudioBuffer<float> io (2, L + 256);
        io.clear();
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) vel), 0);
        proc.processBlock (io, m);
        return io.getSample (ch, L + 100);
    }

    // work done on worker threads lands via the message loop; pump it until the result arrives.
    void waitUntil (std::function<bool()> done, int ms = 8000)
    {
        for (int waited = 0; waited < ms && ! done(); waited += 20)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    static bool ready (OpenTriggerProcessor& p, int slot) { return p.slotInfo (slot).status == OpenTriggerProcessor::SlotStatus::ready; }

    void runTest() override
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_gui_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        auto lib = root.getChildFile ("lib");
        auto src = root.getChildFile ("src");
        lib.createDirectory(); src.createDirectory();
        setenv ("OPENTRIGGER_LIBRARY", lib.getFullPathName().toRawUTF8(), 1);

        auto a = writeWav (src.getChildFile ("a.wav"), 0.1f), b = writeWav (src.getChildFile ("b.wav"), 0.3f), c = writeWav (src.getChildFile ("c.wav"), 0.6f);
        juce::Image img (juce::Image::ARGB, 800, 400, true);
        auto cover = src.getChildFile ("cover.png");
        { juce::FileOutputStream o (cover); juce::PNGImageFormat().writeImageToStream (img, o); }
        auto kitA = writeKit (lib, "Kit A", "Lib One", 0.1f);
        writeKit (lib, "Kit B", "Lib Two", 0.15f);

        beginTest ("editor builds: 8 slots, scope, header buttons");
        OpenTriggerProcessor proc;
        proc.setPlayConfigDetails (2, 2, 44100.0, 512);
        proc.prepareToPlay (44100.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto* ui = static_cast<OpenTriggerEditor*> (ed.get());
        ed->setSize (1280, 720);
        auto& content = ui->contentComponent();
        auto* panel = &ui->instrumentEditor();
        auto* newBtn = findChild<juce::TextButton> (content, [] (auto& b) { return b.getButtonText() == "New"; });
        auto* editBtn = findChild<juce::TextButton> (content, [] (auto& b) { return b.getButtonText() == "Edit"; });
        auto* browseBtn = findChild<juce::TextButton> (content, [] (auto& b) { return b.getButtonText() == "Browser"; });
        expect (newBtn != nullptr && editBtn != nullptr && browseBtn != nullptr);
        if (! newBtn || ! editBtn || ! browseBtn) return;
        int cardCount = 0;
        for (auto* ch : content.getChildren()) cardCount += dynamic_cast<SlotCard*> (ch) != nullptr;
        expectEquals (cardCount, kSlots);
        expect (ui->scope().getWidth() > 300 && ui->scope().getHeight() > 200, "scope has room");
        for (int i = 1; i < kSlots; ++i) expect (! ui->card (i).getBounds().intersects (ui->card (i - 1).getBounds()), "slot cards do not overlap");

        beginTest ("browser: lists the library, filter by library, pick a piece and load it into the chosen slot");
        browseBtn->onClick();
        auto& br = ui->browser();
        expect (br.isVisible());
        expectEquals (br.visibleCards(), 2);
        {
            const int idx = content.getIndexOfChildComponent (&br);
            expect (idx > 0);
            int above = 0;
            for (int i = 0; i < content.getNumChildComponents(); ++i)
                if (content.getChildComponent (i) != &br && content.getChildComponent (i) != panel && i > content.getIndexOfChildComponent (&br)) ++above;
            expectEquals (above, 0, "nothing may draw over the browser");
        }
        br.selectVisible (0);
        br.choose();
        expect (! br.isVisible());
        waitUntil ([&] { return ready (proc, 0); });
        expect (ready (proc, 0) && proc.slotInfo (0).data->meta.name == "Kit A");
        expect (proc.slotInfo (0).data->cover.isValid());
        expectWithinAbsoluteError (hit (proc, 60, 100), 0.1f, 2e-3f);
        ui->openBrowser (3);
        br.selectVisible (1);
        br.choose();
        waitUntil ([&] { return ready (proc, 3); });
        expect (ready (proc, 3) && proc.slotInfo (3).data->meta.name == "Kit B");
        expectEquals (ui->selectedSlot(), 3);
        expect (ready (proc, 0), "loading another slot leaves slot 1 alone");
        expectWithinAbsoluteError (hit (proc, 60, 100), 0.1f + 0.15f, 3e-3f); // both slots sound together

        beginTest ("browser: clicking the selected piece plays it (no loading) and animates the picture; another piece just selects");
        {
            ui->openBrowser (6);
            auto& b2 = ui->browser();
            const int before = b2.previewsPlayed();
            b2.hitCard (0, 0.2f);                                // not selected yet: selects only
            expectEquals (b2.previewsPlayed(), before);
            expect (! b2.isAnimating());
            waitUntil ([&] { return b2.previewReady(); });
            expect (b2.previewReady(), "takes are decoded in the background as soon as a piece is selected");
            proc.prepareToPlay (44100.0, 512);
            b2.hitCard (0, 0.2f);                                // selected: plays
            expectEquals (b2.previewsPlayed(), before + 1);
            expect (b2.isAnimating());
            juce::AudioBuffer<float> io (2, 512);
            io.clear();
            juce::MidiBuffer m;
            proc.processBlock (io, m);
            expect (io.getMagnitude (0, 512) > 0.03f, "the preview is audible through the plug-in output");
            waitUntil ([&] { return b2.previewReady(); });         // a fresh take is already being prepared for the next hit
            b2.hitCard (0, 0.9f);                                // soft hit plays too
            expectEquals (b2.previewsPlayed(), before + 2);
            waitUntil ([&] { return ! b2.isAnimating(); });
            expect (! b2.isAnimating(), "the animation settles");
            b2.setVisible (false);
        }

        beginTest ("clicking a slot picture plays it (audition) even with Mix at 0 and no input");
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f);
        {
            proc.prepareToPlay (44100.0, 512);
            auto& cardA = ui->card (0);
            const auto thumb = cardA.thumbRect();
            cardA.mouseDown (mouseAt (cardA, (float) thumb.getCentreX(), (float) thumb.getCentreY()));
            juce::AudioBuffer<float> io (2, 256);
            io.clear();
            juce::MidiBuffer m;
            proc.processBlock (io, m);
            expectWithinAbsoluteError (io.getSample (0, 100), 0.1f, 2e-3f);
            expectEquals (ui->selectedSlot(), 0);
        }
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);

        beginTest ("mic chips: clicking one switches the articulation, decodes it, and plays it");
        {
            auto& d = ui->detail();
            expectEquals (d.chipCount(), 2);
            const auto r = d.chipRect (1);
            d.mouseDown (mouseAt (d, (float) r.getCentreX(), (float) r.getCentreY()));
            expectEquals (proc.slotArticulation (0), 1);
            waitUntil ([&] { return proc.slotInfo (0).contentArticulation == 1; });
            expectEquals (proc.slotInfo (0).contentArticulation, 1);
            // the click also queues an audition once the new mic is decoded: let the UI timer fire it and one block consume it
            for (int i = 0; i < 15; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            { juce::AudioBuffer<float> io (2, 256); io.clear(); juce::MidiBuffer m; proc.processBlock (io, m); }
            // slot 4 (Kit B) keeps articulation 0: 0.15; slot 1 now plays Bottom: 0.2
            expectWithinAbsoluteError (hit (proc, 60, 100), 0.2f + 0.15f, 3e-3f);
        }

        beginTest ("articulation names \"Group / Label\" are split; chips are grouped under headings with their own rows");
        {
            const auto n1 = ArticulationName::parse ("Cross Stick / Top (Batter Head)"), n2 = ArticulationName::parse ("Full Mix");
            expectEquals (n1.group, juce::String ("Cross Stick"));
            expectEquals (n1.label, juce::String ("Top (Batter Head)"));
            expect (n2.group.isEmpty() && n2.label == "Full Mix");

            otci::Instrument k;
            k.name = "Grouped"; k.library = "X";
            k.waves = { constWave (0.1f), constWave (0.2f), constWave (0.3f) };
            // a "Hit" articulation listed AFTER the cross stick one must still be grouped with the other Hits
            const char* names[] = { "Hit / Top (Batter Head)", "Cross Stick / Top (Batter Head)", "Hit / Room (Punch Boost)" };
            for (int i = 0; i < 3; ++i)
            {
                otci::Articulation a;
                a.name = names[i];
                a.layers = { { 0, 1.0f, 1.0f, { { i } } } };
                k.articulations.push_back (a);
            }
            auto gf = src.getChildFile ("grouped.otci");
            juce::String gerr;
            expect (otci::write (gf, k, gerr), gerr);
            proc.loadSlot (5, gf);
            waitUntil ([&] { return ready (proc, 5); });
            ui->selectSlot (5);
            auto& d = ui->detail();
            expectEquals (d.chipCount(), 3);
            expect (d.strokeMode() && d.strokeTypes().size() == 2, "two styles: the Stroke Type menu is shown");
            expectEquals (d.strokeType(), juce::String ("Hit"));
            expectEquals (d.chipRect (0).getY(), d.chipRect (2).getY());                 // both Hits, same row, despite file order
            expect (d.chipRect (1).isEmpty(), "Cross Stick mics are hidden while Hit is the stroke type");
            expect (d.chipRect (0).getY() >= d.strokeBoxRect().getBottom(), "mic chips sit below the Stroke Type menu");
            d.selectStroke ("Cross Stick");
            expectEquals (proc.slotArticulation (5), 1);                                // same mic (Top), other style
            expect (d.chipRect (0).isEmpty() && ! d.chipRect (1).isEmpty(), "Cross Stick mics shown");
            proc.clearSlot (5);
            ui->selectSlot (0);
        }

        beginTest ("a kit with 45 articulations: style tabs appear, a mic beyond #31 can be selected and plays");
        {
            otci::Instrument k;
            k.name = "Big"; k.library = "X";
            k.waves = { constWave (0.1f), constWave (0.2f) };
            const char* styles[] = { "Hit", "Cross Stick", "Flam", "Wires Off", "Roll" };
            for (int st = 0; st < 5; ++st)
                for (int m = 0; m < 9; ++m)
                {
                    otci::Articulation a;
                    a.name = juce::String (styles[st]) + " / Mic " + juce::String (m);
                    a.layers = { { 0, 1.0f, 1.0f, { { (st * 9 + m) == 40 ? 1 : 0 } } } };   // articulation 40 plays the louder wave
                    k.articulations.push_back (a);
                }
            auto bf = src.getChildFile ("big.otci");
            juce::String berr;
            expect (otci::write (bf, k, berr), berr);
            proc.loadSlot (5, bf);
            waitUntil ([&] { return ready (proc, 5); });
            ui->selectSlot (5);
            auto& d = ui->detail();
            expect (d.strokeMode(), "several styles: Stroke Type menu");
            expectEquals ((int) d.strokeTypes().size(), 5);
            expect (d.chipRect (0).getWidth() > 0 && d.chipRect (40).isEmpty(), "only the active style's mics are shown");
            d.selectStroke ("Roll");
            expect (d.chipRect (0).isEmpty() && d.chipRect (40).getWidth() > 0, "choosing Roll shows Roll mics");
            const auto r = d.chipRect (40);
            d.mouseDown (mouseAt (d, (float) r.getCentreX(), (float) r.getCentreY()));
            expectEquals (proc.slotArticulation (5), 40);
            waitUntil ([&] { return proc.slotInfo (5).contentArticulation == 40; });
            expectEquals (proc.slotInfo (5).contentArticulation, 40);
            for (int i = 0; i < 15; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            { juce::AudioBuffer<float> io (2, 256); io.clear(); juce::MidiBuffer m; proc.processBlock (io, m); } // consume the audition
            proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
            for (int sl : { 0, 3, 6, 7 }) proc.apvts.getParameter (OpenTriggerProcessor::slotParam (sl, "mute"))->setValueNotifyingHost (1.0f);
            proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (1.0f);
            expectWithinAbsoluteError (hit (proc, 60, 100), 0.2f, 3e-3f);               // the louder wave: it really is articulation 40
            proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (0.0f);
            for (int sl : { 0, 3, 6, 7 }) proc.apvts.getParameter (OpenTriggerProcessor::slotParam (sl, "mute"))->setValueNotifyingHost (0.0f);
            proc.clearSlot (5);
            ui->selectSlot (0);
        }

        beginTest ("slot parameters: mute silences that slot, volume and pan apply");
        proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (1.0f);
        expectWithinAbsoluteError (hit (proc, 60, 100), 0.15f, 3e-3f);
        proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (0.0f);
        proc.apvts.getParameter ("s4_pan")->setValueNotifyingHost (1.0f); // hard right
        expectWithinAbsoluteError (hit (proc, 60, 100, 0), 0.2f, 3e-3f);
        expectWithinAbsoluteError (hit (proc, 60, 100, 1), 0.2f + 0.15f * 1.4142f, 4e-3f);
        proc.apvts.getParameter ("s4_pan")->setValueNotifyingHost (0.5f);

        beginTest ("scope: dragging the white line changes the threshold; clicking elsewhere does not");
        {
            auto& sc = ui->scope();
            auto* thr = proc.apvts.getParameter ("threshold");
            const float before = proc.apvts.getRawParameterValue ("threshold")->load();
            const float y0 = sc.thresholdY();
            sc.mouseDown (mouseAt (sc, 50.0f, y0 - 40.0f)); // above the upper line: not grabbable
            sc.mouseDrag (mouseAt (sc, 50.0f, y0 - 70.0f));
            sc.mouseUp (mouseAt (sc, 50.0f, y0 - 70.0f));
            expectEquals (proc.apvts.getRawParameterValue ("threshold")->load(), before);
            sc.mouseDown (mouseAt (sc, 50.0f, y0 + 1.0f));
            sc.mouseDrag (mouseAt (sc, 50.0f, y0 - 30.0f)); // up = higher threshold
            sc.mouseUp (mouseAt (sc, 50.0f, y0 - 30.0f));
            const float after = proc.apvts.getRawParameterValue ("threshold")->load();
            expect (after > before + 3.0f, "threshold went up: " + juce::String (after));
            const float half = (float) sc.getHeight() * 0.5f;
            const float amp = juce::Decibels::decibelsToGain (after);
            expectWithinAbsoluteError (half - ScopeView::shape (amp) * (half - 4.0f), sc.thresholdY(), 0.5f);
            // the Threshold knob drives the same parameter and the lines follow it
            auto& tk = ui->thresholdKnob();
            const float yBefore = sc.thresholdY();
            tk.slider.setValue (-12.0, juce::sendNotificationSync);
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("threshold")->load(), -12.0f, 0.15f);
            expect (sc.thresholdY() < yBefore - 10.0f, "raising the threshold knob lifts the line");
            tk.slider.setValue (-48.0, juce::sendNotificationSync);
            expect (sc.thresholdY() > yBefore, "lowering it drops the line");
            sc.mouseWheelMove (mouseAt (sc, 50.0f, 50.0f), juce::MouseWheelDetails { 0.0f, 0.5f, false, false, false });
            expect (proc.apvts.getRawParameterValue ("threshold")->load() > -48.0f, "wheel up raises the threshold");
            sc.mouseDoubleClick (mouseAt (sc, 50.0f, sc.thresholdY()));
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("threshold")->load(), -30.0f, 0.2f);
            juce::ignoreUnused (thr);
        }

        beginTest ("scope shows what the plug-in hears: input paints the waveform, hits add markers");
        {
            proc.prepareToPlay (44100.0, 512);
            proc.apvts.getParameter ("source")->setValueNotifyingHost (0.0f); // Audio only
            juce::AudioBuffer<float> io (2, 512);
            juce::MidiBuffer m;
            for (int blk = 0; blk < 30; ++blk)
            {
                io.clear();
                if (blk == 5) for (int i = 100; i < 400; ++i) { io.setSample (0, i, 0.7f); io.setSample (1, i, 0.7f); }
                proc.processBlock (io, m);
            }
            auto img2 = ed->createComponentSnapshot (ui->scope().getBoundsInParent(), true, 1.0f);
            auto sn = ui->scope().createComponentSnapshot (ui->scope().getLocalBounds(), true, 1.0f);
            int blue = 0, orange = 0;
            for (int y = 0; y < sn.getHeight(); y += 2)
                for (int x = 0; x < sn.getWidth(); ++x)
                {
                    const auto col = sn.getPixelAt (x, y);
                    blue += col.getBlue() > 200 && col.getRed() < 120;
                    orange += col.getRed() > 200 && col.getGreen() > 120 && col.getBlue() < 100;
                }
            expect (blue > 60, "waveform pixels drawn: " + juce::String (blue));
            expect (orange > 20, "trigger marker pixels drawn: " + juce::String (orange));
            juce::ignoreUnused (img2);
            proc.apvts.getParameter ("source")->setValueNotifyingHost (1.0f); // back to Audio + MIDI
        }

        beginTest ("Smart Trigger: Start, play the track through the plug-in, Stop; it reads the hits and Apply sets the parameters");
        {
            proc.prepareToPlay (44100.0, 512);
            auto track = makeTrack (44100.0, 16);
            auto& sp = ui->smart();
            auto setParam = [&] (const char* id, float v) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
            setParam ("threshold", -10.0f); setParam ("holdoff", 5.0f); setParam ("sensitivity", 10.0f);
            const float before = proc.apvts.getRawParameterValue ("threshold")->load();
            sp.open();
            expect (sp.isVisible() && sp.state() == SmartTriggerPanel::State::intro);
            sp.startListening();
            expect (sp.state() == SmartTriggerPanel::State::listening);
            playThrough (proc, track);
            sp.stopAndAnalyze();
            waitUntil ([&] { return sp.state() != SmartTriggerPanel::State::listening; });
            expect (sp.state() == SmartTriggerPanel::State::result, sp.result().message);
            expect (sp.result().hits >= 14 && sp.result().bleed >= 4, "hits " + juce::String (sp.result().hits) + " bleed " + juce::String (sp.result().bleed));
            expectEquals (proc.apvts.getRawParameterValue ("threshold")->load(), before); // nothing changes until Apply
            const auto res = sp.result();
            sp.apply();
            expect (! sp.isVisible());
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("threshold")->load(), res.thresholdDb, 0.15f);
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("holdoff")->load(), res.retriggerMs, 1.0f);
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("sensitivity")->load(), res.sensitivity, 1.0f);
            expect (proc.apvts.getRawParameterValue ("threshold")->load() < -23.0f, "threshold now sits under the quietest hit");
            expect (res.gateOn && proc.apvts.getRawParameterValue ("gateOn")->load() > 0.5f, "gate switched on for a track with bleed");
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("gateHold")->load(), res.gateHoldMs, 1.0f);
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("gateRelease")->load(), res.gateReleaseMs, 1.0f);
            expect (res.hasOutput, "a kit piece is loaded, so the output level is matched");
            expectWithinAbsoluteError (proc.apvts.getRawParameterValue ("gain")->load(), res.outputGainDb, 0.15f);
            // a failed attempt leaves the settings alone and says why
            sp.open();
            sp.startListening();
            std::vector<float> silence ((size_t) (44100 * 6), 0.0f);
            playThrough (proc, silence);
            const float keep = proc.apvts.getRawParameterValue ("threshold")->load();
            sp.stopAndAnalyze();
            waitUntil ([&] { return sp.state() != SmartTriggerPanel::State::listening; });
            expect (sp.state() == SmartTriggerPanel::State::failed && sp.result().message.isNotEmpty());
            expectEquals (proc.apvts.getRawParameterValue ("threshold")->load(), keep);
            sp.setVisible (false);
            setParam ("threshold", -30.0f); setParam ("holdoff", 30.0f); setParam ("sensitivity", 50.0f);
            setParam ("gateOn", 0.0f); setParam ("gain", 0.0f);
        }

        beginTest ("Escape closes the browser; Edit on an empty slot says so instead of opening");
        ui->openBrowser (5);
        expect (br.isVisible());
        expect (br.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        expect (! br.isVisible());
        ui->selectSlot (6);
        editBtn->onClick();
        expect (! panel->isVisible());
        ui->selectSlot (0);

        beginTest ("New opens the instrument editor panel with an empty 3x2 grid, on top of everything");
        expect (! panel->isVisible());
        newBtn->onClick();
        expect (panel->isVisible());
        {
            const int panelIdx = content.getIndexOfChildComponent (panel);
            int above = 0;
            for (int i = 0; i < content.getNumChildComponents(); ++i) above += i > panelIdx;
            expectEquals (above, 0, "widgets above the editor panel would draw over it");
        }
        auto& grid = panel->getGrid();
        auto& draft = panel->getDraft();
        expectEquals ((int) draft.articulations[0].layers.size(), 3);
        expectEquals (draft.articulations[0].columns(), 2);

        beginTest ("dropping 3 WAVs on the first cell fills cells left-to-right then next row");
        expect (grid.isInterestedInFileDrag ({ a.getFullPathName() }));
        expect (! grid.isInterestedInFileDrag ({ cover.getFullPathName() }));
        grid.filesDropped ({ a.getFullPathName(), b.getFullPathName(), c.getFullPathName() }, SampleGrid::labelW + 5, SampleGrid::headH + 5);
        auto& L = draft.articulations[0].layers;
        expect (! L[0].cells[0].empty() && ! L[0].cells[1].empty() && ! L[1].cells[0].empty() && L[1].cells[1].empty());
        expectEquals (L[0].cells[0].waves[0]->label, juce::String ("a"));

        beginTest ("dropping a non-audio file reports an error and changes nothing");
        juce::String msg;
        grid.onMessage = [&] (const juce::String& m) { msg = m; };
        auto junk = src.getChildFile ("junk.wav");
        junk.replaceWithText ("nope");
        grid.filesDropped ({ junk.getFullPathName() }, SampleGrid::labelW + 5, SampleGrid::headH + SampleGrid::cellH * 2 + 5);
        expect (msg.contains ("unsupported"), msg);
        expect (L[2].cells[0].empty());

        beginTest ("dragging a velocity number up moves the layer range, clamped");
        const float labY = (float) (SampleGrid::headH + SampleGrid::cellH * 1 + 20);
        grid.mouseDown (mouseAt (grid, 20.0f, labY));
        grid.mouseDrag (mouseAt (grid, 20.0f, labY - 20.0f));
        grid.mouseUp (mouseAt (grid, 20.0f, labY - 20.0f));
        expectEquals (L[1].velo, 55);
        grid.mouseDown (mouseAt (grid, 20.0f, labY));
        grid.mouseDrag (mouseAt (grid, 20.0f, labY - 4000.0f));
        grid.mouseUp (mouseAt (grid, 20.0f, labY - 4000.0f));
        expectEquals (L[1].velo, 89); // next layer starts at 90
        draft.setVelo (0, 1, 55);
        expectEquals (L[1].velo, 55);
        expectEquals (L[0].velo, 0);

        beginTest ("clicking a filled cell auditions it through the audio engine");
        grid.mouseDown (mouseAt (grid, (float) (SampleGrid::labelW + 20), (float) (SampleGrid::headH + 20)));
        {
            proc.prepareToPlay (44100.0, 512);
            proc.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f);
            // the audition request is picked up by the next block after prepare cleared the preview: click again
            grid.mouseDown (mouseAt (grid, (float) (SampleGrid::labelW + 20), (float) (SampleGrid::headH + 20)));
            juce::AudioBuffer<float> io (2, 256);
            io.clear();
            juce::MidiBuffer m;
            proc.processBlock (io, m);
            expectWithinAbsoluteError (io.getSample (0, 100), 0.1f * 0.8f, 2e-3f); // sample value x preview gain
            proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
        }

        beginTest ("Delete clears the selected cell");
        expect (grid.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)));
        expect (L[0].cells[0].empty());
        grid.filesDropped ({ a.getFullPathName() }, SampleGrid::labelW + 5, SampleGrid::headH + 5);
        expect (! L[0].cells[0].empty());

        beginTest ("cover image drop on the panel sets a downscaled PNG");
        expect (panel->isInterestedInFileDrag ({ cover.getFullPathName() }));
        panel->filesDropped ({ cover.getFullPathName() }, 0, 0);
        expect (draft.cover.getSize() > 0);

        beginTest ("save: refuses unnamed, writes into the library, closes panel, loads into the selected slot");
        ui->selectSlot (6);
        draft.name = "";
        panel->save();
        expectEquals ((int) lib.findChildFiles (juce::File::findFiles, false, "GUI*.otci").size(), 0);
        expect (panel->isVisible());
        draft.name = "GUI Kit";
        panel->save();
        auto saved = lib.getChildFile ("GUI Kit.otci");
        expect (! panel->isEnabled(), "panel is locked while the save runs in the background");
        waitUntil ([&] { return ! panel->isVisible(); });
        expect (panel->isEnabled());
        expect (saved.existsAsFile());
        waitUntil ([&] { return ready (proc, 6); });
        auto info = proc.slotInfo (6);
        expect (info.status == OpenTriggerProcessor::SlotStatus::ready && info.data->meta.name == "GUI Kit");
        if (info.data)
        {
            expectEquals ((int) info.data->meta.waves.size(), 3);
            expect (info.data->cover.isValid());
            expectEquals ((int) info.data->meta.articulations[0].layers.size(), 2); // empty third layer dropped
            float g = 0;
            const auto* lay = otci::pickLayer (info.data->meta.articulations[0], 100, g);
            expect (lay != nullptr && lay->velo == 55);
        }

        beginTest ("hit with velocity inside the soft layer plays one of the soft takes");
        {
            proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (1.0f);
            proc.apvts.getParameter ("s4_mute")->setValueNotifyingHost (1.0f);
            std::set<int> seen;
            float g = 0;
            otci::pickLayer (proc.slotInfo (6).data->meta.articulations[0], 20, g);
            for (int i = 0; i < 60; ++i) seen.insert ((int) std::lround (hit (proc, 60, 20) / g * 10.0f));
            expect (seen == std::set<int> ({ 1, 3 }), "random RR should alternate 0.1 / 0.3 takes");
            proc.apvts.getParameter ("s1_mute")->setValueNotifyingHost (0.0f);
            proc.apvts.getParameter ("s4_mute")->setValueNotifyingHost (0.0f);
        }

        beginTest ("Edit reopens the slot's instrument; renaming replaces the file, no duplicate");
        ui->startEdit (6);
        waitUntil ([&] { return panel->isVisible(); });
        expect (panel->isVisible());
        expectEquals (panel->getDraft().name, juce::String ("GUI Kit"));
        expectEquals ((int) panel->getDraft().articulations[0].layers.size(), 2);
        expect (panel->getDraft().cover.getSize() > 0);
        panel->getDraft().name = "GUI Kit v2";
        panel->save();
        waitUntil ([&] { return ! panel->isVisible(); });
        expect (lib.getChildFile ("GUI Kit v2.otci").existsAsFile());
        expect (! saved.existsAsFile());
        expectEquals ((int) lib.findChildFiles (juce::File::findFiles, false, "GUI*.otci").size(), 1);
        waitUntil ([&] { return ready (proc, 6) && proc.slotInfo (6).data->meta.name == "GUI Kit v2"; });
        expect (ready (proc, 6) && proc.slotInfo (6).data->meta.name == "GUI Kit v2");

        beginTest ("plugin state round trip restores parameters, all slots and each slot's articulation");
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (0.25f);
        juce::MemoryBlock state;
        proc.getStateInformation (state);
        {
            OpenTriggerProcessor proc2;
            proc2.prepareToPlay (44100.0, 512);
            proc2.setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (proc2.apvts.getRawParameterValue ("mix")->load(), 0.25f, 1e-4f);
            waitUntil ([&] { return ready (proc2, 0) && ready (proc2, 3) && ready (proc2, 6); });
            expect (ready (proc2, 0) && proc2.slotInfo (0).data->meta.name == "Kit A");
            expect (ready (proc2, 3) && proc2.slotInfo (3).data->meta.name == "Kit B");
            expect (ready (proc2, 6) && proc2.slotInfo (6).data->meta.name == "GUI Kit v2");
            expectEquals (proc2.slotInfo (0).contentArticulation, 1); // slot 1 was on Bottom
            expect (proc2.slotInfo (1).status == OpenTriggerProcessor::SlotStatus::empty);
        }

        beginTest ("state pointing at a deleted file does not crash and that slot stays silent");
        lib.getChildFile ("GUI Kit v2.otci").deleteFile();
        {
            OpenTriggerProcessor proc3;
            proc3.prepareToPlay (44100.0, 512);
            proc3.setStateInformation (state.getData(), (int) state.getSize());
            waitUntil ([&] { return ready (proc3, 0) && ready (proc3, 3); });
            expect (proc3.slotInfo (6).status == OpenTriggerProcessor::SlotStatus::empty);
            expectWithinAbsoluteError (hit (proc3, 60, 100), (0.2f + 0.15f) * 0.25f, 3e-3f); // mix was saved at 25%
        }

        beginTest ("clear button empties a slot and frees it");
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
        proc.apvts.getParameter ("s7_mute")->setValueNotifyingHost (1.0f);
        ui->card (3).onClear (3);
        expect (proc.slotInfo (3).status == OpenTriggerProcessor::SlotStatus::empty);
        expectWithinAbsoluteError (hit (proc, 60, 100), 0.2f, 3e-3f);

        beginTest ("a missing or corrupt instrument file ends in a failed slot, not a crash");
        {
            auto bad = lib.getChildFile ("Broken.otci");
            bad.replaceWithText ("not an instrument");
            proc.loadSlot (7, bad);
            waitUntil ([&] { return proc.slotInfo (7).status == OpenTriggerProcessor::SlotStatus::failed; });
            expect (proc.slotInfo (7).status == OpenTriggerProcessor::SlotStatus::failed);
            expect (proc.slotInfo (7).error.isNotEmpty());
            bad.deleteFile();
        }

        beginTest ("library scan ignores deleted files");
        Library l;
        l.scan (lib);
        expectEquals ((int) l.entries().size(), 2); // Kit A, Kit B

        ed.reset();
        root.deleteRecursively();
    }
};
static GuiTests guiTests;
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    LibraryCache::setSynchronous (true);
    juce::UnitTestRunner runner;
    runner.runAllTests();
    int fails = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) fails += runner.getResult (i)->failures;
    return fails == 0 ? 0 : 1;
}
