#pragma once
#include "Engine.h"
#include "Library.h"
#include <juce_audio_processors/juce_audio_processors.h>

class OpenTriggerProcessor : public juce::AudioProcessor,
                             private juce::AsyncUpdater,
                             private juce::AudioProcessorValueTreeState::Listener
{
public:
    OpenTriggerProcessor();
    ~OpenTriggerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "OpenTrigger"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- slots (message thread only) ----
    enum class SlotStatus { empty, loading, ready, failed };
    struct SlotInfo
    {
        SlotStatus status = SlotStatus::empty;
        juce::File file;
        std::shared_ptr<const InstrumentData> data;
        std::shared_ptr<const SlotContent> content;   // decoded audio of the current mic (shared with the engine, read-only)
        int contentArticulation = 0; // articulation currently decoded in the engine
        float fundamentalHz = 0.0f;  // pitch the piece rings at (0 = unknown), measured from the decoded sound; basis of the Tune readout
        juce::String error;
    };
    SlotInfo slotInfo (int slot) const { return slotState[(size_t) slot].info; }
    void loadSlot (int slot, const juce::File&, bool keepLanes = false);

    // ---- mic lanes and outputs (message thread) ----
    // Each slot can play several mics of the same hit at once (lane 0 is the slot's own mic, set with setSlotArticulation).
    // Every lane goes to an output: 0 = Main, 1..7 = auxiliary outputs ("Out 2".."Out 8") the DAW can route to other tracks.
    struct LaneInfo { bool on = false, ready = false, mute = false, solo = false, phase = false; int articulation = 0, bus = 0; float gainDb = 0.0f, pan = 0.0f; };
    LaneInfo laneInfo (int slot, int lane) const;
    void setLaneOn (int slot, int lane, bool on);                 // lanes 1.. only (lane 0 is always on)
    void setLaneArticulation (int slot, int lane, int articulation);
    void setLaneBus (int slot, int lane, int bus);
    void setLaneGain (int slot, int lane, float gainDb);
    void setLanePan (int slot, int lane, float pan);              // -1 .. 1, added to the slot's pan
    void setLaneMute (int slot, int lane, bool on);
    void setLaneSolo (int slot, int lane, bool on);
    void setLanePhase (int slot, int lane, bool on);
    // Mixer, per mic (articulation) of the slot: the strip for that mic. A level above -60 dB puts the mic on a lane (decoding it);
    // at -60 dB it is off again. Returns false when all the lanes are taken.
    int laneOfArticulation (int slot, int articulation) const;    // -1 if that mic is not playing
    bool setMicLevel (int slot, int articulation, float gainDb);
    // Makes the mics of one stroke type, added together, sound like that type's Full Mix (the Mixed sound): lane 0 becomes the
    // Full Mix at silence and every other mic gets the calibrated level. Runs in the background.
    void calibrateMixed (int slot, const juce::String& group);
    void resetMixer (int slot);                                   // back to the plain Full Mix on Main
    juce::String mixerMessage() const { return mixerNote; }
    // Puts every mic of one stroke type (e.g. "Hit") on its own lane and output: lane 0 stays where it is, the other mics
    // go to Out 2, Out 3... Mic names come from the instrument ("Hit / Kick In" -> group "Hit").
    void splitMicsToOutputs (int slot, const juce::String& group);
    bool busEnabled (int bus) const;                              // is that output enabled in the host?
    static juce::String busName (int bus) { return bus <= 0 ? juce::String ("Main") : bus < kBuses ? "Out " + juce::String (bus + 1) : "Mono " + juce::String (bus - kBuses + 1); }
    void clearSlot (int slot);
    // Drives the s<N>_art parameter (0..63) and starts decoding that mic right away; with auditionWhenReady the slot plays
    // one hit the moment the new mic is in.
    void setSlotArticulation (int slot, int articulation, bool auditionWhenReady = false);
    int slotArticulation (int slot) const;
    void auditionSlot (int slot, int velocity = 100) { engine.auditionSlot (slot, velocity); }
    void auditionSample (std::shared_ptr<const juce::AudioBuffer<float>> b, double rate) { engine.audition (std::move (b), rate); }
    juce::ChangeBroadcaster slotsChanged;
    EngineTelemetry& telemetry() noexcept { return engine.telemetry(); }

    // ---- Smart Trigger (message thread) ----
    void startLearn (double seconds = 30.0) { engine.startLearn (seconds); }
    void stopLearn() { engine.stopLearn(); }
    std::shared_ptr<LearnBuffer> learnBuffer() const { return engine.learnBuffer(); }
    void applyLearn (const struct LearnResult&);          // sets the whole detection / gate / level chain from the analysis
    // Output volume (dB) that makes the loaded, unmuted slots as loud as the original hit; false if nothing is loaded.
    bool estimateOutputDb (struct LearnResult&) const;
    // Same, measured for real: plays the loaded slots (as the mixer has them, all outputs summed) over the audio Smart Trigger
    // just heard, with the proposed settings, and sets the volume so the kit hits are as loud as the recorded ones.
    bool calibrateOutputByRendering (struct LearnResult&, const float* audio, int numSamples) const;
    double currentSampleRate() const { return sampleRateNow; }

    static juce::String slotParam (int slot, const char* name) { return "s" + juce::String (slot + 1) + "_" + name; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String&, float) override;
    void handleAsyncUpdate() override;
    void syncArticulations();
    void syncLanes();
    void startLaneBuild (int slot, int lane);
    void resetLanes (int slot, bool keepConfig);
    void startBuild (int slot, std::shared_ptr<const InstrumentData>, int articulation, bool fromFile, juce::File file);
    void finishLoad (int slot, juce::uint64 gen, std::shared_ptr<InstrumentData>, std::shared_ptr<SlotContent>, juce::String error, juce::File file);
    TriggerEngine::Params snapshotParams() const;

    struct SlotState
    {
        SlotInfo info;
        juce::uint64 gen = 0;
        bool building = false;
    };
    double sampleRateNow = 44100.0;
    std::array<SlotState, kSlots> slotState;
    struct LaneCfg { std::atomic<int> art { 0 }, bus { 0 }; std::atomic<float> gainDb { 0.0f }, pan { 0.0f }; std::atomic<bool> on { false }, mute { false }, solo { false }, phase { false }; };
    juce::String mixerNote;
    struct LaneState { std::shared_ptr<const SlotContent> content; int contentArt = -1; bool building = false; juce::uint64 gen = 0; };
    std::array<std::array<LaneCfg, kLanes>, kSlots> laneCfg;
    std::array<std::array<LaneState, kLanes>, kSlots> laneState;
    std::array<bool, kSlots> auditionAfterBuild {};
    TriggerEngine engine;
    std::atomic<float>* raw (const juce::String& id) const { return apvts.getRawParameterValue (id); }
    std::array<std::atomic<float>*, kSlots> slotRaw[6] {}; // vol, pan, tune, mute, solo, phase
    std::atomic<float>* gRaw[16] {};
    JUCE_DECLARE_WEAK_REFERENCEABLE (OpenTriggerProcessor)
    struct Reaper : juce::Timer { std::function<void()> fn; void timerCallback() override { fn(); } } reaper;
    // Decoding runs on worker threads (never the audio or UI thread); the DAW's audio threads outrank them.
    // Last member: destroyed first, so no job outlives the processor.
    juce::ThreadPool pool { juce::ThreadPoolOptions().withNumberOfThreads (2).withThreadName ("OpenTrigger loader")
                                                    .withDesiredThreadPriority (juce::Thread::Priority::normal) };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OpenTriggerProcessor)
};
