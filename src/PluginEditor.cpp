#include "PluginEditor.h"
#include "Pack.h"

OpenTriggerEditor::OpenTriggerEditor (OpenTriggerProcessor& p) : AudioProcessorEditor (&p), proc (p), smartPanel (p)
{
    setLookAndFeel (&look);
    addAndMakeVisible (content);
    content.setBounds (0, 0, kW, kH);
    content.painter = [this] (juce::Graphics& g) { paintContent (g); };

    // ---- header
    for (auto* b : { &browseBtn, &newBtn, &editBtn, &smartBtn }) content.addAndMakeVisible (*b);
    smartBtn.setColour (juce::TextButton::buttonColourId, ui::accent.darker (0.35f));
    smartBtn.onClick = [this] { smartPanel.open(); };
    browseBtn.onClick = [this] { openBrowser (selected); };
    newBtn.onClick = [this] { showEditor (Draft::createEmpty(), {}); };
    editBtn.onClick = [this] { startEdit (selected); };
    content.addAndMakeVisible (sourceBox);
    sourceBox.addItemList (proc.apvts.getParameter ("source")->getAllValueStrings(), 1);
    sourceAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "source", sourceBox);
    midiNote.setSliderStyle (juce::Slider::IncDecButtons);
    midiNote.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 24);
    content.addAndMakeVisible (midiNote);
    noteAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "midiNote", midiNote);
    for (auto* l : { &sourceLabel, &noteLabel }) { content.addAndMakeVisible (*l); l->setFont (ui::font (11.0f, true)); l->setColour (juce::Label::textColourId, ui::dim); }
    sourceLabel.setText ("TRIGGER", juce::dontSendNotification);
    noteLabel.setText ("MIDI NOTE", juce::dontSendNotification);

    // ---- signal chain (left to right, like a trigger plug-in: gate > input > filters > display > detection > output)
    gateBtn.setClickingTogglesState (true);
    gateAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "gateOn", gateBtn);
    gateBtn.onStateChange = [this] { gateBtn.setButtonText (gateBtn.getToggleState() ? "On" : "Off"); };
    gateBtn.setButtonText (gateBtn.getToggleState() ? "On" : "Off");
    content.addAndMakeVisible (gateBtn);
    auto knob = [&] (std::unique_ptr<ParamKnob>& k, const char* id, const char* title)
    {
        k = std::make_unique<ParamKnob> (proc.apvts, id, title);
        content.addAndMakeVisible (*k);
    };
    knob (gateAtk, "gateAttack", "Attack");
    knob (gateHold, "gateHold", "Hold");
    knob (gateRel, "gateRelease", "Release");
    knob (inVol, "inGain", "Volume");
    knob (lowcut, "lowcut", "Low cut");
    knob (highcut, "highcut", "High cut");
    knob (suppress, "suppress", "Suppress");
    knob (threshKnob, "threshold", "Threshold");
    knob (sens, "sensitivity", "Sensitivity");
    knob (retrig, "holdoff", "Retrigger");
    knob (detailKnob, "detail", "Detail");
    knob (outVol, "gain", "Volume");
    knob (mixKnob, "mix", "Mix");
    auto& tel = proc.telemetry();
    inMeter = std::make_unique<MeterView> (&tel.inPeak[0], &tel.inPeak[1]);
    outMeter = std::make_unique<MeterView> (&tel.outPeak[0], &tel.outPeak[1]);
    content.addAndMakeVisible (*inMeter);
    content.addAndMakeVisible (*outMeter);
    scopeView = std::make_unique<ScopeView> (proc.apvts, tel);
    content.addAndMakeVisible (*scopeView);

    // ---- slots
    for (int i = 0; i < kSlots; ++i)
    {
        cards[(size_t) i] = std::make_unique<SlotCard> (proc, i);
        auto& c = *cards[(size_t) i];
        c.onSelect = [this] (int s) { selectSlot (s); };
        c.onBrowse = [this] (int s) { selectSlot (s); openBrowser (s); };
        c.onEdit = [this] (int s) { selectSlot (s); startEdit (s); };
        c.onClear = [this] (int s) { proc.clearSlot (s); };
        c.onUnlock = [this] (int s) { promptPassword (s); };
        content.addAndMakeVisible (c);
    }
    slotDetail = std::make_unique<SlotDetail> (proc);
    content.addAndMakeVisible (*slotDetail);

    // ---- overlays
    content.addChildComponent (smartPanel);
    smartPanel.onApplied = [this] (const LearnResult& r) { say ("Smart Trigger applied. " + r.message); };
    content.addChildComponent (browserOverlay);
    browserOverlay.onCancel = [this] { browserOverlay.setVisible (false); };
    browserOverlay.onPreview = [this] (std::shared_ptr<const juce::AudioBuffer<float>> b, double rate) { proc.auditionSample (std::move (b), rate); };
    browserOverlay.onRescan = [this]
    {
        rescan();
        if (libHold) browserOverlay.open (*libHold, selected, proc.slotInfo (selected).file);
    };
    browserOverlay.onImport = [this] { startImport(); };
    browserOverlay.onExport = [this] (const juce::Array<juce::File>& f, const juce::String& n) { startExport (f, n); };
    browserOverlay.onOpenFolder = [] { Library::defaultFolder().createDirectory(); Library::defaultFolder().revealToUser(); };
    browserOverlay.onChoose = [this] (const juce::File& f, int slot)
    {
        browserOverlay.setVisible (false);
        proc.loadSlot (slot, f);
        selectSlot (slot);
        say ("Loading " + f.getFileNameWithoutExtension() + "...");
    };
    content.addChildComponent (editorPanel);
    editorPanel.onClose = [this] { editorPanel.setVisible (false); };
    editorPanel.onAudition = [this] (std::shared_ptr<DraftWave> w) { if (w && w->audio) proc.auditionSample (w->audio, w->rate); };
    editorPanel.onSaved = [this] (const juce::File& f)
    {
        editorPanel.setVisible (false);
        rescan();
        proc.loadSlot (selected, f);
        say ("Saved " + f.getFileName() + " into slot " + juce::String (selected + 1));
    };

    proc.slotsChanged.addChangeListener (this);
    rescan();
    selectSlot (0);
    setResizable (true, true);
    setResizeLimits (800, 450, 2560, 1440);
    getConstrainer()->setFixedAspectRatio ((double) kW / (double) kH);
    setSize (1152, 648);
}

