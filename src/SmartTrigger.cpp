#include "SmartTrigger.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace
{
float toDb (float lin) { return 20.0f * std::log10 (std::max (lin, 1e-7f)); }

float percentile (std::vector<float> v, float p) // p in 0..1
{
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    const float idx = p * (float) (v.size() - 1);
    const size_t lo = (size_t) std::floor (idx), hi = std::min (lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - (float) lo);
}

void fft (std::vector<std::complex<float>>& a) // in place, size = power of two
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const float ang = -2.0f * juce::MathConstants<float>::pi / (float) len;
        const std::complex<float> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<float> w (1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Onset { int pos; float peakLin; float ratio; float riseMs; int peakHop; };
} // namespace

LearnResult analyzeLearn (const float* x, int n, double sr)
{
    LearnResult r;
    r.sampleRate = sr;
    r.length = n;
    if (x == nullptr || n < (int) (2.0 * sr))
    {
        r.message = "Too short. Let the track play for at least 5 seconds with the instrument hitting.";
        return r;
    }
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = std::max (peak, std::abs (x[i]));
    if (peak < 1e-4f) { r.message = "No signal heard. Check that audio is reaching the plug-in and the track is playing."; return r; }

    // ---- envelope at 0.5 ms resolution: peak per hop, with a short release so a decay stays above its own tail
    const int hop = std::max (1, (int) std::lround (sr * 0.0005));
    const int hops = n / hop;
    std::vector<float> env ((size_t) hops);
    const float rel = (float) std::exp (-1.0 / (0.008 / 0.0005)); // 8 ms release, per hop
    float e = 0.0f;
    for (int h = 0; h < hops; ++h)
    {
        float m = 0.0f;
        for (int i = h * hop; i < (h + 1) * hop; ++i) m = std::max (m, std::abs (x[i]));
        e = std::max (m, e * rel);
        env[(size_t) h] = e;
    }
    {
        std::vector<float> db ((size_t) hops);
        for (int h = 0; h < hops; ++h) db[(size_t) h] = toDb (env[(size_t) h]);
        r.noiseFloorDb = percentile (db, 0.20f);
    }
    const float floorLin = std::pow (10.0f, (r.noiseFloorDb + 6.0f) / 20.0f);

    // ---- onsets: the envelope jumps well above what it was 12 ms ago
    std::vector<Onset> onsets;
    const int back = 24, peakSpan = 24, dead = (int) (0.025 / 0.0005);
    int h = back;
    while (h < hops - peakSpan)
    {
        float prevMin = 1e9f;
        for (int k = h - back; k < h; ++k) prevMin = std::min (prevMin, env[(size_t) k]);
        const float now = env[(size_t) h];
        if (now > floorLin && now > 2.0f * std::max (prevMin, 1e-6f))
        {
            int pk = h;
            for (int k = h; k < h + peakSpan; ++k) if (env[(size_t) k] > env[(size_t) pk]) pk = k;
            Onset o;
            o.pos = h * hop;
            o.peakLin = env[(size_t) pk];
            o.ratio = o.peakLin / std::max (prevMin, 1e-6f);
            o.riseMs = (float) ((pk - h) * 0.5);
            o.peakHop = pk;
            onsets.push_back (o);
            h = pk + dead;
        }
        else ++h;
    }
    if (onsets.size() < 4)
    {
        r.message = "Found only " + juce::String ((int) onsets.size()) + " hits. Pick a stretch where the instrument plays clearly, about 10 to 20 hits, soft and loud.";
        return r;
    }

    // ---- hits versus bleed: split at the biggest gap between sorted peak levels, if there is a real one (>= 6 dB)
    std::vector<float> peaksDb;
    for (auto& o : onsets) peaksDb.push_back (toDb (o.peakLin));
    auto sorted = peaksDb;
    std::sort (sorted.begin(), sorted.end());
    float cut = -1000.0f, bestGap = 0.0f;
    for (size_t i = 0; i + 1 < sorted.size(); ++i)
    {
        const size_t upper = sorted.size() - (i + 1);
        const float gap = sorted[i + 1] - sorted[i];
        if (gap >= 6.0f && gap > bestGap && upper >= 3 && upper * 4 >= sorted.size())
        {
            bestGap = gap;
            cut = 0.5f * (sorted[i] + sorted[i + 1]);
        }
    }
    std::vector<Onset> hits;
    for (auto& o : onsets)
    {
        if (toDb (o.peakLin) >= cut) hits.push_back (o);
        else { r.bleedPos.push_back (o.pos); r.bleedDb.push_back (toDb (o.peakLin)); }
    }
    for (auto& o : hits) { r.hitPos.push_back (o.pos); r.hitDb.push_back (toDb (o.peakLin)); }
    r.hits = (int) hits.size();
    r.bleed = (int) r.bleedPos.size();
    std::vector<float> hitDb = r.hitDb;
    const float hitLow = percentile (hitDb, 0.10f), hitHigh = percentile (hitDb, 0.95f);

    // ---- input volume: lift very quiet tracks so the threshold has room to work (max +14 dB, like the plug-in)
    r.inputGainDb = hitHigh < -18.0f ? juce::jlimit (0.0f, 14.0f, -6.0f - hitHigh) : 0.0f;

    // ---- threshold: between bleed and the quietest real hits; or a little under the quietest hits if nothing to split
    float thr;
    if (r.bleed > 0)
    {
        std::vector<float> bd = r.bleedDb;
        thr = 0.5f * (percentile (bd, 0.95f) + hitLow);
        thr = std::min (thr, hitLow - 3.0f);
    }
    else thr = hitLow - 4.0f;
    thr = std::max (thr, r.noiseFloorDb + 9.0f);
    r.thresholdDb = juce::jlimit (-60.0f, -3.0f, thr + r.inputGainDb);

    // ---- retrigger: shorter than the closest real hits, never below 8 ms or above 40 ms
    std::vector<float> ioi;
    for (size_t i = 1; i < hits.size(); ++i) ioi.push_back ((float) (hits[i].pos - hits[i - 1].pos) * 1000.0f / (float) sr);
    const float ioiLow = ioi.empty() ? 200.0f : percentile (ioi, 0.05f);
    // never above 40 ms: this stretch may be slower than the fastest rolls elsewhere in the song
    r.retriggerMs = juce::jlimit (8.0f, 40.0f, std::round (ioiLow * 0.45f));

    // ---- detail (velocity scan window) from how long the hits take to reach their peak
    std::vector<float> rises;
    for (auto& o : hits) rises.push_back (o.riseMs);
    const float window = juce::jlimit (0.5f, 3.5f, 1.2f * percentile (rises, 0.5f) + 0.5f);
    r.detail = juce::jlimit (20.0f, 90.0f, std::round ((window - 0.5f) / 3.0f * 100.0f));

    // ---- sensitivity: how big a jump the quietest accepted hits make over the signal before them
    std::vector<float> ratios;
    for (auto& o : hits) ratios.push_back (o.ratio);
    const float ratioMin = percentile (ratios, 0.10f);
    // stay in the middle of the range: clean hits here say little about ghost notes or tails elsewhere in the song
    const float need = juce::jlimit (1.6f, 2.4f, 0.8f * ratioMin);
    r.sensitivity = std::round ((3.0f - need) / 2.0f * 100.0f);

    // ---- suppress: how close the bleed is to the real hits
    if (r.bleed >= 2)
    {
        std::vector<float> bd = r.bleedDb;
        const float margin = hitLow - percentile (bd, 0.90f);
        r.suppress = std::round (juce::jlimit (0.0f, 1.0f, (12.0f - margin) / 12.0f) * 60.0f);
    }

    // ---- how long the hits ring: time for the envelope to fall 30 dB (or to the noise floor) after the peak
    std::vector<float> decays;
    for (auto& o : hits)
    {
        const float stop = std::max (floorLin, o.peakLin * 0.0316f);
        int k = o.peakHop;
        while (k < hops - 1 && k - o.peakHop < 3000 && env[(size_t) k] > stop) ++k;
        decays.push_back ((float) (k - o.peakHop) * 0.5f);
    }
    r.medianDecayMs = percentile (decays, 0.5f);
    r.medianHitDb = percentile (hitDb, 0.5f);

    // ---- gate: worth having when there is bleed to close on or a noisy floor; hold/release follow the decay
    r.gateOn = r.bleed >= 2 || r.noiseFloorDb > -55.0f;
    r.gateAttackMs = juce::jlimit (0.2f, 5.0f, std::round (std::max (0.5f, 0.5f * percentile (rises, 0.5f)) * 10.0f) / 10.0f);
    r.gateHoldMs = juce::jlimit (10.0f, 400.0f, std::round (0.35f * r.medianDecayMs));
    r.gateReleaseMs = juce::jlimit (60.0f, 1500.0f, std::round (0.6f * r.medianDecayMs));

    // ---- filters: keep the band the hits actually occupy (average spectrum of the first 23 ms of each hit)
    const int N = 1024;
    std::vector<double> power ((size_t) N / 2, 0.0);
    int used = 0;
    for (auto& o : hits)
    {
        if (o.pos + N > n) continue;
        std::vector<std::complex<float>> a ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (N - 1));
            a[(size_t) i] = { x[o.pos + i] * w, 0.0f };
        }
        fft (a);
        for (int k = 1; k < N / 2; ++k) power[(size_t) k] += std::norm (a[(size_t) k]);
        ++used;
    }
    if (used > 0)
    {
        double total = 0.0;
        for (double p : power) total += p;
        double acc = 0.0;
        float f3 = 0.0f, f97 = (float) (sr * 0.5);
        bool got3 = false;
        for (int k = 1; k < N / 2; ++k)
        {
            acc += power[(size_t) k];
            const float f = (float) (k * sr / N);
            if (! got3 && acc >= 0.03 * total) { f3 = f; got3 = true; }
            if (acc >= 0.97 * total) { f97 = f; break; }
        }
        r.lowcutHz = f3 < 40.0f ? 0.0f : juce::jlimit (0.0f, 1000.0f, std::round (f3 * 0.6f));
        const float hc = f97 * 1.6f;
        r.highcutHz = hc > 16000.0f ? 20000.0f : juce::jlimit (400.0f, 20000.0f, std::round (hc));
    }

    r.ok = true;
    r.message = juce::String (r.hits) + " hits heard" + (r.bleed > 0 ? ", " + juce::String (r.bleed) + " other sounds ignored" : juce::String())
                + ". Threshold " + juce::String (r.thresholdDb, 1) + " dB, retrigger " + juce::String ((int) r.retriggerMs) + " ms.";
    return r;
}
