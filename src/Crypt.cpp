#include "Crypt.h"
#include "third_party/monocypher/monocypher.h"
#include <cstring>
#include <memory>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <bcrypt.h>
 #pragma comment (lib, "bcrypt.lib")
#elif JUCE_MAC || JUCE_IOS || JUCE_BSD
 #include <stdlib.h>
#else
 #include <sys/random.h>
#endif

namespace otcrypt
{
void randomBytes (void* dst, size_t n)
{
#if JUCE_WINDOWS
    BCryptGenRandom (nullptr, (PUCHAR) dst, (ULONG) n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif JUCE_MAC || JUCE_IOS || JUCE_BSD
    arc4random_buf (dst, n);
#else
    size_t got = 0;
    while (got < n) { const auto r = getrandom ((char*) dst + got, n - got, 0); if (r > 0) got += (size_t) r; }
#endif
}

void wipe (void* p, size_t n) { crypto_wipe (p, n); }

Key sealedKey()
{
    static const char tag[] = "OpenTrigger sealed pack key, format 1. This is obfuscation, not a secret: the source is open.";
    Key k {};
    crypto_blake2b (k.data(), k.size(), (const std::uint8_t*) tag, sizeof (tag) - 1);
    return k;
}

Key deriveFromPassword (const juce::String& pass, const std::uint8_t salt[kSalt])
{
    constexpr std::uint32_t blocks = 32768; // 32 MiB
    std::unique_ptr<std::uint8_t[]> work (new std::uint8_t[(size_t) blocks * 1024]);
    const auto utf8 = pass.toRawUTF8();
    crypto_argon2_config cfg { CRYPTO_ARGON2_ID, blocks, 3, 1 };
    crypto_argon2_inputs in { (const std::uint8_t*) utf8, salt, (std::uint32_t) std::strlen (utf8), (std::uint32_t) kSalt };
    Key k {};
    crypto_argon2 (k.data(), (std::uint32_t) k.size(), work.get(), cfg, in, crypto_argon2_no_extras);
    crypto_wipe (work.get(), (size_t) blocks * 1024);
    return k;
}

std::vector<std::uint8_t> wrapKey (const Key& packKey, const Key& kek)
{
    std::vector<std::uint8_t> out (kNonce + kMac + packKey.size());
    randomBytes (out.data(), kNonce);
    crypto_aead_lock (out.data() + kNonce + kMac, out.data() + kNonce, kek.data(), out.data(), nullptr, 0, packKey.data(), packKey.size());
    return out;
}

bool unwrapKey (const std::vector<std::uint8_t>& w, const Key& kek, Key& out)
{
    if (w.size() != kNonce + kMac + out.size()) return false;
    return crypto_aead_unlock (out.data(), w.data() + kNonce, kek.data(), w.data(), nullptr, 0, w.data() + kNonce + kMac, out.size()) == 0;
}

static void nonceFor (std::uint8_t nonce[kNonce], const std::uint8_t base[16], std::uint64_t index)
{
    std::memcpy (nonce, base, 16);
    for (int i = 0; i < 8; ++i) nonce[16 + i] = (std::uint8_t) (index >> (8 * i));
}

void encryptWave (const Key& key, const std::uint8_t base[16], std::uint64_t index, const void* plain, size_t n, std::uint8_t* out)
{
    std::uint8_t nonce[kNonce];
    nonceFor (nonce, base, index);
    crypto_aead_lock (out + kMac, out, key.data(), nonce, nullptr, 0, (const std::uint8_t*) plain, n);
}

bool decryptWave (const Key& key, const std::uint8_t base[16], std::uint64_t index, const void* sealed, size_t sealedSize, std::vector<std::uint8_t>& plain)
{
    if (sealedSize < kMac) return false;
    std::uint8_t nonce[kNonce];
    nonceFor (nonce, base, index);
    plain.resize (sealedSize - kMac);
    const auto* s = (const std::uint8_t*) sealed;
    return crypto_aead_unlock (plain.data(), s, key.data(), nonce, nullptr, 0, s + kMac, plain.size()) == 0;
}

namespace
{
juce::CriticalSection vaultLock;
std::vector<std::pair<std::array<std::uint8_t, 16>, Key>>& vaultData() { static std::vector<std::pair<std::array<std::uint8_t, 16>, Key>> v; return v; }
}
void Vault::remember (const std::array<std::uint8_t, 16>& salt, const Key& kek)
{
    const juce::ScopedLock sl (vaultLock);
    for (auto& e : vaultData()) if (e.first == salt) { e.second = kek; return; }
    vaultData().push_back ({ salt, kek });
}
bool Vault::find (const std::array<std::uint8_t, 16>& salt, Key& kek)
{
    const juce::ScopedLock sl (vaultLock);
    for (auto& e : vaultData()) if (e.first == salt) { kek = e.second; return true; }
    return false;
}
void Vault::forgetAll() { const juce::ScopedLock sl (vaultLock); for (auto& e : vaultData()) wipe (e.second.data(), e.second.size()); vaultData().clear(); }

juce::String toHex (const void* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    juce::String s;
    s.preallocateBytes (n * 2 + 1);
    for (size_t i = 0; i < n; ++i) { const auto b = ((const std::uint8_t*) p)[i]; s += juce::String::charToString ((juce::juce_wchar) d[b >> 4]); s += juce::String::charToString ((juce::juce_wchar) d[b & 15]); }
    return s;
}

std::vector<std::uint8_t> fromHex (const juce::String& s)
{
    std::vector<std::uint8_t> out;
    auto v = [] (juce::juce_wchar c) { return c >= '0' && c <= '9' ? (int) (c - '0') : c >= 'a' && c <= 'f' ? (int) (c - 'a' + 10) : c >= 'A' && c <= 'F' ? (int) (c - 'A' + 10) : -1; };
    for (int i = 0; i + 1 < s.length(); i += 2)
    {
        const int a = v (s[i]), b = v (s[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back ((std::uint8_t) (a * 16 + b));
    }
    return out;
}
} // namespace otcrypt
