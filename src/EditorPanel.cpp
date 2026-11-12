#include "EditorPanel.h"

namespace
{
const juce::Colour bg (0xff15181d), panel (0xff1d2128), cellBg (0xff252a33), accent (0xff4aa3df), warn (0xffe0793a);

bool isAudio (const juce::File& f)
{
    return f.hasFileExtension ("wav;wave;aif;aiff;flac;ogg;mp3");
}
bool isImage (const juce::File& f) { return f.hasFileExtension ("png;jpg;jpeg;gif"); }
} // namespace

// ------------------------------------------------------------------ grid
SampleGrid::SampleGrid (Draft& d) : draft (d)
{
    setWantsKeyboardFocus (true);
    updateSize();
}

void SampleGrid::updateSize()
{
    if (draft.articulations.empty()) { setSize (labelW + cellW, headH + cellH); return; }
    art = juce::jlimit (0, (int) draft.articulations.size() - 1, art);
    const auto& a = draft.articulations[(size_t) art];
    setSize (labelW + std::max (1, a.columns()) * cellW + 1, headH + (int) a.layers.size() * cellH + 1);
}

bool SampleGrid::hit (juce::Point<int> p, int& row, int& col) const
{
    row = col = -2;
    if (draft.articulations.empty()) return false;
    const auto& a = draft.articulations[(size_t) art];
    row = (p.y - headH) / cellH;
    col = (p.x - labelW) / cellW;
    if (p.y < headH) row = -1;
    if (p.x < labelW) col = -1;
    if (row >= (int) a.layers.size()) row = -2;
    if (col >= a.columns()) col = -2;
    return row != -2 && col != -2;
}

void SampleGrid::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    if (draft.articulations.empty()) return;
    const auto& a = draft.articulations[(size_t) art];
    g.setFont (12.0f);
    g.setColour (juce::Colours::grey);
    for (int c = 0; c < a.columns(); ++c)
        g.drawText ("RR " + juce::String (c + 1), labelW + c * cellW, 0, cellW, headH, juce::Justification::centred);

    for (int r = 0; r < (int) a.layers.size(); ++r)
    {
        const int y = headH + r * cellH;
        const auto& l = a.layers[(size_t) r];
        // velocity label (drag up/down to change)
        auto lab = juce::Rectangle<int> (2, y + 2, labelW - 6, cellH - 4);
        g.setColour (panel);
        g.fillRoundedRectangle (lab.toFloat(), 4.0f);
        g.setColour (r == 0 ? juce::Colours::grey : accent);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText (juce::String (l.velo), lab.removeFromTop (cellH / 2 + 2), juce::Justification::centred);
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("to " + juce::String (draft.veloTop (art, r)) + (r == 0 ? "" : "  (drag)"), lab, juce::Justification::centredTop);

        for (int c = 0; c < a.columns(); ++c)
        {
            auto cell = juce::Rectangle<int> (labelW + c * cellW + 2, y + 2, cellW - 4, cellH - 4);
            const auto& cc = l.cells[(size_t) c];
            g.setColour (cellBg);
            g.fillRoundedRectangle (cell.toFloat(), 4.0f);
            if (! cc.empty())
            {
                const auto& w = *cc.waves[0];
                auto wf = cell.reduced (4, 14).withTrimmedBottom (0);
                g.setColour (accent.withAlpha (0.8f));
                const int nb = (int) w.peaks.size();
                for (int x = 0; x < wf.getWidth() && nb > 0; ++x)
                {
                    const float pk = w.peaks[(size_t) (x * nb / wf.getWidth())];
                    const float h = std::max (1.0f, pk * wf.getHeight());
                    g.fillRect ((float) wf.getX() + x, wf.getCentreY() - h * 0.5f, 1.0f, h);
                }
                g.setColour (juce::Colours::white);
                g.setFont (11.0f);
                g.drawText (w.label + (cc.waves.size() > 1 ? " +" + juce::String ((int) cc.waves.size() - 1) : ""), cell.getX() + 4, cell.getY() + 1, cell.getWidth() - 8, 14, juce::Justification::left, true);
            }
            else
            {
                g.setColour (juce::Colours::grey.withAlpha (0.6f));
                g.setFont (11.0f);
                g.drawText ("drop WAV", cell, juce::Justification::centred);
            }
            if (r == selRow && c == selCol) { g.setColour (accent); g.drawRoundedRectangle (cell.toFloat(), 4.0f, 2.0f); }
            if (r == dropRow && c == dropCol) { g.setColour (warn); g.drawRoundedRectangle (cell.toFloat(), 4.0f, 2.0f); }
        }
    }
}

