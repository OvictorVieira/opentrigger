#pragma once
#include "Engine.h"
#include <memory>
#include <vector>

// Editable model behind the Instrument Editor. GUI-free so it can be unit tested.
// Rows = velocity layers, columns = round robins.
struct DraftWave
{
    juce::String label;
    std::shared_ptr<juce::AudioBuffer<float>> audio;
    double rate = 44100.0;
    int bits = 24;
    std::vector<float> peaks; // 0..1 overview for drawing
};

struct DraftCell { std::vector<std::shared_ptr<DraftWave>> waves; bool empty() const { return waves.empty(); } };

struct DraftLayer
{
    int velo = 0;
    float minVol = 0.1f, maxVol = 1.0f;
    std::vector<DraftCell> cells; // one per round-robin column
};

struct DraftArticulation
{
    juce::String name = "Hit";
    std::vector<DraftLayer> layers;
    float relTime[4] { 0.2f, 1.3f, 0.2f, 0.4f };
    int columns() const { return layers.empty() ? 0 : (int) layers[0].cells.size(); }
};

struct Draft
{
    juce::String name = "New Instrument", library = "User";
    std::vector<DraftArticulation> articulations;
    juce::MemoryBlock cover; // PNG, already downscaled

    static Draft createEmpty();
    static Draft fromLoaded (const LoadedInstrument&);
    static std::shared_ptr<DraftWave> loadWave (const juce::File&, juce::String& error);

    void addLayer (int art);                 // new row above the highest velocity
    void removeLayer (int art, int layer);   // keeps at least one
    void addColumn (int art);
    void removeColumn (int art, int col);    // keeps at least one
    int setVelo (int art, int layer, int velo); // clamped between neighbours; layer 0 stays 0
    int veloTop (int art, int layer) const;  // last velocity covered by this layer

    bool setCoverFromImage (const juce::Image&, juce::String& error); // downscale to 512px, store PNG
    // Cheap checks (name, samples present) so the UI can reject before the slow encode.
    bool validate (juce::String& error) const;
    // Fails with a readable message when something cannot be saved.
    bool toInstrument (otci::Instrument&, juce::String& error) const;
};
