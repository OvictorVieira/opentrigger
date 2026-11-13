#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SmartTrigger.h"

namespace
{
enum G { gMix, gGain, gThreshold, gHoldoff, gSource, gInGain, gLowcut, gHighcut, gGateOn, gGateAtk, gGateHold, gGateRel,
         gSuppress, gSens, gDetail, gMidiNote, gCount };

juce::String dbText (float v) { return juce::String (v, 1) + " dB"; }
juce::String msText (float v) { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; }
juce::String hzText (float v) { return juce::String (juce::roundToInt (v)) + " hz"; }

template <typename Fn>
juce::AudioParameterFloatAttributes attrs (Fn&& toText)
{
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([toText] (float v, int) { return toText (v); })
                                                .withValueFromStringFunction ([] (const juce::String& s) { return s.getFloatValue(); });
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout OpenTriggerProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto F = [&] (const char* id, const char* name, float lo, float hi, float step, float def, float skew, auto&& text)
    {
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, step, skew), def, attrs (text)));
    };
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; })
                                       .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue() / 100.0f; })));
    F ("gain", "Output Volume", -60.0f, 14.0f, 0.1f, 0.0f, 1.0f, dbText);
    F ("threshold", "Threshold", -60.0f, 0.0f, 0.1f, -30.0f, 1.0f, dbText);
    F ("holdoff", "Retrigger", 1.0f, 100.0f, 1.0f, 30.0f, 0.6f, msText);
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Trigger Source", StringArray { "Audio", "MIDI", "Audio + MIDI" }, 2));
    F ("inGain", "Input Volume", -60.0f, 14.0f, 0.1f, 0.0f, 1.0f, dbText);
    F ("lowcut", "Low Cut", 0.0f, 1000.0f, 1.0f, 0.0f, 0.4f, hzText);
    F ("highcut", "High Cut", 400.0f, 20000.0f, 1.0f, 20000.0f, 0.4f, hzText);
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { "gateOn", 1 }, "Gate", false));
    F ("gateAttack", "Gate Attack", 0.2f, 50.0f, 0.1f, 0.5f, 0.5f, msText);
    F ("gateHold", "Gate Hold", 0.0f, 1000.0f, 1.0f, 4.0f, 0.5f, msText);
    F ("gateRelease", "Gate Release", 10.0f, 3000.0f, 1.0f, 450.0f, 0.5f, msText);
    F ("suppress", "Suppress", 0.0f, 100.0f, 1.0f, 0.0f, 1.0f, [] (float v) { return String (roundToInt (v)) + "%"; });
    F ("sensitivity", "Sensitivity", 0.0f, 100.0f, 1.0f, 50.0f, 1.0f, [] (float v) { return String (roundToInt (v)); });
    F ("detail", "Detail", 0.0f, 100.0f, 1.0f, 50.0f, 1.0f, [] (float v) { return String (roundToInt (v)); });
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { "midiNote", 1 }, "MIDI Note (0 = any)", 0, 128, 0,
        AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return v <= 0 ? String ("Any") : MidiMessage::getMidiNoteName (v - 1, true, true, 4); })));

    for (int s = 0; s < kSlots; ++s)
    {
        const String n = "Slot " + String (s + 1) + " ";
        const auto id = [&] (const char* x) { return slotParam (s, x); };
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id ("vol"), 1 }, n + "Volume", NormalisableRange<float> (-60.0f, 12.0f, 0.1f, 2.5f), 0.0f, attrs (dbText)));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id ("pan"), 1 }, n + "Pan", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f,
            attrs ([] (float v) { const int c = roundToInt (v * 100.0f); return c == 0 ? String ("<C>") : (c < 0 ? "L" + String (-c) : "R" + String (c)); })));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { id ("tune"), 1 }, n + "Tune", NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f,
            attrs ([] (float v) { return String (roundToInt (v * 100.0f)); })));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id ("mute"), 1 }, n + "Mute", false));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id ("solo"), 1 }, n + "Solo", false));
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id ("phase"), 1 }, n + "Phase Invert", false));
        p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id ("art"), 1 }, n + "Articulation", 0, 63, 0));
    }
    return { p.begin(), p.end() };
}

