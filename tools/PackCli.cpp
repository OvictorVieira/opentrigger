// ot_pack: protect instruments and bundle / inspect / import libraries from the command line.
#include "Pack.h"
#include <cstdio>

static int usage()
{
    std::printf ("ot_pack protect <in.otci> <out.otci> [--password P]\n"
                 "ot_pack pack <out.otpack> <pack name> <file.otci | folder>... [--password P] [--keep]\n"
                 "ot_pack info <file.otpack | file.otci>\n"
                 "ot_pack verify <file.otci> [--password P]\n"
                 "ot_pack import <file.otpack | file.otci> <library folder> [--password P]\n"
                 "  protect/pack: audio is encrypted. Without --password it is 'sealed' (opens by itself in any OpenTrigger,\n"
                 "  not secret since the source is open); with --password only people who know the pass phrase can open it.\n"
                 "  --keep leaves already-written instruments as they are.\n");
    return 2;
}

int main (int argc, char** argv)
{
    juce::StringArray a;
    juce::String password;
    bool keep = false;
    for (int i = 1; i < argc; ++i)
    {
        juce::String s (argv[i]);
        if (s == "--password" && i + 1 < argc) password = argv[++i];
        else if (s == "--keep") keep = true;
        else a.add (s);
    }
    if (a.size() < 2) return usage();
    auto progress = [] (int d, int t, const juce::String& w) { std::printf ("  [%d/%d] %s\n", d, t, w.toRawUTF8()); std::fflush (stdout); };
    otci::Protection prot;
    prot.mode = keep ? otci::Encryption::none : password.isNotEmpty() ? otci::Encryption::password : otci::Encryption::sealed;
    prot.password = password;
    juce::String err;

    if (a[0] == "protect" && a.size() == 3)
    {
        auto inst = otci::read (juce::File (a[1]), true, err);
        if (! inst) { std::printf ("cannot read: %s\n", err.toRawUTF8()); return 1; }
        if (! otci::write (juce::File (a[2]), *inst, err, &prot)) { std::printf ("failed: %s\n", err.toRawUTF8()); return 1; }
        std::printf ("protected (%s): %s\n", password.isNotEmpty() ? "pass phrase" : "sealed", a[2].toRawUTF8());
        return 0;
    }
    if (a[0] == "pack" && a.size() >= 4)
    {
        juce::Array<juce::File> files;
        for (int i = 3; i < a.size(); ++i)
        {
            juce::File f (a[i]);
            if (f.isDirectory()) files.addArray (f.findChildFiles (juce::File::findFiles, true, "*.otci"));
            else files.add (f);
        }
        std::sort (files.begin(), files.end(), [] (const juce::File& x, const juce::File& y) { return x.getFileName() < y.getFileName(); });
        if (! otpack::create (juce::File (a[1]), a[2], files, prot, err, progress)) { std::printf ("failed: %s\n", err.toRawUTF8()); return 1; }
        std::printf ("pack written: %s (%.1f MB, %d instruments)\n", a[1].toRawUTF8(), juce::File (a[1]).getSize() / 1048576.0, files.size());
        return 0;
    }
    if (a[0] == "info" || a[0] == "import")
    {
        const bool doImport = a[0] == "import";
        if (doImport && a.size() < 3) return usage();
        auto r = doImport ? otpack::import (juce::File (a[1]), juce::File (a[2]), password, progress) : otpack::inspect (juce::File (a[1]));
        std::printf ("%s: %s\n", r.packName.toRawUTF8(), r.message.toRawUTF8());
        for (auto& l : r.lines) std::printf ("  %s\n", l.toRawUTF8());
        return r.ok ? 0 : 1;
    }
    if (a[0] == "verify")
    {
        if (password.isNotEmpty())
        {
            auto inst = otci::read (juce::File (a[1]), false, err);
            if (inst && ! otci::unlockWithPassword (inst->enc, password, err)) { std::printf ("FAILED: %s\n", err.toRawUTF8()); return 1; }
        }
        if (! otpack::verifyAudio (juce::File (a[1]), err)) { std::printf ("FAILED: %s\n", err.toRawUTF8()); return 1; }
        std::printf ("ok: every wave decrypts and authenticates\n");
        return 0;
    }
    return usage();
}
