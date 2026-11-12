#include <set>
#include "Engine.h"
#include "Library.h"
#include "Draft.h"
#include "SmartTrigger.h"
#include "Pack.h"
#include <juce_core/juce_core.h>

namespace
{
juce::AudioBuffer<float> constant (float v, int frames, int ch = 1)
{
    juce::AudioBuffer<float> b (ch, frames);
    for (int c = 0; c < ch; ++c) juce::FloatVectorOperations::fill (b.getWritePointer (c), v, frames);
    return b;
}

otci::Wave makeWave (float v, int frames = 4000, int ch = 1)
{
    otci::Wave w;
    w.flac = encodeFlac (constant (v, frames, ch), 44100.0, 24);
    w.frames = frames; w.channels = ch; w.rate = 44100.0;
    return w;
}

// 3 layers (velo 0/40/90), one wave each with distinct constant value; layer 1 has 2 RR
otci::Instrument makeInstrument()
{
    otci::Instrument inst;
    inst.name = "Test"; inst.library = "Lib";
    inst.waves = { makeWave (0.1f), makeWave (0.3f), makeWave (0.35f), makeWave (0.6f) };
    otci::Articulation a; a.name = "Hit";
    a.layers = { { 0, 1.0f, 1.0f, { { 0 } } }, { 40, 1.0f, 1.0f, { { 1 }, { 2 } } }, { 90, 1.0f, 1.0f, { { 3 } } } };
    inst.articulations.push_back (a);
    otci::Articulation b; b.name = "Other";
    b.layers = { { 0, 1.0f, 1.0f, { { 3 } } } };
    inst.articulations.push_back (b);
    inst.image.append ("\x89PNGfake", 8);
    return inst;
}

struct OtciTests : juce::UnitTest
{
    OtciTests() : juce::UnitTest ("OTCI", "OpenTrigger") {}

    void runTest() override
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_tests_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        auto file = dir.getChildFile ("t.otci");
        juce::String err;

        beginTest ("write/read roundtrip keeps structure, audio blobs and cover");
        auto inst = makeInstrument();
        expect (otci::write (file, inst, err), err);
        auto back = otci::read (file, true, err);
        expect (back.has_value(), err);
        if (back)
        {
            expectEquals (back->name, juce::String ("Test"));
            expectEquals ((int) back->waves.size(), 4);
            expect (back->waves[2].flac == inst.waves[2].flac);
            expect (back->image == inst.image);
            expectEquals ((int) back->articulations[0].layers[1].roundRobins.size(), 2);
            expectEquals (back->articulations[0].layers[2].velo, 90);
        }

        beginTest ("metadata-only read skips audio but keeps cover");
        auto info = otci::read (file, false, err);
        expect (info.has_value() && info->waves[0].flac.getSize() == 0 && info->image.getSize() == 8);

        beginTest ("corrupt files are rejected, not crashed on");
        auto bad = dir.getChildFile ("bad.otci");
        bad.replaceWithText ("nope");
        expect (! otci::read (bad, true, err).has_value());
        auto trunc = dir.getChildFile ("trunc.otci");
        juce::MemoryBlock mb; file.loadFileAsData (mb);
        trunc.replaceWithData (mb.getData(), mb.getSize() / 2);
        expect (! otci::read (trunc, true, err).has_value());

        beginTest ("pickLayer: highest velo <= velocity, gain lerps min..max");
        otci::Articulation art;
        art.layers = { { 0, 0.1f, 1.4f, {} }, { 30, 0.45f, 1.26f, {} }, { 87, 0.63f, 1.0f, {} } };
        float g = 0;
        expectEquals (otci::pickLayer (art, 1, g)->velo, 0);
        expectEquals (otci::pickLayer (art, 30, g)->velo, 30);
        expectWithinAbsoluteError (g, 0.45f, 1e-5f);
        expectEquals (otci::pickLayer (art, 127, g)->velo, 87);
        expectWithinAbsoluteError (g, 1.0f, 1e-5f);

        beginTest ("library scan lists instrument with cover bytes");
        Library lib;
        lib.scan (dir);
        expectEquals ((int) lib.entries().size(), 1);
        if (! lib.entries().empty())
        {
            expectEquals (lib.entries()[0].layers, 4);
            expectEquals (lib.entries()[0].waves, 4);
        }

