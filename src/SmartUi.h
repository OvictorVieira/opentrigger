#pragma once
#include "PluginProcessor.h"
#include "SmartTrigger.h"
#include "UiKit.h"

// Smart Trigger dialog: explains what to do, listens while the DAW plays, shows what it understood, applies it.
class SmartTriggerPanel : public juce::Component, private juce::Timer
{
public:
    explicit SmartTriggerPanel (OpenTriggerProcessor&);
    enum class State { intro, listening, result, failed };
    void open();                              // shows the panel in its intro state
    State state() const { return st; }
    const LearnResult& result() const { return res; }
    void startListening();                    // = pressing Start
    void stopAndAnalyze();                    // = pressing Stop
    void apply();                             // = pressing Apply
    std::function<void()> onClose;
    std::function<void (const LearnResult&)> onApplied;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void finishAnalysis();
    void drawWave (juce::Graphics&, juce::Rectangle<int>, bool withMarkers);
    OpenTriggerProcessor& proc;
    State st = State::intro;
    LearnResult res;
    std::shared_ptr<LearnBuffer> buf;
    int liveHits = 0, tick = 0, stopAt = -1;
    juce::Rectangle<int> card;
    juce::TextButton startBtn { "Start" }, stopBtn { "Stop and analyze" }, applyBtn { "Apply" }, againBtn { "Try again" }, closeBtn { "Close" };
};