void SampleGrid::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    int r, c;
    if (! hit (e.getPosition(), r, c)) return;
    if (e.mods.isPopupMenu()) { menuFor (r, c); return; }
    if (c == -1 && r >= 0) // velocity label
    {
        dragLayer = r;
        dragStartVelo = draft.articulations[(size_t) art].layers[(size_t) r].velo;
        dragStartY = e.y;
        return;
    }
    if (r >= 0 && c >= 0)
    {
        selRow = r; selCol = c;
        auto& cell = draft.articulations[(size_t) art].layers[(size_t) r].cells[(size_t) c];
        if (! cell.empty() && onAudition) onAudition (cell.waves[0]);
        repaint();
    }
}

void SampleGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (dragLayer < 0) return;
    draft.setVelo (art, dragLayer, dragStartVelo + (dragStartY - e.y) / 2);
    repaint();
}

void SampleGrid::mouseUp (const juce::MouseEvent&)
{
    if (dragLayer >= 0 && onChanged) onChanged();
    dragLayer = -1;
}

void SampleGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    int r, c;
    if (hit (e.getPosition(), r, c) && r >= 0 && c >= 0) chooseFor (r, c);
}

bool SampleGrid::keyPressed (const juce::KeyPress& k)
{
    if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && selRow >= 0 && selCol >= 0)
    {
        draft.articulations[(size_t) art].layers[(size_t) selRow].cells[(size_t) selCol].waves.clear();
        if (onChanged) onChanged();
        repaint();
        return true;
    }
    return false;
}

void SampleGrid::menuFor (int r, int c)
{
    juce::PopupMenu m;
    m.addItem (1, "Choose sample...", r >= 0 && c >= 0);
    m.addItem (2, "Clear cell", r >= 0 && c >= 0);
    m.addSeparator();
    m.addItem (3, "Remove this velocity layer", r >= 1);
    m.addItem (4, "Remove this round-robin column", c >= 0);
    m.showMenuAsync ({}, [this, r, c] (int id)
    {
        if (id == 1) chooseFor (r, c);
        if (id == 2) draft.articulations[(size_t) art].layers[(size_t) r].cells[(size_t) c].waves.clear();
        if (id == 3) draft.removeLayer (art, r);
        if (id == 4) draft.removeColumn (art, c);
        if (id == 0 || id == 1) return;
        selRow = selCol = -1;
        updateSize();
        if (onChanged) onChanged();
        repaint();
    });
}

void SampleGrid::chooseFor (int r, int c)
{
    chooser = std::make_shared<juce::FileChooser> ("Choose samples (several files fill the next cells)", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this, r, c] (const juce::FileChooser& fc) { fillFrom (r, c, fc.getResults()); });
}

bool SampleGrid::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (isAudio (juce::File (f))) return true;
    return false;
}

void SampleGrid::fileDragMove (const juce::StringArray&, int x, int y)
{
    int r, c;
    if (! hit ({ x, y }, r, c) || r < 0 || c < 0) r = c = -1;
    if (r != dropRow || c != dropCol) { dropRow = r; dropCol = c; repaint(); }
}

