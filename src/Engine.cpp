#include "Engine.h"
#include <cmath>
#include <mutex>
#include <set>
#include <thread>

namespace
{
bool decodeFlacBlob (const juce::MemoryBlock& blob, juce::AudioBuffer<float>& out, double& rate)
{
    juce::FlacAudioFormat flac;
    auto* mem = new juce::MemoryInputStream (blob, false);
    std::unique_ptr<juce::AudioFormatReader> r (flac.createReaderFor (mem, true));
    if (r == nullptr || r->numChannels < 1 || r->numChannels > 2) return false;
    out.setSize ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&out, 0, (int) r->lengthInSamples, 0, true, true);
    rate = r->sampleRate;
    return true;
}

juce::Image coverOf (const otci::Instrument& m)
{
    if (m.image.getSize() == 0) return {};
    return juce::ImageFileFormat::loadFrom (m.image.getData(), m.image.getSize());
}
} // namespace

std::shared_ptr<LoadedInstrument> loadInstrument (const juce::File& f, juce::String& error)
{
    auto parsed = otci::read (f, true, error);
    if (! parsed) return nullptr;
    if (parsed->enc.active()) { error = "This instrument is protected and cannot be opened in the editor."; return nullptr; }

    auto li = std::make_shared<LoadedInstrument>();
    li->meta = std::move (*parsed);
    li->file = f;
    for (auto& w : li->meta.waves)
    {
        juce::AudioBuffer<float> b;
        double rate = 44100.0;
        if (! decodeFlacBlob (w.flac, b, rate))
        {
            error = "cannot decode wave in " + f.getFileName();
            return nullptr;
        }
        w.rate = rate;
        li->audio.push_back (std::move (b));
        w.flac.reset(); // keep RAM down: decoded copy is the working set
    }
    for (auto& a : li->meta.articulations) li->lastRR.emplace_back (a.layers.size(), -1);
    li->cover = coverOf (li->meta);
    return li;
}

// VeloSplit: virtual stroke types "Hit -> <other style>": soft hits play the Hit layers, hard hits play the other style's
// layers from kSplitVelocity up. They reuse the same waves (no extra audio), only the Full Mix of each style is offered.
static void addVeloSplits (otci::Instrument& m)
{
    auto find = [&] (const juce::String& group, const juce::String& label) -> int
    {
        for (size_t i = 0; i < m.articulations.size(); ++i)
        {
            const auto n = m.articulations[i].name;
            if (n == group + " / " + label) return (int) i;
        }
        return -1;
    };
    const int hit = find ("Hit", "Full Mix");
    if (hit < 0) return;
    std::vector<juce::String> styles;
    for (auto& a : m.articulations)
    {
        const int i = a.name.indexOf (" / ");
        if (i < 0) continue;
        const auto g = a.name.substring (0, i);
        if (g != "Hit" && ! g.startsWith ("VeloSplit") && std::find (styles.begin(), styles.end(), g) == styles.end() && find (g, "Full Mix") >= 0)
            styles.push_back (g);
    }
    for (auto& g : styles)
    {
        if (m.articulations.size() >= 64) return; // the articulation parameter holds 0..63
        const auto& lo = m.articulations[(size_t) hit];
        const auto& hi = m.articulations[(size_t) find (g, "Full Mix")];
        otci::Articulation v;
        v.name = "VeloSplit: Hit -> " + g + " / Full Mix";
        for (int k = 0; k < 4; ++k) v.relTime[k] = lo.relTime[k];
        for (auto& l : lo.layers) if (l.velo < kSplitVelocity) v.layers.push_back (l);
        const otci::Layer* start = nullptr;
        for (auto& l : hi.layers) if (l.velo <= kSplitVelocity && (start == nullptr || l.velo > start->velo)) start = &l;
        if (start != nullptr) { auto l = *start; l.velo = kSplitVelocity; v.layers.push_back (l); }
        for (auto& l : hi.layers) if (l.velo > kSplitVelocity) v.layers.push_back (l);
        if (! v.layers.empty()) m.articulations.push_back (std::move (v));
    }
}

std::shared_ptr<InstrumentData> readInstrumentData (const juce::File& f, juce::String& error)
{
    auto parsed = otci::read (f, false, error); // metadata + cover only; waves are read on demand
    if (! parsed) return nullptr;
    if (parsed->articulations.empty()) { error = "instrument has no articulations"; return nullptr; }
    auto d = std::make_shared<InstrumentData>();
    d->meta = std::move (*parsed);
    addVeloSplits (d->meta);
    d->file = f;
    d->cover = coverOf (d->meta);
    return d;
}

