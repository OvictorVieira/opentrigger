#include "Draft.h"
#include <algorithm>
#include <map>

namespace
{
std::vector<float> makePeaks (const juce::AudioBuffer<float>& b, int bins = 160)
{
    std::vector<float> p ((size_t) bins, 0.0f);
    const int n = b.getNumSamples();
    if (n == 0) return p;
    for (int i = 0; i < bins; ++i)
    {
        const int a = (int) ((juce::int64) n * i / bins), z = std::max (a + 1, (int) ((juce::int64) n * (i + 1) / bins));
        float m = 0;
        for (int c = 0; c < b.getNumChannels(); ++c)
            m = std::max (m, b.findMinMax (c, a, std::min (z, n) - a).getLength() * 0.5f);
        p[(size_t) i] = juce::jlimit (0.0f, 1.0f, m);
    }
    return p;
}

DraftLayer emptyLayer (int velo, int cols, bool first)
{
    DraftLayer l;
    l.velo = velo;
    l.minVol = first ? 0.1f : 0.5f;
    l.cells.resize ((size_t) cols);
    return l;
}
} // namespace

Draft Draft::createEmpty()
{
    Draft d;
    DraftArticulation a;
    a.layers = { emptyLayer (0, 2, true), emptyLayer (45, 2, false), emptyLayer (90, 2, false) };
    d.articulations.push_back (std::move (a));
    return d;
}

Draft Draft::fromLoaded (const LoadedInstrument& li)
{
    Draft d;
    d.name = li.meta.name;
    d.library = li.meta.library;
    d.cover = li.meta.image;

    std::vector<std::shared_ptr<DraftWave>> waves;
    for (size_t i = 0; i < li.audio.size(); ++i)
    {
        auto w = std::make_shared<DraftWave>();
        w->label = "wave " + juce::String ((int) i + 1);
        w->audio = std::make_shared<juce::AudioBuffer<float>> (li.audio[i]);
        w->rate = li.meta.waves[i].rate;
        w->peaks = makePeaks (*w->audio);
        waves.push_back (std::move (w));
    }
    for (auto& a : li.meta.articulations)
    {
        DraftArticulation da;
        da.name = a.name;
        std::copy (std::begin (a.relTime), std::end (a.relTime), std::begin (da.relTime));
        size_t cols = 1;
        for (auto& l : a.layers) cols = std::max (cols, l.roundRobins.size());
        for (auto& l : a.layers)
        {
            DraftLayer dl = emptyLayer (l.velo, (int) cols, false);
            dl.minVol = l.minVol;
            dl.maxVol = l.maxVol;
            for (size_t r = 0; r < l.roundRobins.size(); ++r)
                for (int idx : l.roundRobins[r])
                    if (idx >= 0 && idx < (int) waves.size()) dl.cells[r].waves.push_back (waves[(size_t) idx]);
            da.layers.push_back (std::move (dl));
        }
        if (da.layers.empty()) da.layers.push_back (emptyLayer (0, 1, true));
        d.articulations.push_back (std::move (da));
    }
    if (d.articulations.empty()) return createEmpty();
    return d;
}

std::shared_ptr<DraftWave> Draft::loadWave (const juce::File& f, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr) { error = f.getFileName() + ": unsupported audio file"; return nullptr; }
    if (r->numChannels < 1 || r->numChannels > 2) { error = f.getFileName() + ": only mono or stereo files are supported"; return nullptr; }
    if (r->lengthInSamples <= 0) { error = f.getFileName() + ": empty file"; return nullptr; }
    if (r->lengthInSamples > (juce::int64) r->sampleRate * 60) { error = f.getFileName() + ": longer than 60 s"; return nullptr; }

    auto w = std::make_shared<DraftWave>();
    w->label = f.getFileNameWithoutExtension();
    w->audio = std::make_shared<juce::AudioBuffer<float>> ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (w->audio.get(), 0, (int) r->lengthInSamples, 0, true, true);
    w->rate = r->sampleRate;
    w->bits = (r->bitsPerSample <= 16 && ! r->usesFloatingPointData) ? 16 : 24;
    w->peaks = makePeaks (*w->audio);
    return w;
}

void Draft::addLayer (int a)
{
    auto& art = articulations[(size_t) a];
    const int top = art.layers.empty() ? -45 : art.layers.back().velo;
    art.layers.push_back (emptyLayer (std::min (127, top + std::max (1, (127 - top) / 2)), std::max (1, art.columns()), art.layers.empty()));
}

void Draft::removeLayer (int a, int l)
{
    auto& art = articulations[(size_t) a];
    if (art.layers.size() <= 1 || l < 0 || l >= (int) art.layers.size()) return;
    art.layers.erase (art.layers.begin() + l);
    art.layers[0].velo = 0;
}

