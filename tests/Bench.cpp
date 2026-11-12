// How heavy are many instances? Loads N plug-ins with real instruments and measures RAM and audio-thread CPU.
//   ot_bench <library dir> <instances> [seconds]
#include "PluginProcessor.h"
#include "TestSignals.h"
#include <chrono>
#include <mach/mach.h>

static double footprintMB()
{
    task_vm_info_data_t info; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &n);
    return (double) info.phys_footprint / 1048576.0;
}
static void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end) juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
}

int main (int argc, char** argv)
{
    if (argc < 3) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    LibraryCache::setSynchronous (std::getenv ("BENCH_ASYNC") == nullptr);
    const int N = std::atoi (argv[2]);
    const int secs = argc > 3 ? std::atoi (argv[3]) : 10;
    if (std::getenv ("BENCH_STARTUP"))
    {
        // what the user feels when adding the plug-in to a track and opening it
        const auto ms = [] { return juce::Time::getMillisecondCounterHiRes(); };
        double t = ms();
        auto p = std::make_unique<OpenTriggerProcessor>();
        const double ctor = ms() - t;
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        t = ms(); p->prepareToPlay (48000.0, 512); const double prep = ms() - t;
        t = ms(); std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor()); const double edTime = ms() - t;
        t = ms(); ed->setSize (1152, 648); const double show = ms() - t;
        std::printf ("COLD  create processor: %.0f ms | prepareToPlay: %.0f ms | open window: %.0f ms (+%.0f ms sizing)\n", ctor, prep, edTime, show);
        pump (1500); // background scan finishes
        ed.reset();
        t = ms(); ed.reset (p->createEditor()); const double edTime2 = ms() - t;
        t = ms(); ed->setSize (1152, 648); const double show2 = ms() - t;
        std::printf ("WARM  open window: %.0f ms (+%.0f ms sizing)\n", edTime2, show2);
        ed.reset();
        // restoring a saved project: slot state -> loaded and playable
        juce::MemoryBlock state;
        auto lib2 = Library(); lib2.scan (juce::File (argv[1]));
        p->loadSlot (0, lib2.entries().back().file);
        const double l0 = ms();
        while (p->slotInfo (0).status != OpenTriggerProcessor::SlotStatus::ready && ms() - l0 < 60000) pump (5);
        std::printf ("load saved instrument (%s): %.0f ms, UI thread free meanwhile\n", lib2.entries().back().name.toRawUTF8(), ms() - l0);
        return 0;
    }
    Library lib; lib.scan (juce::File (argv[1]));
    if (lib.entries().empty()) { std::printf ("no instruments\n"); return 1; }
    const double base = footprintMB();
    std::vector<std::unique_ptr<OpenTriggerProcessor>> procs;
    for (int i = 0; i < N; ++i)
    {
        auto p = std::make_unique<OpenTriggerProcessor>();
        p->setPlayConfigDetails (2, 2, 48000.0, 512);
        p->prepareToPlay (48000.0, 512);
        p->loadSlot (0, lib.entries()[(size_t) i % lib.entries().size()].file);
        procs.push_back (std::move (p));
    }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (;;)
    {
        bool all = true;
        for (auto& p : procs) all = all && p->slotInfo (0).status == OpenTriggerProcessor::SlotStatus::ready;
        if (all || juce::Time::getMillisecondCounterHiRes() - t0 > 120000) break;
        pump (20);
    }
    const double loadSec = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    pump (3500); // let freed memory settle
    const double ram = footprintMB() - base;
    std::printf ("%d instances loaded in %.1f s, RAM %.0f MB total (%.0f MB each)\n", N, loadSec, ram, ram / std::max (N, 1));

    auto track = makeTrack (48000.0, secs);
    juce::MidiBuffer m;
    auto run = [&] (bool silent)
    {
        double busy = 0.0;
        const int blocks = (int) track.size() / 512;
        for (int b = 0; b < blocks; ++b)
            for (auto& p : procs)
            {
                juce::AudioBuffer<float> io (2, 512);
                for (int i = 0; i < 512; ++i) { const float v = silent ? 0.0f : track[(size_t) (b * 512 + i)]; io.setSample (0, i, v); io.setSample (1, i, v); }
                const auto a = std::chrono::steady_clock::now();
                p->processBlock (io, m);
                busy += std::chrono::duration<double> (std::chrono::steady_clock::now() - a).count();
            }
        const double audio = (double) blocks * 512 / 48000.0;
        return busy / audio * 100.0; // percent of ONE core for all instances together
    };
    const double idle = run (true), play = run (false);
    std::printf ("CPU, idle (silence in):      %.2f %% of one core for all %d  -> %.3f %% per instance\n", idle, N, idle / N);
    std::printf ("CPU, playing (drum hits):    %.2f %% of one core for all %d  -> %.3f %% per instance\n", play, N, play / N);
    return 0;
}
