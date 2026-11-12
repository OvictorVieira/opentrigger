#include "Pack.h"
#include <juce_cryptography/juce_cryptography.h>

namespace otpack
{
namespace
{
juce::String sha256Of (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return {};
    juce::SHA256 h (in);
    return h.toHexString();
}

juce::String safeName (juce::String s)
{
    s = juce::File::createLegalFileName (s.trim());
    return s.isEmpty() ? juce::String ("Pack") : s;
}

Entry describe (const juce::File& f, juce::String& error)
{
    Entry e;
    e.file = f.getFileName();
    e.size = f.getSize();
    e.sha256 = sha256Of(f);
    auto inst = otci::read (f, false, error);
    if (! inst) return e;
    e.name = inst->name;
    e.library = inst->library;
    e.waves = (int) inst->waves.size();
    e.articulations = (int) inst->articulations.size();
    e.protectedAudio = inst->enc.active();
    e.needsPassword = inst->enc.mode == otci::Encryption::password;
    return e;
}

juce::String lineFor (const Entry& e, const juce::String& verdict)
{
    return e.name + ": " + juce::String (e.waves) + " samples, " + juce::String (e.articulations) + " mic positions, "
         + (e.protectedAudio ? (e.needsPassword ? "protected (pass phrase)" : "protected") : "NOT protected") + " - " + verdict;
}
} // namespace

bool verifyAudio (const juce::File& f, juce::String& error)
{
    auto inst = otci::read (f, true, error);
    if (! inst) return false;
    if (! inst->enc.active()) return true;
    otcrypt::Key key {};
    if (! otci::resolveKey (inst->enc, key, error)) return false;
    for (size_t i = 0; i < inst->waves.size(); ++i)
    {
        juce::MemoryBlock blob = inst->waves[i].flac;
        if (! otci::decryptBlob (inst->enc, key, (int) i, blob, error)) { otcrypt::wipe (key.data(), key.size()); return false; }
    }
    otcrypt::wipe (key.data(), key.size());
    return true;
}

bool create (const juce::File& out, const juce::String& packName, const juce::Array<juce::File>& instruments,
             const otci::Protection& prot, juce::String& error, Progress progress)
{
    if (instruments.isEmpty()) { error = "nothing to pack"; return false; }
    auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_pack_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    tmpDir.createDirectory();
    struct Cleanup { juce::File d; ~Cleanup() { d.deleteRecursively(); } } cleanup { tmpDir };

    otci::Protection p = prot;
    if (p.mode == otci::Encryption::password && ! p.hasSalt) { otcrypt::randomBytes (p.salt.data(), p.salt.size()); p.hasSalt = true; } // one derivation for the whole pack

    juce::ZipFile::Builder zip;
    juce::Array<juce::File> staged;
    auto manifest = new juce::DynamicObject();
    juce::Array<juce::var> items;
    int n = 0;
    for (auto& f : instruments)
    {
        if (progress) progress (n, instruments.size(), f.getFileNameWithoutExtension());
        juce::String e1;
        auto inst = otci::read (f, true, e1);
        if (! inst) { error = f.getFileName() + ": " + e1; return false; }
        juce::File use = f;
        if (! inst->enc.active() && p.mode != otci::Encryption::none)
        {
            use = tmpDir.getChildFile (f.getFileName());
            if (! otci::write (use, *inst, e1, &p)) { error = f.getFileName() + ": " + e1; return false; }
        }
        staged.add (use);
        juce::String e2;
        auto d = describe (use, e2);
        auto o = new juce::DynamicObject();
        o->setProperty ("file", d.file); o->setProperty ("name", d.name); o->setProperty ("library", d.library);
        o->setProperty ("sha256", d.sha256); o->setProperty ("size", d.size); o->setProperty ("waves", d.waves);
        o->setProperty ("articulations", d.articulations); o->setProperty ("protected", d.protectedAudio);
        items.add (juce::var (o));
        zip.addFile (use, 0, use.getFileName());
        ++n;
    }
    manifest->setProperty ("format", 1);
    manifest->setProperty ("pack", packName);
    manifest->setProperty ("created", juce::Time::getCurrentTime().toISO8601 (true));
    manifest->setProperty ("instruments", items);
    const auto json = juce::JSON::toString (juce::var (manifest), false);
    zip.addEntry (new juce::MemoryInputStream (json.toRawUTF8(), json.getNumBytesAsUTF8(), true), 0, "manifest.json", juce::Time::getCurrentTime());

    auto tmpOut = out.getSiblingFile (out.getFileName() + ".tmp");
    {
        juce::FileOutputStream os (tmpOut);
        if (! os.openedOk()) { error = "cannot write " + out.getFullPathName(); return false; }
        os.setPosition (0); os.truncate();
        double prog = 0.0;
        if (! zip.writeToStream (os, &prog)) { error = "could not write the pack"; return false; }
        os.flush();
    }
    if (! tmpOut.moveFileTo (out)) { error = "cannot replace " + out.getFullPathName(); return false; }
    if (progress) progress (instruments.size(), instruments.size(), "done");
    return true;
}

static bool readManifest (juce::ZipFile& zip, juce::String& packName, std::vector<Entry>& entries, juce::String& error)
{
    const int mi = zip.getIndexOfFileName ("manifest.json");
    if (mi < 0) { error = "not an OpenTrigger pack (no manifest)"; return false; }
    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (mi));
    if (! in) { error = "cannot read the manifest"; return false; }
    const auto v = juce::JSON::parse (in->readEntireStreamAsString());
    if (! v.isObject() || (int) v.getProperty ("format", 0) != 1) { error = "unknown pack format"; return false; }
    packName = v.getProperty ("pack", "Pack").toString();
    if (auto* arr = v.getProperty ("instruments", {}).getArray())
        for (auto& it : *arr)
        {
            Entry e;
            e.file = it.getProperty ("file", "").toString();
            e.name = it.getProperty ("name", "").toString();
            e.library = it.getProperty ("library", "").toString();
            e.sha256 = it.getProperty ("sha256", "").toString();
            e.size = (juce::int64) it.getProperty ("size", 0);
            e.waves = (int) it.getProperty ("waves", 0);
            e.articulations = (int) it.getProperty ("articulations", 0);
            e.protectedAudio = (bool) it.getProperty ("protected", false);
            if (e.file.isEmpty() || e.file.containsAnyOf ("/\\") || e.file.startsWith (".")) { error = "unsafe file name in the pack"; return false; }
            entries.push_back (e);
        }
    return true;
}