void SampleGrid::filesDropped (const juce::StringArray& files, int x, int y)
{
    dropRow = dropCol = -1;
    int r, c;
    if (hit ({ x, y }, r, c) && r >= 0 && c >= 0)
    {
        juce::Array<juce::File> fs;
        for (auto& f : files) if (isAudio (juce::File (f))) fs.add (juce::File (f));
        fs.sort();
        fillFrom (r, c, fs);
    }
    repaint();
}

// Files fill cells left to right; overflow wraps to the next layer, and the grid grows if needed.
void SampleGrid::fillFrom (int row, int col, const juce::Array<juce::File>& files)
{
    auto& a = draft.articulations[(size_t) art];
    int r = row, c = col, loaded = 0;
    juce::String firstError;
    for (auto& f : files)
    {
        juce::String err;
        auto w = Draft::loadWave (f, err);
        if (w == nullptr) { if (firstError.isEmpty()) firstError = err; continue; }
        if (c >= a.columns()) { c = 0; ++r; }
        if (r >= (int) a.layers.size()) draft.addLayer (art);
        a.layers[(size_t) r].cells[(size_t) c].waves = { w };
        ++c; ++loaded;
        if (w->rate != 44100.0 && onMessage) onMessage ("Note: " + f.getFileName() + " is " + juce::String (w->rate, 0) + " Hz (44.1 kHz is recommended; it is resampled on playback).");
    }
    if (firstError.isNotEmpty() && onMessage) onMessage (firstError);
    updateSize();
    if (loaded > 0 && onChanged) onChanged();
    repaint();
}

// ----------------------------------------------------------------- panel
InstrumentEditorPanel::InstrumentEditorPanel() : grid (draft)
{
    for (auto* l : { &nameL, &libraryL, &artL })
    {
        l->setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (l);
    }
    nameL.setText ("Instrument", juce::dontSendNotification);
    libraryL.setText ("Library", juce::dontSendNotification);
    artL.setText ("Articulation", juce::dontSendNotification);
    for (auto* e : { &nameEd, &libraryEd, &artNameEd }) addAndMakeVisible (e);
    nameEd.onTextChange = [this] { draft.name = nameEd.getText(); };
    libraryEd.onTextChange = [this] { draft.library = libraryEd.getText(); };
    artNameEd.onTextChange = [this]
    {
        draft.articulations[(size_t) art].name = artNameEd.getText();
        artBox.changeItemText (art + 1, artNameEd.getText().isEmpty() ? "Hit" : artNameEd.getText());
        artBox.setText (artNameEd.getText().isEmpty() ? "Hit" : artNameEd.getText(), juce::dontSendNotification);
    };

    addAndMakeVisible (artBox);
    artBox.onChange = [this]
    {
        art = std::max (0, artBox.getSelectedItemIndex());
        grid.setArticulation (art);
        artNameEd.setText (draft.articulations[(size_t) art].name, juce::dontSendNotification);
    };
    addAndMakeVisible (addArt);
    addAndMakeVisible (delArt);
    addArt.onClick = [this]
    {
        auto a = Draft::createEmpty().articulations[0];
        a.name = "Articulation " + juce::String ((int) draft.articulations.size() + 1);
        const int cols = draft.articulations[0].columns();
        while (a.columns() < cols) for (auto& l : a.layers) l.cells.emplace_back();
        draft.articulations.push_back (std::move (a));
        art = (int) draft.articulations.size() - 1;
        refreshAll();
    };
    delArt.onClick = [this]
    {
        if (draft.articulations.size() <= 1) return;
        draft.articulations.erase (draft.articulations.begin() + art);
        art = std::max (0, art - 1);
        refreshAll();
    };

    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);
    grid.onChanged = [this] { grid.updateSize(); grid.repaint(); say ("Edited. Click Save to Library when ready."); };
    grid.onAudition = [this] (std::shared_ptr<DraftWave> w) { if (onAudition) onAudition (std::move (w)); };
    grid.onMessage = [this] (const juce::String& m) { say (m, m.startsWith ("Note") ? false : true); };

    addAndMakeVisible (addRow); addAndMakeVisible (delRow); addAndMakeVisible (addCol); addAndMakeVisible (delCol);
    addRow.onClick = [this] { draft.addLayer (art); refreshAll(); };
    delRow.onClick = [this] { draft.removeLayer (art, (int) draft.articulations[(size_t) art].layers.size() - 1); refreshAll(); };
    addCol.onClick = [this] { draft.addColumn (art); refreshAll(); };
    delCol.onClick = [this] { draft.removeColumn (art, draft.articulations[(size_t) art].columns() - 1); refreshAll(); };

    addAndMakeVisible (coverBtn);
    coverBtn.onClick = [this] { chooseCover(); };
    addAndMakeVisible (closeBtn);
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (saveBtn);
    saveBtn.onClick = [this] { save(); };

    help.setText ("Rows are velocity layers (drag the number to move the range). Columns are round robins, played at random. "
                  "Drop WAV files on the cells: several files fill the next cells. Click a cell to audition. 44.1 kHz, 16/24 bit WAV recommended.",
                  juce::dontSendNotification);
    help.setColour (juce::Label::textColourId, juce::Colours::grey);
    help.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (help);
    addAndMakeVisible (status);
}