OpenTriggerEditor::~OpenTriggerEditor()
{
    proc.slotsChanged.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void OpenTriggerEditor::say (const juce::String& s)
{
    status = s;
    content.repaint();
}

void OpenTriggerEditor::rescan()
{
    // never scan here: it reads every instrument file and would freeze the host while the window opens
    auto folder = Library::defaultFolder();
    folder.createDirectory();
    libHold = LibraryCache::get().current();
    if (libHold) say (juce::String ((int) libHold->entries().size()) + " kit pieces in the library");
    else say ("Reading the library...");
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    LibraryCache::get().refresh (folder, [self]
    {
        if (self == nullptr) return;
        self->libHold = LibraryCache::get().current();
        if (self->libHold)
        {
            self->say (juce::String ((int) self->libHold->entries().size()) + " kit pieces in the library");
            if (self->browserOverlay.isVisible()) self->browserOverlay.open (*self->libHold, self->selected, self->proc.slotInfo (self->selected).file);
        }
    });
}

void OpenTriggerEditor::selectSlot (int s)
{
    selected = juce::jlimit (0, kSlots - 1, s);
    for (int i = 0; i < kSlots; ++i) cards[(size_t) i]->setSelected (i == selected);
    slotDetail->setSlot (selected);
    content.repaint();
}

void OpenTriggerEditor::openBrowser (int slot)
{
    rescan();
    browserOverlay.open (libHold ? *libHold : emptyLib, slot, proc.slotInfo (slot).file);
}

void OpenTriggerEditor::showEditor (Draft d, juce::File existing)
{
    editorPanel.open (std::move (d), std::move (existing));
    editorPanel.setVisible (true);
    editorPanel.toFront (true);
}

void OpenTriggerEditor::startEdit (int slot)
{
    const auto info = proc.slotInfo (slot);
    if (info.data == nullptr) { say ("Load a kit piece into the slot first, or press New."); return; }
    say ("Opening " + info.file.getFileNameWithoutExtension() + " in the editor...");
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    const auto file = info.file;
    pool.addJob ([self, file]
    {
        juce::String err;
        auto li = loadInstrument (file, err); // the editor works on fully decoded audio
        juce::MessageManager::callAsync ([self, li, err, file]
        {
            if (self == nullptr) return;
            if (li == nullptr) { self->say ("Cannot open for editing: " + err); return; }
            self->showEditor (Draft::fromLoaded (*li), file);
            self->say ("Editing " + li->meta.name);
        });
    });
}

void OpenTriggerEditor::showMessage (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::InfoIcon).withTitle (title).withMessage (text).withButton ("OK"), nullptr);
}

void OpenTriggerEditor::unlockSlot (int slot, const juce::String& password)
{
    const auto info = proc.slotInfo (slot);
    juce::String err;
    auto meta = otci::read (info.file, false, err);
    if (! meta || ! meta->enc.active()) { say ("Cannot unlock: " + err); return; }
    if (! otci::unlockWithPassword (meta->enc, password, err)) { say ("Pass phrase not accepted: " + err); return; }
    proc.loadSlot (slot, info.file);
    say ("Unlocked. Loading...");
}

