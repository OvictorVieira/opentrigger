#pragma once
#include <juce_core/juce_core.h>
#include <vector>

// Smart Trigger: listens to a stretch of the track and proposes the detection settings for it.
// Pure analysis, no GUI, no audio-thread code.
struct LearnResult
{
    bool ok = false;
    juce::String message;            // what went wrong, or a one-line summary

    int hits = 0, bleed = 0;         // events judged to be the instrument / events judged to be other drums or noise
    float noiseFloorDb = -100.0f;

    // proposed settings (same units as the plug-in parameters)
    float thresholdDb = -30.0f, retriggerMs = 30.0f, lowcutHz = 0.0f, highcutHz = 20000.0f;
    float sensitivity = 50.0f, detail = 50.0f, suppress = 0.0f, inputGainDb = 0.0f;
    // gate: only turned on when the track has something to clean (bleed or a noisy floor); times follow how long the hits ring
    bool gateOn = false;
    float gateAttackMs = 0.5f, gateHoldMs = 4.0f, gateReleaseMs = 450.0f;
    float medianHitDb = -100.0f;     // typical hit peak (dBFS, before the proposed input gain), for level matching
    float medianDecayMs = 0.0f;      // how long a typical hit takes to fall 30 dB
    // output volume that makes the loaded kit piece as loud as the original hit; filled in by the caller (needs the samples)
    bool hasOutput = false;
    float outputGainDb = 0.0f;

    // for drawing the result
    double sampleRate = 44100.0;
    int length = 0;
    std::vector<int> hitPos, bleedPos;      // sample positions of the onsets
    std::vector<float> hitDb, bleedDb;      // their peak levels (dBFS, before the proposed input gain)
};

// x: mono audio as the plug-in received it (before its own input gain / gate).
LearnResult analyzeLearn (const float* x, int numSamples, double sampleRate);

// Tuning: the pitch a drum rings at, in Hz (0 when no clear pitch is found). x: one hit, mono, starting at the attack.
float estimateFundamentalHz (const float* x, int numSamples, double sampleRate);
// Tune knob helpers: semitones <-> the Hz the piece sounds at, and the note name with cents ("F#3 +12c").
float tunedHz (float baseHz, float semis);
float semisForHz (float baseHz, float targetHz);
juce::String noteWithCents (float hz);