OpenTriggerProcessor::OpenTriggerProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Out 2", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 3", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 4", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 5", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 6", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 7", juce::AudioChannelSet::stereo(), false)
                                       .withOutput ("Out 8", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "OPENTRIGGER", createLayout())
{
    const char* gids[gCount] = { "mix", "gain", "threshold", "holdoff", "source", "inGain", "lowcut", "highcut", "gateOn",
                                 "gateAttack", "gateHold", "gateRelease", "suppress", "sensitivity", "detail", "midiNote" };
    for (int i = 0; i < gCount; ++i) gRaw[i] = raw (gids[i]);
    const char* sids[6] = { "vol", "pan", "tune", "mute", "solo", "phase" };
    for (int k = 0; k < 6; ++k)
        for (int s = 0; s < kSlots; ++s) slotRaw[k][(size_t) s] = raw (slotParam (s, sids[k]));
    setLatencySamples (TriggerEngine::kLatency);
    LibraryCache::get().refresh (Library::defaultFolder()); // warm up in the background so the window opens instantly
    for (int s = 0; s < kSlots; ++s) apvts.addParameterListener (slotParam (s, "art"), this);
    reaper.fn = [this] { engine.releaseParked(); };
    reaper.startTimer (3000);
}

OpenTriggerProcessor::~OpenTriggerProcessor()
{
    reaper.stopTimer();
    cancelPendingUpdate();
    pool.removeAllJobs (true, 20000);
    for (int s = 0; s < kSlots; ++s) apvts.removeParameterListener (slotParam (s, "art"), this);
}

bool OpenTriggerProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    const auto mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
    for (int b = 1; b < l.outputBuses.size(); ++b)                      // the extra outputs are stereo or off
        if (! l.outputBuses[b].isDisabled() && l.outputBuses[b] != stereo) return false;
    // stereo -> stereo, mono -> mono (Pro Tools keeps mono tracks mono), and mono -> stereo where the host allows it
    return (in == stereo && out == stereo) || (in == mono && out == mono) || (in == mono && out == stereo);
}

void OpenTriggerProcessor::prepareToPlay (double sr, int block)
{
    sampleRateNow = sr;
    engine.prepare (sr, block);
}

TriggerEngine::Params OpenTriggerProcessor::snapshotParams() const
{
    TriggerEngine::Params p;
    p.mix = gRaw[gMix]->load();
    p.outGainDb = gRaw[gGain]->load();
    p.thresholdDb = gRaw[gThreshold]->load();
    p.holdoffMs = gRaw[gHoldoff]->load();
    p.source = (int) gRaw[gSource]->load();
    p.inGainDb = gRaw[gInGain]->load();
    p.lowcutHz = gRaw[gLowcut]->load();
    p.highcutHz = gRaw[gHighcut]->load();
    p.gateOn = gRaw[gGateOn]->load() > 0.5f;
    p.gateAttackMs = gRaw[gGateAtk]->load();
    p.gateHoldMs = gRaw[gGateHold]->load();
    p.gateReleaseMs = gRaw[gGateRel]->load();
    p.suppress = gRaw[gSuppress]->load();
    p.sensitivity = gRaw[gSens]->load();
    p.detail = gRaw[gDetail]->load();
    p.midiNote = (int) gRaw[gMidiNote]->load() - 1; // 0 = any -> -1
    p.mainChannels = std::max (1, getMainBusNumOutputChannels());
    for (int b = 1; b < kBuses; ++b)
    {
        const auto* bus = getBus (false, b);
        p.busChannel[(size_t) b] = (bus != nullptr && bus->isEnabled()) ? getChannelIndexInProcessBlockBuffer (false, b, 0) : -1;
    }
    for (size_t s = 0; s < (size_t) kSlots; ++s)
    {
        for (int ln = 0; ln < kLanes; ++ln)
        {
            const auto& c = laneCfg[s][(size_t) ln];
            auto& lp = p.lane[s][(size_t) ln];
            lp.bus = c.bus.load (std::memory_order_relaxed);
            lp.gainDb = c.gainDb.load (std::memory_order_relaxed);
            lp.pan = c.pan.load (std::memory_order_relaxed);
            lp.mute = c.mute.load (std::memory_order_relaxed);
            lp.solo = c.solo.load (std::memory_order_relaxed);
            lp.phase = c.phase.load (std::memory_order_relaxed);
            lp.on = ln == 0 || c.on.load (std::memory_order_relaxed);
        }
        auto& sp = p.slot[s];
        sp.gainDb = slotRaw[0][s]->load();
        sp.pan = slotRaw[1][s]->load();
        sp.tuneSemis = slotRaw[2][s]->load();
        sp.mute = slotRaw[3][s]->load() > 0.5f;
        sp.solo = slotRaw[4][s]->load() > 0.5f;
        sp.phase = slotRaw[5][s]->load() > 0.5f;
    }
    return p;
}

void OpenTriggerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
    engine.process (buffer, midi, snapshotParams());
}

void OpenTriggerProcessor::applyLearn (const LearnResult& r)
{
    if (! r.ok) return;
    auto set = [this] (const char* id, float v)
    {
        if (auto* prm = apvts.getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
            prm->endChangeGesture();
        }
    };
    set ("inGain", r.inputGainDb);
    set ("threshold", r.thresholdDb);
    if (auto* g = apvts.getParameter ("gateOn")) { g->beginChangeGesture(); g->setValueNotifyingHost (r.gateOn ? 1.0f : 0.0f); g->endChangeGesture(); }
    if (r.gateOn)
    {
        set ("gateAttack", r.gateAttackMs);
        set ("gateHold", r.gateHoldMs);
        set ("gateRelease", r.gateReleaseMs);
    }
    if (r.hasOutput) set ("gain", r.outputGainDb);
    set ("holdoff", r.retriggerMs);
    set ("lowcut", r.lowcutHz);
    set ("highcut", r.highcutHz);
    set ("sensitivity", r.sensitivity);
    set ("detail", r.detail);
    set ("suppress", r.suppress);
}

bool OpenTriggerProcessor::estimateOutputDb (LearnResult& r) const
{
    // velocity the median hit will play at (same mapping as the engine)
    const float levelDb = r.medianHitDb + r.inputGainDb;
    const float t = juce::jlimit (0.0f, 1.0f, (levelDb - r.thresholdDb) / std::max (-r.thresholdDb, 1.0f));
    const int vel = 1 + (int) std::lround (t * 126.0f);
    float total = 0.0f;
    bool any = false;
    bool anySolo = false;
    for (int s = 0; s < kSlots; ++s) anySolo = anySolo || (slotState[(size_t) s].info.content != nullptr && slotRaw[4][(size_t) s]->load() > 0.5f);
    for (int s = 0; s < kSlots; ++s)
    {
        const auto& c = slotState[(size_t) s].info.content;
        if (c == nullptr) continue;
        const bool solo = slotRaw[4][(size_t) s]->load() > 0.5f, mute = slotRaw[3][(size_t) s]->load() > 0.5f;
        if (anySolo ? ! solo : mute) continue;
        float g = 1.0f;
        const auto* layer = otci::pickLayer (c->art(), vel, g);
        if (layer == nullptr) continue;
        float peak = 0.0f;
        for (auto& group : layer->roundRobins)
        {
            float sum = 0.0f;
            for (int idx : group)
                if (idx >= 0 && idx < (int) c->audio.size() && c->audio[(size_t) idx].getNumSamples() > 0)
                    sum += c->audio[(size_t) idx].getMagnitude (0, c->audio[(size_t) idx].getNumSamples());
            peak = std::max (peak, sum);
        }
        total += peak * g * juce::Decibels::decibelsToGain (slotRaw[0][(size_t) s]->load());
        any = any || peak > 0.0f;
    }
    if (! any || total <= 1e-6f) return false;
    r.outputGainDb = juce::jlimit (-24.0f, 14.0f, std::round ((levelDb - juce::Decibels::gainToDecibels (total)) * 10.0f) / 10.0f);
    r.hasOutput = true;
    return true;
}

// ---------------------------------------------------------------------------------------------- slots
int OpenTriggerProcessor::slotArticulation (int slot) const
{
    return (int) raw (slotParam (slot, "art"))->load();
}