std::shared_ptr<SlotContent> buildSlotContent (std::shared_ptr<const InstrumentData> data, int articulation, juce::String& error, int threads)
{
    if (data == nullptr || data->meta.articulations.empty()) { error = "empty instrument"; return nullptr; }
    auto c = std::make_shared<SlotContent>();
    c->data = std::move (data);
    c->articulation = juce::jlimit (0, (int) c->data->meta.articulations.size() - 1, articulation);
    const auto& waves = c->data->meta.waves;
    c->audio.resize (waves.size());
    c->rate.assign (waves.size(), 44100.0);
    const auto& art = c->art();
    std::set<int> need;
    for (auto& l : art.layers)
        for (auto& group : l.roundRobins)
            for (int idx : group)
            {
                if (idx < 0 || idx >= (int) waves.size()) { error = "wave index out of range"; return nullptr; }
                need.insert (idx);
            }
    otcrypt::Key packKey {};
    const bool encrypted = c->data->meta.enc.active();
    if (encrypted && ! otci::resolveKey (c->data->meta.enc, packKey, error)) return nullptr;
    const std::vector<int> todo (need.begin(), need.end());
    std::atomic<size_t> next { 0 };
    std::mutex errLock;
    juce::String firstError;
    auto work = [&]
    {
        juce::FileInputStream in (c->data->file);
        for (size_t k = next.fetch_add (1); k < todo.size(); k = next.fetch_add (1))
        {
            const int idx = todo[k];
            const auto& w = waves[(size_t) idx];
            juce::MemoryBlock blob;
            juce::String derr;
            const bool ok = in.openedOk() && w.size > 0 && in.setPosition (w.offset)
                            && in.readIntoMemoryBlock (blob, (juce::int64) w.size) == (size_t) w.size
                            && (! encrypted || otci::decryptBlob (c->data->meta.enc, packKey, idx, blob, derr))
                            && decodeFlacBlob (blob, c->audio[(size_t) idx], c->rate[(size_t) idx]); // each wave writes its own slot
            if (! ok)
            {
                const std::lock_guard<std::mutex> g (errLock);
                if (firstError.isEmpty()) firstError = derr.isNotEmpty() ? derr : "cannot decode wave " + juce::String (idx) + " in " + c->data->file.getFileName();
                next.store (todo.size());
                return;
            }
        }
    };
    const int n = std::max (1, std::min (threads, (int) std::min<size_t> (todo.size(), 8)));
    std::vector<std::thread> helpers;
    for (int t = 1; t < n; ++t) helpers.emplace_back (work);
    work();
    for (auto& h : helpers) h.join();
    otcrypt::wipe (packKey.data(), packKey.size());
    if (firstError.isNotEmpty()) { error = firstError; return nullptr; }
    c->lastRR.assign (art.layers.size(), -1);
    return c;
}

bool calibrateMixTrim (std::shared_ptr<const InstrumentData> data, int fullArt, const std::vector<int>& micArts, float& trimDb, juce::String& error)
{
    if (data == nullptr || micArts.empty()) { error = "nothing to calibrate"; return false; }
    const auto& arts = data->meta.articulations;
    juce::FileInputStream in (data->file);
    if (! in.openedOk()) { error = "cannot open " + data->file.getFullPathName(); return false; }
    otcrypt::Key key {};
    const bool encrypted = data->meta.enc.active();
    if (encrypted && ! otci::resolveKey (data->meta.enc, key, error)) return false;
    // the loudest layer's first take, both channels one after the other, first 1.5 s
    auto take = [&] (int artIdx, std::vector<float>& out) -> bool
    {
        if (artIdx < 0 || artIdx >= (int) arts.size()) return false;
        float g = 1.0f;
        const auto* layer = otci::pickLayer (arts[(size_t) artIdx], 127, g);
        if (layer == nullptr || layer->roundRobins.empty()) return false;
        out.assign (2 * 66150, 0.0f);
        for (int idx : layer->roundRobins[0])
        {
            if (idx < 0 || idx >= (int) data->meta.waves.size()) return false;
            const auto& w = data->meta.waves[(size_t) idx];
            juce::MemoryBlock blob;
            juce::AudioBuffer<float> b;
            double rate = 44100.0;
            if (w.size <= 0 || ! in.setPosition (w.offset) || in.readIntoMemoryBlock (blob, (juce::int64) w.size) != (size_t) w.size
                || (encrypted && ! otci::decryptBlob (data->meta.enc, key, idx, blob, error)) || ! decodeFlacBlob (blob, b, rate))
                return false;
            const int n = std::min (b.getNumSamples(), 66150);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i) out[(size_t) (c * 66150 + i)] += b.getSample (std::min (c, b.getNumChannels() - 1), i);
        }
        return true;
    };
    std::vector<float> full, sum, one;
    if (! take (fullArt, full)) { if (error.isEmpty()) error = "no Full Mix to compare with"; return false; }
    sum.assign (full.size(), 0.0f);
    for (int a : micArts)
    {
        if (! take (a, one)) { if (error.isEmpty()) error = "cannot read a mic"; return false; }
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += one[i];
    }
    double num = 0.0, den = 0.0, ff = 0.0;
    for (size_t i = 0; i < sum.size(); ++i) { num += (double) full[i] * sum[i]; den += (double) sum[i] * sum[i]; ff += (double) full[i] * full[i]; }
    if (den <= 1e-12 || ff <= 1e-12) { error = "silent take"; return false; }
    const double corr = num / std::sqrt (den * ff);
    if (corr < 0.9) { error = "the mics do not line up with the Full Mix (correlation " + juce::String (corr, 2) + ")"; return false; }
    trimDb = juce::jlimit (-24.0f, 6.0f, (float) (20.0 * std::log10 (num / den)));
    return true;
}

