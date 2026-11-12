#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <vector>

// Audio protection for OpenTrigger instruments: XChaCha20-Poly1305 (Monocypher) over each wave's compressed audio.
//
//  sealed   - opens by itself in any OpenTrigger. The key is derived from a constant in the (open) source, so this keeps
//             the audio out of casual reach (no WAV/FLAC lying around, nothing a normal audio tool can open) but it is
//             NOT secret: anyone reading the source can derive it. Honest obfuscation, not DRM.
//  password - the pack key is wrapped with a key derived (Argon2id) from a pass phrase the author chooses and shares.
//             That one is as strong as the pass phrase.
namespace otcrypt
{
using Key = std::array<std::uint8_t, 32>;
constexpr size_t kMac = 16, kNonce = 24, kSalt = 16;

Key sealedKey();                                            // see above: not a secret
Key deriveFromPassword (const juce::String& pass, const std::uint8_t salt[kSalt]); // Argon2id, ~0.2 s
void randomBytes (void* dst, size_t n);                     // OS CSPRNG

// Pack key wrapping: nonce(24) | mac(16) | key(32)
std::vector<std::uint8_t> wrapKey (const Key& packKey, const Key& kek);
bool unwrapKey (const std::vector<std::uint8_t>& wrapped, const Key& kek, Key& out);

// One wave: out = mac(16) | cipher(n). nonce = base(16) | index(8).
void encryptWave (const Key&, const std::uint8_t base[16], std::uint64_t index, const void* plain, size_t n, std::uint8_t* out);
bool decryptWave (const Key&, const std::uint8_t base[16], std::uint64_t index, const void* sealed, size_t sealedSize, std::vector<std::uint8_t>& plain);

// Keys derived from pass phrases, remembered for this session (by salt) so a pack asks only once.
class Vault
{
public:
    static void remember (const std::array<std::uint8_t, 16>& salt, const Key& kek);
    static bool find (const std::array<std::uint8_t, 16>& salt, Key& kek);
    static void forgetAll();
};

juce::String toHex (const void* p, size_t n);
std::vector<std::uint8_t> fromHex (const juce::String&);
void wipe (void* p, size_t n);
} // namespace otcrypt