static Report run (const juce::File& pack, const juce::File* libraryDir, const juce::String& password, Progress progress)
{
    Report r;
    r.packName = pack.getFileNameWithoutExtension();
    auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ot_imp_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
    tmpDir.createDirectory();
    struct Cleanup { juce::File d; ~Cleanup() { d.deleteRecursively(); } } cleanup { tmpDir };

    // a single instrument file is handled like a pack of one
    if (pack.hasFileExtension ("otci"))
    {
        juce::String err;
        auto e = describe (pack, err);
        if (e.name.isEmpty()) { r.message = "Not a valid OpenTrigger instrument: " + err; r.lines.add (pack.getFileName() + ": " + r.message); r.failed = 1; return r; }
        r.entries.push_back (e);
        juce::String verdict = "ok";
        if (e.protectedAudio && ! e.needsPassword) { juce::String ve; if (! verifyAudio (pack, ve)) verdict = "FAILED: " + ve; }
        r.lines.add (lineFor (e, verdict));
        if (verdict != "ok") { r.failed = 1; r.message = verdict; return r; }
        if (libraryDir)
        {
            auto dest = libraryDir->getChildFile ("Imported");
            dest.createDirectory();
            auto target = dest.getChildFile (pack.getFileName());
            if (! pack.copyFileTo (target)) { r.failed = 1; r.message = "could not copy to the library"; return r; }
            r.files.add (target);
            r.imported = 1;
        }
        r.ok = true;
        r.message = "1 instrument";
        return r;
    }

    juce::ZipFile zip (pack);
    if (zip.getNumEntries() == 0) { r.message = "This is not an OpenTrigger pack."; return r; }
    juce::String err;
    if (! readManifest (zip, r.packName, r.entries, err)) { r.message = err; return r; }
    juce::File dest = libraryDir ? libraryDir->getChildFile (safeName (r.packName)) : tmpDir;
    if (libraryDir) dest.createDirectory();
    int idx = 0;
    for (auto& e : r.entries)
    {
        if (progress) progress (idx, (int) r.entries.size(), e.name);
        ++idx;
        const int zi = zip.getIndexOfFileName (e.file);
        if (zi < 0) { ++r.failed; r.lines.add (e.name + ": missing from the pack - FAILED"); continue; }
        auto stage = tmpDir.getChildFile (e.file);
        {
            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (zi));
            juce::FileOutputStream os (stage);
            if (! in || ! os.openedOk()) { ++r.failed; r.lines.add (e.name + ": cannot be read - FAILED"); continue; }
            os.setPosition (0); os.truncate();
            os.writeFromInputStream (*in, -1);
        }
        if (sha256Of (stage) != e.sha256) { ++r.failed; r.lines.add (e.name + ": checksum does not match, the file is damaged or was altered - FAILED"); stage.deleteFile(); continue; }
        juce::String e2;
        auto d = describe (stage, e2);
        if (d.name.isEmpty()) { ++r.failed; r.lines.add (e.name + ": not a valid instrument (" + e2 + ") - FAILED"); stage.deleteFile(); continue; }
        d.sha256 = e.sha256;
        juce::String verdict = "ok";
        if (d.protectedAudio && ! d.needsPassword)
        {
            juce::String ve;
            if (! verifyAudio (stage, ve)) { ++r.failed; r.lines.add (lineFor (d, "FAILED: " + ve)); stage.deleteFile(); continue; }
        }
        else if (d.needsPassword)
        {
            auto inst = otci::read (stage, false, e2);
            juce::String ve;
            if (password.isNotEmpty() && inst)
            {
                if (otci::unlockWithPassword (inst->enc, password, ve)) { if (! verifyAudio (stage, ve)) { ++r.failed; r.lines.add (lineFor (d, "FAILED: " + ve)); stage.deleteFile(); continue; } }
                else { ++r.failed; r.lines.add (lineFor (d, "FAILED: " + ve)); stage.deleteFile(); continue; }
            }
            else { verdict = "locked, it asks for the pass phrase the first time it is loaded"; ++r.locked; }
        }
        if (libraryDir)
        {
            auto target = dest.getChildFile (e.file);
            if (target.existsAsFile()) target.deleteFile();
            if (! stage.moveFileTo (target)) { ++r.failed; r.lines.add (lineFor (d, "FAILED: cannot write to the library")); continue; }
            r.files.add (target);
        }
        ++r.imported;
        r.lines.add (lineFor (d, verdict));
    }
    r.ok = r.imported > 0 && r.failed == 0;
    r.message = juce::String (r.imported) + " of " + juce::String ((int) r.entries.size()) + " instruments"
              + (r.failed ? ", " + juce::String (r.failed) + " failed" : juce::String()) + (r.locked ? ", " + juce::String (r.locked) + " locked" : juce::String());
    if (progress) progress ((int) r.entries.size(), (int) r.entries.size(), "done");
    return r;
}

Report inspect (const juce::File& pack) { return run (pack, nullptr, {}, {}); }
Report import (const juce::File& pack, const juce::File& libraryDir, const juce::String& password, Progress progress) { return run (pack, &libraryDir, password, progress); }
} // namespace otpack