PreviewSound buildPreview (std::shared_ptr<const InstrumentData> data, int velocity, juce::Random& rng, juce::String& error)
{
    PreviewSound out;
    if (data == nullptr || data->meta.articulations.empty()) { error = "empty instrument"; return out; }
    const auto& art = data->meta.articulations[0];
    float gain = 1.0f;
    const auto* layer = otci::pickLayer (art, juce::jlimit (1, 127, velocity), gain);
    if (layer == nullptr || layer->roundRobins.empty()) { error = "no samples"; return out; }
    const auto& group = layer->roundRobins[(size_t) rng.nextInt ((int) layer->roundRobins.size())];
    juce::FileInputStream in (data->file);
    if (! in.openedOk()) { error = "cannot open " + data->file.getFullPathName(); return out; }
    otcrypt::Key packKey {};
    const bool encrypted = data->meta.enc.active();
    if (encrypted && ! otci::resolveKey (data->meta.enc, packKey, error)) return out;
    std::vector<juce::AudioBuffer<float>> parts;
    int length = 0;
    for (int idx : group)
    {
        if (idx < 0 || idx >= (int) data->meta.waves.size()) continue;
        const auto& w = data->meta.waves[(size_t) idx];
        juce::MemoryBlock blob;
        juce::AudioBuffer<float> b;
        double rate = 44100.0;
        if (w.size <= 0 || ! in.setPosition (w.offset) || in.readIntoMemoryBlock (blob, (juce::int64) w.size) != (size_t) w.size
            || (encrypted && ! otci::decryptBlob (data->meta.enc, packKey, idx, blob, error))
            || ! decodeFlacBlob (blob, b, rate))
        { if (error.isEmpty()) error = "cannot decode preview"; return out; }
        out.rate = rate;
        length = std::max (length, b.getNumSamples());
        parts.push_back (std::move (b));
    }
    if (parts.empty() || length < 2) { error = "no samples"; return out; }
    out.audio = std::make_shared<juce::AudioBuffer<float>> (2, length);
    out.audio->clear();
    for (auto& b : parts)
        for (int c = 0; c < 2; ++c)
            out.audio->addFrom (c, 0, b, std::min (c, b.getNumChannels() - 1), 0, b.getNumSamples(), gain);
    return out;
}

juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& audio, double rate, int bits)
{
    juce::MemoryBlock out;
    {
        juce::FlacAudioFormat flac;
        auto* os = new juce::MemoryOutputStream (out, false);
        std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (os, rate, (unsigned) audio.getNumChannels(), bits, {}, 5));
        if (w == nullptr) { delete os; return {}; }
        w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
    return out;
}

// ============================================================================================================
TriggerEngine::TriggerEngine() = default;

void TriggerEngine::prepare (double sampleRate, int blockSize)
{
    sr = sampleRate;
    maxBlock = std::max (blockSize, 64);
    wet.setSize (2 * kBuses, maxBlock, false, true, false);
    aud.setSize (2, maxBlock, false, true, false);
    rng.setSeedRandomly(); // instances must not share one round-robin sequence
    latency = kLatency;
    for (auto& d : delayBuf) d.assign ((size_t) latency, 0.0f);
    delayPos = 0;
    colSamples = std::max (1, (int) std::lround (sr * 0.002));
    tel.columnSeconds.store ((double) colSamples / sr);
    lastLow = lastHigh = -1.0f;
    reset();
}