void Draft::addColumn (int a)
{
    for (auto& l : articulations[(size_t) a].layers) l.cells.emplace_back();
}

void Draft::removeColumn (int a, int c)
{
    auto& art = articulations[(size_t) a];
    if (art.columns() <= 1 || c < 0 || c >= art.columns()) return;
    for (auto& l : art.layers) l.cells.erase (l.cells.begin() + c);
}

int Draft::setVelo (int a, int l, int v)
{
    auto& ls = articulations[(size_t) a].layers;
    if (l <= 0 || l >= (int) ls.size()) return ls.empty() ? 0 : ls[(size_t) std::max (l, 0)].velo;
    const int lo = ls[(size_t) l - 1].velo + 1;
    const int hi = l + 1 < (int) ls.size() ? ls[(size_t) l + 1].velo - 1 : 127;
    ls[(size_t) l].velo = juce::jlimit (lo, std::max (lo, hi), v);
    return ls[(size_t) l].velo;
}

int Draft::veloTop (int a, int l) const
{
    auto& ls = articulations[(size_t) a].layers;
    return l + 1 < (int) ls.size() ? ls[(size_t) l + 1].velo - 1 : 127;
}

bool Draft::setCoverFromImage (const juce::Image& img, juce::String& error)
{
    if (! img.isValid()) { error = "not a readable image"; return false; }
    juce::Image scaled = img;
    const int longest = std::max (img.getWidth(), img.getHeight());
    if (longest > 512)
    {
        const float k = 512.0f / (float) longest;
        scaled = img.rescaled (std::max (1, (int) (img.getWidth() * k)), std::max (1, (int) (img.getHeight() * k)), juce::Graphics::highResamplingQuality);
    }
    juce::MemoryOutputStream out;
    juce::PNGImageFormat png;
    if (! png.writeImageToStream (scaled, out)) { error = "cannot encode cover"; return false; }
    cover = out.getMemoryBlock();
    return true;
}

bool Draft::validate (juce::String& error) const
{
    if (name.trim().isEmpty()) { error = "Give the instrument a name."; return false; }
    if (articulations.empty()) { error = "Add at least one articulation."; return false; }
    for (auto& art : articulations)
    {
        bool any = false;
        for (auto& l : art.layers) for (auto& c : l.cells) any = any || ! c.empty();
        if (! any) { error = "Articulation \"" + (art.name.trim().isEmpty() ? juce::String ("Hit") : art.name.trim()) + "\" has no samples."; return false; }
    }
    return true;
}

bool Draft::toInstrument (otci::Instrument& out, juce::String& error) const
{
    out = {};
    out.name = name.trim();
    out.library = library.trim().isEmpty() ? "User" : library.trim();
    if (out.name.isEmpty()) { error = "Give the instrument a name."; return false; }

    std::map<const DraftWave*, int> index;
    for (auto& art : articulations)
    {
        otci::Articulation oa;
        oa.name = art.name.trim().isEmpty() ? "Hit" : art.name.trim();
        std::copy (std::begin (art.relTime), std::end (art.relTime), std::begin (oa.relTime));
        for (auto& l : art.layers)
        {
            otci::Layer ol;
            ol.velo = l.velo;
            ol.minVol = l.minVol;
            ol.maxVol = l.maxVol;
            for (auto& c : l.cells)
            {
                if (c.empty()) continue;
                std::vector<int> group;
                for (auto& w : c.waves)
                {
                    auto it = index.find (w.get());
                    if (it == index.end())
                    {
                        otci::Wave ow;
                        ow.flac = encodeFlac (*w->audio, w->rate, w->bits);
                        if (ow.flac.getSize() == 0)
                        {
                            error = w->label + ": cannot store this sample rate (" + juce::String (w->rate) + " Hz)";
                            return false;
                        }
                        ow.frames = w->audio->getNumSamples();
                        ow.channels = w->audio->getNumChannels();
                        ow.rate = w->rate;
                        out.waves.push_back (std::move (ow));
                        it = index.emplace (w.get(), (int) out.waves.size() - 1).first;
                    }
                    group.push_back (it->second);
                }
                ol.roundRobins.push_back (std::move (group));
            }
            if (! ol.roundRobins.empty()) oa.layers.push_back (std::move (ol));
        }
        if (oa.layers.empty()) { error = "Articulation \"" + oa.name + "\" has no samples."; return false; }
        oa.layers[0].velo = 0;
        out.articulations.push_back (std::move (oa));
    }
    if (out.articulations.empty()) { error = "Add at least one articulation."; return false; }
    out.image = cover;
    return true;
}
