#include "SignalsmithPitchRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;

struct Result
{
    double f0Hz = 0.0, rms = 0.0, peak = 0.0, averageUs = 0.0, worstUs = 0.0;
    int latency = 0;
    bool finite = true;
};

Result run (int blockSize, double sourceF0, double ratio)
{
    SignalsmithPitchRenderer renderer;
    renderer.prepare (sampleRate, blockSize);
    const auto length = static_cast<int> (sampleRate * 6.0);
    std::vector<float> input (static_cast<size_t> (blockSize)), output (static_cast<size_t> (blockSize)), all;
    all.reserve (static_cast<size_t> (length));
    auto totalUs = 0.0, worstUs = 0.0;
    for (int start = 0; start < length; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto t = static_cast<double> (start + sample) / sampleRate;
            input[static_cast<size_t> (sample)] = static_cast<float> (0.30 * std::sin (2.0 * std::numbers::pi * sourceF0 * t)
                + 0.10 * std::sin (4.0 * std::numbers::pi * sourceF0 * t));
        }
        const auto begin = std::chrono::steady_clock::now();
        renderer.process (input.data(), output.data(), blockSize, ratio, true);
        const auto elapsed = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - begin).count();
        totalUs += elapsed;
        worstUs = std::max (worstUs, elapsed);
        all.insert (all.end(), output.begin(), output.end());
    }

    const auto first = renderer.latencySamples() + static_cast<int> (sampleRate * 1.0);
    auto crossings = 0.0, energy = 0.0, peak = 0.0;
    auto finite = true;
    for (int sample = first + 1; sample < static_cast<int> (all.size()); ++sample)
    {
        const auto value = all[static_cast<size_t> (sample)];
        finite = finite && std::isfinite (value);
        energy += value * value;
        peak = std::max (peak, std::abs (static_cast<double> (value)));
        if (all[static_cast<size_t> (sample - 1)] <= 0.0f && value > 0.0f)
            crossings += 1.0;
    }
    const auto count = static_cast<double> (all.size() - first);
    return { crossings * sampleRate / count, std::sqrt (energy / count), peak,
             totalUs / static_cast<double> (length / blockSize), worstUs, renderer.latencySamples(), finite };
}

bool runSibilanceGuard()
{
    constexpr int blockSize = 64;
    SignalsmithPitchRenderer renderer;
    renderer.prepare (sampleRate, blockSize);
    std::vector<float> input (blockSize), output (blockSize);
    auto finite = true;
    auto peak = 0.0;
    uint32_t noiseState = 0x13579bdfu;
    const auto length = static_cast<int> (sampleRate * 2.0);
    for (int start = 0; start < length; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto index = start + sample;
            const auto vowel = 0.20 * std::sin (2.0 * std::numbers::pi * 220.0 * index / sampleRate);
            noiseState = noiseState * 1664525u + 1013904223u;
            const auto noise = (static_cast<double> ((noiseState >> 8) & 0xffffu) / 32767.5 - 1.0);
            const auto isSibilant = index >= static_cast<int> (sampleRate * 0.75)
                                && index < static_cast<int> (sampleRate * 0.95);
            input[static_cast<size_t> (sample)] = static_cast<float> (vowel + (isSibilant ? noise * 0.30 : 0.0));
        }
        renderer.process (input.data(), output.data(), blockSize, std::exp2 (100.0 / 1200.0), true, 220.0);
        for (const auto value : output)
        {
            finite = finite && std::isfinite (value);
            peak = std::max (peak, std::abs (static_cast<double> (value)));
        }
    }
    const auto engaged = renderer.getMaximumSibilanceResidualMix() > 0.05;
    std::cout << "signalsmith_sibilance | engaged=" << engaged
              << " max_mix=" << renderer.getMaximumSibilanceResidualMix()
              << " peak=" << peak << " finite=" << finite << '\n';
    return engaged && finite && peak < 1.0;
}
}

int main()
{
    std::cout << std::fixed << std::setprecision (3);
    auto passed = true;
    for (const auto sourceF0 : { 220.0, 110.0 })
        for (const auto ratio : { 1.0, std::exp2 (-40.0 / 1200.0), std::exp2 (40.0 / 1200.0) })
            for (const auto block : { 64, 128, 256, 512, 1024 })
            {
                const auto result = run (block, sourceF0, ratio);
                const auto expected = sourceF0 * ratio;
                const auto cents = 1200.0 * std::log2 (result.f0Hz / expected);
                // The spectral renderer is allowed a wider instantaneous
                // error than a phase-locked oscillator on a tiny synthetic
                // shift. The production regression is the Artist WAV score;
                // this gate protects its real-time invariants instead.
                passed = passed && result.latency == 1152 && result.finite
                      && result.rms > 0.01 && result.peak < 1.0
                      && std::abs (cents) < 30.0;
                std::cout << "signalsmith_renderer | source_hz=" << sourceF0 << " ratio=" << ratio
                          << " block=" << block << " latency=" << result.latency << " f0_hz=" << result.f0Hz
                          << " expected_hz=" << expected << " cents=" << cents << " rms=" << result.rms
                          << " peak=" << result.peak << " avg_us=" << result.averageUs
                          << " worst_us=" << result.worstUs << " finite=" << result.finite << '\n';
            }
    passed = runSibilanceGuard() && passed;
    std::cout << "SIGNALSMITH_PITCH_RENDERER: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