void TriggerEngine::reset()
{
    for (auto& v : voices) v = {};
    for (auto& v : auditionVoices) v = {};
    previewVoice = {};
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        hasPreview = false;                    // a preview still waiting to start belongs to the old session
    }
    holdoff = armed = 0;
    armedPeak = riseEnv = suppressThr = 0.0f;
    gateGain = 1.0f; gateHold = 0; gateOpen = false;
    for (auto& f : hp) f.reset();
    for (auto& f : lp) f.reset();
    colCount = 0; colLo = colHi = 0.0f; colTrig = 0;
    for (auto& d : delayBuf) std::fill (d.begin(), d.end(), 0.0f);
    delayPos = 0;
}

void TriggerEngine::setSlotContent (int slot, std::shared_ptr<SlotContent> content) { setLaneContent (slot, 0, std::move (content)); }

void TriggerEngine::setLaneContent (int slot, int lane, std::shared_ptr<SlotContent> content)
{
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return;
    const auto idx = (size_t) (slot * kLanes + lane);
    std::shared_ptr<SlotContent> dropped;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        dropped = std::move (pendingSlots[idx]); // unconsumed older request, or the parked old content
        pendingSlots[idx] = std::move (content);
        pendingMask |= juce::uint64 (1) << idx;
    }
    // `dropped` is destroyed here, outside the lock and off the audio thread.
}

void TriggerEngine::releaseParked()
{
    std::array<std::shared_ptr<SlotContent>, kContents> drop;
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        for (int i = 0; i < kContents; ++i)
            if (! (pendingMask & (juce::uint64 (1) << i))) drop[(size_t) i] = std::move (pendingSlots[(size_t) i]); // only parked (already swapped out) entries
    }
    // `drop` is destroyed here, outside the lock.
    const auto now = juce::Time::getMillisecondCounter();
    learnRetired.erase (std::remove_if (learnRetired.begin(), learnRetired.end(),
                                        [now] (auto& e) { return now - e.first > 1500; }), learnRetired.end());
}

void TriggerEngine::startLearn (double seconds)
{
    learnPtr.store (nullptr);
    if (learnOwner) learnRetired.emplace_back (juce::Time::getMillisecondCounter(), std::move (learnOwner));
    auto b = std::make_shared<LearnBuffer>();
    b->sampleRate = sr;
    b->data.assign ((size_t) std::max (1.0, seconds * sr), 0.0f);
    learnOwner = b;
    learnPtr.store (b.get(), std::memory_order_release);
}

void TriggerEngine::stopLearn()
{
    learnPtr.store (nullptr, std::memory_order_release); // the buffer stays alive in learnOwner until the next start
}

void TriggerEngine::auditionSlot (int slot, int velocity)
{
    if (slot >= 0 && slot < kSlots) auditionRequest[(size_t) slot].store (juce::jlimit (1, 127, velocity));
}

void TriggerEngine::audition (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    previewPending = std::move (buffer);
    previewPendingRate = rate;
    hasPreview = true;
}

int TriggerEngine::activeVoices() const noexcept
{
    int n = 0;
    for (auto& v : voices) n += v.buf != nullptr;
    return n;
}

void TriggerEngine::adoptPending()
{
    const juce::SpinLock::ScopedTryLockType tl (lock);
    if (! tl.isLocked()) return;
    if (pendingMask != 0)
    {
        for (int i = 0; i < kContents; ++i)
            if (pendingMask & (juce::uint64 (1) << i))
            {
                std::swap (slots[(size_t) i], pendingSlots[(size_t) i]); // old content parks in pendingSlots
                const int sl = i / kLanes, ln = i % kLanes;
                for (auto& v : voices) if (v.slot == sl && v.lane == ln) v = {};
                for (auto& v : auditionVoices) if (v.slot == sl && v.lane == ln) v = {};
            }
        pendingMask = 0;
    }
    if (hasPreview)
    {
        std::swap (previewHold, previewPending);
        previewRate = previewPendingRate;
        hasPreview = false;
        previewVoice = {};
        if (previewHold != nullptr && previewHold->getNumSamples() > 1)
        {
            previewVoice.buf = previewHold.get();
            previewVoice.step = previewRate / sr;
            previewVoice.gL = previewVoice.gR = 0.8f;
        }
    }
}

