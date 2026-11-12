#include "Otci.h"
#include <zlib.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace otci
{
namespace
{
bool inflateAll (const void* src, size_t n, std::vector<uint8_t>& out)
{
    z_stream s {};
    if (inflateInit (&s) != Z_OK) return false;
    s.next_in = (Bytef*) src;
    s.avail_in = (uInt) n;
    uint8_t buf[16384];
    int rc;
    do
    {
        s.next_out = buf;
        s.avail_out = sizeof (buf);
        rc = inflate (&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd (&s); return false; }
        out.insert (out.end(), buf, buf + (sizeof (buf) - s.avail_out));
    } while (rc != Z_STREAM_END);
    inflateEnd (&s);
    return true;
}

juce::String key (const char* fmt, int a) { return juce::String::formatted (fmt, a); }
juce::String key (const char* fmt, int a, int b) { return juce::String::formatted (fmt, a, b); }
juce::String key (const char* fmt, int a, int b, int c) { return juce::String::formatted (fmt, a, b, c); }
juce::String key (const char* fmt, int a, int b, int c, int d) { return juce::String::formatted (fmt, a, b, c, d); }

// The metadata is one flat tag with thousands of attributes. juce::XmlElement keeps attributes in a linked list, so
// every lookup is linear and reading a 2500-wave instrument took over a second. A hash table makes it a few ms.
using Attrs = std::unordered_map<std::string, std::string>;

void unescapeInto (std::string& out, const char* p, const char* e)
{
    while (p < e)
    {
        if (*p == '&')
        {
            auto starts = [&] (const char* t) { const size_t n = std::strlen (t); return (size_t) (e - p) >= n && std::strncmp (p, t, n) == 0; };
            if (starts ("&amp;")) { out += '&'; p += 5; continue; }
            if (starts ("&lt;")) { out += '<'; p += 4; continue; }
            if (starts ("&gt;")) { out += '>'; p += 4; continue; }
            if (starts ("&quot;")) { out += '"'; p += 6; continue; }
            if (starts ("&apos;")) { out += '\''; p += 6; continue; }
            if (starts ("&#")) // numeric reference (juce writes these for control characters)
            {
                const char* q = p + 2; int base = 10;
                if (q < e && (*q == 'x' || *q == 'X')) { base = 16; ++q; }
                char* end = nullptr;
                const long cp = std::strtol (q, &end, base);
                if (end != q && end < e && *end == ';')
                {
                    char buf[8]; int n = 0;
                    if (cp < 0x80) buf[n++] = (char) cp;
                    else if (cp < 0x800) { buf[n++] = (char) (0xC0 | (cp >> 6)); buf[n++] = (char) (0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { buf[n++] = (char) (0xE0 | (cp >> 12)); buf[n++] = (char) (0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char) (0x80 | (cp & 0x3F)); }
                    else { buf[n++] = (char) (0xF0 | (cp >> 18)); buf[n++] = (char) (0x80 | ((cp >> 12) & 0x3F)); buf[n++] = (char) (0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char) (0x80 | (cp & 0x3F)); }
                    out.append (buf, (size_t) n);
                    p = end + 1;
                    continue;
                }
            }
        }
        out += *p++;
    }
}

// Parses `<trigger_instrument a="1" b="2" .../>`. Returns false if it is not that tag.
bool parseFlatTag (const char* d, size_t n, Attrs& out)
{
    const char* p = d; const char* e = d + n;
    auto ws = [&] { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; };
    ws();
    while (p < e && *p != '<') ++p;                      // skip a possible <?xml ... ?> prolog
    if (p + 1 < e && p[1] == '?') { while (p < e && ! (p[-1] == '?' && *p == '>')) ++p; if (p < e) ++p; ws(); }
    static const char tag[] = "<trigger_instrument";
    if ((size_t) (e - p) < sizeof (tag) || std::strncmp (p, tag, sizeof (tag) - 1) != 0) return false;
    p += sizeof (tag) - 1;
    out.reserve (4096);
    for (;;)
    {
        ws();
        if (p >= e) return false;
        if (*p == '/' || *p == '>') return true;
        const char* ns = p;
        while (p < e && *p != '=' && *p != ' ' && *p != '/' && *p != '>') ++p;
        if (p >= e || *p != '=') return false;
        std::string name (ns, (size_t) (p - ns));
        ++p;
        if (p >= e || (*p != '"' && *p != '\'')) return false;
        const char q = *p++;
        const char* vs = p;
        while (p < e && *p != q) ++p;
        if (p >= e) return false;
        std::string val;
        val.reserve ((size_t) (p - vs));
        unescapeInto (val, vs, p);
        ++p;
        out.emplace (std::move (name), std::move (val));
    }
}

std::string k (const char* fmt, ...)
{
    char buf[160];
    va_list ap; va_start (ap, fmt);
    std::vsnprintf (buf, sizeof (buf), fmt, ap);
    va_end (ap);
    return buf;
}

uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
} // namespace

std::optional<Instrument> read (const juce::File& f, bool audio, juce::String& error)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) { error = "cannot open " + f.getFullPathName(); return {}; }

    uint8_t head[kHeaderSize];
    if (in.read (head, (int) kHeaderSize) != (int) kHeaderSize || std::memcmp (head, kMagic, std::strlen (kMagic)) != 0)
    {
        error = "not an OTCI file";
        return {};
    }
    juce::int64 psize = 0;
    for (int i = 7; i >= 0; --i) psize = (psize << 8) | head[0x40 + i];
    if (kHeaderSize + psize + 12 > in.getTotalLength()) { error = "truncated file"; return {}; }

    in.setPosition (kHeaderSize + psize + 12);
    juce::MemoryBlock z;
    in.readIntoMemoryBlock (z);
    std::vector<uint8_t> blob;
    if (! inflateAll (z.getData(), z.getSize(), blob) || blob.size() < 8 || std::memcmp (blob.data(), "OTI1", 4) != 0)
    {
        error = "bad metadata chunk";
        return {};
    }
    const size_t xmlLen = rd32 (blob.data() + 4);
    if (8 + xmlLen > blob.size()) { error = "bad metadata length"; return {}; }
    Attrs at;
    if (! parseFlatTag ((const char*) blob.data() + 8, xmlLen, at)) { error = "bad metadata xml"; return {}; }
    auto has = [&] (const std::string& key_) { return at.find (key_) != at.end(); };
    auto str = [&] (const std::string& key_) -> juce::String { auto it = at.find (key_); return it == at.end() ? juce::String() : juce::String::fromUTF8 (it->second.c_str()); };
    auto num = [&] (const std::string& key_, long long def = 0) -> long long { auto it = at.find (key_); return it == at.end() ? def : std::strtoll (it->second.c_str(), nullptr, 10); };
    auto real = [&] (const std::string& key_, double def = 0.0) -> double { auto it = at.find (key_); return it == at.end() ? def : std::strtod (it->second.c_str(), nullptr); };

    Instrument inst;
    inst.name = str ("instrument_name");
    inst.library = str ("library_name");
    if (const auto em = str ("enc"); em.isNotEmpty())
    {
        inst.enc.mode = em == "sealed" ? Encryption::sealed : em == "password" ? Encryption::password : Encryption::none;
        const auto nb = otcrypt::fromHex (str ("enc_nonce")), sl = otcrypt::fromHex (str ("enc_salt"));
        inst.enc.wrappedKey = otcrypt::fromHex (str ("enc_key"));
        if (inst.enc.mode == Encryption::none || nb.size() != 16 || sl.size() != 16 || inst.enc.wrappedKey.empty()) { error = "bad protection header"; return {}; }
        std::copy (nb.begin(), nb.end(), inst.enc.nonceBase.begin());
        std::copy (sl.begin(), sl.end(), inst.enc.salt.begin());
    }

    const int n = (int) num ("data_count");
    if (n < 0 || n > 1000000) { error = "bad wave count"; return {}; }
    const juce::int64 imgOff = has ("img_offset1") ? (juce::int64) num ("img_offset1") : psize;
    std::vector<juce::int64> offs;
    offs.reserve ((size_t) n + 1);
    for (int i = 0; i < n; ++i) offs.push_back ((juce::int64) num (k ("data%d_offset1", i)));
    offs.push_back (imgOff);

    inst.waves.reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        Wave w;
        w.frames = (int) num (k ("wd%dframes", i));
        w.channels = num (k ("wd%dstereo", i)) == 1 ? 2 : 1;
        w.rate = real (k ("wd%drate", i), 44100.0);
        const auto len = offs[(size_t) i + 1] - offs[(size_t) i];
        if (len < 0 || offs[(size_t) i + 1] > psize) { error = "bad wave offsets"; return {}; }
        w.offset = kHeaderSize + offs[(size_t) i];
        w.size = len;
        if (audio)
        {
            in.setPosition (kHeaderSize + offs[(size_t) i]);
            in.readIntoMemoryBlock (w.flac, (juce::int64) len);
        }
        inst.waves.push_back (std::move (w));
    }

    if (has ("img_size"))
    {
        const auto sz = (juce::int64) num ("img_size");
        if (sz > 0 && imgOff + sz <= psize)
        {
            in.setPosition (kHeaderSize + imgOff);
            in.readIntoMemoryBlock (inst.image, (juce::int64) sz);
        }
    }

    const int na = (int) num ("articulation_count");
    for (int a = 0; a < na; ++a)
    {
        Articulation art;
        art.name = str (k ("art_Idx_%dartName", a));
        const char* rel[] = { "minTop", "maxTop", "minBot", "maxBot" };
        for (int j = 0; j < 4; ++j)
            art.relTime[j] = (float) real (k ("art_Idx_%d_rel_time_%s", a, rel[j]), art.relTime[j]);
        const int nl = (int) num (k ("art_Idx_%dlayers_count", a));
        for (int l = 0; l < nl; ++l)
        {
            Layer lay;
            lay.velo = (int) num (k ("art_Idx_%d_layer_Idx_%dvelo", a, l));
            lay.minVol = (float) real (k ("art_Idx_%d_layer_Idx_%dminVol", a, l), 0.1);
            lay.maxVol = (float) real (k ("art_Idx_%d_layer_Idx_%dmaxVol", a, l), 1.0);
            const int nr = (int) num (k ("art_Idx_%d_layer_Idx_%dmulty_wave_count", a, l));
            for (int r = 0; r < nr; ++r)
            {
                const int nk = (int) num (k ("art_Idx_%d_layer_Idx_%dmulty_wave_Idx_%dsamples_count_in_mw", a, l, r));
                std::vector<int> rr;
                for (int j = 0; j < nk; ++j)
                {
                    const int idx = (int) num (k ("art_Idx_%d_layer_Idx_%dmulty_wave_Idx_%dwave_Idx_%d", a, l, r, j), -1);
                    if (idx < 0 || idx >= n) { error = "wave index out of range"; return {}; }
                    rr.push_back (idx);
                }
                lay.roundRobins.push_back (std::move (rr));
            }
            art.layers.push_back (std::move (lay));
        }
        inst.articulations.push_back (std::move (art));
    }
    return inst;
}

