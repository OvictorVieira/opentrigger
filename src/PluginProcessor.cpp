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
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
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
    for (size_t s = 0; s < (size_t) kSlots; ++s)
    {
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

void OpenTriggerProcessor::loadSlot (int slot, const juce::File& file)
{
    if (slot < 0 || slot >= kSlots) return;
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
    engine.setSlotContent (slot, std::move (content));
    if (auditionAfterBuild[(size_t) slot] && decoded == juce::jlimit (0, (int) st.info.data->meta.articulations.size() - 1, slotArticulation (slot)))
    {
        auditionAfterBuild[(size_t) slot] = false;
        engine.auditionSlot (slot, 100); // consumed in the same audio block that adopts the new content
    }
    slotsChanged.sendChangeMessage();
    syncArticulations(); // the parameter may have moved while we were decoding
}

void OpenTriggerProcessor::parameterChanged (const juce::String&, float) { triggerAsyncUpdate(); }
void OpenTriggerProcessor::handleAsyncUpdate() { syncArticulations(); }

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

// ---------------------------------------------------------------------------------------------- state
void OpenTriggerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    for (int s = 0; s < kSlots; ++s)
        state.setProperty ("slot" + juce::String (s + 1), slotState[(size_t) s].info.file.getFullPathName(), nullptr);
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
        const auto cur = slotState[(size_t) s].info;
        if (files[s].existsAsFile())
        {
            if (cur.file != files[s] || cur.status == SlotStatus::failed) loadSlot (s, files[s]);
        }
        else if (cur.status != SlotStatus::empty) clearSlot (s); // missing file: keep running silent
    }
}

juce::AudioProcessorEditor* OpenTriggerProcessor::createEditor() { return new OpenTriggerEditor (*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OpenTriggerProcessor(); }