        auto slotOf = [&] (const juce::File& f, int art = 0)
        {
            juce::String e;
            auto d = readInstrumentData (f, e);
            auto c = d ? buildSlotContent (d, art, e) : nullptr;
            expect (c != nullptr, e);
            return c;
        };
        auto run = [&] (TriggerEngine& eng, const TriggerEngine::Params& p, int frames, juce::MidiBuffer m = {}, float fill = 0.0f)
        {
            juce::AudioBuffer<float> io = constant (fill, frames, 2);
            eng.process (io, m, p);
            return io;
        };
        auto note = [] (int vel, int at) { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) vel), at); return m; };
        auto freshEngine = [&] (std::shared_ptr<SlotContent> c, int slot = 0)
        {
            auto e = std::make_unique<TriggerEngine>();
            e->prepare (44100.0, 512);
            e->setSlotContent (slot, std::move (c));
            return e;
        };

        beginTest ("slot content decodes only the waves of its articulation (lazy RAM)");
        {
            auto c0 = slotOf (file, 0), c1 = slotOf (file, 1);
            expect (c0 && c1);
            if (c0 && c1)
            {
                int d0 = 0, d1 = 0;
                for (auto& b : c0->audio) d0 += b.getNumSamples() > 0;
                for (auto& b : c1->audio) d1 += b.getNumSamples() > 0;
                expectEquals (d0, 4);  // Hit uses waves 0..3
                expectEquals (d1, 1);  // Other uses wave 3 only
                expectEquals (c1->articulation, 1);
                juce::String e;
                auto clamped = buildSlotContent (c0->data, 99, e);
                expectEquals (clamped->articulation, 1);
            }
        }

        beginTest ("library lists pieces in kit order: kick, snares, rack tom, floor tom");
        {
            auto d2 = dir.getChildFile ("order");
            d2.createDirectory();
            for (auto* n : { "Floor Tom", "Rack Tom", "Zebra Snare", "Kick", "Bell Snare" })
            {
                auto i2 = makeInstrument();
                i2.name = n; i2.library = "L";
                expect (otci::write (d2.getChildFile (juce::String (n) + ".otci"), i2, err), err);
            }
            Library l2;
            l2.scan (d2);
            juce::StringArray names;
            for (auto& e : l2.entries()) names.add (e.name);
            expectEquals (names.joinIntoString (","), juce::String ("Kick,Bell Snare,Zebra Snare,Rack Tom,Floor Tom"));
        }

        beginTest ("engine: MIDI velocity picks the layer, sample accurate; any note triggers");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::midiOnly;
            auto hit = [&] (int vel, int art = 0)
            {
                auto eng = freshEngine (slotOf (file, art));
                const int L = eng->latencySamples();           // everything is delayed by the fixed look-ahead
                auto io = run (*eng, p, 10 + L + 300, note (vel, 10));
                expectEquals (io.getSample (0, 10 + L - 1), 0.0f); // nothing before the event (+ look-ahead): sample accurate
                return io.getSample (0, 10 + L + 90);
            };
            expectWithinAbsoluteError (hit (10), 0.1f, 2e-3f);
            expectWithinAbsoluteError (hit (100), 0.6f, 2e-3f);
            expectWithinAbsoluteError (hit (1, 1), 0.6f, 2e-3f); // articulation 1
            std::set<int> seen;
            for (int i = 0; i < 60; ++i) seen.insert ((int) std::lround (hit (50) * 100));
            expect (seen.count (30) && seen.count (35) && seen.size() == 2, "random round robin should hit both takes of layer 1");
            p.midiNote = 61;
            auto eng = freshEngine (slotOf (file));
            expectEquals (run (*eng, p, 256, note (100, 0)).getMagnitude (0, 256), 0.0f); // note 60 filtered out
        }

        beginTest ("engine: a mono track (one channel) triggers and plays, for hosts that keep mono tracks mono");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::midiOnly;
            auto eng = freshEngine (slotOf (file));
            const int L = eng->latencySamples();
            juce::AudioBuffer<float> mono (1, L + 300);
            mono.clear();
            juce::MidiBuffer m = note (100, 0);
            eng->process (mono, m, p);
            expectWithinAbsoluteError (mono.getSample (0, L + 100), 0.6f, 5e-3f);
            // and audio-triggered on a mono input
            TriggerEngine::Params q;
            q.source = TriggerEngine::audioOnly;
            auto eng2 = freshEngine (slotOf (file));
            juce::AudioBuffer<float> in (1, 2048);
            in.clear();
            for (int i = 100; i < 400; ++i) in.setSample (0, i, 0.9f);
            juce::MidiBuffer none;
            eng2->process (in, none, q);
            expect (in.getMagnitude (0, 2048) > 0.05f, "mono audio hit produced sound");
        }

        beginTest ("engine: round robin never repeats the previous take and uses all takes");
        {
            otci::Instrument rr;
            rr.name = "RR";
            for (float v : { 0.1f, 0.2f, 0.3f }) rr.waves.push_back (makeWave (v));
            otci::Articulation a; a.name = "A";
            a.layers = { { 0, 1.0f, 1.0f, { { 0 }, { 1 }, { 2 } } } };
            rr.articulations.push_back (a);
            auto f = dir.getChildFile ("rr.otci");
            expect (otci::write (f, rr, err), err);
            auto eng = freshEngine (slotOf (f));
            TriggerEngine::Params p;
            p.source = TriggerEngine::midiOnly;
            std::set<int> seen;
            int prev = -1, repeats = 0;
            for (int i = 0; i < 300; ++i)
            {
                eng->reset();
                const int L = eng->latencySamples();
                const int take = (int) std::lround (run (*eng, p, L + 128, note (100, 0)).getSample (0, L + 50) * 10.0f);
                repeats += take == prev;
                prev = take;
                seen.insert (take);
            }
            expectEquals (repeats, 0);
            expectEquals ((int) seen.size(), 3);
        }

        beginTest ("engine: audio transient triggers a hit; below threshold does not");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::audioOnly;
            auto eng = freshEngine (slotOf (file));
            juce::AudioBuffer<float> quiet (2, 2048);
            quiet.clear();
            quiet.setSample (0, 100, 0.005f); // -46 dB < -30 dB threshold
            juce::MidiBuffer m;
            eng->process (quiet, m, p);
            expectEquals (quiet.getMagnitude (0, 2048), 0.0f);
            auto eng2 = freshEngine (slotOf (file));
            juce::AudioBuffer<float> loud (2, 2048);
            loud.clear();
            for (int i = 100; i < 300; ++i) loud.setSample (0, i, 0.9f);
            eng2->process (loud, m, p);
            expect (loud.getMagnitude (0, 2048) > 0.05f, "loud transient should produce sample output");
        }

        beginTest ("engine: sustained loud signal triggers once, not on every retrigger interval");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::audioOnly;
            p.holdoffMs = 20.0f;
            auto eng = freshEngine (slotOf (file));
            for (int i = 0; i < 100; ++i) run (*eng, p, 512, {}, 0.5f); // ~1.1 s of constant signal
            expectEquals ((int) eng->telemetry().hits[0].load(), 1);
        }

        beginTest ("engine: low cut keeps bass bleed from triggering; without it the same signal does");
        {
            auto burst = [&] (float lowcut)
            {
                TriggerEngine::Params p;
                p.source = TriggerEngine::audioOnly;
                p.lowcutHz = lowcut;
                auto eng = freshEngine (slotOf (file));
                juce::AudioBuffer<float> io (2, 4096);
                for (int i = 0; i < 4096; ++i) { const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 50.0f * i / 44100.0f); io.setSample (0, i, v); io.setSample (1, i, v); }
                juce::MidiBuffer m;
                eng->process (io, m, p);
                return (int) eng->telemetry().hits[0].load();
            };
            expectEquals (burst (0.0f), 1);
            expectEquals (burst (600.0f), 0);
        }

        beginTest ("engine: input volume scales the dry path; gate closes quiet signal");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::audioOnly;
            p.mix = 0.0f;
            p.inGainDb = 6.0206f;
            auto eng = freshEngine (nullptr);
            expectWithinAbsoluteError (run (*eng, p, 1024, {}, 0.1f).getSample (0, 900), 0.2f, 1e-4f); // dry is delayed by the look-ahead
            p.inGainDb = 0.0f;
            p.gateOn = true;
            p.thresholdDb = -20.0f;
            p.gateReleaseMs = 10.0f;
            auto eng2 = freshEngine (nullptr);
            for (int i = 0; i < 4; ++i) run (*eng2, p, 512, {}, 0.01f); // -40 dB, below threshold
            expect (run (*eng2, p, 256, {}, 0.01f).getMagnitude (0, 256) < 0.0005f, "gate should have closed");
            auto loud = run (*eng2, p, 1024, {}, 0.5f);
            expectWithinAbsoluteError (loud.getSample (0, 1000), 0.5f, 0.01f); // opens again
        }

        beginTest ("engine: two slots sum; mute and solo; volume, pan, phase and tune per slot");
        {
            auto eng = freshEngine (slotOf (file), 0);
            eng->setSlotContent (1, slotOf (file, 1)); // slot 1 plays articulation 1 (0.6 at any velocity)
            TriggerEngine::Params p;
            p.source = TriggerEngine::midiOnly;
            auto level = [&] (const TriggerEngine::Params& q, int ch = 0)
            {
                eng->reset();
                const int L = eng->latencySamples();
                return run (*eng, q, L + 256, note (100, 0)).getSample (ch, L + 100);
            };
            expectWithinAbsoluteError (level (p), 1.2f, 5e-3f);               // 0.6 + 0.6
            p.slot[1].mute = true;
            expectWithinAbsoluteError (level (p), 0.6f, 5e-3f);
            p.slot[1].mute = false; p.slot[0].solo = true;
            expectWithinAbsoluteError (level (p), 0.6f, 5e-3f);               // solo wins over the unmuted slot 1
            p.slot[0].solo = false;
            p.slot[0].gainDb = -6.0206f;
            expectWithinAbsoluteError (level (p), 0.6f * 0.5f + 0.6f, 5e-3f);
            p.slot[0].gainDb = 0.0f; p.slot[1].phase = true;
            expectWithinAbsoluteError (level (p), 0.0f, 5e-3f);               // 0.6 - 0.6
            p.slot[1].phase = false; p.slot[1].pan = -1.0f;
            expectWithinAbsoluteError (level (p, 0), 0.6f + 0.6f * 1.4142f, 5e-3f); // hard left: +3 dB on L
            expectWithinAbsoluteError (level (p, 1), 0.6f, 5e-3f);                  // R only has slot 0
        }

        beginTest ("engine: tune shifts playback speed");
        {
            otci::Instrument ramp;
            ramp.name = "Ramp";
            juce::AudioBuffer<float> b (1, 4000);
            for (int i = 0; i < 4000; ++i) b.setSample (0, i, (float) i / 8000.0f);
            otci::Wave w; w.flac = encodeFlac (b, 44100.0, 24); w.frames = 4000; w.channels = 1; w.rate = 44100.0;
            ramp.waves = { w };
            otci::Articulation a; a.name = "R"; a.layers = { { 0, 1.0f, 1.0f, { { 0 } } } };
            ramp.articulations = { a };
            auto f = dir.getChildFile ("ramp.otci");
            expect (otci::write (f, ramp, err), err);
            TriggerEngine::Params p;
            p.source = TriggerEngine::midiOnly;
            auto at = [&] (float semis)
            {
                p.slot[0].tuneSemis = semis;
                auto eng = freshEngine (slotOf (f));
                const int L = eng->latencySamples();
                return run (*eng, p, L + 300, note (100, 0)).getSample (0, L + 100);
            };
            expectWithinAbsoluteError (at (0.0f), 100.0f / 8000.0f, 2e-4f);
            expectWithinAbsoluteError (at (12.0f), 200.0f / 8000.0f, 3e-4f); // one octave up = twice as fast
        }

        beginTest ("engine: an audio hit's sample starts exactly with the transient it came from (look-ahead alignment)");
        {
            otci::Instrument click;
            click.name = "Click";
            juce::AudioBuffer<float> b (1, 4000);
            b.clear();
            for (int i = 0; i < 4000; ++i) b.setSample (0, i, i < 40 ? 0.9f : 0.0f);       // a 40-sample burst, starts at sample 0
            otci::Wave w; w.flac = encodeFlac (b, 44100.0, 24); w.frames = 4000; w.channels = 1; w.rate = 44100.0;
            click.waves = { w };
            otci::Articulation a; a.name = "C"; a.layers = { { 0, 1.0f, 1.0f, { { 0 } } } };
            click.articulations = { a };
            auto f = dir.getChildFile ("click.otci");
            expect (otci::write (f, click, err), err);
            TriggerEngine::Params p;
            p.source = TriggerEngine::audioOnly;
            p.mix = 0.5f;                                     // sample and (delayed) dry together
            auto eng = freshEngine (slotOf (f));
            const int L = eng->latencySamples();
            const int onset = 300;
            juce::AudioBuffer<float> io (2, onset + L + 600);
            io.clear();
            for (int i = onset; i < onset + 200; ++i) { io.setSample (0, i, 0.8f); io.setSample (1, i, 0.8f); }
            juce::MidiBuffer m;
            eng->process (io, m, p);
            // the dry onset appears at onset + L; the sample must begin on that same sample, neither earlier nor later
            expectWithinAbsoluteError (io.getSample (0, onset + L - 1), 0.0f, 1e-6f);
            expectWithinAbsoluteError (io.getSample (0, onset + L), 0.5f * 0.8f + 0.5f * 0.9f, 0.02f); // half dry (0.8) + half sample (0.9) on the very same sample
            expect (std::abs (io.getSample (0, onset + L - 1)) < 1e-6f && std::abs (io.getSample (0, onset + L)) > 0.3f, "both start together");
        }

        beginTest ("engine: scope records the waveform and trigger markers; auditionSlot ignores Mix");
        {
            TriggerEngine::Params p;
            p.source = TriggerEngine::audioOnly;
            auto eng = freshEngine (slotOf (file));
            juce::AudioBuffer<float> io (2, 2048);
            io.clear();
            for (int i = 200; i < 600; ++i) { io.setSample (0, i, 0.8f); io.setSample (1, i, 0.8f); }
            juce::MidiBuffer m;
            eng->process (io, m, p);
            auto& tel = eng->telemetry();
            expect (tel.head.load() > 10, "columns written");
            float hi = 0.0f; int trig = 0;
            for (int c = 0; c < tel.head.load(); ++c) { hi = std::max (hi, tel.cols[(size_t) c].hi.load()); trig = std::max (trig, tel.cols[(size_t) c].trigVel.load()); }
            expect (hi > 0.7f, "waveform captured");
            expect (trig > 0, "trigger marker recorded");
            expect (tel.inPeak[0].load() > 0.7f && tel.outPeak[0].load() > 0.0f, "meters");

            auto eng2 = freshEngine (slotOf (file));
            p.mix = 0.0f;
            eng2->auditionSlot (0, 100);
            auto a = run (*eng2, p, 300);
            expect (a.getSample (0, 100) > 0.5f, "audition is audible with Mix at 0");
        }

        if (auto* fx = std::getenv ("OT_FIXTURE")) // file produced by the Python prototype
        {
            beginTest ("reads a file written by the Python tool");
            auto py = loadInstrument (juce::File (fx), err);
            expect (py != nullptr, err);
            if (py) { expectEquals (py->meta.name, juce::String ("Test Kick")); expectEquals ((int) py->audio.size(), 6); }
        }
        dir.deleteRecursively();
    }
};

