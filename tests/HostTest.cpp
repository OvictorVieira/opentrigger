// Loads the BUILT plugin bundles (VST3 and AU) the way a host does and exercises them.
#include "Engine.h"
#include "Otci.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
juce::File g_lib, g_vst3, g_au;
juce::String g_only;

juce::File makeKit (const juce::File& dir)
{
    otci::Instrument inst;
    inst.name = "Host Kit"; inst.library = "T";
    for (float v : { 0.1f, 0.5f })
    {
        juce::AudioBuffer<float> b (1, 4000);
        for (int i = 0; i < 4000; ++i) b.setSample (0, i, v);
        otci::Wave w; w.flac = encodeFlac (b, 44100.0, 24); w.frames = 4000; w.channels = 1; w.rate = 44100.0;
        inst.waves.push_back (std::move (w));
    }
    otci::Articulation a; a.name = "Hit";
    a.layers = { { 0, 1.0f, 1.0f, { { 0 } } }, { 64, 1.0f, 1.0f, { { 1 } } } };
    inst.articulations.push_back (a);
    auto f = dir.getChildFile ("host.otci");
    juce::String err;
    const bool ok = otci::write (f, inst, err);
    if (! ok) std::fprintf (stderr, "makeKit failed: %s\n", err.toRawUTF8());
    return f;
}

// Instruments load on a worker thread and land via the message loop, like in a real host.
void pumpMs (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
}

struct HostTests : juce::UnitTest
{
    HostTests() : juce::UnitTest ("Host", "OpenTrigger") {}