void OpenTriggerEditor::promptPassword (int slot)
{
    const auto info = proc.slotInfo (slot);
    if (info.file == juce::File()) return;
    auto* aw = new juce::AlertWindow ("Protected instrument", info.file.getFileNameWithoutExtension() + " is protected with a pass phrase. Enter it to use it in this session.", juce::MessageBoxIconType::QuestionIcon);
    aw->addTextEditor ("pass", "", "Pass phrase", true);
    aw->addButton ("Unlock", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, aw, slot] (int r)
    {
        if (r == 1 && self != nullptr) self->unlockSlot (slot, aw->getTextEditorContents ("pass"));
    }), true);
}

void OpenTriggerEditor::importPack (const juce::File& file, const juce::String& password)
{
    say ("Importing " + file.getFileName() + "...");
    ++working;
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    const auto dest = Library::defaultFolder();
    pool.addJob ([self, file, password, dest]
    {
        dest.createDirectory();
        auto r = otpack::import (file, dest, password);
        juce::MessageManager::callAsync ([self, r]
        {
            if (self == nullptr) return;
            --self->working;
            self->report = r.message + "\n" + r.lines.joinIntoString ("\n");
            self->say ((r.ok ? "Imported: " : "Import problem: ") + r.message);
            self->rescan();
            self->showMessage (r.ok ? "Library imported" : "Import finished with problems", self->report);
        });
    });
}

void OpenTriggerEditor::exportPack (const juce::Array<juce::File>& files, const juce::String& packName, const juce::File& dest, const juce::String& password)
{
    say ("Exporting " + packName + "...");
    ++working;
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    pool.addJob ([self, files, packName, dest, password]
    {
        otci::Protection prot;
        prot.mode = password.isNotEmpty() ? otci::Encryption::password : otci::Encryption::sealed;
        prot.password = password;
        juce::String err;
        const bool ok = otpack::create (dest, packName, files, prot, err);
        juce::MessageManager::callAsync ([self, ok, err, dest, password]
        {
            if (self == nullptr) return;
            --self->working;
            self->report = ok ? "Pack written: " + dest.getFullPathName() : "Export failed: " + err;
            self->say (self->report);
            self->showMessage (ok ? "Pack exported" : "Export failed",
                               ok ? dest.getFullPathName() + "\n\n" + (password.isNotEmpty() ? "Protected with your pass phrase: only people who know it can use the instruments." : "Sealed: it opens by itself in any OpenTrigger. The audio is encrypted inside the file, but this is not secret protection (the source is open).") : err);
        });
    });
}

void OpenTriggerEditor::startImport()
{
    chooser = std::make_shared<juce::FileChooser> ("Import a library pack or instrument", juce::File(), "*.otpack;*.otci");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, safe = juce::Component::SafePointer<OpenTriggerEditor> (this)] (const juce::FileChooser& fc)
                          { if (safe && fc.getResult().existsAsFile()) importPack (fc.getResult()); });
}

void OpenTriggerEditor::startExport (const juce::Array<juce::File>& files, const juce::String& packName)
{
    if (files.isEmpty()) { say ("Nothing to export."); return; }
    auto* aw = new juce::AlertWindow ("Export " + packName, "Exports " + juce::String (files.size()) + " instruments as one pack with the audio encrypted. Leave the pass phrase empty for a sealed pack that opens by itself in any OpenTrigger, or choose a pass phrase to lock it.", juce::MessageBoxIconType::QuestionIcon);
    aw->addTextEditor ("pass", "", "Pass phrase (optional)", true);
    aw->addButton ("Choose file...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<OpenTriggerEditor> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, aw, files, packName] (int r)
    {
        if (r != 1 || self == nullptr) return;
        const auto pass = aw->getTextEditorContents ("pass");
        self->chooser = std::make_shared<juce::FileChooser> ("Save the pack", juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (juce::File::createLegalFileName (packName) + otpack::kExtension), "*.otpack");
        self->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                    [self, files, packName, pass] (const juce::FileChooser& fc)
                                    { if (self && fc.getResult() != juce::File()) self->exportPack (files, packName, fc.getResult().withFileExtension ("otpack"), pass); });
    }), true);
}

void OpenTriggerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    for (auto& c : cards) c->refresh();
    slotDetail->refresh();
    for (int i = 0; i < kSlots; ++i)   // a protected instrument that needs its pass phrase asks once per file
    {
        const auto si = proc.slotInfo (i);
        if (si.status == OpenTriggerProcessor::SlotStatus::failed && si.error.startsWith ("password needed")
            && ! promptedFiles.contains (si.file.getFullPathName()) && ! browserOverlay.isVisible())
        {
            promptedFiles.add (si.file.getFullPathName());
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<OpenTriggerEditor> (this), i] { if (safe) safe->promptPassword (i); });
        }
    }
    const auto info = proc.slotInfo (selected);
    if (info.status == OpenTriggerProcessor::SlotStatus::ready) say (info.data->meta.name + " ready in slot " + juce::String (selected + 1));
    else if (info.status == OpenTriggerProcessor::SlotStatus::failed) say ("Could not load: " + info.error);
    content.repaint();
}

