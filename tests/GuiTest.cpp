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

        beginTest ("Tune reads like a tuner: Hz and note of the piece, typing Hz sets the semitones");
        {
            const double f0 = 110.0;
            juce::AudioBuffer<float> tone (1, 44100);
            for (int i = 0; i < tone.getNumSamples(); ++i) tone.setSample (0, i, (float) (std::exp (-i / 44100.0 * 5.0) * std::sin (6.2831853 * f0 * i / 44100.0)));
            otci::Instrument k; k.name = "Tom Tone"; k.library = "Lib One";
            otci::Wave w; w.flac = encodeFlac (tone, 44100.0, 24); w.frames = tone.getNumSamples(); w.channels = 1; w.rate = 44100.0;
            k.waves = { w };
            otci::Articulation a0; a0.name = "Hit"; a0.layers = { { 0, 1.0f, 1.0f, { { 0 } } } };
            k.articulations = { a0 };
            auto tf = lib.getChildFile ("Tom Tone.otci");
            juce::String terr; otci::write (tf, k, terr);
            proc.loadSlot (6, tf);
            waitUntil ([&] { return ready (proc, 6) && proc.slotInfo (6).fundamentalHz > 0.0f; });
            expect (std::abs (proc.slotInfo (6).fundamentalHz - 110.0f) < 1.0f, "measured " + juce::String (proc.slotInfo (6).fundamentalHz));
            auto& d = ui->detail();
            d.setSlot (6);
            for (int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            ParamKnob* knob = nullptr;
            for (auto* ch : d.getChildren())
                if (auto* pk = dynamic_cast<ParamKnob*> (ch); pk != nullptr && pk->isVisible() && pk->slider.getMinimum() == -12.0 && pk->slider.getMaximum() == 12.0
                    && pk->slider.getProperties()["bipolar"] && pk->slider.getTextBoxWidth() == 126) knob = pk;
            expect (knob != nullptr, "tune knob found");
            if (knob != nullptr)
            {
                expect (knob->slider.getTextFromValue (0.0).startsWith ("110."), knob->slider.getTextFromValue (0.0));
                expect (knob->slider.getTextFromValue (0.0).contains ("A2"), knob->slider.getTextFromValue (0.0));
                expect (knob->slider.getTextFromValue (12.0).startsWith ("220."), knob->slider.getTextFromValue (12.0));
                const double semis = knob->slider.getValueFromText ("123.47 Hz");           // B2
                expectWithinAbsoluteError (semis, 2.0, 0.02);
                expect (knob->slider.getValueFromText ("500 Hz") == 12.0, "clamped to the knob range");
                expectWithinAbsoluteError (knob->slider.getValueFromText ("3.5 st"), 3.5, 1e-6);
            }
            proc.clearSlot (6);
            tf.deleteFile();
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

// ------------------------------------------------------------------------------------------------ multi-output
juce::File writeMicKit (const juce::File& dir)
{
    otci::Instrument k;
    k.name = "Mics"; k.library = "L";
    k.waves = { constWave (0.1f), constWave (0.2f), constWave (0.3f), constWave (0.4f), constWave (0.5f) };
    const char* names[] = { "Hit / Full Mix", "Hit / Kick In", "Hit / Kick Out", "Hit / Room", "Cross Stick / Kick In" };
    for (int i = 0; i < 5; ++i)
    {
        otci::Articulation a;
        a.name = names[i];
        a.layers = { { 0, 1.0f, 1.0f, { { i } } } };
        k.articulations.push_back (a);
    }
    auto f = dir.getChildFile ("Mics.otci");
    juce::String err;
    otci::write (f, k, err);
    return f;
}

struct LaneTests : juce::UnitTest
{
    LaneTests() : juce::UnitTest ("Mic lanes and outputs", "OpenTrigger") {}

    static void waitUntil (std::function<bool()> done, int ms = 8000)
    {
        for (int waited = 0; waited < ms && ! done(); waited += 20)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    }
    // One MIDI hit through processBlock with as many channels as the host currently gives the plug-in; returns the channels at one sample.
    static std::vector<float> hitAll (OpenTriggerProcessor& proc, int vel = 100)
    {
        proc.prepareToPlay (44100.0, 512);
        const int L = proc.getLatencySamples();
        juce::AudioBuffer<float> io (std::max (2, proc.getTotalNumOutputChannels()), L + 256);
        io.clear();
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) vel), 0);
        proc.processBlock (io, m);
        std::vector<float> out;
        for (int c = 0; c < io.getNumChannels(); ++c) out.push_back (io.getSample (c, L + 100));
        return out;
    }
    static bool slotReady (OpenTriggerProcessor& p, int s) { return p.slotInfo (s).status == OpenTriggerProcessor::SlotStatus::ready; }
    static void loadAndWait (OpenTriggerProcessor& p, int s, const juce::File& f)
    {
        p.loadSlot (s, f);
        waitUntil ([&] { return slotReady (p, s); });
    }
    static void lanesReady (OpenTriggerProcessor& p, int s, std::initializer_list<int> lanes)
    {
        waitUntil ([&] { for (int l : lanes) if (! p.laneInfo (s, l).ready) return false; return true; });
    }
    static void silenceOthers (OpenTriggerProcessor& p)
    {
        p.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
    }

    void runTest() override
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_lane_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        auto lib = root.getChildFile ("lib");
        lib.createDirectory();
        setenv ("OPENTRIGGER_LIBRARY", lib.getFullPathName().toRawUTF8(), 1);
        const auto kit = writeMicKit (lib);

        beginTest ("the extra outputs exist, are off by default, and the old stereo behaviour is untouched");
        OpenTriggerProcessor proc;
        proc.setPlayConfigDetails (2, 2, 44100.0, 512);
        silenceOthers (proc);
        expectEquals (proc.getBusCount (false), 8);
        for (int b = 1; b < 8; ++b) expect (! proc.getBus (false, b)->isEnabled(), "Out " + juce::String (b + 1) + " off by default");
        expectEquals (proc.getTotalNumOutputChannels(), 2);
        loadAndWait (proc, 0, kit);
        expect (slotReady (proc, 0));
        {
            auto h = hitAll (proc);
            expectWithinAbsoluteError (h[0], 0.1f, 3e-3f);                      // slot 0, lane 0, mic "Full Mix"
            expect (! proc.laneInfo (0, 1).on, "no extra lane until asked for");
        }

        beginTest ("a lane routed to an output the host has not enabled plays on Main (never silent)");
        {
            proc.setLaneArticulation (0, 1, 1);
            proc.setLaneBus (0, 1, 1);
            proc.setLaneOn (0, 1, true);
            lanesReady (proc, 0, { 1 });
            expect (proc.laneInfo (0, 1).ready);
            auto h = hitAll (proc);
            expectEquals ((int) h.size(), 2);
            expectWithinAbsoluteError (h[0], 0.1f + 0.2f, 3e-3f);
        }

        beginTest ("with Out 2 enabled: Main carries lane 0 only, Out 2 carries the extra mic");
        {
            expect (proc.getBus (false, 1)->enable (true), "host can enable Out 2");
            expectEquals (proc.getTotalNumOutputChannels(), 4);
            expectEquals (proc.getChannelIndexInProcessBlockBuffer (false, 1, 0), 2);
            auto h = hitAll (proc);
            expectEquals ((int) h.size(), 4);
            expectWithinAbsoluteError (h[0], 0.1f, 3e-3f);
            expectWithinAbsoluteError (h[1], 0.1f, 3e-3f);
            expectWithinAbsoluteError (h[2], 0.2f, 3e-3f);
            expectWithinAbsoluteError (h[3], 0.2f, 3e-3f);
        }

        beginTest ("lane level, lane off, lane back on; output volume applies to the outputs too; Mix only touches Main");
        {
            proc.setLaneGain (0, 1, -6.0206f);
            expectWithinAbsoluteError (hitAll (proc)[2], 0.1f, 3e-3f);
            proc.setLaneGain (0, 1, 0.0f);
            proc.setLaneOn (0, 1, false);
            { auto h = hitAll (proc); expectWithinAbsoluteError (h[0], 0.1f, 3e-3f); expectWithinAbsoluteError (h[2], 0.0f, 1e-4f); }
            proc.setLaneOn (0, 1, true);
            lanesReady (proc, 0, { 1 });
            expectWithinAbsoluteError (hitAll (proc)[2], 0.2f, 3e-3f);
            proc.apvts.getParameter ("gain")->setValueNotifyingHost (proc.apvts.getParameter ("gain")->convertTo0to1 (6.0206f));
            { auto h = hitAll (proc); expectWithinAbsoluteError (h[0], 0.2f, 4e-3f); expectWithinAbsoluteError (h[2], 0.4f, 4e-3f); }
            proc.apvts.getParameter ("gain")->setValueNotifyingHost (proc.apvts.getParameter ("gain")->convertTo0to1 (0.0f));
            proc.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f);          // dry only on Main; the outputs keep the sampled sound
            { auto h = hitAll (proc); expectWithinAbsoluteError (h[0], 0.0f, 1e-4f); expectWithinAbsoluteError (h[2], 0.2f, 3e-3f); }
            proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
        }

        beginTest ("changing a lane's mic while Out 2 is on swaps the sound; lane 0's mic is independent");
        {
            proc.setLaneArticulation (0, 1, 2);                               // Kick Out
            lanesReady (proc, 0, { 1 });
            waitUntil ([&] { return proc.laneInfo (0, 1).ready && std::abs (hitAll (proc)[2] - 0.3f) < 5e-3f; });
            expectWithinAbsoluteError (hitAll (proc)[2], 0.3f, 3e-3f);
            proc.setSlotArticulation (0, 3);                                  // slot's own mic -> Room
            waitUntil ([&] { return proc.slotInfo (0).contentArticulation == 3; });
            auto h = hitAll (proc);
            expectWithinAbsoluteError (h[0], 0.4f, 3e-3f);
            expectWithinAbsoluteError (h[2], 0.3f, 3e-3f);
            proc.setSlotArticulation (0, 0);
            waitUntil ([&] { return proc.slotInfo (0).contentArticulation == 0; });
        }

        beginTest ("Split mics to outputs: each mic of the stroke type gets its own lane and output, Full Mix and other strokes are left out");
        {
            proc.setLaneOn (0, 1, false);
            proc.splitMicsToOutputs (0, "Hit");
            expect (proc.laneInfo (0, 1).on && proc.laneInfo (0, 1).articulation == 1 && proc.laneInfo (0, 1).bus == 1, "Kick In -> Out 2");
            expect (proc.laneInfo (0, 2).on && proc.laneInfo (0, 2).articulation == 2 && proc.laneInfo (0, 2).bus == 2, "Kick Out -> Out 3");
            expect (proc.laneInfo (0, 3).on && proc.laneInfo (0, 3).articulation == 3 && proc.laneInfo (0, 3).bus == 3, "Room -> Out 4");
            expect (! proc.laneInfo (0, 4).on && ! proc.laneInfo (0, 5).on, "nothing else (Cross Stick is another stroke type)");
            expectEquals (proc.laneInfo (0, 0).articulation, 0);
            expectEquals (proc.laneInfo (0, 0).bus, 0);
            for (int b = 2; b <= 3; ++b) proc.getBus (false, b)->enable (true);
            lanesReady (proc, 0, { 1, 2, 3 });
            expectEquals (proc.getTotalNumOutputChannels(), 8);
            auto h = hitAll (proc);
            expectEquals ((int) h.size(), 8);
            expectWithinAbsoluteError (h[0], 0.1f, 3e-3f);   // Main: Full Mix
            expectWithinAbsoluteError (h[2], 0.2f, 3e-3f);   // Out 2: Kick In
            expectWithinAbsoluteError (h[4], 0.3f, 3e-3f);   // Out 3: Kick Out
            expectWithinAbsoluteError (h[6], 0.4f, 3e-3f);   // Out 4: Room
        }
        silenceOthers (proc);

        beginTest ("project state keeps the lanes; a project saved before this feature opens with lanes off");
        {
            juce::MemoryBlock saved;
            proc.getStateInformation (saved);
            OpenTriggerProcessor p2;
            p2.setPlayConfigDetails (2, 2, 44100.0, 512);
            p2.setStateInformation (saved.getData(), (int) saved.getSize());
            waitUntil ([&] { return slotReady (p2, 0); });
            lanesReady (p2, 0, { 1, 2 });
            expect (p2.laneInfo (0, 1).on && p2.laneInfo (0, 1).articulation == 1 && p2.laneInfo (0, 1).bus == 1);
            expect (p2.laneInfo (0, 2).on && p2.laneInfo (0, 2).articulation == 2 && p2.laneInfo (0, 2).bus == 2);
            expect (p2.laneInfo (0, 1).ready && p2.laneInfo (0, 2).ready, "lane content is rebuilt after loading");

            // same state without the lane properties = a project from the previous version
            auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
            auto vt = juce::ValueTree::fromXml (*xml);
            for (int s = 1; s <= kSlots; ++s) vt.removeProperty ("lanes" + juce::String (s), nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*vt.createXml(), old);
            OpenTriggerProcessor p3;
            p3.setPlayConfigDetails (2, 2, 44100.0, 512);
            p3.setStateInformation (old.getData(), (int) old.getSize());
            waitUntil ([&] { return slotReady (p3, 0); });
            for (int l = 1; l < kLanes; ++l) expect (! p3.laneInfo (0, l).on, "lane " + juce::String (l) + " off in an old project");
            silenceOthers (p3);
            expectWithinAbsoluteError (hitAll (p3)[0], 0.1f, 3e-3f);
        }

        beginTest ("loading another instrument into the slot resets its lanes; clearing the slot frees them");
        {
            proc.loadSlot (0, kit);
            waitUntil ([&] { return slotReady (proc, 0); });
            for (int l = 1; l < kLanes; ++l) expect (! proc.laneInfo (0, l).on, "lane " + juce::String (l) + " reset");
            proc.splitMicsToOutputs (0, "Hit");
            lanesReady (proc, 0, { 1 });
            proc.clearSlot (0);
            for (int l = 1; l < kLanes; ++l) expect (! proc.laneInfo (0, l).on && ! proc.laneInfo (0, l).ready);
        }

        beginTest ("mixer: a level on a mic's strip puts it on a lane, the bottom of the fader takes it off; pan, polarity, mute and solo act on that mic");
        {
            OpenTriggerProcessor p5;
            p5.setPlayConfigDetails (2, 2, 44100.0, 512);
            silenceOthers (p5);
            loadAndWait (p5, 0, kit);
            expectEquals (p5.laneOfArticulation (0, 0), 0);
            expectEquals (p5.laneOfArticulation (0, 1), -1);
            expect (p5.setMicLevel (0, 1, 0.0f), "Kick In on");
            const int lnIn = p5.laneOfArticulation (0, 1);
            expect (lnIn > 0);
            lanesReady (p5, 0, { lnIn });
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f + 0.2f, 3e-3f);
            p5.setMicLevel (0, 1, -6.0206f);
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f + 0.1f, 3e-3f);
            p5.setLanePhase (0, lnIn, true);
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f - 0.1f, 3e-3f);
            p5.setLanePhase (0, lnIn, false);
            p5.setLanePan (0, lnIn, 1.0f);                                    // hard right: the left channel loses this mic
            { auto h = hitAll (p5); expectWithinAbsoluteError (h[0], 0.1f, 3e-3f); expectWithinAbsoluteError (h[1], 0.1f + 0.1f * 1.4142f, 4e-3f); }
            p5.setLanePan (0, lnIn, 0.0f);
            p5.setLaneMute (0, lnIn, true);
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f, 3e-3f);
            p5.setLaneMute (0, lnIn, false);
            p5.setLaneSolo (0, lnIn, true);                                   // solo: only this mic (the Full Mix on lane 0 stops)
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f, 3e-3f);
            p5.setLaneSolo (0, lnIn, false);
            p5.setMicLevel (0, 0, -60.0f);                                    // the slot's own mic to the bottom: silent
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f, 3e-3f);
            p5.setMicLevel (0, 0, 0.0f);
            p5.setMicLevel (0, 1, -60.0f);                                    // bottom of the fader: lane freed
            expectEquals (p5.laneOfArticulation (0, 1), -1);
            expect (! p5.laneInfo (0, lnIn).on);
            expectWithinAbsoluteError (hitAll (p5)[0], 0.1f, 3e-3f);
            // every lane can be taken, the next mic is refused instead of overwriting one
            for (int a = 1; a <= 4; ++a) expect (p5.setMicLevel (0, a, 0.0f));
            // (only 5 mics in this kit; the lane limit is exercised in the engine tests)
        }

        beginTest ("MIXED: the mics together sound like the Mixed sound (calibrated against the Full Mix); FULL MIX goes back");
        {
            // Full Mix = (Top + Room + Close) x 0.7, the way the libraries are built
            otci::Instrument k;
            k.name = "Cal"; k.library = "L";
            k.waves = { constWave (0.42f), constWave (0.2f), constWave (0.3f), constWave (0.1f) };
            const char* names[] = { "Hit / Full Mix", "Hit / Top", "Hit / Room", "Hit / Close" };
            for (int i = 0; i < 4; ++i)
            {
                otci::Articulation a; a.name = names[i];
                a.layers = { { 0, 1.0f, 1.0f, { { i } } } };
                k.articulations.push_back (a);
            }
            auto cf = lib.getChildFile ("Cal.otci");
            juce::String cerr;
            expect (otci::write (cf, k, cerr), cerr);
            OpenTriggerProcessor p6;
            p6.setPlayConfigDetails (2, 2, 44100.0, 512);
            silenceOthers (p6);
            loadAndWait (p6, 0, cf);
            const float mixed = hitAll (p6)[0];
            expectWithinAbsoluteError (mixed, 0.42f, 3e-3f);
            p6.calibrateMixed (0, "Hit");
            waitUntil ([&] { return p6.mixerMessage().contains ("together") || p6.mixerMessage().startsWith ("Could not"); });
            expect (p6.mixerMessage().contains ("together"), p6.mixerMessage());
            for (int a = 1; a <= 3; ++a)
            {
                const int ln = p6.laneOfArticulation (0, a);
                expect (ln > 0, "mic " + juce::String (a) + " has a lane");
                if (ln > 0) expectWithinAbsoluteError (p6.laneInfo (0, ln).gainDb, 20.0f * std::log10 (0.7f), 0.2f);
            }
            expect (p6.laneInfo (0, 0).gainDb <= kLaneOffDb, "the Full Mix lane is silent while the mics play");
            lanesReady (p6, 0, { p6.laneOfArticulation (0, 1), p6.laneOfArticulation (0, 2), p6.laneOfArticulation (0, 3) });
            expectWithinAbsoluteError (hitAll (p6)[0], mixed, 0.42f * 0.02f);        // within 2 %: the same sound as the Full Mix
            p6.resetMixer (0);
            expectWithinAbsoluteError (hitAll (p6)[0], mixed, 3e-3f);
            for (int a = 1; a <= 3; ++a) expectEquals (p6.laneOfArticulation (0, a), -1);
        }

        beginTest ("the Mixer panel: strips per mic of the stroke type, fader drag, buttons, MIXED and FULL MIX");
        {
            OpenTriggerProcessor p4;
            p4.setPlayConfigDetails (2, 2, 44100.0, 512);
            p4.prepareToPlay (44100.0, 512);
            silenceOthers (p4);
            loadAndWait (p4, 0, kit);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p4.createEditor());
            auto* ui = static_cast<OpenTriggerEditor*> (ed.get());
            ed->setSize (1280, 720);
            ui->selectSlot (0);
            auto& d = ui->detail();
            expect (d.view() == SlotDetail::View::mics);
            const auto tab = d.viewTabRect (SlotDetail::View::mixer);
            d.mouseDown (mouseAt (d, (float) tab.getCentreX(), (float) tab.getCentreY()));
            expect (d.view() == SlotDetail::View::mixer, "MIXER tab");
            expectEquals (d.stripCount(), 4);                                  // Hit: Full Mix, Kick In, Kick Out, Room
            for (int i = 0; i < d.stripCount(); ++i)
            {
                const auto& st = d.strip (i);
                expect (d.getLocalBounds().contains (st.area) && st.area.contains (st.fader) && st.area.contains (st.out) && st.area.contains (st.name), "strip " + juce::String (i) + " fits in the panel");
                if (i > 0) expect (! st.area.intersects (d.strip (i - 1).area), "strips do not overlap");
            }
            // drag the Kick In fader (strip 1) from the bottom up to the unity mark
            const auto& s1 = d.strip (1);
            d.mouseDown (mouseAt (d, (float) s1.fader.getCentreX(), (float) s1.fader.getBottom()));
            expectEquals (p4.laneOfArticulation (0, 1), -1);                    // still at the bottom = off
            d.mouseDrag (mouseAt (d, (float) s1.fader.getCentreX(), (float) SlotDetail::dbToY (s1.fader, 0.0f)));
            const int ln = p4.laneOfArticulation (0, 1);
            expect (ln > 0, "dragging the fader up turns the mic on");
            if (ln > 0) expectWithinAbsoluteError (p4.laneInfo (0, ln).gainDb, 0.0f, 0.6f);
            // buttons of that strip
            d.mouseDown (mouseAt (d, (float) s1.mute.getCentreX(), (float) s1.mute.getCentreY()));
            expect (p4.laneInfo (0, ln).mute);
            d.mouseDown (mouseAt (d, (float) s1.mute.getCentreX(), (float) s1.mute.getCentreY()));
            d.mouseDown (mouseAt (d, (float) s1.phase.getCentreX(), (float) s1.phase.getCentreY()));
            expect (p4.laneInfo (0, ln).phase && ! p4.laneInfo (0, ln).mute);
            d.mouseDoubleClick (mouseAt (d, (float) s1.fader.getCentreX(), (float) s1.fader.getCentreY()));
            expectWithinAbsoluteError (p4.laneInfo (0, ln).gainDb, 0.0f, 1e-4f);
            // FULL MIX button
            const auto rb = d.resetButtonRect();
            d.mouseDown (mouseAt (d, (float) rb.getCentreX(), (float) rb.getCentreY()));
            expectEquals (p4.laneOfArticulation (0, 1), -1);
            // MIXED button (the kit has no separate full-mix wave to compare: it must say so, not break)
            const auto mb = d.mixedButtonRect();
            d.mouseDown (mouseAt (d, (float) mb.getCentreX(), (float) mb.getCentreY()));
            waitUntil ([&] { return p4.mixerMessage().isNotEmpty() && p4.mixerMessage() != "Calibrating..."; });
            expect (p4.mixerMessage().isNotEmpty());
            // hearing the slot from the editor plays every mic on Main
            p4.resetMixer (0);
            p4.setMicLevel (0, 1, 0.0f); p4.setMicLevel (0, 2, 0.0f); p4.setMicLevel (0, 3, 0.0f);
            lanesReady (p4, 0, { p4.laneOfArticulation (0, 1), p4.laneOfArticulation (0, 2), p4.laneOfArticulation (0, 3) });
            p4.auditionSlot (0, 100);
            juce::AudioBuffer<float> io (2, 700);
            io.clear();
            juce::MidiBuffer m;
            p4.processBlock (io, m);
            expectWithinAbsoluteError (io.getSample (0, 300), 0.1f + 0.2f + 0.3f + 0.4f, 6e-3f);   // all the mics together
            ed.reset();
        }
        root.deleteRecursively();
    }
};
static LaneTests laneTests;
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
