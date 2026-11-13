#pragma once
#include "Otci.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>
#include <memory>

constexpr int kSlots = 8;
constexpr int kLanes = 12;  // mic lanes per slot: lane 0 is the slot's own mic, lanes 1.. are extra mics played with it
constexpr int kBuses = 8;   // stereo outputs: 0 = Main, 1..7 = auxiliary outputs the host can route to other tracks

// ---- fully decoded instrument (used by the instrument editor and tests) -------------------------------
struct LoadedInstrument
{
    otci::Instrument meta;                     // waves' flac blobs are released after decoding
    std::vector<juce::AudioBuffer<float>> audio; // 1 or 2 channels each, at meta.waves[i].rate
    juce::Image cover;
    juce::File file;
    mutable std::vector<std::vector<int>> lastRR;
};
std::shared_ptr<LoadedInstrument> loadInstrument (const juce::File&, juce::String& error);
juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& audio, double rate, int bits = 24);

// ---- lazily decoded instrument (used by the plugin) ----------------------------------------------------
// Immutable after reading: metadata, still-compressed waves, cover.
struct InstrumentData
{
    otci::Instrument meta;
    juce::Image cover;
    juce::File file;
};
// Velocity at which a VeloSplit stroke type switches from the soft style to the hard one.
constexpr int kSplitVelocity = 96;
std::shared_ptr<InstrumentData> readInstrumentData (const juce::File&, juce::String& error);

// What one slot plays: only the waves of ONE articulation are decoded (RAM stays small).
struct SlotContent
{
    std::shared_ptr<const InstrumentData> data;
    int articulation = 0;
    std::vector<juce::AudioBuffer<float>> audio; // indexed like data->meta.waves; undecoded entries stay empty
    std::vector<double> rate;
    mutable std::vector<int> lastRR;             // per layer, audio thread only
    const otci::Articulation& art() const { return data->meta.articulations[(size_t) articulation]; }
};
// articulation is clamped into range.
// `threads` > 1 decodes the waves in parallel (used when the user switches mic and is waiting for it).
std::shared_ptr<SlotContent> buildSlotContent (std::shared_ptr<const InstrumentData>, int articulation, juce::String& error, int threads = 1);

// ---- engine --------------------------------------------------------------------------------------------
// A short sound for auditioning a kit piece from the browser without loading it: the first articulation (the Full Mix
// in our kits), one random take at `velocity`. Reads only that take's waves from the file.
struct PreviewSound
{
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    double rate = 44100.0;
};
// Mixer calibration: the level (dB) that makes the given mics, added together, as loud as the Full Mix articulation. The
// Full Mix is the same hit with the mics already summed, so this is a least-squares fit on one loud take.
bool calibrateMixTrim (std::shared_ptr<const InstrumentData>, int fullArt, const std::vector<int>& micArts, float& trimDb, juce::String& error);

PreviewSound buildPreview (std::shared_ptr<const InstrumentData>, int velocity, juce::Random&, juce::String& error);

// Where one mic lane goes: an output bus (0 = Main) and its own level.
struct LaneParams
{
    int bus = 0;
    float gainDb = 0.0f;          // at or below kLaneOffDb the lane is silent
    float pan = 0.0f;             // added to the slot's pan
    bool on = true, mute = false, solo = false, phase = false;
};
constexpr float kLaneOffDb = -59.9f;

struct SlotParams
{
    float gainDb = 0.0f, pan = 0.0f, tuneSemis = 0.0f;
    bool mute = false, solo = false, phase = false;
};

// Written by the audio thread, read by the editor (relaxed atomics: it is only a display).
struct EngineTelemetry
{
    static constexpr int kColumns = 2048;
    struct Column { std::atomic<float> lo { 0.0f }, hi { 0.0f }; std::atomic<int> trigVel { 0 }; };
    std::array<Column, kColumns> cols;
    std::atomic<int> head { 0 };                  // index of the next column to write
    std::atomic<double> columnSeconds { 0.002 };
    std::atomic<float> inPeak[2] {}, outPeak[2] {}; // max since last exchange(0)
    std::atomic<unsigned> hits[kSlots] {};          // trigger counter per slot
    std::atomic<float> gateGain { 1.0f };
};

// Audio the plug-in received while Smart Trigger listens (written by the audio thread, read once it has stopped).
struct LearnBuffer
{
    std::vector<float> data;
    std::atomic<int> pos { 0 };
    double sampleRate = 44100.0;
};

class TriggerEngine
{
public:
    enum Source { audioOnly = 0, midiOnly = 1, both = 2 };
    struct Params
    {
        float mix = 1.0f;          // 0 dry .. 1 wet only
        float outGainDb = 0.0f, inGainDb = 0.0f;
        float thresholdDb = -30.0f, holdoffMs = 60.0f;
        float lowcutHz = 0.0f, highcutHz = 20000.0f; // detection-path filters (0 / 20000 = off)
        bool gateOn = false;
        float gateAttackMs = 0.5f, gateHoldMs = 4.0f, gateReleaseMs = 450.0f;
        float suppress = 0.0f, sensitivity = 50.0f, detail = 50.0f; // 0..100
        int source = both;
        int midiNote = -1;         // -1: any note triggers
        std::array<SlotParams, kSlots> slot;
        std::array<std::array<LaneParams, kLanes>, kSlots> lane;
        // channel index (in the host's buffer) of each output bus, -1 when the host has not enabled it. A lane routed to a
        // bus that is not enabled plays on Main instead, so nothing ever goes silent.
        std::array<int, kBuses> busChannel { 0, -1, -1, -1, -1, -1, -1, -1 };
        int mainChannels = 0;      // channels of the main bus (1 or 2); 0 = every channel of the buffer is main
    };

