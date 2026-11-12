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
        juce::String error;
    };
    SlotInfo slotInfo (int slot) const { return slotState[(size_t) slot].info; }
    void loadSlot (int slot, const juce::File&);
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
    double currentSampleRate() const { return sampleRateNow; }

    static juce::String slotParam (int slot, const char* name) { return "s" + juce::String (slot + 1) + "_" + name; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String&, float) override;
    void handleAsyncUpdate() override;
    void syncArticulations();
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
