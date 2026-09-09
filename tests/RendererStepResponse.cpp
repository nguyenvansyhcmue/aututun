#include "CausalTDPSOLARenderer.h"

#include <cmath>
#include <iostream>
#include <numbers>
#include <vector>

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr int block = 128;
    constexpr int latency = 2048;
    constexpr int commandSample = 48000;
    constexpr int returnSample = commandSample + 3072; // 64 ms
    constexpr int totalSamples = 96000;
    constexpr double sourceF0 = 233.0818808; // MIDI 58
    constexpr double downRatio = 0.8908987181; // -200 cents

    CausalTDPSOLARenderer renderer;
    renderer.prepare (sampleRate, block, latency);
    std::vector<float> input (block), output (block), rendered;
    rendered.reserve (totalSamples);
    double phase = 0.0;
    for (int start = 0; start < totalSamples; start += block)
    {
        const auto ratio = start >= commandSample && start < returnSample ? downRatio : 1.0;
        for (int i = 0; i < block; ++i)
        {
            input[static_cast<size_t> (i)] = static_cast<float> (0.30 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * sourceF0 / sampleRate;
        }
        renderer.process (input.data(), output.data(), block, sourceF0, ratio, true);
        if (start == commandSample || start == returnSample)
        {
            const auto& state = renderer.getDiagnosticState();
            std::cout << "renderer_step_control sample=" << start
                      << " future_grains=" << state.scheduledFutureGrains
                      << " audible_ratio=" << state.audibleRatioAtStart
                      << " spawn_ratio=" << state.ratioAtSpawn << '\n';
        }
        rendered.insert (rendered.end(), output.begin(), output.end());
    }

    std::vector<double> crossings;
    for (int i = 1; i < static_cast<int> (rendered.size()); ++i)
    {
        const auto a = rendered[static_cast<size_t> (i - 1)];
        const auto b = rendered[static_cast<size_t> (i)];
        if (a <= 0.0f && b > 0.0f)
            crossings.push_back (static_cast<double> (i - 1) + (-a) / (b - a));
    }

    const auto basePeriod = sampleRate / sourceF0;
    const auto downPeriod = sampleRate / (sourceF0 * downRatio);
    const auto midpoint = 0.5 * (basePeriod + downPeriod);
    const auto ninety = basePeriod + 0.9 * (downPeriod - basePeriod);
    auto onset = -1.0, settled = -1.0, largestPeriod = 0.0, largestPeriodTime = 0.0;
    for (size_t i = 1; i < crossings.size(); ++i)
    {
        const auto period = crossings[i] - crossings[i - 1];
        if (crossings[i] >= commandSample && period > largestPeriod)
        {
            largestPeriod = period;
            largestPeriodTime = crossings[i];
        }
        if (crossings[i] >= commandSample && onset < 0.0 && period >= midpoint)
            onset = crossings[i];
        if (crossings[i] >= commandSample && settled < 0.0 && period >= ninety)
            settled = crossings[i];
    }
    const auto& state = renderer.getDiagnosticState();
    std::cout << "renderer_step_response"
              << " onset_after_command_ms=" << (onset - commandSample) * 1000.0 / sampleRate
              << " settle90_after_command_ms=" << (settled - commandSample) * 1000.0 / sampleRate
              << " base_period=" << basePeriod << " down_period=" << downPeriod
              << " max_period=" << largestPeriod
              << " max_period_after_command_ms=" << (largestPeriodTime - commandSample) * 1000.0 / sampleRate
              << " final_audible_ratio=" << state.audibleRatioAtStart
              << " final_spawn_ratio=" << state.ratioAtSpawn << '\n';
    return 0;
}
