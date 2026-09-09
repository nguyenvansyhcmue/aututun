#include "LiveControlPipeline.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int tick = 64;

void require (bool condition, const char* message)
{
    if (! condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}

NotePolicySnapshot chromaticPolicy()
{
    NotePolicySnapshot policy;
    policy.pitchClassDefaults.fill (NoteRule::Legal);
    policy.midiNoteOverrides.fill (NoteRule::Legal);
    return policy;
}

double frequencyForCents (double cents)
{
    return 440.0 * std::exp2 (cents / 1200.0);
}
}

int main()
{
    LiveControlPipeline live;
    live.prepare (sampleRate, tick);
    const auto policy = chromaticPolicy();

    LiveControlParameters parameters;
    parameters.retuneSpeedMs = 20.0;
    parameters.flexTunePercent = 0.0;

    // A 6 Hz, 40-cent vibrato stays on one target. This is the exact musical
    // protection needed before a Live renderer sees the control stream.
    auto targetChanges = 0;
    auto maxRatioStep = 0.0;
    auto previousRatio = 1.0;
    for (int sample = 0; sample < static_cast<int> (sampleRate); sample += tick)
    {
        const auto seconds = static_cast<double> (sample) / sampleRate;
        const auto cents = 40.0 * std::sin (2.0 * std::numbers::pi * 6.0 * seconds);
        const auto frame = live.observePitch (frequencyForCents (cents), true, false, parameters, policy);
        targetChanges += frame.targetChanged ? 1 : 0;
        const auto advanced = live.advance();
        maxRatioStep = std::max (maxRatioStep, std::abs (advanced.pitchRatio - previousRatio));
        previousRatio = advanced.pitchRatio;
    }
    std::cout << "live_control_vibrato | tick=" << tick << " target_changes=" << targetChanges
              << " max_ratio_step=" << maxRatioStep << '\n';
    require (targetChanges == 0, "vibrato changed the committed target");
    require (maxRatioStep < 0.01, "vibrato created a renderer control step");

    // A smooth two-semitone glide must not produce a staircase of target
    // restarts. The trajectory may publish target changes, but its ratio is
    // still continuous at the 1.33 ms Live control cadence.
    live.reset();
    auto glideMaxRatioStep = 0.0;
    previousRatio = 1.0;
    for (int sample = 0; sample < static_cast<int> (0.45 * sampleRate); sample += tick)
    {
        const auto t = static_cast<double> (sample) / (0.45 * sampleRate);
        const auto cents = -60.0 + 240.0 * std::clamp (t, 0.0, 1.0);
        live.observePitch (frequencyForCents (cents), true, false, parameters, policy);
        const auto advanced = live.advance();
        glideMaxRatioStep = std::max (glideMaxRatioStep, std::abs (advanced.pitchRatio - previousRatio));
        previousRatio = advanced.pitchRatio;
    }
    std::cout << "live_control_glide | max_ratio_step=" << glideMaxRatioStep << '\n';
    require (glideMaxRatioStep < 0.01, "glide created a stepped renderer ratio");

    // A voiced/unvoiced boundary invalidates renderer control rather than
    // inventing pitch for breath or fricative samples.
    for (int i = 0; i < 3; ++i)
        live.observePitch (0.0, false, true, parameters, policy);
    const auto unvoiced = live.advance();
    std::cout << "live_control_unvoiced | valid=" << unvoiced.valid << " ratio=" << unvoiced.pitchRatio << '\n';
    require (! unvoiced.valid && unvoiced.pitchRatio == 1.0,
             "unvoiced control was sent to the pitch renderer");
}