std::shared_ptr<DraftWave> constWave (float v, int frames = 3000, int ch = 1, int bits = 24)
{
    auto w = std::make_shared<DraftWave>();
    w->label = "c" + juce::String (v);
    w->audio = std::make_shared<juce::AudioBuffer<float>> (constant (v, frames, ch));
    w->rate = 44100.0;
    w->bits = bits;
    return w;
}

struct DraftTests : juce::UnitTest
{
    DraftTests() : juce::UnitTest ("Draft", "OpenTrigger") {}

    void runTest() override
    {
        juce::String err;
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_draft_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();

        beginTest ("empty draft refuses to save with a readable message");
        auto d = Draft::createEmpty();
        otci::Instrument out;
        expect (! d.toInstrument (out, err));
        expect (err.contains ("no samples"), err);
        d.name = "  ";
        expect (! d.toInstrument (out, err) && err.contains ("name"), err);

        beginTest ("velocity edits are clamped between neighbours; first layer stays 0");
        d = Draft::createEmpty(); // 0, 45, 90
        expectEquals (d.setVelo (0, 1, 200), 89);
        expectEquals (d.setVelo (0, 1, -5), 1);
        expectEquals (d.setVelo (0, 0, 50), 0);
        expectEquals (d.setVelo (0, 2, 500), 127);
        expectEquals (d.veloTop (0, 1), d.articulations[0].layers[2].velo - 1);

        beginTest ("grid edits: add/remove rows and columns keep at least one");
        d = Draft::createEmpty();
        d.addColumn (0); d.addLayer (0);
        expectEquals ((int) d.articulations[0].layers.size(), 4);
        expectEquals (d.articulations[0].columns(), 3);
        expect (d.articulations[0].layers[3].velo > d.articulations[0].layers[2].velo);
        for (int i = 0; i < 10; ++i) { d.removeColumn (0, 0); d.removeLayer (0, 0); }
        expectEquals (d.articulations[0].columns(), 1);
        expectEquals ((int) d.articulations[0].layers.size(), 1);
        expectEquals (d.articulations[0].layers[0].velo, 0);

        beginTest ("draft -> file -> draft keeps grid, shares repeated waves, drops empty cells");
        d = Draft::createEmpty();
        auto soft = constWave (0.1f), mid = constWave (0.3f, 3000, 2, 16), loud = constWave (0.6f);
        auto& L = d.articulations[0].layers;
        L[0].cells[0].waves = { soft };
        L[1].cells[0].waves = { mid };
        L[1].cells[1].waves = { constWave (0.35f) };
        L[2].cells[1].waves = { loud };                // column 0 left empty on purpose
        d.articulations.push_back (d.articulations[0]); // second articulation reuses the same waves
        d.articulations[1].name = "Double";
        d.name = "Edited Kit"; d.library = "Mine";
        expect (d.toInstrument (out, err), err);
        expectEquals ((int) out.waves.size(), 4);       // shared waves stored once
        expectEquals ((int) out.articulations.size(), 2);
        expectEquals ((int) out.articulations[0].layers[2].roundRobins.size(), 1); // empty cell dropped
        auto file = dir.getChildFile ("e.otci");
        expect (otci::write (file, out, err), err);
        auto li = loadInstrument (file, err);
        expect (li != nullptr, err);
        if (li)
        {
            auto back = Draft::fromLoaded (*li);
            expectEquals (back.name, juce::String ("Edited Kit"));
            expectEquals ((int) back.articulations.size(), 2);
            expectEquals ((int) back.articulations[0].layers.size(), 3);
            expectEquals (back.articulations[0].columns(), 2);
            expect (back.articulations[0].layers[1].cells[0].waves[0] == back.articulations[1].layers[1].cells[0].waves[0], "shared wave identity survives");
            expectWithinAbsoluteError (back.articulations[0].layers[1].cells[0].waves[0]->audio->getSample (1, 100), 0.3f, 1e-3f);
            expectEquals (li->meta.waves[1].channels, 2);
        }

        beginTest ("loadWave: reads wav, rejects non-audio and >2 channels");
        {
            auto wav = dir.getChildFile ("a.wav");
            {
                juce::WavAudioFormat f;
                std::unique_ptr<juce::AudioFormatWriter> w (f.createWriterFor (new juce::FileOutputStream (wav), 48000.0, 2, 16, {}, 0));
                auto b = constant (0.5f, 1000, 2);
                w->writeFromAudioSampleBuffer (b, 0, 1000);
            }
            auto ok = Draft::loadWave (wav, err);
            expect (ok != nullptr, err);
            if (ok) { expectEquals (ok->bits, 16); expectEquals (ok->rate, 48000.0); expectEquals (ok->audio->getNumSamples(), 1000); expect (! ok->peaks.empty()); }
            auto txt = dir.getChildFile ("n.wav");
            txt.replaceWithText ("not audio");
            expect (Draft::loadWave (txt, err) == nullptr && err.contains ("unsupported"), err);
            auto six = dir.getChildFile ("six.wav");
            {
                juce::WavAudioFormat f;
                std::unique_ptr<juce::AudioFormatWriter> w (f.createWriterFor (new juce::FileOutputStream (six), 44100.0, 6, 16, {}, 0));
                auto b = constant (0.1f, 100, 6);
                w->writeFromAudioSampleBuffer (b, 0, 100);
            }
            expect (Draft::loadWave (six, err) == nullptr && err.contains ("mono or stereo"), err);
        }

        beginTest ("cover: large image is downscaled to 512px PNG");
        {
            juce::Image big (juce::Image::ARGB, 2000, 1000, true);
            d = Draft::createEmpty();
            expect (d.setCoverFromImage (big, err), err);
            auto img = juce::ImageFileFormat::loadFrom (d.cover.getData(), d.cover.getSize());
            expect (img.isValid() && img.getWidth() == 512 && img.getHeight() == 256);
            expect (! d.setCoverFromImage ({}, err));
        }

        beginTest ("audition plays even with no instrument and ignores mix");
        {
            TriggerEngine eng;
            eng.prepare (44100.0, 512);
            auto buf = std::make_shared<juce::AudioBuffer<float>> (constant (0.5f, 2000));
            eng.audition (buf, 44100.0);
            TriggerEngine::Params p;
            p.mix = 0.0f;
            juce::AudioBuffer<float> io (2, 300);
            io.clear();
            juce::MidiBuffer m;
            eng.process (io, m, p);
            expectWithinAbsoluteError (io.getSample (0, 100), 0.4f, 1e-4f); // 0.5 * 0.8 preview gain
        }
        dir.deleteRecursively();
    }
};
// A drum track in miniature: 60 Hz kick-like hits of different strength, a noise floor, and quiet bursts from "other drums".
std::vector<float> makeTrack (double sr, int seconds, float levelDb, bool withBleed, std::vector<int>* hitPos = nullptr)
{
    const int n = (int) (sr * seconds);
    std::vector<float> x ((size_t) n);
    juce::Random rnd (11);
    for (auto& v : x) v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.001f;
    const float gain = juce::Decibels::decibelsToGain (levelDb);
    const double gaps[] = { 0.62, 0.9, 1.3, 0.7, 1.1, 0.8, 0.65, 1.2, 0.95, 0.75, 1.0, 0.68, 1.15, 0.85, 0.7, 1.05, 0.9 };
    const float ampsDb[] = { -6, -14, -9, -20, -8, -17, -6, -12, -19, -7, -15, -10, -20, -6, -13, -9, -18 };
    double t = 0.8;
    for (int k = 0; k < 17 && t < seconds - 1.0; ++k)
    {
        const int start = (int) (t * sr);
        if (hitPos) hitPos->push_back (start);
        const float a = juce::Decibels::decibelsToGain (ampsDb[k]) * gain;
        for (int i = 0; i < (int) (0.5 * sr) && start + i < n; ++i)
        {
            const float tt = (float) i / (float) sr;
            const float env = std::min (1.0f, tt / 0.002f) * std::exp (-tt / 0.12f);
            x[(size_t) (start + i)] += a * env * std::sin (2.0f * juce::MathConstants<float>::pi * 60.0f * tt);
        }
        t += gaps[k];
    }
    if (withBleed)
        for (double bt : { 1.2, 3.3, 5.9, 8.3, 10.6, 12.2 }) // 3 kHz bursts well away from any hit
        {
            const int start = (int) (bt * sr) + (int) (0.3 * sr);
            for (int i = 0; i < (int) (0.03 * sr) && start + i < n; ++i)
            {
                const float tt = (float) i / (float) sr;
                x[(size_t) (start + i)] += juce::Decibels::decibelsToGain (-40.0f) * gain * std::exp (-tt / 0.01f) * std::sin (2.0f * juce::MathConstants<float>::pi * 3000.0f * tt);
            }
        }
    return x;
}

