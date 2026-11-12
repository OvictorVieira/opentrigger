#pragma once
#include "Engine.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace ui
{
const juce::Colour bg (0xff12151a), panel (0xff1a1e25), panelHi (0xff232935), line (0xff2d3441), text (0xffdde3ee),
                   dim (0xff8791a3), accent (0xff4aa3ff), orange (0xfff0a040), green (0xff5ad17a), red (0xffe0584f);
juce::Font font (float h, bool bold = false);
void drawPanel (juce::Graphics&, juce::Rectangle<float>, const juce::String& title = {});
}

class OtLook : public juce::LookAndFeel_V4
{
public:
    OtLook();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool hover, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool hover, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool, int, int, int, int, juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return ui::font (13.0f, true); }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return ui::font (13.0f); }
    juce::Font getLabelFont (juce::Label&) override { return ui::font (12.0f); }
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;
};

// Rotary knob + title + value readout, bound to a parameter.
class ParamKnob : public juce::Component
{
public:
    ParamKnob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& title, bool bipolar = false);
    void resized() override;
    juce::Slider slider;
private:
    juce::Label caption;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
};

// Stereo peak meter fed from engine telemetry.
class MeterView : public juce::Component, private juce::Timer
{
public:
    MeterView (std::atomic<float>* l, std::atomic<float>* r);
    void paint (juce::Graphics&) override;
private:
    void timerCallback() override;
    std::atomic<float>* peak[2];
    float level[2] { 0, 0 }, hold[2] { 0, 0 };
};

// Scrolling input waveform with draggable threshold and trigger markers (the "see what the plugin hears" view).
class ScopeView : public juce::Component, private juce::Timer
{
public:
    ScopeView (juce::AudioProcessorValueTreeState&, const EngineTelemetry&);
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    static float shape (float amp) { return std::copysign (std::sqrt (std::abs (amp)), amp); }  // display curve
    static float unshape (float v) { return v * std::abs (v); }
    float thresholdY() const; // y of the upper threshold line (for tests)
    static constexpr int kShownColumns = 800;
private:
    void timerCallback() override;
    void setThresholdFromY (float y);
    bool nearThreshold (float y) const;
    juce::AudioProcessorValueTreeState& apvts;
    const EngineTelemetry& tel;
    juce::RangedAudioParameter* thr;
    bool dragging = false, hover = false;
    int lastHead = -1;
    float lastThr = 1000.0f;
};