namespace
{
// The pitch of a decoded piece: the loudest layer's first take, channels summed.
float measureFundamental (const SlotContent& c)
{
    float g = 1.0f;
    const auto* layer = otci::pickLayer (c.art(), 127, g);
    if (layer == nullptr || layer->roundRobins.empty()) return 0.0f;
    std::vector<float> mono;
    double rate = 44100.0;
    for (int idx : layer->roundRobins[0])
    {
        if (idx < 0 || idx >= (int) c.audio.size() || c.audio[(size_t) idx].getNumSamples() == 0) continue;
        const auto& b = c.audio[(size_t) idx];
        rate = c.rate[(size_t) idx];
        if (mono.size() < (size_t) b.getNumSamples()) mono.resize ((size_t) b.getNumSamples(), 0.0f);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i) mono[(size_t) i] += b.getSample (ch, i);
    }
    return estimateFundamentalHz (mono.data(), (int) mono.size(), rate);
}
}

void OpenTriggerProcessor::setSlotArticulation (int slot, int a, bool auditionWhenReady)
{
    if (slot < 0 || slot >= kSlots) return;
    if (auto* prm = apvts.getParameter (slotParam (slot, "art")))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) juce::jlimit (0, 63, a)));
        prm->endChangeGesture();
    }
    auditionAfterBuild[(size_t) slot] = auditionWhenReady;
    syncArticulations(); // start decoding now instead of waiting for the async parameter callback
    if (auditionWhenReady)
    {
        // already on that mic (e.g. clicked the active one again): play right away
        const auto& st = slotState[(size_t) slot];
        if (st.info.status == SlotStatus::ready && st.info.data != nullptr && ! st.building
            && st.info.contentArticulation == juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, a))
        {
            auditionAfterBuild[(size_t) slot] = false;
            engine.auditionSlot (slot, 100);
        }
    }
}

void OpenTriggerProcessor::loadSlot (int slot, const juce::File& file, bool keepLanes)
{
    if (slot < 0 || slot >= kSlots) return;
    resetLanes (slot, keepLanes);
    auto& st = slotState[(size_t) slot];
    st.info = {};
    st.info.status = SlotStatus::loading;
    st.info.file = file;
    const auto gen = ++st.gen;
    st.building = true;
    slotsChanged.sendChangeMessage();
    const int art = slotArticulation (slot);
    juce::WeakReference<OpenTriggerProcessor> weak (this);
    pool.addJob ([weak, slot, gen, file, art]
    {
        juce::String err;
        auto data = readInstrumentData (file, err);
        std::shared_ptr<SlotContent> content;
        if (data) content = buildSlotContent (data, art, err, 2);
        juce::MessageManager::callAsync ([weak, slot, gen, data, content, err, file]
        {
            if (auto* self = weak.get()) self->finishLoad (slot, gen, data, content, err, file);
        });
    });
}

void OpenTriggerProcessor::clearSlot (int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    auto& st = slotState[(size_t) slot];
    st.info = {};
    ++st.gen;
    st.building = false;
    resetLanes (slot, false);
    engine.setSlotContent (slot, nullptr);
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::startBuild (int slot, std::shared_ptr<const InstrumentData> data, int articulation, bool, juce::File)
{
    auto& st = slotState[(size_t) slot];
    const auto gen = ++st.gen;
    st.building = true;
    juce::WeakReference<OpenTriggerProcessor> weak (this);
    pool.addJob ([weak, slot, gen, data, articulation]
    {
        juce::String err;
        auto content = buildSlotContent (data, articulation, err, 4); // someone is waiting for this mic: decode in parallel
        juce::MessageManager::callAsync ([weak, slot, gen, content, err]
        {
            if (auto* self = weak.get()) self->finishLoad (slot, gen, nullptr, content, err, {});
        });
    });
}

void OpenTriggerProcessor::finishLoad (int slot, juce::uint64 gen, std::shared_ptr<InstrumentData> data,
                                       std::shared_ptr<SlotContent> content, juce::String error, juce::File file)
{
    auto& st = slotState[(size_t) slot];
    if (gen != st.gen) return; // superseded by a newer load/clear
    st.building = false;
    if (content == nullptr)
    {
        if (data != nullptr || st.info.data == nullptr) { st.info.status = SlotStatus::failed; st.info.error = error; }
        slotsChanged.sendChangeMessage();
        return;
    }
    if (data != nullptr) { st.info.data = data; st.info.file = file; }
    st.info.status = SlotStatus::ready;
    st.info.error = {};
    st.info.contentArticulation = content->articulation;
    const int decoded = content->articulation;
    st.info.content = content;                       // message-thread copy, read-only (level matching)
    st.info.fundamentalHz = measureFundamental (*content);
    engine.setSlotContent (slot, std::move (content));
    if (auditionAfterBuild[(size_t) slot] && decoded == juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, slotArticulation (slot)))
    {
        auditionAfterBuild[(size_t) slot] = false;
        engine.auditionSlot (slot, 100); // consumed in the same audio block that adopts the new content
    }
    slotsChanged.sendChangeMessage();
    syncArticulations(); // the parameter may have moved while we were decoding
    syncLanes();         // extra mics wanted by the project (or by the user before this instrument finished loading)
}