struct SmartTests : juce::UnitTest
{
    SmartTests() : juce::UnitTest ("Smart Trigger", "OpenTrigger") {}

    void runTest() override
    {
        const double sr = 44100.0;
        beginTest ("finds the hits, ignores quiet bleed, and puts the threshold between them");
        {
            auto x = makeTrack (sr, 16, 0.0f, true);
            auto r = analyzeLearn (x.data(), (int) x.size(), sr);
            expect (r.ok, r.message);
            expect (r.hits >= 14 && r.hits <= 17, "hits: " + juce::String (r.hits));
            expect (r.bleed >= 4, "bleed: " + juce::String (r.bleed));
            expect (r.thresholdDb < -23.0f && r.thresholdDb > -40.0f, "threshold " + juce::String (r.thresholdDb));
            expectEquals (r.retriggerMs, 40.0f);                   // hits are >= 0.6 s apart: capped so fast rolls elsewhere still work
            expect (r.sensitivity >= 30.0f && r.sensitivity <= 70.0f && r.detail >= 20.0f && r.detail <= 90.0f, "no extreme proposals");
            expect (r.highcutHz <= 1000.0f, "a 60 Hz kick does not need the highs: " + juce::String (r.highcutHz));
            expect (r.lowcutHz <= 60.0f, "low cut must not eat the kick: " + juce::String (r.lowcutHz));
            expect (r.suppress >= 0.0f && r.suppress <= 60.0f);
            expectEquals (r.inputGainDb, 0.0f);
            expect (r.gateOn, "bleed is present, so the gate is worth turning on");
            expect (r.gateAttackMs >= 0.2f && r.gateAttackMs <= 5.0f, "gate attack " + juce::String (r.gateAttackMs));
            expect (r.gateHoldMs >= 10.0f && r.gateHoldMs <= 400.0f && r.gateReleaseMs >= 60.0f && r.gateReleaseMs <= 1500.0f, "gate times");
            expect (r.medianDecayMs > 100.0f && r.medianDecayMs < 700.0f, "kick rings ~0.4 s to -30 dB: " + juce::String (r.medianDecayMs));
            expect (r.gateReleaseMs > r.gateHoldMs, "release outlasts hold");
        }
        beginTest ("analysis time: seconds of audio in, how long the maths take (it must not freeze the UI)");
        {
            for (int secs : { 10, 30 })
            {
                auto x = makeTrack (sr, secs, 0.0f, true);
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                auto r = analyzeLearn (x.data(), (int) x.size(), sr);
                const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
                logMessage ("analysis of " + juce::String (secs) + " s of audio took " + juce::String (ms, 1) + " ms");
                expect (r.ok && ms < 500.0, "analysis should stay well under half a second: " + juce::String (ms));
            }
        }
        beginTest ("without bleed every event is a hit and the threshold sits under the quietest one");
        {
            auto x = makeTrack (sr, 16, 0.0f, false);
            auto r = analyzeLearn (x.data(), (int) x.size(), sr);
            expect (r.ok, r.message);
            expectEquals (r.bleed, 0);
            expect (r.thresholdDb < -20.0f, "threshold " + juce::String (r.thresholdDb));
            expect (! r.gateOn, "a clean track with a quiet floor does not need a gate");
        }
        beginTest ("a very quiet track gets input volume (up to +14 dB) and the threshold follows it");
        {
            auto quiet = makeTrack (sr, 16, -30.0f, false);
            auto loud = makeTrack (sr, 16, 0.0f, false);
            auto rq = analyzeLearn (quiet.data(), (int) quiet.size(), sr), rl = analyzeLearn (loud.data(), (int) loud.size(), sr);
            expect (rq.ok && rl.ok);
            expect (rq.inputGainDb > 5.0f && rq.inputGainDb <= 14.0f, "gain " + juce::String (rq.inputGainDb));
            expect (rq.thresholdDb > rl.thresholdDb - 31.0f && rq.thresholdDb < rl.thresholdDb - 10.0f, "threshold moves with the gain");
        }
        beginTest ("too short, silent, or hit-less audio gives a clear message instead of numbers");
        {
            std::vector<float> few ((size_t) (sr * 1.0), 0.1f);
            auto a = analyzeLearn (few.data(), (int) few.size(), sr);
            expect (! a.ok && a.message.containsIgnoreCase ("short"), a.message);
            std::vector<float> silence ((size_t) (sr * 6.0), 0.0f);
            auto b = analyzeLearn (silence.data(), (int) silence.size(), sr);
            expect (! b.ok && b.message.containsIgnoreCase ("no signal"), b.message);
            juce::Random rnd (5);
            std::vector<float> noise ((size_t) (sr * 6.0));
            for (auto& v : noise) v = (rnd.nextFloat() - 0.5f) * 0.02f;
            auto c = analyzeLearn (noise.data(), (int) noise.size(), sr);
            expect (! c.ok && c.message.containsIgnoreCase ("hits"), c.message);
        }
    }
};
struct ProtectTests : juce::UnitTest
{
    ProtectTests() : juce::UnitTest ("Protected audio and packs", "OpenTrigger") {}

