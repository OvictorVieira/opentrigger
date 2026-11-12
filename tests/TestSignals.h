#pragma once
#include "PluginProcessor.h"
#include <cmath>
#include <vector>

// A drum track in miniature (kick-like 60 Hz hits of different strength, noise floor, quiet bursts from "other drums").
inline std::vector<float> makeTrack (double sr, int seconds)
{
    const int n = (int) (sr * seconds);
    std::vector<float> x ((size_t) n);
    juce::Random rnd (11);
    for (auto& v : x) v = (rnd.nextFloat() * 2.0f - 1.0f) * 0.001f;
    const double gaps[] = { 0.62, 0.9, 1.3, 0.7, 1.1, 0.8, 0.65, 1.2, 0.95, 0.75, 1.0, 0.68, 1.15, 0.85, 0.7, 1.05, 0.9 };
    const float ampsDb[] = { -6, -14, -9, -20, -8, -17, -6, -12, -19, -7, -15, -10, -20, -6, -13, -9, -18 };
    double t = 0.8;
    for (int k = 0; k < 17 && t < seconds - 1.0; ++k)
    {
        const int start = (int) (t * sr);
        const float a = juce::Decibels::decibelsToGain (ampsDb[k]);
        for (int i = 0; i < (int) (0.5 * sr) && start + i < n; ++i)
        {
            const float tt = (float) i / (float) sr;
            x[(size_t) (start + i)] += a * std::min (1.0f, tt / 0.002f) * std::exp (-tt / 0.12f) * std::sin (2.0f * juce::MathConstants<float>::pi * 60.0f * tt);
        }
        t += gaps[k];
    }
    for (double bt : { 1.2, 3.3, 5.9, 8.3, 10.6, 12.2 })
    {
        const int start = (int) (bt * sr) + (int) (0.3 * sr);
        for (int i = 0; i < (int) (0.03 * sr) && start + i < n; ++i)
        {
            const float tt = (float) i / (float) sr;
            x[(size_t) (start + i)] += juce::Decibels::decibelsToGain (-40.0f) * std::exp (-tt / 0.01f) * std::sin (2.0f * juce::MathConstants<float>::pi * 3000.0f * tt);
        }
    }
    return x;
}

// Plays a signal through the plug-in the way a host would: 512-sample blocks.
inline void playThrough (OpenTriggerProcessor& proc, const std::vector<float>& x, int from = 0, int to = -1)
{
    if (to < 0) to = (int) x.size();
    juce::MidiBuffer m;
    for (int pos = from; pos + 512 <= to; pos += 512)
    {
        juce::AudioBuffer<float> io (2, 512);
        for (int i = 0; i < 512; ++i) { io.setSample (0, i, x[(size_t) (pos + i)]); io.setSample (1, i, x[(size_t) (pos + i)]); }
        proc.processBlock (io, m);
    }
}

