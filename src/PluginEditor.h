#pragma once
#include "EditorPanel.h"
#include "PluginProcessor.h"
#include "SlotUi.h"
#include "SmartUi.h"
#include "UiKit.h"

class OpenTriggerEditor : public juce::AudioProcessorEditor, private juce::ChangeListener
{
public:
    explicit OpenTriggerEditor (OpenTriggerProcessor&);
    ~OpenTriggerEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kW = 1280, kH = 720; // design size; the window scales it

    // ---- used by the tests ----
    SlotCard& card (int i) { return *cards[(size_t) i]; }
    SlotDetail& detail() { return *slotDetail; }
    ScopeView& scope() { return *scopeView; }
    ParamKnob& thresholdKnob() { return *threshKnob; }
    BrowserOverlay& browser() { return browserOverlay; }
    SmartTriggerPanel& smart() { return smartPanel; }
    InstrumentEditorPanel& instrumentEditor() { return editorPanel; }
    juce::Component& contentComponent() { return content; }
    int selectedSlot() const { return selected; }
    void selectSlot (int);
    void openBrowser (int slot);
    void startEdit (int slot);
    // Library packs. All asynchronous; the result is shown in a dialog and the library list refreshes.
    void importPack (const juce::File& file, const juce::String& password = {});
    void exportPack (const juce::Array<juce::File>& files, const juce::String& packName, const juce::File& dest, const juce::String& password);
    void unlockSlot (int slot, const juce::String& password); // pass phrase for a protected instrument, then loads it
    juce::String lastReport() const { return report; }
    bool busy() const { return working > 0; }

private:
    struct Content : juce::Component
    {
        std::function<void (juce::Graphics&)> painter;
        void paint (juce::Graphics& g) override { if (painter) painter (g); }
    };
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void paintContent (juce::Graphics&);
    void rescan();
    void showEditor (Draft, juce::File existing);
    void say (const juce::String&);
    void startImport();
    void startExport (const juce::Array<juce::File>&, const juce::String& packName);
    void promptPassword (int slot);
    void showMessage (const juce::String& title, const juce::String& text);
    std::shared_ptr<juce::FileChooser> chooser;
    juce::String report;
    std::atomic<int> working { 0 };
    juce::StringArray promptedFiles;

    OtLook look;
    OpenTriggerProcessor& proc;
    std::shared_ptr<const Library> libHold;   // what the browser shows; refreshed in the background
    Library emptyLib;
    int selected = 0;
    juce::String status;

    Content content;
    juce::TextButton browseBtn { "Browser" }, newBtn { "New" }, editBtn { "Edit" }, smartBtn { "Smart Trigger" };
    juce::ComboBox sourceBox;
    juce::Slider midiNote;
    juce::Label sourceLabel, noteLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noteAtt;

    juce::TextButton gateBtn { "Off" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateAtt;
    std::unique_ptr<ParamKnob> threshKnob, gateAtk, gateHold, gateRel, inVol, lowcut, highcut, suppress, sens, retrig, detailKnob, outVol, mixKnob;
    std::unique_ptr<MeterView> inMeter, outMeter;
    std::unique_ptr<ScopeView> scopeView;
    std::array<std::unique_ptr<SlotCard>, kSlots> cards;
    std::unique_ptr<SlotDetail> slotDetail;
    SmartTriggerPanel smartPanel;
    BrowserOverlay browserOverlay;
    InstrumentEditorPanel editorPanel;
    juce::Rectangle<int> gatePanel, inPanel, filtPanel, scopePanel, trigPanel, outPanel;
    juce::ThreadPool pool { 1 }; // last member: destroyed first
};
