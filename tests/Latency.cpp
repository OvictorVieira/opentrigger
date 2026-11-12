// Measures how long things take from the user's point of view, with a real instrument.
//   ot_latency <file.otci>
#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>

static void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
}

int main (int argc, char** argv)
{
    if (argc < 2) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    OpenTriggerProcessor proc;
    proc.setPlayConfigDetails (2, 2, 44100.0, 512);
    proc.prepareToPlay (44100.0, 512);

    // 1. load time (nobody is blocked: it all happens on worker threads, we just time until ready)
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    proc.loadSlot (0, juce::File (argv[1]));
    while (proc.slotInfo (0).status != OpenTriggerProcessor::SlotStatus::ready) pump (1);
    std::printf ("slot load (read + decode first mic):   %6.0f ms  (UI thread stays free meanwhile)\n", juce::Time::getMillisecondCounterHiRes() - t0);

    // 2. switching to another mic and hearing it: from the click to the first audible sample at the output
    juce::AudioBuffer<float> io (2, 512);
    juce::MidiBuffer m;
    const int nArt = (int) proc.slotInfo (0).data->meta.articulations.size();
    double worst = 0.0, sum = 0.0;
    int count = 0;
    for (int a = 1; a < std::min (nArt, 6); ++a)
    {
        proc.prepareToPlay (44100.0, 512);
        t0 = juce::Time::getMillisecondCounterHiRes();
        proc.setSlotArticulation (0, a, true);
        bool heard = false;
        while (! heard && juce::Time::getMillisecondCounterHiRes() - t0 < 5000)
        {
            pump (1);
            io.clear();
            proc.processBlock (io, m);
            heard = io.getMagnitude (0, 512) > 0.0f;
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("  click mic %d -> sound out:            %6.0f ms%s\n", a, ms, heard ? "" : "  (NOT HEARD)");
        worst = std::max (worst, ms); sum += ms; ++count;
        pump (150);
    }
    std::printf ("mic switch average %.0f ms, worst %.0f ms\n", sum / std::max (count, 1), worst);

    // 3. playing: delay between an audio transient and the first output sample (in samples and ms), and for MIDI
    auto measure = [&] (bool midi)
    {
        proc.prepareToPlay (44100.0, 512);
        proc.apvts.getParameter ("source")->setValueNotifyingHost (midi ? 0.5f : 0.0f);
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (0.0f); // dry removed? keep wet only below
        proc.apvts.getParameter ("mix")->setValueNotifyingHost (1.0f);
        int firstOut = -1;
        for (int blk = 0; blk < 8 && firstOut < 0; ++blk)
        {
            io.clear();
            juce::MidiBuffer mm;
            if (blk == 2)
            {
                if (midi) mm.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 100);
                else for (int i = 100; i < 400; ++i) { io.setSample (0, i, 0.8f); io.setSample (1, i, 0.8f); }
            }
            proc.processBlock (io, mm);
            if (blk >= 2)
                for (int i = 0; i < 512; ++i)
                    if (std::abs (io.getSample (0, i)) > (midi ? 1e-5f : 0.81f) && (midi || i >= 400 || blk > 2)) { firstOut = (blk - 2) * 512 + i; break; }
        }
        return firstOut;
    };
    const int midiOut = measure (true);
    std::printf ("MIDI note at sample 100 -> first output sample at %d (extra delay %d samples)\n", midiOut, midiOut - 100);
    return 0;
}
