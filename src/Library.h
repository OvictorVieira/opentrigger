#pragma once
#include "Otci.h"
#include <functional>
#include <memory>
#include <juce_graphics/juce_graphics.h>

struct LibraryEntry
{
    juce::File file;
    juce::String name, library;
    juce::Image cover; // embedded image, else <file>.png/.jpg sidecar, else invalid
    int articulations = 0, layers = 0, waves = 0;
};

class Library
{
public:
    static juce::File defaultFolder(); // ~/Documents/OpenTrigger/Library
    void scan (const juce::File& folder);
    const std::vector<LibraryEntry>& entries() const noexcept { return items; }
private:
    std::vector<LibraryEntry> items;
};

// Process-wide, background-refreshed view of the library folder. Scanning reads every instrument's header and decodes
// its cover, so it must never run on the host's UI thread: instances warm it up at creation and the window just uses
// whatever is ready (an empty list for a moment on the very first open of the session).
class LibraryCache
{
public:
    static LibraryCache& get();
    std::shared_ptr<const Library> current() const;
    // Rescans `folder` on a worker thread; `done` runs on the message thread afterwards (synchronously if setSynchronous).
    void refresh (const juce::File& folder, std::function<void()> done = {});
    static void setSynchronous (bool b) { synchronous = b; } // tests
private:
    LibraryCache();
    struct Impl;
    std::shared_ptr<Impl> impl;
    static inline bool synchronous = false;
};