bool resolveKey (const Encryption& enc, otcrypt::Key& out, juce::String& error)
{
    otcrypt::Key kek {};
    if (enc.mode == Encryption::sealed) kek = otcrypt::sealedKey();
    else if (enc.mode == Encryption::password)
    {
        if (! otcrypt::Vault::find (enc.salt, kek)) { error = "password needed"; return false; }
    }
    else { error = "not protected"; return false; }
    if (! otcrypt::unwrapKey (enc.wrappedKey, kek, out)) { error = enc.mode == Encryption::password ? "password needed (wrong pass phrase)" : "protected file is damaged"; return false; }
    return true;
}

bool unlockWithPassword (const Encryption& enc, const juce::String& password, juce::String& error)
{
    if (enc.mode != Encryption::password) { error = "this instrument does not use a pass phrase"; return false; }
    auto kek = otcrypt::deriveFromPassword (password, enc.salt.data());
    otcrypt::Key probe {};
    if (! otcrypt::unwrapKey (enc.wrappedKey, kek, probe)) { error = "wrong pass phrase"; return false; }
    otcrypt::wipe (probe.data(), probe.size());
    otcrypt::Vault::remember (enc.salt, kek);
    return true;
}

bool decryptBlob (const Encryption& enc, const otcrypt::Key& key, int waveIndex, juce::MemoryBlock& blob, juce::String& error)
{
    std::vector<std::uint8_t> plain;
    if (! otcrypt::decryptWave (key, enc.nonceBase.data(), (std::uint64_t) waveIndex, blob.getData(), blob.getSize(), plain))
    {
        error = "protected audio is damaged or has been altered";
        return false;
    }
    blob.replaceAll (plain.data(), plain.size());
    return true;
}