    TriggerEngine();
    void prepare (double sampleRate, int maxBlock);
    void reset();
    // Message thread: hand over new content for one slot (nullptr empties it).
    void setSlotContent (int slot, std::shared_ptr<SlotContent>);   // lane 0
    void setLaneContent (int slot, int lane, std::shared_ptr<SlotContent>);
    // Message thread: play the slot once, as if a hit of this velocity came in (ignores Mix).
    // Message thread: free content the audio thread has already swapped out (call every few seconds).
    void releaseParked();
    void auditionSlot (int slot, int velocity = 100);
    // Message thread: play one raw sample once (instrument editor preview).
    void audition (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double rate);
    // Audio thread.
    void process (juce::AudioBuffer<float>& io, juce::MidiBuffer& midi, const Params&);

    EngineTelemetry& telemetry() noexcept { return tel; }
    // Message thread: record up to `seconds` of the incoming audio (mono, before input volume and gate).
    void startLearn (double seconds);
    void stopLearn();
    std::shared_ptr<LearnBuffer> learnBuffer() const { return learnOwner; }
    int activeVoices() const noexcept;
    // Fixed look-ahead (samples): the dry signal is delayed by this much so a triggered sample can start exactly on
    // the transient it was detected from. The plug-in reports it to the host, which compensates.
    static constexpr int kLatency = 192;   // fixed (~4 ms at 48 kHz) so the host is told once, at construction
    int latencySamples() const noexcept { return latency; }

private:
    struct Voice
    {
        const juce::AudioBuffer<float>* buf = nullptr;
        double pos = 0.0, step = 1.0;
        float gL = 0.0f, gR = 0.0f;
        juce::uint64 started = 0;
        int slot = -1, lane = 0, bus = 0;
        int delay = 0;      // samples to wait before this voice starts sounding
    };
    static constexpr int kVoices = 192, kAuditionVoices = 24;

    void adoptPending();
    void fire (int velocity, const Params&, int delay);
    struct BusOut { float* l[kBuses] {}; float* r[kBuses] {}; };
    void triggerSlot (int slot, int velocity, const Params&, bool audition, int delay);
    void startVoice (std::array<Voice, kVoices>&, const SlotContent&, int slot, int lane, int bus, int waveIdx, float gain, const SlotParams&, int delay);
    void renderVoice (Voice&, const BusOut&, int n);
    template <size_t N> void renderVoices (std::array<Voice, N>&, const BusOut&, int n);
    void updateFilters (const Params&);

    static constexpr int kContents = kSlots * kLanes;               // index = slot * kLanes + lane
    std::array<std::shared_ptr<SlotContent>, kContents> slots;        // audio thread only
    std::array<std::shared_ptr<SlotContent>, kContents> pendingSlots; // guarded by lock; holds the *old* content after a swap
    juce::uint64 pendingMask = 0;
    juce::SpinLock lock;
    std::array<std::atomic<int>, kSlots> auditionRequest {};

    std::shared_ptr<const juce::AudioBuffer<float>> previewPending, previewHold;
    double previewRate = 44100.0, previewPendingRate = 44100.0;
    bool hasPreview = false;
    Voice previewVoice;

    std::array<Voice, kVoices> voices {};
    std::array<Voice, kVoices> auditionVoices {}; // sized like voices; only the first kAuditionVoices are used
    juce::AudioBuffer<float> wet, aud;
    juce::Random rng { 1234 };
    double sr = 44100.0;
    int maxBlock = 0;
    juce::uint64 clock = 0;

    // detection state
    int holdoff = 0, armed = 0;
    float armedPeak = 0.0f, riseEnv = 0.0f, suppressThr = 0.0f;
    // look-ahead delay line for the dry signal
    std::vector<float> delayBuf[2];
    int delayPos = 0, latency = kLatency;
    // gate state
    float gateGain = 1.0f;
    int gateHold = 0;
    bool gateOpen = false;
    // filters
    juce::IIRFilter hp[2], lp[2];
    float lastLow = -1.0f, lastHigh = -1.0f;
    std::atomic<LearnBuffer*> learnPtr { nullptr };
    std::shared_ptr<LearnBuffer> learnOwner;
    std::vector<std::pair<juce::uint32, std::shared_ptr<LearnBuffer>>> learnRetired; // freed a moment later: the audio thread may still be inside it
    // scope
    EngineTelemetry tel;
    int colCount = 0, colSamples = 88;
    float colLo = 0.0f, colHi = 0.0f;
    int colTrig = 0;
};