    static std::shared_ptr<SlotContent> content (const juce::File& f, juce::String& err, int art = 0)
    {
        auto d = readInstrumentData (f, err);
        return d ? buildSlotContent (d, art, err) : nullptr;
    }

    void runTest() override
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_prot_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        juce::String err;
        beginTest ("setup: a plain instrument to protect");
        auto plain = dir.getChildFile ("plain.otci");
        expect (otci::write (plain, makeInstrument(), err), err);

        beginTest ("sealed: audio is encrypted on disk, names / layers / cover stay readable, playback is identical");
        {
            auto sealed = dir.getChildFile ("sealed.otci");
            otci::Protection p; p.mode = otci::Encryption::sealed;
            auto inst = otci::read (plain, true, err);
            expect (inst.has_value() && otci::write (sealed, *inst, err, &p), err);
            auto back = otci::read (sealed, true, err);
            expect (back.has_value(), err);
            if (back)
            {
                expect (back->enc.mode == otci::Encryption::sealed);
                expectEquals (back->name, juce::String ("Test"));
                expectEquals ((int) back->articulations.size(), 2);
                expect (back->image.getSize() == 8, "cover stays readable");
                juce::FlacAudioFormat flac;
                int openable = 0;
                for (auto& w : back->waves)
                {
                    auto* mem = new juce::MemoryInputStream (w.flac, false);
                    std::unique_ptr<juce::AudioFormatReader> r (flac.createReaderFor (mem, true));
                    openable += r != nullptr;
                    expect (w.flac.getSize() == inst->waves[0].flac.getSize() + otcrypt::kMac || w.flac.getSize() > otcrypt::kMac, "stored bytes carry the auth tag");
                }
                expectEquals (openable, 0); // no normal audio tool can open what is stored
                for (size_t i = 0; i < back->waves.size(); ++i) expect (back->waves[i].flac != inst->waves[i].flac, "bytes differ from the plain FLAC");
            }
            auto a = content (plain, err), b = content (sealed, err);
            expect (a && b, err);
            if (a && b)
                for (size_t i = 0; i < a->audio.size(); ++i)
                {
                    if (a->audio[i].getNumSamples() == 0) continue;
                    expect (a->audio[i].getNumSamples() == b->audio[i].getNumSamples());
                    expectWithinAbsoluteError (a->audio[i].getSample (0, 100), b->audio[i].getSample (0, 100), 0.0f);
                }
            auto again = dir.getChildFile ("sealed2.otci");
            otci::write (again, *inst, err, &p);
            juce::MemoryBlock m1, m2; sealed.loadFileAsData (m1); again.loadFileAsData (m2);
            expect (m1 != m2, "every protected file has its own random key and nonce");
        }