bool write (const juce::File& f, const Instrument& inst, juce::String& error, const Protection* prot)
{
    Encryption enc;
    otcrypt::Key packKey {};
    const bool protect = prot != nullptr && prot->mode != Encryption::none;
    if (protect && inst.enc.active()) { error = "this instrument is already protected"; return false; }
    if (inst.enc.active() && ! protect) { error = "cannot rewrite a protected instrument"; return false; }
    if (protect)
    {
        enc.mode = prot->mode;
        otcrypt::randomBytes (enc.nonceBase.data(), enc.nonceBase.size());
        if (prot->hasSalt) enc.salt = prot->salt; else otcrypt::randomBytes (enc.salt.data(), enc.salt.size());
        otcrypt::randomBytes (packKey.data(), packKey.size());
        otcrypt::Key kek {};
        if (prot->mode == Encryption::sealed) kek = otcrypt::sealedKey();
        else if (! otcrypt::Vault::find (enc.salt, kek)) { kek = otcrypt::deriveFromPassword (prot->password, enc.salt.data()); otcrypt::Vault::remember (enc.salt, kek); }
        enc.wrappedKey = otcrypt::wrapKey (packKey, kek);
        otcrypt::wipe (kek.data(), kek.size());
    }
    juce::MemoryBlock payload;
    std::vector<juce::int64> offsets;
    int waveNo = 0;
    for (auto& w : inst.waves)
    {
        offsets.push_back ((juce::int64) payload.getSize());
        if (protect)
        {
            std::vector<std::uint8_t> sealedBytes (otcrypt::kMac + w.flac.getSize());
            otcrypt::encryptWave (packKey, enc.nonceBase.data(), (std::uint64_t) waveNo, w.flac.getData(), w.flac.getSize(), sealedBytes.data());
            payload.append (sealedBytes.data(), sealedBytes.size());
        }
        else payload.append (w.flac.getData(), w.flac.getSize());
        ++waveNo;
    }
    otcrypt::wipe (packKey.data(), packKey.size());
    const auto imgOffset = (juce::int64) payload.getSize();
    payload.append (inst.image.getData(), inst.image.getSize());

    juce::XmlElement x ("trigger_instrument");
    x.setAttribute ("trigger_instrumentVersion", "1");
    x.setAttribute ("data_count", (int) inst.waves.size());
    for (size_t i = 0; i < offsets.size(); ++i) x.setAttribute (key ("data%d_offset1", (int) i), juce::String (offsets[i]));
    if (inst.image.getSize() > 0)
    {
        x.setAttribute ("img_offset1", juce::String (imgOffset));
        x.setAttribute ("img_size", juce::String ((juce::int64) inst.image.getSize()));
    }
    x.setAttribute ("instrument_name", inst.name);
    x.setAttribute ("instrument_version", "0");
    x.setAttribute ("library_name", inst.library);
    if (protect)
    {
        x.setAttribute ("enc", prot->mode == Encryption::sealed ? "sealed" : "password");
        x.setAttribute ("enc_nonce", otcrypt::toHex (enc.nonceBase.data(), enc.nonceBase.size()));
        x.setAttribute ("enc_salt", otcrypt::toHex (enc.salt.data(), enc.salt.size()));
        x.setAttribute ("enc_key", otcrypt::toHex (enc.wrappedKey.data(), enc.wrappedKey.size()));
    }
    x.setAttribute ("articulation_count", (int) inst.articulations.size());
    for (size_t i = 0; i < inst.waves.size(); ++i)
    {
        auto& w = inst.waves[i];
        const int ii = (int) i;
        x.setAttribute (key ("wd%dvol", ii), "1.0");
        x.setAttribute (key ("wd%dstereo", ii), w.channels == 2 ? 1 : 0);
        x.setAttribute (key ("wd%dframes", ii), w.frames);
        x.setAttribute (key ("wd%dsamples", ii), w.frames * w.channels);
        x.setAttribute (key ("wd%drate", ii), (int) w.rate);
        x.setAttribute (key ("wd%dcodec", ii), protect ? "flac+xchacha20poly1305" : "flac");
    }
    for (size_t ai = 0; ai < inst.articulations.size(); ++ai)
    {
        auto& art = inst.articulations[ai];
        const int a = (int) ai;
        x.setAttribute (key ("art_Idx_%dartName", a), art.name);
        const char* rel[] = { "minTop", "maxTop", "minBot", "maxBot" };
        for (int k = 0; k < 4; ++k)
            x.setAttribute (juce::String::formatted ("art_Idx_%d_rel_time_%s", a, rel[k]), (double) art.relTime[k]);
        x.setAttribute (key ("art_Idx_%dlayers_count", a), (int) art.layers.size());
        for (size_t li = 0; li < art.layers.size(); ++li)
        {
            auto& lay = art.layers[li];
            const int l = (int) li;
            x.setAttribute (key ("art_Idx_%d_layer_Idx_%dvelo", a, l), lay.velo);
            x.setAttribute (key ("art_Idx_%d_layer_Idx_%dminVol", a, l), (double) lay.minVol);
            x.setAttribute (key ("art_Idx_%d_layer_Idx_%dmaxVol", a, l), (double) lay.maxVol);
            x.setAttribute (key ("art_Idx_%d_layer_Idx_%dmulty_wave_count", a, l), (int) lay.roundRobins.size());
            for (size_t ri = 0; ri < lay.roundRobins.size(); ++ri)
            {
                const int r = (int) ri;
                x.setAttribute (key ("art_Idx_%d_layer_Idx_%dmulty_wave_Idx_%dsamples_count_in_mw", a, l, r), (int) lay.roundRobins[ri].size());
                for (size_t ki = 0; ki < lay.roundRobins[ri].size(); ++ki)
                    x.setAttribute (juce::String::formatted ("art_Idx_%d_layer_Idx_%dmulty_wave_Idx_%dwave_Idx_%d", a, l, r, (int) ki), lay.roundRobins[ri][ki]);
            }
        }
    }

    const auto xmlStr = x.toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());
    const auto utf8 = xmlStr.toUTF8();
    const uint32_t xmlLen = (uint32_t) utf8.sizeInBytes() - 1;
    std::vector<uint8_t> raw { 'O', 'T', 'I', '1', (uint8_t) xmlLen, (uint8_t) (xmlLen >> 8), (uint8_t) (xmlLen >> 16), (uint8_t) (xmlLen >> 24) };
    raw.insert (raw.end(), (const uint8_t*) utf8.getAddress(), (const uint8_t*) utf8.getAddress() + xmlLen);
    uLongf zlen = compressBound ((uLong) raw.size());
    std::vector<uint8_t> z (zlen);
    if (compress2 (z.data(), &zlen, raw.data(), (uLong) raw.size(), 9) != Z_OK) { error = "zlib failed"; return {}; }

    // Write to a temp file then move, so a failed save never corrupts an existing library file.
    auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    {
        juce::FileOutputStream out (tmp);
        if (! out.openedOk()) { error = "cannot write " + tmp.getFullPathName(); return false; }
        out.setPosition (0);
        out.truncate();
        char head[kHeaderSize] = {};
        std::memcpy (head, kMagic, std::strlen (kMagic));
        auto ps = (juce::uint64) payload.getSize();
        for (int i = 0; i < 8; ++i) head[0x40 + i] = (char) ((ps >> (8 * i)) & 0xff);
        out.write (head, sizeof (head));
        out.write (payload.getData(), payload.getSize());
        const uint32_t trailer[3] = { 1, 0, 0 };
        out.write (trailer, sizeof (trailer));
        out.write (z.data(), zlen);
        out.flush();
        if (out.getStatus().failed()) { error = out.getStatus().getErrorMessage(); return false; }
    }
    if (! tmp.moveFileTo (f)) { error = "cannot replace " + f.getFullPathName(); return false; }
    return true;
}

const Layer* pickLayer (const Articulation& art, int velocity, float& gain)
{
    // Allocation-free (called on the audio thread); layers may be stored in any order.
    const Layer* best = nullptr;
    int nextVelo = 128;
    for (auto& l : art.layers)
        if (l.velo <= velocity && (best == nullptr || l.velo > best->velo)) best = &l;
    if (best == nullptr)
        for (auto& l : art.layers)
            if (best == nullptr || l.velo < best->velo) best = &l; // velocity below every layer: lowest one
    if (best == nullptr) return nullptr;
    for (auto& l : art.layers)
        if (l.velo > best->velo && l.velo < nextVelo) nextVelo = l.velo;
    const int top = nextVelo - 1;
    const float span = (float) std::max (top - best->velo, 1);
    const float t = juce::jlimit (0.0f, 1.0f, (float) (velocity - best->velo) / span);
    gain = best->minVol + (best->maxVol - best->minVol) * t;
    return best;
}
} // namespace otci