void InstrumentEditorPanel::open (Draft d, juce::File existing)
{
    draft = std::move (d);
    editingFile = std::move (existing);
    art = 0;
    coverImage = draft.cover.getSize() > 0 ? juce::ImageFileFormat::loadFrom (draft.cover.getData(), draft.cover.getSize()) : juce::Image();
    nameEd.setText (draft.name, juce::dontSendNotification);
    libraryEd.setText (draft.library, juce::dontSendNotification);
    saveBtn.setButtonText (editingFile.existsAsFile() ? "Save changes" : "Save to Library");
    say ({});
    refreshAll();
}

void InstrumentEditorPanel::rebuildArticulations()
{
    artBox.clear (juce::dontSendNotification);
    for (size_t i = 0; i < draft.articulations.size(); ++i)
        artBox.addItem (draft.articulations[i].name.isEmpty() ? "Hit" : draft.articulations[i].name, (int) i + 1);
    artBox.setSelectedItemIndex (art, juce::dontSendNotification);
    artNameEd.setText (draft.articulations[(size_t) art].name, juce::dontSendNotification);
}

void InstrumentEditorPanel::refreshAll()
{
    rebuildArticulations();
    grid.setArticulation (art);
    grid.updateSize();
    repaint();
}

void InstrumentEditorPanel::say (const juce::String& m, bool error)
{
    status.setText (m, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, error ? warn : juce::Colours::lightgrey);
}

juce::Rectangle<int> InstrumentEditorPanel::coverRect() const
{
    return { getWidth() - 10 - 190, 10, 190, 190 };
}

void InstrumentEditorPanel::paint (juce::Graphics& g)
{
    g.fillAll (bg);
    auto r = coverRect();
    if (coverImage.isValid()) g.drawImageWithin (coverImage, r.getX(), r.getY(), r.getWidth(), r.getHeight(), juce::RectanglePlacement::centred);
    else
    {
        g.setColour (cellBg);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawFittedText ("Drop a cover image here\n(photo of the drum)", r.reduced (10), juce::Justification::centred, 3);
    }
}