        beginTest ("the editor refuses protected instruments (they cannot be opened to extract audio)");
        {
            auto ed = loadInstrument (dir.getChildFile ("sealed.otci"), err);
            expect (ed == nullptr && err.containsIgnoreCase ("protected"), err);
        }

        beginTest ("tampering with the audio is detected, not played");
        {
            auto bad = dir.getChildFile ("bad.otci");
            dir.getChildFile ("sealed.otci").copyFileTo (bad);
            juce::MemoryBlock mb; bad.loadFileAsData (mb);
            ((unsigned char*) mb.getData())[0x48 + 40] ^= 0x01;              // one bit inside the first wave
            bad.replaceWithData (mb.getData(), mb.getSize());
            juce::String e2;
            expect (content (bad, e2) == nullptr && e2.containsIgnoreCase ("altered"), e2);
            expect (! otpack::verifyAudio (bad, e2));
        }

        beginTest ("pass phrase: locked until the right phrase is given; wrong phrase is refused; key is forgotten on demand");
        {
            auto locked = dir.getChildFile ("locked.otci");
            otci::Protection p; p.mode = otci::Encryption::password; p.password = "correct horse";
            auto inst = otci::read (plain, true, err);
            expect (otci::write (locked, *inst, err, &p), err);
            otcrypt::Vault::forgetAll();
            juce::String e2;
            expect (content (locked, e2) == nullptr && e2.startsWith ("password needed"), e2);
            auto meta = otci::read (locked, false, e2);
            expect (meta && meta->enc.mode == otci::Encryption::password);
            expect (! otci::unlockWithPassword (meta->enc, "wrong", e2) && e2.containsIgnoreCase ("wrong"), e2);
            expect (content (locked, e2) == nullptr);
            expect (otci::unlockWithPassword (meta->enc, "correct horse", e2), e2);
            expect (content (locked, e2) != nullptr, e2);
            otcrypt::Vault::forgetAll();
            expect (content (locked, e2) == nullptr, "forgotten again");
        }