void OpenTriggerProcessor::parameterChanged (const juce::String&, float) { triggerAsyncUpdate(); }
void OpenTriggerProcessor::handleAsyncUpdate() { syncArticulations(); syncLanes(); }

void OpenTriggerProcessor::syncArticulations()
{
    for (int s = 0; s < kSlots; ++s)
    {
        auto& st = slotState[(size_t) s];
        if (st.info.status != SlotStatus::ready || st.building || st.info.data == nullptr) continue;
        const int want = juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, slotArticulation (s));
        if (want != st.info.contentArticulation) startBuild (s, st.info.data, want, false, {});
    }
}

// ---------------------------------------------------------------------------------------------- mic lanes
OpenTriggerProcessor::LaneInfo OpenTriggerProcessor::laneInfo (int slot, int lane) const
{
    LaneInfo i;
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return i;
    const auto& c = laneCfg[(size_t) slot][(size_t) lane];
    i.on = lane == 0 || c.on.load();
    i.articulation = lane == 0 ? slotArticulation (slot) : c.art.load();
    i.bus = c.bus.load();
    i.gainDb = c.gainDb.load();
    i.pan = c.pan.load(); i.mute = c.mute.load(); i.solo = c.solo.load(); i.phase = c.phase.load();
    i.ready = lane == 0 ? slotState[(size_t) slot].info.status == SlotStatus::ready
                        : laneState[(size_t) slot][(size_t) lane].content != nullptr && ! laneState[(size_t) slot][(size_t) lane].building;
    return i;
}

bool OpenTriggerProcessor::busEnabled (int bus) const
{
    if (bus <= 0) return true;
    const auto* b = getBus (false, bus);
    return b != nullptr && b->isEnabled();
}