    void check (juce::AudioPluginFormatManager& mgr, juce::AudioPluginFormat& fmt, const juce::File& bundle, const juce::String& label)
    {
        beginTest (label + ": bundle is discovered and describes an effect that accepts MIDI");
        juce::OwnedArray<juce::PluginDescription> found;
        fmt.findAllTypesForFile (found, bundle.getFullPathName());
        for (auto* d : found) logMessage (label + " found: " + d->name + " in=" + juce::String (d->numInputChannels) + " out=" + juce::String (d->numOutputChannels) + " midi=" + juce::String ((int) d->isInstrument) + " " + d->fileOrIdentifier);
        expectEquals ((int) found.size(), 1);
        if (found.isEmpty()) return;
        expectEquals (found[0]->name, juce::String ("OpenTrigger"));

        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 1, 64, 512, 4096 })
            {
                juce::String err;
                std::unique_ptr<juce::AudioPluginInstance> p (mgr.createPluginInstance (*found[0], sr, block, err).release());
                expect (p != nullptr, label + " create failed: " + err);
                if (! p) return;
                p->setPlayConfigDetails (2, 2, sr, block);
                p->prepareToPlay (sr, block);
                // no instrument: silence out for silent in, MIDI must not crash
                juce::AudioBuffer<float> io (2, block);
                io.clear();
                juce::MidiBuffer m;
                m.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
                p->processBlock (io, m);
                expectEquals (io.getMagnitude (0, block), 0.0f);
                p->releaseResources();
            }

        beginTest (label + ": state with an instrument restores it and MIDI plays the right layer");
        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> a (mgr.createPluginInstance (*found[0], 48000.0, 512, err).release());
        expect (a != nullptr, err);
        if (! a) return;
        // Build a state blob by letting the plugin's own state format carry our instrument path.
        juce::MemoryBlock st;
        a->getStateInformation (st);
        auto xml = juce::AudioProcessor::getXmlFromBinary (st.getData(), (int) st.getSize());
        const bool craftable = xml != nullptr; // AU wraps state in a plist, so only VST3 can be fed a crafted state
        if (label == "VST3") expect (craftable);
        if (! craftable) { runRobustness (mgr, *found[0], label, *a); return; }
        // JUCE's VST3 host wraps the plugin's own state blob in <VST3PluginState><IComponent>base64</IComponent>.
        auto kit = makeKit (g_lib).getFullPathName();
        auto* comp = xml->getChildByName ("IComponent");
        expect (comp != nullptr);
        if (! comp) return;
        juce::MemoryBlock inner;
        inner.fromBase64Encoding (comp->getAllSubText());
        auto innerXml = juce::AudioProcessor::getXmlFromBinary (inner.getData(), (int) inner.getSize());
        expect (innerXml != nullptr);
        if (! innerXml) return;
        innerXml->setAttribute ("slot1", kit);
        juce::MemoryBlock innerNew;
        juce::AudioProcessor::copyXmlToBinary (*innerXml, innerNew);
        comp->deleteAllTextElements();
        comp->addTextElement (innerNew.toBase64Encoding());
        juce::MemoryBlock st2;
        juce::AudioProcessor::copyXmlToBinary (*xml, st2);
        logMessage (label + " buses: in=" + juce::String (a->getBusCount (true)) + " out=" + juce::String (a->getBusCount (false))
                    + " totalIn=" + juce::String (a->getTotalNumInputChannels()) + " totalOut=" + juce::String (a->getTotalNumOutputChannels())
                    + " inEnabled=" + juce::String ((int) (a->getBusCount (true) > 0 && a->getBus (true, 0)->isEnabled()))
                    + " outEnabled=" + juce::String ((int) (a->getBusCount (false) > 0 && a->getBus (false, 0)->isEnabled()))
                    + " acceptsMidi=" + juce::String ((int) a->acceptsMidi()));
        a->setStateInformation (st2.getData(), (int) st2.getSize());
        {
            juce::MemoryBlock back;
            a->getStateInformation (back);
            auto bx = juce::AudioProcessor::getXmlFromBinary (back.getData(), (int) back.getSize());
            juce::MemoryBlock bi;
            if (bx != nullptr && bx->getChildByName ("IComponent") != nullptr) bi.fromBase64Encoding (bx->getChildByName ("IComponent")->getAllSubText());
            auto bix = juce::AudioProcessor::getXmlFromBinary (bi.getData(), (int) bi.getSize());
            expect (bix != nullptr && bix->getStringAttribute ("slot1") == kit, "slot 1 instrument path should be restored by setStateInformation");
        }
        a->setPlayConfigDetails (2, 2, 48000.0, 512);
        a->prepareToPlay (48000.0, 512);
        pumpMs (800);

        int hostLatency = 0;
        auto hit = [&] (int note, int vel, int sampleOffset)
        {
            // fresh instance per hit: hosts do not reset plugin voices on repeated prepareToPlay
            juce::String e2;
            std::unique_ptr<juce::AudioPluginInstance> inst (mgr.createPluginInstance (*found[0], 48000.0, 512, e2).release());
            inst->setStateInformation (st2.getData(), (int) st2.getSize());
            inst->setPlayConfigDetails (2, 2, 48000.0, 512);
            inst->prepareToPlay (48000.0, 512);
            pumpMs (600);
            const int lat = inst->getLatencySamples();
            hostLatency = lat;
            // like a real host: blocks never exceed the size given to prepareToPlay (512)
            const int blocks = (lat + 1024 + 511) / 512;
            juce::AudioBuffer<float> out (2, blocks * 512);
            for (int b = 0; b < blocks; ++b)
            {
                juce::AudioBuffer<float> io (2, 512);
                io.clear();
                juce::MidiBuffer mm;
                if (b == 0) mm.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) vel), sampleOffset);
                inst->processBlock (io, mm);
                for (int c = 0; c < 2; ++c) out.copyFrom (c, b * 512, io, c, 0, 512);
            }
            return out;
        };
        auto soft = hit (36, 30, 0), loud = hit (36, 100, 0);
        expect (hostLatency > 0, "the plug-in reports its look-ahead latency to the host");
        // 44.1k source on a 48k host: ~constant after resampling; everything is delayed by the reported latency
        expectWithinAbsoluteError (soft.getSample (0, hostLatency + 200), 0.1f, 5e-3f);
        expectWithinAbsoluteError (loud.getSample (1, hostLatency + 200), 0.5f, 5e-3f);
        auto late = hit (36, 100, 300);
        expectEquals (late.getSample (0, hostLatency + 299), 0.0f);
        expect (late.getSample (0, hostLatency + 305) > 0.4f);

        beginTest (label + ": parameters exist, are automatable and survive a state round trip");
        auto& params = a->getParameters();
        expectEquals ((int) params.size(), 73); // 16 global + 8 slots x 7 + the host's Bypass
        for (auto* pr : params) expect (pr->isAutomatable(), pr->getName (40));
        params[0]->setValueNotifyingHost (0.3f);
        juce::MemoryBlock s3;
        a->getStateInformation (s3);
        std::unique_ptr<juce::AudioPluginInstance> b (mgr.createPluginInstance (*found[0], 48000.0, 512, err).release());
        b->setStateInformation (s3.getData(), (int) s3.getSize());
        expectWithinAbsoluteError (b->getParameters()[0]->getValue(), 0.3f, 1e-3f);

        beginTest (label + ": editor can be created and destroyed repeatedly");
        for (int i = 0; i < 5; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (a->createEditor());
            expect (ed != nullptr);
            if (ed) { ed->setSize (1152, 648); pumpMs (30); }
        }

        beginTest (label + ": heavy random MIDI and audio does not crash or produce NaN");
        juce::Random r (7);
        for (int blk = 0; blk < 400; ++blk)
        {
            juce::AudioBuffer<float> io (2, 64 + r.nextInt (900));
            for (int c = 0; c < 2; ++c) for (int i = 0; i < io.getNumSamples(); ++i) io.setSample (c, i, r.nextFloat() * 2.0f - 1.0f);
            juce::MidiBuffer mm;
            for (int k = 0; k < r.nextInt (6); ++k)
                mm.addEvent (juce::MidiMessage::noteOn (1, 30 + r.nextInt (20), (juce::uint8) (1 + r.nextInt (127))), r.nextInt (io.getNumSamples()));
            a->prepareToPlay (48000.0, 1024);
            a->processBlock (io, mm);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < io.getNumSamples(); ++i) if (! std::isfinite (io.getSample (c, i))) { expect (false, "non finite output"); return; }
        }
    }

    void runRobustness (juce::AudioPluginFormatManager& mgr, const juce::PluginDescription& d, const juce::String& label, juce::AudioPluginInstance& a)
    {
        juce::ignoreUnused (mgr, d);
        beginTest (label + ": editor can be created and destroyed repeatedly (no instrument)");
        for (int i = 0; i < 5; ++i) { std::unique_ptr<juce::AudioProcessorEditor> ed (a.createEditor()); expect (ed != nullptr); }
        beginTest (label + ": random MIDI and audio, no instrument, no crash or NaN");
        juce::Random r (3);
        for (int blk = 0; blk < 200; ++blk)
        {
            juce::AudioBuffer<float> io (2, 32 + r.nextInt (900));
            for (int c = 0; c < 2; ++c) for (int i = 0; i < io.getNumSamples(); ++i) io.setSample (c, i, r.nextFloat() * 2.0f - 1.0f);
            juce::MidiBuffer mm;
            mm.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100), 0);
            a.prepareToPlay (48000.0, 1024);
            a.processBlock (io, mm);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < io.getNumSamples(); ++i) if (! std::isfinite (io.getSample (c, i))) { expect (false, "non finite"); return; }
        }
    }

    void runTest() override
    {
        juce::AudioPluginFormatManager fm;
        fm.addDefaultFormats();
        beginTest ("formats available");
        expect (g_vst3.exists(), "VST3 bundle missing: " + g_vst3.getFullPathName());
        for (auto* f : fm.getFormats())
        {
            if (f->getName() == "VST3" && g_vst3.exists() && g_only != "au") check (fm, *f, g_vst3, "VST3");
            if (f->getName() == "AudioUnit" && g_au.exists() && g_only != "vst3") check (fm, *f, g_au, "AU");
        }
    }
};
static HostTests hostTests;
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3) { std::fprintf (stderr, "usage: ot_hosttest <vst3> <component>\n"); return 2; }
    g_vst3 = juce::File (argv[1]);
    g_au = juce::File (argv[2]);
    if (argc > 3) g_only = argv[3];
    g_lib = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_host_" + juce::String (juce::Random::getSystemRandom().nextInt64()));
    g_lib.createDirectory();
    juce::UnitTestRunner runner;
    runner.runAllTests();
    int fails = 0;
    for (int i = 0; i < runner.getNumResults(); ++i) fails += runner.getResult (i)->failures;
    g_lib.deleteRecursively();
    return fails == 0 ? 0 : 1;
}
