#pragma once
#include "PluginProcessor.h"
#include "UiKit.h"
#include <map>

// Articulation names may be "<Group> / <Label>" (e.g. "Cross Stick / Top (Batter Head)").
struct ArticulationName
{
    juce::String group, label;
    static ArticulationName parse (const juce::String& n)
    {
        const int i = n.indexOf (" / ");
        return i < 0 ? ArticulationName { {}, n } : ArticulationName { n.substring (0, i), n.substring (i + 3) };
    }
};

// One of the 8 slots in the left column: picture (click = hear it), name, mic, L/E/clear, M/S/phase, volume.
class SlotCard : public juce::Component, private juce::Timer
{
public:
    SlotCard (OpenTriggerProcessor&, int index);
    void refresh();                 // re-read slot state from the processor
    void setSelected (bool s) { selected = s; repaint(); }
    std::function<void (int)> onSelect, onBrowse, onEdit, onClear, onUnlock;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    juce::Rectangle<int> thumbRect() const { return { 6, 7, 64, 64 }; }

private:
    void timerCallback() override;
    OpenTriggerProcessor& proc;
    const int idx;
    bool selected = false, hover = false;
    float flash = 0.0f;
    unsigned lastHits = 0;
    OpenTriggerProcessor::SlotInfo info;
    juce::TextButton lBtn { "L" }, eBtn { "E" }, xBtn { juce::CharPointer_UTF8 ("\xc3\x97") };
    juce::TextButton mBtn { "M" }, sBtn { "S" }, pBtn { juce::CharPointer_UTF8 ("\xc3\x98") };
    juce::Slider vol;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mAtt, sAtt, pAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> vAtt;
};

// Bottom-centre panel for the selected slot: pick the mic / articulation (click = hear it), pan, tune.
class SlotDetail : public juce::Component, private juce::Timer
{
public:
    explicit SlotDetail (OpenTriggerProcessor&);
    void setSlot (int s);
    void refresh();
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    int chipCount() const { return (int) chips.size(); }
    // Stroke Type menu (shown when the piece has more than one playing style, e.g. Hit / Cross Stick / VeloSplit).
    bool strokeMode() const { return groups.size() > 1; }
    const std::vector<juce::String>& strokeTypes() const { return groups; }
    juce::String strokeType() const { return activeGroup; }
    // MICS: pick the slot's mic. MIXER: a channel strip per mic of the piece (fader, pan, mute/solo, polarity, output).
    enum class View { mics, mixer };
    View view() const { return viewMode; }
    void setView (View v);
    juce::Rectangle<int> viewTabRect (View v) const { return v == View::mics ? tabMics : tabOuts; }
    struct Strip { int art = 0; juce::Rectangle<int> area, pan, mute, solo, phase, fader, name, out; };
    int stripCount() const { return (int) strips.size(); }
    const Strip& strip (int i) const { return strips[(size_t) i]; }
    juce::Rectangle<int> mixedButtonRect() const { return mixedBtn; }
    juce::Rectangle<int> resetButtonRect() const { return resetBtn; }
    static float yToDb (const juce::Rectangle<int>& fader, int y);   // -60 (bottom, off) .. +6
    static int dbToY (const juce::Rectangle<int>& fader, float db);
    juce::Rectangle<int> strokeBoxRect() const { return strokeBox; }
    void selectStroke (const juce::String& group);   // switches to that style (same mic if it has it) and plays it
    juce::Rectangle<int> chipRect (int i) const { return chips[(size_t) i]; }

private:
    void timerCallback() override;
    void layoutChips();
    void showBusMenu (int lane);
    View viewMode = View::mics;
    juce::Rectangle<int> tabMics, tabOuts, mixedBtn, resetBtn;
    std::vector<Strip> strips;
    int dragStrip = -1, dragKind = 0, dragStartY = 0;
    float dragStartValue = 0.0f;
    OpenTriggerProcessor& proc;
    int slot = 0, shownArt = -1;
    OpenTriggerProcessor::SlotInfo info;
    std::vector<juce::Rectangle<int>> chips;                       // chips[i] = articulation i
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> headings; // group headings between chips
    juce::Rectangle<int> chipArea;
    std::vector<juce::String> groups;                                 // stroke types in order of first appearance
    juce::Rectangle<int> strokeBox;
    juce::String activeGroup;
    std::unique_ptr<ParamKnob> pan[kSlots], tune[kSlots];
};

// Modal: grid of pictures, pick one, OK loads it into the target slot.
class BrowserOverlay : public juce::Component, private juce::Timer
{
public:
    BrowserOverlay();
    void open (const Library& lib, int targetSlot, juce::File current);
    std::function<void (const juce::File&, int slot)> onChoose;
    // Plays the sound of the selected piece (no loading needed). Set by the editor.
    std::function<void (std::shared_ptr<const juce::AudioBuffer<float>>, double rate)> onPreview;
    std::function<void()> onCancel, onRescan, onOpenFolder, onImport;
    // Export the pieces shown (the selected library, or all) as one pack; name = library name or "OpenTrigger Library".
    std::function<void (const juce::Array<juce::File>& files, const juce::String& packName)> onExport;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    int visibleCards() const { return (int) cardRects.size(); }
    juce::Rectangle<int> cardRect (int i) const { return cardRects[(size_t) i]; }
    void selectVisible (int i) { select (page * kPerPage + i); }
    // A hit on a visible card: selects it, or if it already is the selected one, plays it and animates the picture.
    // yFrac: 0 = top of the picture (hard hit) .. 1 = bottom (soft hit).
    void hitCard (int visibleIndex, float yFrac);
    bool isAnimating() const { return animCard >= 0; }
    bool previewReady() const { return soft.buf != nullptr && hard.buf != nullptr; }
    int previewsPlayed() const { return played; }
    void choose();
    static constexpr int kPerPage = 8;

private:
    std::vector<int> filtered() const;
    int cardAt (juce::Point<int>) const;
    void select (int filteredIndex);
    void layoutCards();
    void timerCallback() override;
    struct Pre { std::shared_ptr<juce::AudioBuffer<float>> buf; double rate = 44100.0; bool loading = false; };
    void prefetch (bool hardOne);
    void play (bool hardOne);
    std::map<juce::String, std::shared_ptr<const InstrumentData>> meta; // message thread: parsed once per piece
    Pre soft, hard;
    juce::String prefetchFor;               // file the buffers belong to
    bool wantPlay = false, wantHard = false;
    int animCard = -1, played = 0;
    double animStart = 0.0;
    juce::Random rng;
    const Library* lib = nullptr;
    int target = 0, page = 0, sel = -1;
    std::vector<juce::Rectangle<int>> cardRects, dotRects;
    juce::Rectangle<int> panelRect, infoRect, gridRect;
    juce::ComboBox filter;
    juce::TextButton okBtn { "Ok" }, cancelBtn { "Cancel" }, rescanBtn { "Rescan" }, folderBtn { "Open Folder" }, importBtn { "Import..." }, exportBtn { "Export..." };
    juce::ThreadPool pool { 1 }; // last member: destroyed first, so no job outlives the overlay
};
