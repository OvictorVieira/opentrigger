#pragma once
#include "Draft.h"
#include "Library.h"
#include <juce_gui_basics/juce_gui_basics.h>

// Rows = velocity layers (drag the number), columns = round robins. Drop WAVs on cells.
class SampleGrid : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    SampleGrid (Draft&);
    void setArticulation (int a) { art = a; selRow = selCol = -1; updateSize(); repaint(); }
    void updateSize();
    std::function<void()> onChanged;
    std::function<void (std::shared_ptr<DraftWave>)> onAudition;
    std::function<void (const juce::String&)> onMessage;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override { dropRow = dropCol = -1; repaint(); }
    void filesDropped (const juce::StringArray&, int x, int y) override;

    static constexpr int labelW = 104, cellW = 124, cellH = 56, headH = 24;

private:
    bool hit (juce::Point<int>, int& row, int& col) const;
    void fillFrom (int row, int col, const juce::Array<juce::File>&);
    void chooseFor (int row, int col);
    void menuFor (int row, int col);

    Draft& draft;
    int art = 0, selRow = -1, selCol = -1, dropRow = -1, dropCol = -1;
    int dragLayer = -1, dragStartVelo = 0, dragStartY = 0;
    std::shared_ptr<juce::FileChooser> chooser;
};

class InstrumentEditorPanel : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    InstrumentEditorPanel();
    // Start with an empty draft or a copy of an existing instrument.
    void open (Draft, juce::File existingFile);
    void save();
    Draft& getDraft() { return draft; }
    SampleGrid& getGrid() { return grid; }

    std::function<void (std::shared_ptr<DraftWave>)> onAudition;
    std::function<void (const juce::File& saved)> onSaved;
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    void rebuildArticulations();
    void refreshAll();
    void startSave (const juce::File& target);
    void finishSave (bool ok, const juce::String& error, const juce::File& target);
    void chooseCover();
    void setCover (const juce::File&);
    void say (const juce::String&, bool error = false);
    juce::Rectangle<int> coverRect() const;

    Draft draft = Draft::createEmpty(); // must be non-empty before the grid is constructed
    juce::File editingFile;
    int art = 0;
    juce::Image coverImage;

    juce::TextEditor nameEd, libraryEd, artNameEd;
    juce::Label nameL, libraryL, artL;
    juce::ComboBox artBox;
    juce::TextButton addArt { "+ Articulation" }, delArt { "Remove" };
    SampleGrid grid;
    juce::Viewport viewport;
    juce::TextButton addRow { "+ Velocity Layer" }, delRow { "- Layer" }, addCol { "+ Round Robin" }, delCol { "- Round Robin" };
    juce::TextButton coverBtn { "Cover image..." }, closeBtn { "Cancel" }, saveBtn { "Save to Library" };
    juce::Label help, status;
    std::shared_ptr<juce::FileChooser> chooser;
    bool saving = false;
    juce::ThreadPool pool { 1 }; // last member: destroyed first, so no job outlives the widgets
};