        beginTest ("pack: create, inspect, import; a damaged or altered pack is reported and not imported");
        {
            auto second = dir.getChildFile ("second.otci");
            auto i2 = makeInstrument(); i2.name = "Second"; i2.library = "Lib";
            expect (otci::write (second, i2, err), err);
            otci::Protection p; p.mode = otci::Encryption::sealed;
            auto packFile = dir.getChildFile ("Test.otpack");
            expect (otpack::create (packFile, "Test Pack", { plain, second }, p, err), err);
            auto info = otpack::inspect (packFile);
            expect (info.ok && info.entries.size() == 2, info.message);
            for (auto& e : info.entries) expect (e.protectedAudio, e.name + " must be protected inside the pack");
            auto lib = dir.getChildFile ("lib");
            auto rep = otpack::import (packFile, lib);
            expect (rep.ok && rep.imported == 2 && rep.failed == 0, rep.message);
            Library l; l.scan (lib);
            expectEquals ((int) l.entries().size(), 2);
            expect (lib.getChildFile ("Test Pack").isDirectory());
            juce::String e3;
            for (auto& f : rep.files) expect (otpack::verifyAudio (f, e3), e3);

            juce::MemoryBlock mb; packFile.loadFileAsData (mb);
            auto altered = dir.getChildFile ("Altered.otpack");
            auto* bytes = (unsigned char*) mb.getData();
            bytes[mb.getSize() / 4] ^= 0xff;                                  // somewhere in the stored instrument data
            altered.replaceWithData (mb.getData(), mb.getSize());
            auto lib2 = dir.getChildFile ("lib2");
            auto rep2 = otpack::import (altered, lib2);
            expect (! rep2.ok && rep2.failed >= 1, "altered pack: " + rep2.message);
            expect (lib2.findChildFiles (juce::File::findFiles, true, "*.otci").isEmpty() || rep2.imported < 2, "the altered instrument is not imported");
            auto junk = dir.getChildFile ("junk.otpack");
            junk.replaceWithText ("this is not a pack");
            expect (! otpack::inspect (junk).ok);
        }