void OpenTriggerProcessor::setLaneOn (int slot, int lane, bool on)
{
    if (slot < 0 || slot >= kSlots || lane < 1 || lane >= kLanes) return;
    laneCfg[(size_t) slot][(size_t) lane].on = on;
    syncLanes();
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::setLaneArticulation (int slot, int lane, int a)
{
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return;
    if (lane == 0) { setSlotArticulation (slot, a); return; }
    laneCfg[(size_t) slot][(size_t) lane].art = juce::jlimit (0, 63, a);
    syncLanes();
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::setLaneBus (int slot, int lane, int bus)
{
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return;
    laneCfg[(size_t) slot][(size_t) lane].bus = juce::jlimit (0, kBuses - 1, bus);
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::setLaneGain (int slot, int lane, float db)
{
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return;
    laneCfg[(size_t) slot][(size_t) lane].gainDb = juce::jlimit (-60.0f, 12.0f, db);
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::setLanePan (int slot, int lane, float pan)
{
    if (slot < 0 || slot >= kSlots || lane < 0 || lane >= kLanes) return;
    laneCfg[(size_t) slot][(size_t) lane].pan = juce::jlimit (-1.0f, 1.0f, pan);
    slotsChanged.sendChangeMessage();
}
void OpenTriggerProcessor::setLaneMute (int slot, int lane, bool on) { if (slot >= 0 && slot < kSlots && lane >= 0 && lane < kLanes) { laneCfg[(size_t) slot][(size_t) lane].mute = on; slotsChanged.sendChangeMessage(); } }
void OpenTriggerProcessor::setLaneSolo (int slot, int lane, bool on) { if (slot >= 0 && slot < kSlots && lane >= 0 && lane < kLanes) { laneCfg[(size_t) slot][(size_t) lane].solo = on; slotsChanged.sendChangeMessage(); } }
void OpenTriggerProcessor::setLanePhase (int slot, int lane, bool on) { if (slot >= 0 && slot < kSlots && lane >= 0 && lane < kLanes) { laneCfg[(size_t) slot][(size_t) lane].phase = on; slotsChanged.sendChangeMessage(); } }

int OpenTriggerProcessor::laneOfArticulation (int slot, int art) const
{
    if (slot < 0 || slot >= kSlots) return -1;
    if (slotArticulation (slot) == art) return 0;
    for (int ln = 1; ln < kLanes; ++ln)
        if (laneCfg[(size_t) slot][(size_t) ln].on.load() && laneCfg[(size_t) slot][(size_t) ln].art.load() == art) return ln;
    return -1;
}

bool OpenTriggerProcessor::setMicLevel (int slot, int art, float db)
{
    if (slot < 0 || slot >= kSlots) return false;
    db = juce::jlimit (-60.0f, 12.0f, db);
    int ln = laneOfArticulation (slot, art);
    if (ln == 0) { setLaneGain (slot, 0, db); return true; }
    if (ln < 0)
    {
        if (db <= kLaneOffDb) return true;                           // already off
        for (int k = 1; k < kLanes && ln < 0; ++k) if (! laneCfg[(size_t) slot][(size_t) k].on.load()) ln = k;
        if (ln < 0) return false;                                    // every lane is taken
        auto& c = laneCfg[(size_t) slot][(size_t) ln];
        c.art = art; c.bus = 0; c.pan = 0.0f; c.mute = false; c.solo = false; c.phase = false;
        c.gainDb = db; c.on = true;
        syncLanes();
        slotsChanged.sendChangeMessage();
        return true;
    }
    if (db <= kLaneOffDb)
    {
        auto& c = laneCfg[(size_t) slot][(size_t) ln];
        c.on = false; c.gainDb = 0.0f; c.pan = 0.0f; c.mute = false; c.solo = false; c.phase = false; c.bus = 0;
        syncLanes();
    }
    else setLaneGain (slot, ln, db);
    slotsChanged.sendChangeMessage();
    return true;
}

void OpenTriggerProcessor::resetMixer (int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    for (int ln = 0; ln < kLanes; ++ln)
    {
        auto& c = laneCfg[(size_t) slot][(size_t) ln];
        if (ln > 0) c.on = false;
        c.gainDb = 0.0f; c.pan = 0.0f; c.mute = false; c.solo = false; c.phase = false; c.bus = 0;
    }
    syncLanes();
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::calibrateMixed (int slot, const juce::String& group)
{
    if (slot < 0 || slot >= kSlots) return;
    const auto data = slotState[(size_t) slot].info.data;
    if (data == nullptr) return;
    const auto& arts = data->meta.articulations;
    const auto fullName = group.isEmpty() ? juce::String ("Full Mix") : group + " / Full Mix";
    int full = -1;
    std::vector<int> mics;
    for (int i = 0; i < (int) arts.size(); ++i)
    {
        const auto& n = arts[(size_t) i].name;
        const int sep = n.indexOf (" / ");
        const auto g = sep < 0 ? juce::String() : n.substring (0, sep);
        const auto label = sep < 0 ? n : n.substring (sep + 3);
        if (g != group) continue;
        if (n == fullName) full = i;
        else if (! label.containsIgnoreCase ("Punch Boost") && (int) mics.size() < kLanes - 1) mics.push_back (i);
    }
    if (full < 0 || mics.empty()) { mixerNote = "This stroke type has no Full Mix and separate mics to compare."; slotsChanged.sendChangeMessage(); return; }
    mixerNote = "Calibrating...";
    slotsChanged.sendChangeMessage();
    setSlotArticulation (slot, full);                                // lane 0 = the Full Mix
    juce::WeakReference<OpenTriggerProcessor> weak (this);
    pool.addJob ([weak, slot, data, full, mics]
    {
        juce::String err;
        float trim = 0.0f;
        const bool ok = calibrateMixTrim (data, full, mics, trim, err);
        juce::MessageManager::callAsync ([weak, slot, mics, ok, trim, err]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            const float db = ok ? trim : -3.1f;                      // measured, or the nominal -3 dB the Full Mix is built with
            for (int ln = 1; ln < kLanes; ++ln) self->laneCfg[(size_t) slot][(size_t) ln].on = false;
            for (int a : mics) self->setMicLevel (slot, a, db);
            self->setLaneGain (slot, 0, -60.0f);                     // the Full Mix stays loaded, silent
            self->mixerNote = ok ? "Mics set so that together they sound like the Mixed sound (" + juce::String (db, 1) + " dB each)."
                                 : "Could not measure (" + err + "); used " + juce::String (db, 1) + " dB each.";
            self->slotsChanged.sendChangeMessage();
        });
    });
}

void OpenTriggerProcessor::splitMicsToOutputs (int slot, const juce::String& group)
{
    if (slot < 0 || slot >= kSlots) return;
    const auto& st = slotState[(size_t) slot];
    if (st.info.data == nullptr) return;
    const auto& arts = st.info.data->meta.articulations;
    const int lead = juce::jlimit (0, (int) arts.size() - 1, slotArticulation (slot));
    int lane = 1;
    for (int i = 0; i < (int) arts.size() && lane < kLanes; ++i)
    {
        const auto name = arts[(size_t) i].name;
        const int sep = name.indexOf (" / ");
        const auto g = sep < 0 ? juce::String() : name.substring (0, sep);
        const auto label = sep < 0 ? name : name.substring (sep + 3);
        if (g != group || i == lead || label == "Full Mix" || label.containsIgnoreCase ("Punch Boost")) continue;
        auto& c = laneCfg[(size_t) slot][(size_t) lane];
        c.art = i; c.bus = std::min (lane, kBuses - 1); c.gainDb = 0.0f; c.on = true;
        ++lane;
    }
    for (; lane < kLanes; ++lane) laneCfg[(size_t) slot][(size_t) lane].on = false;
    laneCfg[(size_t) slot][0].bus = 0;
    syncLanes();
    slotsChanged.sendChangeMessage();
}

void OpenTriggerProcessor::resetLanes (int slot, bool keepConfig)
{
    for (int ln = 1; ln < kLanes; ++ln)
    {
        auto& s = laneState[(size_t) slot][(size_t) ln];
        ++s.gen;
        s.content = nullptr; s.contentArt = -1; s.building = false;
        engine.setLaneContent (slot, ln, nullptr);
        if (! keepConfig)
        {
            auto& c = laneCfg[(size_t) slot][(size_t) ln];
            c.on = false; c.art = 0; c.bus = 0; c.gainDb = 0.0f; c.pan = 0.0f; c.mute = false; c.solo = false; c.phase = false;
        }
    }
    if (! keepConfig) { auto& c0 = laneCfg[(size_t) slot][0]; c0.bus = 0; c0.gainDb = 0.0f; c0.pan = 0.0f; c0.mute = false; c0.solo = false; c0.phase = false; }
}

void OpenTriggerProcessor::startLaneBuild (int slot, int lane)
{
    auto& st = slotState[(size_t) slot];
    auto& ls = laneState[(size_t) slot][(size_t) lane];
    if (st.info.data == nullptr) return;
    const int art = juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, laneCfg[(size_t) slot][(size_t) lane].art.load());
    const auto gen = ++ls.gen;
    ls.building = true;
    auto data = st.info.data;
    juce::WeakReference<OpenTriggerProcessor> weak (this);
    pool.addJob ([weak, slot, lane, gen, data, art]
    {
        juce::String err;
        auto content = buildSlotContent (data, art, err, 2);
        juce::MessageManager::callAsync ([weak, slot, lane, gen, content]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            auto& s = self->laneState[(size_t) slot][(size_t) lane];
            if (gen != s.gen) return;                // superseded
            s.building = false;
            if (content != nullptr)
            {
                s.contentArt = content->articulation;
                s.content = content;
                self->engine.setLaneContent (slot, lane, content);
            }
            self->slotsChanged.sendChangeMessage();
            self->syncLanes();
        });
    });
}

void OpenTriggerProcessor::syncLanes()
{
    for (int s = 0; s < kSlots; ++s)
    {
        const auto& st = slotState[(size_t) s];
        if (st.info.status != SlotStatus::ready || st.info.data == nullptr) continue;
        for (int ln = 1; ln < kLanes; ++ln)
        {
            auto& ls = laneState[(size_t) s][(size_t) ln];
            const auto& c = laneCfg[(size_t) s][(size_t) ln];
            if (! c.on.load())
            {
                if (ls.content != nullptr || ls.building) { ++ls.gen; ls.content = nullptr; ls.contentArt = -1; ls.building = false; engine.setLaneContent (s, ln, nullptr); }
                continue;
            }
            const int want = juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, c.art.load());
            if (! ls.building && (ls.content == nullptr || ls.contentArt != want)) startLaneBuild (s, ln);
        }
    }
}

// ---------------------------------------------------------------------------------------------- state
void OpenTriggerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    for (int s = 0; s < kSlots; ++s)
    {
        state.setProperty ("slot" + juce::String (s + 1), slotState[(size_t) s].info.file.getFullPathName(), nullptr);
        juce::StringArray lanes;
        for (int ln = 0; ln < kLanes; ++ln)
        {
            const auto& c = laneCfg[(size_t) s][(size_t) ln];
            lanes.add (juce::String (ln == 0 || c.on.load() ? 1 : 0) + "," + juce::String (c.art.load()) + "," + juce::String (c.bus.load()) + "," + juce::String (c.gainDb.load(), 2)
                       + "," + juce::String (c.pan.load(), 3) + "," + juce::String (c.mute.load() ? 1 : 0) + "," + juce::String (c.solo.load() ? 1 : 0) + "," + juce::String (c.phase.load() ? 1 : 0));
        }
        state.setProperty ("lanes" + juce::String (s + 1), lanes.joinIntoString (";"), nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void OpenTriggerProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr) return;
    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;
    juce::File files[kSlots];
    for (int s = 0; s < kSlots; ++s)
        files[s] = juce::File (state.getProperty ("slot" + juce::String (s + 1)).toString());
    apvts.replaceState (state);
    for (int s = 0; s < kSlots; ++s)
    {
        // mic lanes saved with the project (older projects have none: lanes stay off and everything plays on Main)
        const auto saved = juce::StringArray::fromTokens (state.getProperty ("lanes" + juce::String (s + 1)).toString(), ";", "");
        for (int ln = 0; ln < kLanes; ++ln)
        {
            auto& c = laneCfg[(size_t) s][(size_t) ln];
            const auto f = ln < saved.size() ? juce::StringArray::fromTokens (saved[ln], ",", "") : juce::StringArray();
            const bool have = f.size() == 4 || f.size() == 8;           // 4 fields: projects from the first beta
            c.on = ln > 0 && have && f[0].getIntValue() != 0;
            c.art = have ? juce::jlimit (0, 63, f[1].getIntValue()) : 0;
            c.bus = have ? juce::jlimit (0, kBuses - 1, f[2].getIntValue()) : 0;
            c.gainDb = have ? juce::jlimit (-60.0f, 12.0f, f[3].getFloatValue()) : 0.0f;
            c.pan = f.size() == 8 ? juce::jlimit (-1.0f, 1.0f, f[4].getFloatValue()) : 0.0f;
            c.mute = f.size() == 8 && f[5].getIntValue() != 0;
            c.solo = f.size() == 8 && f[6].getIntValue() != 0;
            c.phase = f.size() == 8 && f[7].getIntValue() != 0;
        }
        const auto cur = slotState[(size_t) s].info;
        if (files[s].existsAsFile())
        {
            if (cur.file != files[s] || cur.status == SlotStatus::failed) loadSlot (s, files[s], true);
        }
        else if (cur.status != SlotStatus::empty) clearSlot (s); // missing file: keep running silent
    }
    syncLanes();
}

juce::AudioProcessorEditor* OpenTriggerProcessor::createEditor() { return new OpenTriggerEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenTriggerProcessor(); }
