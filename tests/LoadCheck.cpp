// Loads a real .otci through the production loader + engine, one articulation at a time, and checks it.
//   ot_loadcheck <file.otci> [reference.wav [articulation]]
//   reference = source WAV of the first decoded wave of that articulation (default 1), compared sample-exact
#include "Engine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    if (! ok) { ++failures; std::printf ("FAIL: %s\n", what.toRawUTF8()); }
}

int main (int argc, char** argv)
{
    if (argc < 2) return 2;
    juce::ScopedJuceInitialiser_GUI init;
    juce::String err;
    auto data = readInstrumentData (juce::File (argv[1]), err);
    check (data != nullptr, "read: " + err);
    if (! data) return 1;
    const int nArt = (int) data->meta.articulations.size();
    std::printf ("%s | library=%s | articulations=%d waves=%d cover=%s\n", data->meta.name.toRawUTF8(), data->meta.library.toRawUTF8(),
                 nArt, (int) data->meta.waves.size(), data->cover.isValid() ? "yes" : "NO");
    check (data->cover.isValid(), "cover image decodes");

    TriggerEngine eng;
    eng.prepare (44100.0, 512);
    TriggerEngine::Params p; p.source = TriggerEngine::midiOnly; p.mix = 1.0f;
    juce::AudioBuffer<float> io (2, 512);
    double totalMB = 0.0, maxMB = 0.0;
    const int refArt = argc > 3 ? juce::String (argv[3]).getIntValue() : 1;

    for (int a = 0; a < nArt; ++a)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto content = buildSlotContent (data, a, err);
        const double decodeMs = juce::Time::getMillisecondCounterHiRes() - t0;
        check (content != nullptr, "build articulation " + juce::String (a) + ": " + err);
        if (! content) continue;
        int decoded = 0; double bytes = 0.0; bool finiteAll = true;
        for (size_t i = 0; i < content->audio.size(); ++i)
        {
            auto& b = content->audio[i];
            if (b.getNumSamples() == 0) continue;
            ++decoded; bytes += (double) b.getNumSamples() * b.getNumChannels() * 4.0;
            float pk = 0.0f;
            for (int c = 0; c < b.getNumChannels(); ++c)
                for (int s = 0; s < b.getNumSamples(); ++s) { const float v = b.getSample (c, s); finiteAll = finiteAll && std::isfinite (v); pk = std::max (pk, std::abs (v)); }
            if (pk <= 0.0f) check (false, "articulation " + juce::String (a) + " wave " + juce::String ((int) i) + " is silent");
        }
        check (finiteAll, "finite samples");
        const double mb = bytes / 1048576.0;
        totalMB += mb; maxMB = std::max (maxMB, mb);

        if (argc > 2 && a == refArt)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (argv[2])));
            check (r != nullptr, "reference wav opens");
            size_t first = 0;
            while (first < content->audio.size() && content->audio[first].getNumSamples() == 0) ++first;
            if (r && first < content->audio.size())
            {
                juce::AudioBuffer<float> ref ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&ref, 0, (int) r->lengthInSamples, 0, true, true);
                auto& got = content->audio[first];
                const bool same = got.getNumChannels() == ref.getNumChannels() && got.getNumSamples() == ref.getNumSamples();
                check (same, "first decoded wave has the reference's shape");
                float maxDiff = 0.0f;
                if (same)
                    for (int c = 0; c < got.getNumChannels(); ++c)
                        for (int i = 0; i < got.getNumSamples(); ++i) maxDiff = std::max (maxDiff, std::abs (got.getSample (c, i) - ref.getSample (c, i)));
                std::printf ("  wave %d vs source WAV: max abs diff = %g\n", (int) first, maxDiff);
                check (maxDiff <= 1.0f / 8388608.0f, "bit-exact vs source");
            }
        }

        eng.setSlotContent (0, content);
        io.clear(); { juce::MidiBuffer m; eng.process (io, m, p); } // adopt
        float lastPeak = 0.0f; bool monotonic = true;
        for (int vel : { 10, 60, 127 })
        {
            eng.reset();
            juce::MidiBuffer m;
            m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) vel), 0);
            float peak = 0.0f; bool finite = true;
            for (int blk = 0; blk < 40; ++blk)
            {
                io.clear();
                eng.process (io, m, p);
                for (int c = 0; c < 2; ++c)
                    for (int s = 0; s < 512; ++s) { const float v = io.getSample (c, s); finite = finite && std::isfinite (v); peak = std::max (peak, std::abs (v)); }
                m.clear();
            }
            check (finite && peak > 0.0f, data->meta.articulations[(size_t) a].name + " vel " + juce::String (vel) + " audible");
            if (vel > 10 && peak + 0.05f < lastPeak) monotonic = false;
            lastPeak = peak;
        }
        std::printf ("  [%2d] %-38s layers=%d waves decoded=%3d (%.0f MB, %.0f ms)  peak@127=%.3f%s\n", a, data->meta.articulations[(size_t) a].name.toRawUTF8(),
                     (int) data->meta.articulations[(size_t) a].layers.size(), decoded, mb, decodeMs, lastPeak, monotonic ? "" : "  (non-monotonic)");
    }
    std::printf ("RAM for one slot (decoded float): largest articulation %.0f MB, average %.0f MB\n", maxMB, totalMB / std::max (nArt, 1));
    std::printf (failures == 0 ? "OK\n" : "FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