void OpenTriggerEditor::paint (juce::Graphics& g) { g.fillAll (ui::bg); }

void OpenTriggerEditor::resized()
{
    content.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) kW));
    content.setBounds (0, 0, kW, kH);

    browseBtn.setBounds (210, 14, 92, 30);
    newBtn.setBounds (308, 14, 60, 30);
    editBtn.setBounds (374, 14, 60, 30);
    smartBtn.setBounds (446, 14, 140, 30);
    sourceLabel.setBounds (904, 4, 100, 12);
    sourceBox.setBounds (904, 18, 150, 26);
    noteLabel.setBounds (1070, 4, 100, 12);
    midiNote.setBounds (1070, 18, 150, 26);

    const int top = 64, colH = 330;
    for (int i = 0; i < kSlots; ++i) cards[(size_t) i]->setBounds (8, top + i * 80, 270, 78);

    int x = 286;
    gatePanel = { x, top, 100, colH };  x += 106;
    inPanel = { x, top, 80, colH };     x += 86;
    filtPanel = { x, top, 100, colH };  x += 106;
    const int rightEdge = kW - 8;
    outPanel = { rightEdge - 80, top, 80, colH };
    trigPanel = { outPanel.getX() - 106, top, 100, colH };
    scopePanel = { x, top, trigPanel.getX() - 6 - x, colH };

    gateBtn.setBounds (gatePanel.getX() + 18, top + 26, 64, 24);
    gateAtk->setBounds (gatePanel.getX() + 6, top + 56, 88, 88);
    gateHold->setBounds (gatePanel.getX() + 6, top + 56 + 90, 88, 88);
    gateRel->setBounds (gatePanel.getX() + 6, top + 56 + 180, 88, 88);

    inMeter->setBounds (inPanel.getX() + 10, top + 28, 60, 190);
    inVol->setBounds (inPanel.getX() + 2, top + 226, 76, 96);

    lowcut->setBounds (filtPanel.getX() + 6, top + 10, 88, 96);
    highcut->setBounds (filtPanel.getX() + 6, top + 118, 88, 96);
    suppress->setBounds (filtPanel.getX() + 6, top + 226, 88, 96);

    scopeView->setBounds (scopePanel.reduced (6));

    // four knobs: the threshold comes first, it is the one you reach for (it moves the white lines on the display)
    threshKnob->setBounds (trigPanel.getX() + 6, top + 6, 88, 80);
    sens->setBounds (trigPanel.getX() + 6, top + 86, 88, 80);
    retrig->setBounds (trigPanel.getX() + 6, top + 166, 88, 80);
    detailKnob->setBounds (trigPanel.getX() + 6, top + 246, 88, 80);

    outMeter->setBounds (outPanel.getX() + 10, top + 28, 60, 106);
    outVol->setBounds (outPanel.getX() + 2, top + 140, 76, 92);
    mixKnob->setBounds (outPanel.getX() + 2, top + 234, 76, 92);

    slotDetail->setBounds (286, top + colH + 8, kW - 286 - 8, kH - (top + colH + 8) - 16);
    smartPanel.setBounds (0, 0, kW, kH);
    browserOverlay.setBounds (0, 0, kW, kH);
    editorPanel.setBounds (0, 0, kW, kH);
}

void OpenTriggerEditor::paintContent (juce::Graphics& g)
{
    using namespace ui;
    g.fillAll (bg);
    // header
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d222b), 0, 0, juce::Colour (0xff161a21), 0, 56, false));
    g.fillRect (0, 0, kW, 56);
    g.setColour (line);
    g.drawHorizontalLine (56, 0.0f, (float) kW);
    g.setFont (font (22.0f, true));
    g.setColour (text);
    g.drawText ("OPEN", 18, 10, 74, 36, juce::Justification::centredLeft);
    g.setColour (accent);
    g.drawText ("TRIGGER", 78, 10, 130, 36, juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (font (12.0f));
    g.drawText (status, 596, 10, 300, 36, juce::Justification::centred);

    drawPanel (g, gatePanel.toFloat(), "Gate");
    drawPanel (g, inPanel.toFloat(), "Input");
    drawPanel (g, filtPanel.toFloat());
    drawPanel (g, scopePanel.toFloat());
    drawPanel (g, trigPanel.toFloat());
    drawPanel (g, outPanel.toFloat(), "Output");
}
