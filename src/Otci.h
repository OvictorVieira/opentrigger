#pragma once
#include "Crypt.h"
#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

// OTCI: Open Trigger Compressed Instrument.
// Layout:
//   0x00 64B magic "OPEN TRIGGER INSTRUMENT 1" | 0x40 u64 payload_size | 0x48 payload
//   payload = FLAC waves (dataN_offset1) then optional cover image (img_offset1/img_size)
//   after payload: u32 version=1, u32 0, u32 0, zlib("OTI1" + u32 xml_len + xml)
namespace otci
{
constexpr const char* kMagic = "OPEN TRIGGER INSTRUMENT 1";
constexpr juce::int64 kHeaderSize = 0x48;

struct Wave
{
    juce::MemoryBlock flac; // empty when loaded with audio=false
    int frames = 0;
    int channels = 1;
    double rate = 44100.0;
    juce::int64 offset = 0, size = 0; // where the FLAC bytes live in the file (set by read(), any mode)
};

struct Layer
{
    int velo = 0; // lowest velocity (0..127) that selects this layer
    float minVol = 0.1f, maxVol = 1.0f;
    std::vector<std::vector<int>> roundRobins; // each entry: wave indices played together
};

struct Articulation
{
    juce::String name;
    std::vector<Layer> layers;
    float relTime[4] { 0.2f, 1.3f, 0.2f, 0.4f }; // minTop,maxTop,minBot,maxBot (meaning unverified)
};

// Audio protection (see Crypt.h). Names, layers and the cover stay readable so libraries can be listed; only the waves
// are encrypted. A wave's stored bytes are then mac(16) | cipher.
struct Encryption
{
    enum Mode { none, sealed, password } mode = none;
    std::array<std::uint8_t, 16> nonceBase {}, salt {};
    std::vector<std::uint8_t> wrappedKey;
    bool active() const { return mode != none; }
};

struct Protection
{
    Encryption::Mode mode = Encryption::none;
    juce::String password;                       // password mode
    std::array<std::uint8_t, 16> salt {};        // optional: share one salt (hence one derivation) across a whole pack
    bool hasSalt = false;
};

struct Instrument
{
    Encryption enc;
    juce::String name, library;
    std::vector<Articulation> articulations;
    std::vector<Wave> waves;
    juce::MemoryBlock image; // PNG/JPEG cover, may be empty
};

// audio=false reads only header, metadata and cover (fast library scans).
std::optional<Instrument> read (const juce::File&, bool audio, juce::String& error);
// With `prot`, the audio is encrypted while writing (the instrument must not already be protected).
bool write (const juce::File&, const Instrument&, juce::String& error, const Protection* prot = nullptr);

// Key access. error starts with "password needed" when a pass phrase has not been given yet this session.
bool resolveKey (const Encryption&, otcrypt::Key& out, juce::String& error);
bool unlockWithPassword (const Encryption&, const juce::String& password, juce::String& error); // remembers the key on success
bool decryptBlob (const Encryption&, const otcrypt::Key&, int waveIndex, juce::MemoryBlock& blob, juce::String& error);

// Layer = highest `velo` <= velocity. Gain lerps minVol..maxVol over the layer's span .
const Layer* pickLayer (const Articulation&, int velocity, float& gain);
} // namespace otci
