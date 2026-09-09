#include "BufferedSpectralPitchRenderer.h"

#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 128;
    BufferedSpectralPitchRenderer renderer;
    renderer.prepare (sampleRate, block);
    std::vector<float> input (block), output (block);
    std::vector<float> rendered;
    auto phase = 0.0;
    auto energy = 0.0;
    for (int start = 0; start < 48000; start += block)
    {
        for (int n = 0; n < block; ++n)
        {
            input[static_cast<size_t> (n)] = static_cast<float> (0.3 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * 220.0 / sampleRate;
        }
        renderer.process (input.data(), output.data(), block, 1.0, start < 24000 ? 1 : 2);
        for (const auto sample : output) { energy += sample * sample; rendered.push_back (sample); }
    }
    auto crossings = 0;
    for (int n = 24001; n < 36000; ++n)
        if (rendered[static_cast<size_t> (n - 1)] <= 0.0f && rendered[static_cast<size_t> (n)] > 0.0f) ++crossings;
    const auto frequency = crossings * sampleRate / 12000.0;
    std::cout << "buffered_spectral_probe | energy=" << energy << " unity_hz=" << frequency << '\n';
    if (! std::isfinite (energy) || energy < 1.0 || std::abs (frequency - 220.0) > 8.0)
        return 1;
    std::cout << "BUFFERED_SPECTRAL_FOUNDATION: PASS | energy=" << energy
              << " unity_hz=" << frequency << '\n';

    BufferedSpectralPitchRenderer shiftedRenderer;
    shiftedRenderer.prepare (sampleRate, block);
    rendered.clear();
    phase = 0.0;
    constexpr auto upRatio = 1.04;
    for (int start = 0; start < 48000; start += block)
    {
        for (int n = 0; n < block; ++n)
        {
            input[static_cast<size_t> (n)] = static_cast<float> (0.3 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * 220.0 / sampleRate;
        }
        shiftedRenderer.process (input.data(), output.data(), block, upRatio, 1);
        rendered.insert (rendered.end(), output.begin(), output.end());
    }
    crossings = 0;
    for (int n = 24001; n < 36000; ++n)
        if (rendered[static_cast<size_t> (n - 1)] <= 0.0f && rendered[static_cast<size_t> (n)] > 0.0f) ++crossings;
    const auto shiftedFrequency = crossings * sampleRate / 12000.0;
    std::cout << "buffered_spectral_shift | expected_hz=" << 220.0 * upRatio
              << " measured_hz=" << shiftedFrequency << '\n';
    if (std::abs (shiftedFrequency - 220.0 * upRatio) > 8.0)
        return 1;

    BufferedSpectralPitchRenderer handoffRenderer;
    handoffRenderer.prepare (sampleRate, block);
    phase = 0.0;
    auto maximumStep = 0.0;
    auto previous = 0.0f;
    std::vector<float> handoffRendered;
    for (int start = 0; start < 48000; start += block)
    {
        const auto ratio = start >= 19200 && start < 23040 ? upRatio : 1.0;
        const auto event = start < 19200 ? 1 : start < 23040 ? 2 : 3;
        for (int n = 0; n < block; ++n)
        {
            input[static_cast<size_t> (n)] = static_cast<float> (0.3 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * 220.0 / sampleRate;
        }
        handoffRenderer.process (input.data(), output.data(), block, ratio, event);
        for (const auto sample : output)
        {
            if (! std::isfinite (sample)) return 1;
            maximumStep = std::max (maximumStep, std::abs (static_cast<double> (sample - previous)));
            previous = sample;
            handoffRendered.push_back (sample);
        }
    }
    crossings = 0;
    constexpr int bProbeStart = 20736;
    constexpr int bProbeEnd = 22784;
    for (int n = bProbeStart + 1; n < bProbeEnd; ++n)
        if (handoffRendered[static_cast<size_t> (n - 1)] <= 0.0f
            && handoffRendered[static_cast<size_t> (n)] > 0.0f) ++crossings;
    const auto bFrequency = crossings * sampleRate / static_cast<double> (bProbeEnd - bProbeStart);
    std::cout << "buffered_spectral_handoff | max_step=" << maximumStep << '\n';
    std::cout << "buffered_spectral_handoff_b | expected_hz=" << 220.0 * upRatio
              << " measured_hz=" << bFrequency << '\n';
    if (maximumStep > 0.20 || std::abs (bFrequency - 220.0 * upRatio) > 8.0)
        return 1;
}