        beginTest ("pack with a pass phrase: imports locked, verifies with the phrase, refuses a wrong one");
        {
            otci::Protection p; p.mode = otci::Encryption::password; p.password = "s3cret phrase";
            auto packFile = dir.getChildFile ("Locked.otpack");
            expect (otpack::create (packFile, "Locked Pack", { plain }, p, err), err);
            otcrypt::Vault::forgetAll();
            auto lib = dir.getChildFile ("lib3");
            auto rep = otpack::import (packFile, lib);
            expect (rep.ok && rep.locked == 1, rep.message);
            auto rep2 = otpack::import (packFile, dir.getChildFile ("lib4"), "wrong");
            expect (! rep2.ok && rep2.failed == 1, rep2.message);
            otcrypt::Vault::forgetAll();
            auto rep3 = otpack::import (packFile, dir.getChildFile ("lib5"), "s3cret phrase");
            expect (rep3.ok && rep3.locked == 0, rep3.message);
            otcrypt::Vault::forgetAll();
        }
        dir.deleteRecursively();
    }
};
static ProtectTests protectTests;

struct VeloSplitTests : juce::UnitTest
{
    VeloSplitTests() : juce::UnitTest ("VeloSplit stroke types", "OpenTrigger") {}
    void runTest() override
    {
        beginTest ("Hit -> other style: soft hits use Hit layers, hard hits the other style's, same waves, nothing written back");
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_split_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        auto base = makeInstrument();
        otci::Instrument k;
        k.name = "Snare"; k.library = "X";
        k.waves = base.waves;                       // at least 4 waves in the shared fixture is not assumed: reuse indices 0 and 1
        auto mk = [&] (const char* n, int wA, int wB)
        {
            otci::Articulation a; a.name = n;
            a.layers = { { 0, 0.3f, 0.6f, { { wA } } }, { 64, 0.6f, 1.0f, { { wB } } } };
            return a;
        };
        k.articulations = { mk ("Hit / Full Mix", 0, 0), mk ("Cross Stick / Full Mix", 1, 1), mk ("Flam / Full Mix", 1, 0), mk ("Hit / Top", 0, 0) };
        auto f = dir.getChildFile ("s.otci");
        juce::String err;
        expect (otci::write (f, k, err), err);
        auto d = readInstrumentData (f, err);
        expect (d != nullptr, err);
        if (d)
        {
            const auto& a = d->meta.articulations;
            expectEquals ((int) a.size(), 6);
            expectEquals (a[4].name, juce::String ("VeloSplit: Hit -> Cross Stick / Full Mix"));
            expectEquals (a[5].name, juce::String ("VeloSplit: Hit -> Flam / Full Mix"));
            float g = 0.0f;
            const auto* soft = otci::pickLayer (a[4], 50, g);
            expect (soft != nullptr && soft->roundRobins[0][0] == 0, "soft hit: Hit wave");
            const auto* hard = otci::pickLayer (a[4], kSplitVelocity + 4, g);
            expect (hard != nullptr && hard->roundRobins[0][0] == 1, "hard hit: Cross Stick wave");
            const auto* edge = otci::pickLayer (a[4], kSplitVelocity - 1, g);
            expect (edge != nullptr && edge->roundRobins[0][0] == 0, "just below the split: still Hit");
            const auto* atSplit = otci::pickLayer (a[4], kSplitVelocity, g);
            expect (atSplit != nullptr && atSplit->roundRobins[0][0] == 1 && atSplit->velo == kSplitVelocity, "at the split: switches");
            auto c = buildSlotContent (d, 4, err);
            expect (c != nullptr, err);
            if (c) { expect (c->audio[0].getNumSamples() > 0 && c->audio[1].getNumSamples() > 0, "waves of both styles decoded"); }
        }
        auto plain = otci::read (f, false, err);
        expectEquals ((int) plain->articulations.size(), 4); // the virtual ones live only in memory
        dir.deleteRecursively();
    }
};
static VeloSplitTests veloSplitTests;
static SmartTests smartTests;
static DraftTests draftTests;
static OtciTests otciTests;
} // namespace

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();
    int fails = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) fails += runner.getResult (i)->failures;
    return fails == 0 ? 0 : 1;
}