void TriggerEngine::startVoice (std::array<Voice, kVoices>& pool, const SlotContent& c, int slot, int lane, int bus, int waveIdx, float gain, const SlotParams& sp, int delay)
{
    if (waveIdx < 0 || waveIdx >= (int) c.audio.size() || c.audio[(size_t) waveIdx].getNumSamples() < 2) return;
    Voice* v = nullptr;
    for (auto& x : pool) if (x.buf == nullptr) { v = &x; break; }
    if (v == nullptr) // steal the oldest
    {
        v = &pool[0];
        for (auto& x : pool) if (x.started < v->started) v = &x;
    }
    const float theta = (juce::jlimit (-1.0f, 1.0f, sp.pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    const float g = gain * juce::Decibels::decibelsToGain (sp.gainDb) * (sp.phase ? -1.0f : 1.0f);
    v->buf = &c.audio[(size_t) waveIdx];
    v->pos = 0.0;
    v->step = c.rate[(size_t) waveIdx] / sr * std::pow (2.0, (double) sp.tuneSemis / 12.0);
    v->gL = g * std::cos (theta) * juce::MathConstants<float>::sqrt2;
    v->gR = g * std::sin (theta) * juce::MathConstants<float>::sqrt2;
    v->slot = slot;
    v->lane = lane;
    v->bus = bus;
    v->delay = delay;
    v->started = ++clock;
}

void TriggerEngine::triggerSlot (int slot, int velocity, const Params& p, bool audition, int delay)
{
    auto& c = slots[(size_t) (slot * kLanes)];
    if (c == nullptr || velocity < 1) return;
    const auto& sp = p.slot[(size_t) slot];
    const auto& lp = p.lane[(size_t) slot];
    // an unavailable output (not enabled by the host) plays on Main; auditions always play on Main
    auto busOf = [&] (int lane)
    {
        const int b = lp[(size_t) lane].bus;
        if (audition || b <= 0) return 0;
        if (b < kBuses) return p.busChannel[(size_t) b] >= 0 ? b : 0;
        const int m = b - kBuses;                                    // mono output: lives on channel m % 2 of stereo bus m / 2
        if (m >= kMonoOuts) return 0;
        const int sb = m / 2;
        if (sb == 0) return (m == 0 || p.mainChannels != 1) ? b : 0;
        return p.busChannel[(size_t) sb] >= 0 ? b : 0;
    };
    const auto& art = c->art();
    float gain = 1.0f;
    const auto* layer = otci::pickLayer (art, velocity, gain);
    if (layer == nullptr || layer->roundRobins.empty()) return;

    // Random round robin that never repeats the previous take (avoids the machine-gun effect).
    const int takes = (int) layer->roundRobins.size();
    const auto layerIdx = (size_t) (layer - art.layers.data());
    int& last = c->lastRR[layerIdx];
    int pick = 0;
    if (takes > 1 && last < 0) pick = rng.nextInt (takes);  // first hit: any take
    else if (takes > 1) { pick = rng.nextInt (takes - 1); if (pick >= last) ++pick; }
    last = pick;
    auto& pool = audition ? auditionVoices : voices;
    // Every mic of the hit: same velocity and the same take number, so each mic hears the same strike and the phase between
    // them is the one that was recorded. Each lane has its own level, pan, polarity, mute/solo and output.
    bool laneSolo = false;
    for (int lane = 0; lane < kLanes; ++lane)
        laneSolo = laneSolo || (slots[(size_t) (slot * kLanes + lane)] != nullptr && (lane == 0 || lp[(size_t) lane].on) && lp[(size_t) lane].solo);
    for (int lane = 0; lane < kLanes; ++lane)
    {
        auto& e = slots[(size_t) (slot * kLanes + lane)];
        const auto& L = lp[(size_t) lane];
        if (e == nullptr || (lane > 0 && ! L.on) || L.gainDb <= kLaneOffDb) continue;
        if (laneSolo ? ! L.solo : L.mute) continue;
        float g2 = gain;
        const otci::Layer* l2 = layer;
        int take2 = pick;
        if (lane > 0)
        {
            g2 = 1.0f;
            l2 = otci::pickLayer (e->art(), velocity, g2);
            if (l2 == nullptr || l2->roundRobins.empty()) continue;
            take2 = pick % (int) l2->roundRobins.size();
        }
        SlotParams q = sp;
        q.pan = juce::jlimit (-1.0f, 1.0f, sp.pan + L.pan);
        q.phase = sp.phase != L.phase;
        const float lg = juce::Decibels::decibelsToGain (L.gainDb);
        for (int idx : l2->roundRobins[(size_t) take2])
            startVoice (pool, *e, slot, lane, busOf (lane), idx, g2 * lg, q, delay);
    }
    if (! audition) tel.hits[slot].fetch_add (1, std::memory_order_relaxed);
}

void TriggerEngine::fire (int velocity, const Params& p, int delay)
{
    bool anySolo = false;
    for (int i = 0; i < kSlots; ++i) anySolo = anySolo || (slots[(size_t) (i * kLanes)] != nullptr && p.slot[(size_t) i].solo);
    for (int i = 0; i < kSlots; ++i)
    {
        if (slots[(size_t) (i * kLanes)] == nullptr) continue;
        const auto& sp = p.slot[(size_t) i];
        if (anySolo ? ! sp.solo : sp.mute) continue;
        triggerSlot (i, velocity, p, false, delay);
    }
    colTrig = std::max (colTrig, velocity);
}

void TriggerEngine::renderVoice (Voice& v, const BusOut& o, int n)
{
    if (v.buf == nullptr) return;
    const bool mono = v.bus >= kBuses;
    const int sb = mono ? (v.bus - kBuses) / 2 : v.bus;
    float* l = o.l[sb] != nullptr ? o.l[sb] : o.l[0];
    float* r = o.r[sb] != nullptr ? o.r[sb] : o.r[0];
    float* m1 = nullptr;                                  // mono output: the voice, summed, goes to one channel only
    if (mono) m1 = ((v.bus - kBuses) % 2 == 0) ? o.l[sb] : o.r[sb];
    const int len = v.buf->getNumSamples();
    const float* a = v.buf->getReadPointer (0);
    const float* b = v.buf->getNumChannels() > 1 ? v.buf->getReadPointer (1) : a;
    if (m1 != nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            if (v.delay > 0) { --v.delay; continue; }
            const int i0 = (int) v.pos;
            if (i0 >= len - 1) { v.buf = nullptr; break; }
            const float fr = (float) (v.pos - i0);
            m1[i] += 0.5f * (v.gL * (a[i0] + (a[i0 + 1] - a[i0]) * fr) + v.gR * (b[i0] + (b[i0 + 1] - b[i0]) * fr));
            v.pos += v.step;
        }
        return;
    }
    for (int i = 0; i < n; ++i)
    {
        if (v.delay > 0) { --v.delay; continue; }
        const int i0 = (int) v.pos;
        if (i0 >= len - 1) { v.buf = nullptr; break; }
        const float fr = (float) (v.pos - i0);
        l[i] += v.gL * (a[i0] + (a[i0 + 1] - a[i0]) * fr);
        r[i] += v.gR * (b[i0] + (b[i0 + 1] - b[i0]) * fr);
        v.pos += v.step;
    }
}

template <size_t N>
void TriggerEngine::renderVoices (std::array<Voice, N>& pool, const BusOut& o, int n)
{
    for (auto& v : pool) renderVoice (v, o, n);
}

void TriggerEngine::updateFilters (const Params& p)
{
    if (p.lowcutHz != lastLow)
    {
        lastLow = p.lowcutHz;
        if (p.lowcutHz > 10.0f)
            for (auto& f : hp) f.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, (double) p.lowcutHz));
    }
    if (p.highcutHz != lastHigh)
    {
        lastHigh = p.highcutHz;
        if (p.highcutHz < 19990.0f && p.highcutHz < sr * 0.45)
            for (auto& f : lp) f.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, (double) p.highcutHz));
    }
}

