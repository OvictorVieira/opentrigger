#pragma once
#include "Otci.h"
#include <functional>

// .otpack: one file that carries a library (a zip, entries stored as they are: the audio inside is already encrypted).
//   manifest.json   pack name, version, per-instrument name / library / size / SHA-256
//   <name>.otci     the instruments (audio encrypted, names / layers / cover readable)
namespace otpack
{
constexpr const char* kExtension = ".otpack";

struct Entry
{
    juce::String file, name, library, sha256;
    juce::int64 size = 0;
    int waves = 0, articulations = 0;
    bool protectedAudio = false, needsPassword = false;
};

struct Report
{
    bool ok = false;
    juce::String packName, message;
    std::vector<Entry> entries;
    int imported = 0, failed = 0, locked = 0;
    juce::StringArray lines;       // one human readable line per instrument: what was found and whether it passed
    juce::Array<juce::File> files; // where the imported instruments ended up
};

using Progress = std::function<void (int done, int total, const juce::String& what)>;

// Protects the instruments that are not protected yet (prot.mode none = leave them as they are) and bundles them.
bool create (const juce::File& out, const juce::String& packName, const juce::Array<juce::File>& instruments,
             const otci::Protection& prot, juce::String& error, Progress progress = {});

// Looks inside a pack (or one .otci) and checks it, writing nothing.
Report inspect (const juce::File& pack);

// Checks every instrument and copies the good ones to <libraryDir>/<pack name>/. A pack protected with a pass phrase
// imports fine without it (it asks on first use); pass it here to also verify it now.
Report import (const juce::File& pack, const juce::File& libraryDir, const juce::String& password = {}, Progress progress = {});

// Decrypts every wave of an instrument and checks its authentication tags (does not decode the audio).
bool verifyAudio (const juce::File& otciFile, juce::String& error);
} // namespace otpack
