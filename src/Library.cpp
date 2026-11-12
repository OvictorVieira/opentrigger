#include "Library.h"

juce::File Library::defaultFolder()
{
    // Lets tests and portable setups point the library elsewhere without touching real user data.
    if (auto env = juce::SystemStats::getEnvironmentVariable ("OPENTRIGGER_LIBRARY", {}); env.isNotEmpty())
        return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("OpenTrigger").getChildFile ("Library");
}

void Library::scan (const juce::File& folder)
{
    items.clear();
    for (auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.otci"))
    {
        juce::String err;
        auto info = otci::read (f, false, err);
        if (! info) continue; // unreadable files are skipped, not fatal
        LibraryEntry e;
        e.file = f;
        e.name = info->name.isNotEmpty() ? info->name : f.getFileNameWithoutExtension();
        e.library = info->library;
        e.articulations = (int) info->articulations.size();
        e.waves = (int) info->waves.size();
        for (auto& a : info->articulations) e.layers += (int) a.layers.size();
        if (info->image.getSize() > 0)
            e.cover = juce::ImageFileFormat::loadFrom (info->image.getData(), info->image.getSize());
        if (! e.cover.isValid())
            for (auto ext : { ".png", ".jpg", ".jpeg" })
                if (auto side = f.withFileExtension (ext); side.existsAsFile())
                {
                    e.cover = juce::ImageFileFormat::loadFrom (side);
                    break;
                }
        items.push_back (std::move (e));
    }
    // Same library together, then in kit order (kick, snares, toms, hats, cymbals), then by name.
    auto rank = [] (const juce::String& n)
    {
        const auto l = n.toLowerCase();
        if (l.contains ("kick") || l.contains ("bumbo")) return 0;
        if (l.contains ("snare") || l.contains ("caixa")) return 1;
        if (l.contains ("rack") || (l.contains ("tom") && ! l.contains ("floor"))) return 2;
        if (l.contains ("floor") || l.contains ("surdo")) return 3;
        if (l.contains ("hat") || l.contains ("chimbal")) return 4;
        if (l.contains ("crash") || l.contains ("ride") || l.contains ("china") || l.contains ("splash") || l.contains ("stack") || l.contains ("cymbal")) return 5;
        return 9;
    };
    std::sort (items.begin(), items.end(), [&] (auto& a, auto& b)
    {
        if (a.library != b.library) return a.library < b.library;
        if (rank (a.name) != rank (b.name)) return rank (a.name) < rank (b.name);
        return a.name < b.name;
    });
}

#include <juce_events/juce_events.h>
#include <mutex>

struct LibraryCache::Impl
{
    std::mutex m;
    std::shared_ptr<const Library> lib;
    juce::ThreadPool pool { juce::ThreadPoolOptions().withNumberOfThreads (1).withThreadName ("OpenTrigger library")
                                                    .withDesiredThreadPriority (juce::Thread::Priority::normal) };
    std::atomic<int> pending { 0 };
};

LibraryCache::LibraryCache() : impl (std::make_shared<Impl>()) {}

LibraryCache& LibraryCache::get()
{
    static LibraryCache* c = new LibraryCache(); // lives for the whole process; plug-in instances come and go
    return *c;
}

std::shared_ptr<const Library> LibraryCache::current() const
{
    std::lock_guard<std::mutex> g (impl->m);
    return impl->lib;
}

void LibraryCache::refresh (const juce::File& folder, std::function<void()> done)
{
    auto work = [impl = impl, folder]
    {
        auto l = std::make_shared<Library>();
        l->scan (folder);
        std::lock_guard<std::mutex> g (impl->m);
        impl->lib = std::move (l);
    };
    if (synchronous)
    {
        work();
        if (done) done();
        return;
    }
    if (impl->pending.load() > 1) // one scan running and one queued is enough
    {
        if (done) juce::MessageManager::callAsync (std::move (done));
        return;
    }
    ++impl->pending;
    impl->pool.addJob ([impl = impl, work, done]
    {
        work();
        --impl->pending;
        if (done) juce::MessageManager::callAsync (done);
    });
}