void TriggerEngine::process (juce::AudioBuffer<float>& io, juce::MidiBuffer& midi, const Params& p)
{
    adoptPending();

    const int total = io.getNumSamples();
    const int nch = p.mainChannels > 0 ? std::min (p.mainChannels, io.getNumChannels()) : io.getNumChannels();
    if (nch == 0) return;

    updateFilters (p);
    const bool useHp = p.lowcutHz > 10.0f, useLp = p.highcutHz < 19990.0f && p.highcutHz < sr * 0.45;
    const float inGain = juce::Decibels::decibelsToGain (p.inGainDb);
    const float thrLin = juce::Decibels::decibelsToGain (p.thresholdDb);
    const int holdSamples = (int) (p.holdoffMs * 0.001 * sr);
    // velocity scan window; always shorter than the look-ahead so the sample can still be placed on the onset
    const int armSamples = juce::jlimit (4, std::max (4, latency - 2), (int) ((0.5 + 3.0 * p.detail / 100.0) * 0.001 * sr));
    const float ratio = 3.0f - 2.0f * juce::jlimit (0.0f, 100.0f, p.sensitivity) / 100.0f;  // rise needed over the recent envelope
    const float riseAtk = 1.0f - (float) std::exp (-1.0 / (0.008 * sr)); // envelope the new hit must stand out from
    const float riseRel = 1.0f - (float) std::exp (-1.0 / (0.03 * sr));
    const float suppressDecay = (float) std::exp (-1.0 / (0.3 * sr));
    const float gAtk = 1.0f - (float) std::exp (-1.0 / (std::max (p.gateAttackMs, 0.05f) * 0.001 * sr));
    const float gRel = 1.0f - (float) std::exp (-1.0 / (std::max (p.gateReleaseMs, 1.0f) * 0.001 * sr));
    const int gHold = (int) (p.gateHoldMs * 0.001 * sr);
    const float wetGain = juce::Decibels::decibelsToGain (p.outGainDb) * p.mix;
    const float dryGain = 1.0f - p.mix;
    const float auxGain = juce::Decibels::decibelsToGain (p.outGainDb);
    const bool useAudio = p.source != midiOnly, useMidi = p.source != audioOnly;
    float inPk[2] = { 0.0f, 0.0f };

    // Hits requested from the editor (click on a slot image).
    for (int i = 0; i < kSlots; ++i)
        if (const int v = auditionRequest[(size_t) i].exchange (0); v > 0 && slots[(size_t) (i * kLanes)] != nullptr)
        {
            auto q = p;
            q.slot[(size_t) i].mute = false;
            triggerSlot (i, v, q, true, 0);
        }

    auto midiIt = midi.begin();
    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        wet.clear (0, n);
        float* wl = wet.getWritePointer (0);
        float* wr = wet.getWritePointer (1);
        auto busOut = [&] (int offset)
        {
            BusOut o;
            for (int b = 0; b < kBuses; ++b) { o.l[b] = wet.getWritePointer (2 * b) + offset; o.r[b] = wet.getWritePointer (2 * b + 1) + offset; }
            return o;
        };
        float* io0 = io.getWritePointer (0) + start;
        float* io1 = nch > 1 ? io.getWritePointer (1) + start : nullptr;

        int done = 0;
        auto fireAt = [&] (int at)
        {
            if (at > done) { renderVoices (voices, busOut (done), at - done); done = at; }
        };
        for (int i = 0; i < n; ++i)
        {
            while (midiIt != midi.end() && (*midiIt).samplePosition < start + i + 1)
            {
                const auto m = (*midiIt).getMessage();
                if (useMidi && m.isNoteOn() && (*midiIt).samplePosition >= start + i
                    && (p.midiNote < 0 || m.getNoteNumber() == p.midiNote))
                {
                    fireAt (i);
                    fire (m.getVelocity(), p, latency); // same delay as the dry path, so everything stays aligned
                }
                ++midiIt;
            }

            if (auto* lb = learnPtr.load (std::memory_order_acquire))
            {
                const int at = lb->pos.load (std::memory_order_relaxed);
                if (at < (int) lb->data.size())
                {
                    lb->data[(size_t) at] = io1 != nullptr ? 0.5f * (io0[i] + io1[i]) : io0[i];
                    lb->pos.store (at + 1, std::memory_order_release);
                }
            }
            float xl = io0[i] * inGain;
            float xr = io1 != nullptr ? io1[i] * inGain : xl;
            if (p.gateOn)
            {
                const float lvl = std::max (std::abs (xl), std::abs (xr));
                if (lvl > thrLin) { gateOpen = true; gateHold = gHold; }
                else if (gateHold > 0) --gateHold;
                else gateOpen = false;
                gateGain += ((gateOpen ? 1.0f : 0.0f) - gateGain) * (gateOpen ? gAtk : gRel);
                xl *= gateGain;
                xr *= gateGain;
            }
            else gateGain = 1.0f;
            {
                auto& d0 = delayBuf[0]; auto& d1 = delayBuf[1];
                const float dl = d0[(size_t) delayPos], dr = d1[(size_t) delayPos];
                d0[(size_t) delayPos] = xl; d1[(size_t) delayPos] = xr;
                if (++delayPos >= latency) delayPos = 0;
                io0[i] = dl;
                if (io1 != nullptr) io1[i] = dr;
            }
            inPk[0] = std::max (inPk[0], std::abs (xl));
            inPk[1] = std::max (inPk[1], std::abs (xr));

            // ---- scope column (mono view of what the detector hears before filtering)
            const float view = io1 != nullptr ? 0.5f * (xl + xr) : xl;
            colLo = std::min (colLo, view);
            colHi = std::max (colHi, view);

            if (useAudio)
            {
                float fl = xl, fr = xr;
                if (useHp) { fl = hp[0].processSingleSampleRaw (fl); fr = hp[1].processSingleSampleRaw (fr); }
                if (useLp) { fl = lp[0].processSingleSampleRaw (fl); fr = lp[1].processSingleSampleRaw (fr); }
                const float level = std::max (std::abs (fl), std::abs (fr));
                if (holdoff > 0) --holdoff;
                suppressThr *= suppressDecay;
                if (armed > 0)
                {
                    armedPeak = std::max (armedPeak, level);
                    if (--armed == 0)
                    {
                        const float db = juce::Decibels::gainToDecibels (armedPeak);
                        const float t = juce::jlimit (0.0f, 1.0f, (db - p.thresholdDb) / std::max (-p.thresholdDb, 1.0f));
                        fireAt (i);
                        fire (1 + (int) std::lround (t * 126.0f), p, std::max (0, latency - armSamples)); // lands on the onset in the delayed dry
                        holdoff = holdSamples;
                        suppressThr = armedPeak * 0.6f * juce::jlimit (0.0f, 100.0f, p.suppress) / 100.0f;
                    }
                }
                else if (holdoff == 0 && level > thrLin && level > suppressThr && level > riseEnv * ratio)
                {
                    armed = armSamples;
                    armedPeak = level;
                }
                riseEnv += (level - riseEnv) * (level > riseEnv ? riseAtk : riseRel);
            }

            if (++colCount >= colSamples)
            {
                const int h = tel.head.load (std::memory_order_relaxed);
                auto& col = tel.cols[(size_t) (h % EngineTelemetry::kColumns)];
                col.lo.store (colLo, std::memory_order_relaxed);
                col.hi.store (colHi, std::memory_order_relaxed);
                col.trigVel.store (colTrig, std::memory_order_relaxed);
                tel.head.store (h + 1, std::memory_order_release);
                colCount = 0; colLo = colHi = 0.0f; colTrig = 0;
            }
        }
        fireAt (n);

        aud.clear (0, n);
        { BusOut ao; ao.l[0] = aud.getWritePointer (0); ao.r[0] = aud.getWritePointer (1); renderVoices (auditionVoices, ao, n); }
        for (int ch = 0; ch < nch; ++ch)
        {
            float* out = io.getWritePointer (ch) + start;
            const float* w = ch == 0 ? wl : wr;
            const float* a = aud.getReadPointer (ch == 0 ? 0 : 1);
            for (int i = 0; i < n; ++i) out[i] = out[i] * dryGain + w[i] * wetGain + a[i];
        }
        // Auxiliary outputs carry only the sampled sound (no dry signal), at the output volume.
        for (int b = 1; b < kBuses; ++b)
        {
            const int c0 = p.busChannel[(size_t) b];
            if (c0 < 0 || c0 + 1 >= io.getNumChannels()) continue;
            float* ol = io.getWritePointer (c0) + start;
            float* orr = io.getWritePointer (c0 + 1) + start;
            const float* sl = wet.getReadPointer (2 * b);
            const float* sr_ = wet.getReadPointer (2 * b + 1);
            for (int i = 0; i < n; ++i) { ol[i] = sl[i] * auxGain; orr[i] = sr_[i] * auxGain; }
        }
    }

    // Editor preview of a raw sample plays on top, untouched by mix/gain, even with no instrument loaded.
    for (int start = 0; previewVoice.buf != nullptr && start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        wet.clear (0, n);
        { BusOut po; po.l[0] = wet.getWritePointer (0); po.r[0] = wet.getWritePointer (1); renderVoice (previewVoice, po, n); }
        for (int ch = 0; ch < nch; ++ch)
            juce::FloatVectorOperations::add (io.getWritePointer (ch) + start, wet.getReadPointer (ch == 0 ? 0 : 1), n);
    }

    // telemetry for the meters
    auto peakMax = [] (std::atomic<float>& a, float v)
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    };
    for (int c = 0; c < 2; ++c)
    {
        peakMax (tel.inPeak[c], inPk[c]);
        peakMax (tel.outPeak[c], io.getMagnitude (std::min (c, nch - 1), 0, total));
    }
    tel.gateGain.store (gateGain, std::memory_order_relaxed);
}