void InstrumentEditorPanel::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto cover = r.removeFromRight (190);
    r.removeFromRight (10);
    coverBtn.setBounds (cover.removeFromTop (190 + 8).removeFromBottom (28));

    auto row = r.removeFromTop (24);
    nameL.setBounds (row.removeFromLeft (80));
    nameEd.setBounds (row.removeFromLeft (260));
    row.removeFromLeft (14);
    libraryL.setBounds (row.removeFromLeft (60));
    libraryEd.setBounds (row.removeFromLeft (180));
    r.removeFromTop (8);
    row = r.removeFromTop (24);
    artL.setBounds (row.removeFromLeft (80));
    artBox.setBounds (row.removeFromLeft (180));
    row.removeFromLeft (8);
    artNameEd.setBounds (row.removeFromLeft (160));
    row.removeFromLeft (8);
    addArt.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (6);
    delArt.setBounds (row.removeFromLeft (80));
    r.removeFromTop (8);
    help.setBounds (r.removeFromTop (44));
    r.removeFromTop (4);

    auto bottom = getLocalBounds().reduced (10).removeFromBottom (30);
    saveBtn.setBounds (bottom.removeFromRight (140));
    bottom.removeFromRight (8);
    closeBtn.setBounds (bottom.removeFromRight (90));
    status.setBounds (bottom);

    auto tools = r.removeFromBottom (r.getHeight() > 0 ? 0 : 0);
    juce::ignoreUnused (tools);
    r.removeFromBottom (36);
    auto bar = r.removeFromBottom (28);
    r.removeFromBottom (4);
    addRow.setBounds (bar.removeFromLeft (140));
    bar.removeFromLeft (6);
    delRow.setBounds (bar.removeFromLeft (80));
    bar.removeFromLeft (18);
    addCol.setBounds (bar.removeFromLeft (140));
    bar.removeFromLeft (6);
    delCol.setBounds (bar.removeFromLeft (110));
    viewport.setBounds (r);
}

bool InstrumentEditorPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (isImage (juce::File (f))) return true;
    return false;
}

void InstrumentEditorPanel::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files) if (isImage (juce::File (f))) { setCover (juce::File (f)); return; }
}

void InstrumentEditorPanel::chooseCover()
{
    chooser = std::make_shared<juce::FileChooser> ("Cover image", juce::File(), "*.png;*.jpg;*.jpeg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) setCover (fc.getResult()); });
}

void InstrumentEditorPanel::setCover (const juce::File& f)
{
    juce::String err;
    if (! draft.setCoverFromImage (juce::ImageFileFormat::loadFrom (f), err)) { say (f.getFileName() + ": " + err, true); return; }
    coverImage = juce::ImageFileFormat::loadFrom (draft.cover.getData(), draft.cover.getSize());
    say ("Cover set.");
    repaint();
}

void InstrumentEditorPanel::save()
{
    if (saving) return;
    juce::String err;
    if (! draft.validate (err)) { say (err, true); return; }
    auto target = Library::defaultFolder().getChildFile (juce::File::createLegalFileName (draft.name.trim()) + ".otci");
    if (target.existsAsFile() && target != editingFile)
    {
        juce::Component::SafePointer<InstrumentEditorPanel> self (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Replace instrument?",
                                          "\"" + target.getFileName() + "\" already exists in the library.", "Replace", "Cancel", this),
                                      [self, target] (int ok) { if (ok && self != nullptr) self->startSave (target); });
        return;
    }
    startSave (target);
}

void InstrumentEditorPanel::startSave (const juce::File& target)
{
    if (saving) return;
    saving = true;
    setEnabled (false); // the draft is copied below; block edits until the save lands
    say ("Saving...");
    juce::Component::SafePointer<InstrumentEditorPanel> self (this);
    pool.addJob ([self, target, snapshot = draft]
    {
        otci::Instrument inst;
        juce::String err;
        bool ok = snapshot.toInstrument (inst, err);
        if (ok)
        {
            target.getParentDirectory().createDirectory();
            ok = otci::write (target, inst, err);
        }
        juce::MessageManager::callAsync ([self, ok, err, target]
        {
            if (self != nullptr) self->finishSave (ok, err, target);
        });
    });
}

void InstrumentEditorPanel::finishSave (bool ok, const juce::String& error, const juce::File& target)
{
    saving = false;
    setEnabled (true);
    if (! ok) { say ("Save failed: " + error, true); return; }
    // Renamed an existing instrument: drop the old file so the library has no stale duplicate.
    if (editingFile.existsAsFile() && editingFile != target) editingFile.moveToTrash();
    editingFile = target;
    say ("Saved.");
    if (onSaved) onSaved (target);
}
