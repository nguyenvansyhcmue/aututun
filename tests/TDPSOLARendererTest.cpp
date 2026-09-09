#include "TDPSOLARenderer.h"

#include <cmath>
#include <chrono>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0, sourceF0 = 220.0;
struct Result
{
    double f0 = 0.0, rms = 0.0, peak = 0.0, boundaryStep = 0.0;
    double averageProcessUs = 0.0, worstProcessUs = 0.0;
    std::array<double, 4> segmentF0 {};
    double minimumLead = 0.0, maximumLead = 0.0;
    int64_t repeats = 0, skips = 0, reanchors = 0;
};

Result run (int blockSize, double ratio, double frequency = sourceF0, int latency = 8192)
{
    TDPSOLARenderer renderer;
    renderer.prepare (sampleRate, blockSize, latency);
    const auto length = static_cast<int> (sampleRate * 8.0);
    std::vector<float> input (static_cast<size_t> (blockSize)), output (static_cast<size_t> (blockSize)), all;
    all.reserve (length);
    double last = 0.0, largestStep = 0.0, totalProcessUs = 0.0, worstProcessUs = 0.0;
    auto blocks = 0;
    for (int start = 0; start < length; start += blockSize)
    {
        for (int n = 0; n < blockSize; ++n)
        {
            const auto t = static_cast<double> (start + n) / sampleRate;
            const auto fundamental = 0.28 * std::sin (2.0 * std::numbers::pi * frequency * t);
            // Keep the required 220 Hz harmonic source; use a pure sine for
            // low-F0 regression so the measurement cannot choose a harmonic.
            input[static_cast<size_t> (n)] = static_cast<float> (frequency > 110.0
                ? fundamental + 0.10 * std::sin (4.0 * std::numbers::pi * frequency * t)
                : fundamental);
        }
        const auto startTime = std::chrono::steady_clock::now();
        renderer.process (input.data(), output.data(), blockSize, frequency, ratio, true);
        const auto elapsedUs = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - startTime).count();
        totalProcessUs += elapsedUs;
        worstProcessUs = std::max (worstProcessUs, elapsedUs);
        ++blocks;
        for (auto value : output)
        {
            largestStep = std::max (largestStep, std::abs (static_cast<double> (value) - last));
            last = value;
            all.push_back (value);
        }
    }
    const auto first = static_cast<int> (sampleRate * 2.0);
    double energy = 0.0, peak = 0.0;
    for (int n = first + 1; n < static_cast<int> (all.size()); ++n)
    {
        const auto value = all[static_cast<size_t> (n)];
        energy += value * value;
        peak = std::max (peak, std::abs (static_cast<double> (value)));
    }
    const auto count = static_cast<double> (all.size() - first);
    const auto estimateDominantF0 = [&all] (int start, int end, double expected) noexcept
    {
        constexpr auto resolutionHz = 0.25;
        constexpr auto twoPi = 2.0 * std::numbers::pi;
        auto bestFrequency = expected;
        auto bestMagnitude = -1.0;
        const auto lower = std::max (45.0, expected * 0.80);
        const auto upper = expected * 1.20;
        auto mean = 0.0;
        for (int n = start; n < end; ++n)
            mean += static_cast<double> (all[static_cast<size_t> (n)]);
        mean /= static_cast<double> (end - start);
        for (auto candidate = lower; candidate <= upper; candidate += resolutionHz)
        {
            const auto increment = twoPi * candidate / sampleRate;
            const auto cosineIncrement = std::cos (increment);
            const auto sineIncrement = std::sin (increment);
            auto cosine = 1.0, sine = 0.0, real = 0.0, imaginary = 0.0;
            for (int n = start; n < end; ++n)
            {
                const auto position = static_cast<double> (n - start) / static_cast<double> (end - start - 1);
                const auto window = 0.5 * (1.0 - std::cos (twoPi * position));
                const auto value = (static_cast<double> (all[static_cast<size_t> (n)]) - mean) * window;
                real += value * cosine;
                imaginary -= value * sine;
                const auto nextCosine = cosine * cosineIncrement - sine * sineIncrement;
                sine = sine * cosineIncrement + cosine * sineIncrement;
                cosine = nextCosine;
            }
            const auto magnitude = real * real + imaginary * imaginary;
            if (magnitude > bestMagnitude) { bestMagnitude = magnitude; bestFrequency = candidate; }
        }
        return bestFrequency;
    };

    std::array<double, 4> segments {};
    constexpr auto segmentCount = 4;
    const auto segmentLength = (static_cast<int> (all.size()) - first) / segmentCount;
    for (int segment = 0; segment < segmentCount; ++segment)
    {
        const auto segmentStart = first + segment * segmentLength;
        const auto segmentEnd = segment == segmentCount - 1 ? static_cast<int> (all.size()) : segmentStart + segmentLength;
        segments[static_cast<size_t> (segment)] = estimateDominantF0 (segmentStart,
                                                                        std::min (segmentEnd, segmentStart + 8192),
                                                                        frequency * ratio);
    }
    return { segments.back(), std::sqrt (energy / count), peak, largestStep,
             totalProcessUs / static_cast<double> (blocks), worstProcessUs, segments,
             renderer.getMinimumSourceLeadSamples(), renderer.getMaximumSourceLeadSamples(),
             renderer.getSourceRepeatCount(), renderer.getSourceSkipCount(), renderer.getSourceReanchorCount() };
}

void require (bool ok, const char* message) { if (! ok) { std::cerr << "FAILED: " << message << '\n'; std::exit (1); } }
}

int main()
{
    std::cout << std::fixed << std::setprecision (3);
    constexpr auto latency = 8192;
    for (const auto frequency : { 220.0, 110.0, 80.0 })
      for (const auto ratio : { 1.0, std::exp2 (-40.0 / 1200.0), std::exp2 (40.0 / 1200.0) })
        for (const auto block : { 64, 128, 256, 512, 1024 })
        {
            const auto result = run (block, ratio, frequency, latency);
            const auto expected = frequency * ratio;
            const auto cents = 1200.0 * std::log2 (result.f0 / expected);
            std::cout << "tdpsola_sustain | latency=" << latency << " source_hz=" << frequency << " block=" << block << " ratio=" << ratio
                      << " f0_hz=" << result.f0 << " expected_hz=" << expected << " cents=" << cents
                      << " rms=" << result.rms << " peak=" << result.peak << " max_step=" << result.boundaryStep
                      << " f0_segments=" << result.segmentF0[0] << ',' << result.segmentF0[1] << ','
                      << result.segmentF0[2] << ',' << result.segmentF0[3]
                      << " lead_min=" << result.minimumLead << " lead_max=" << result.maximumLead
                      << " repeat=" << result.repeats << " skip=" << result.skips << " reanchor=" << result.reanchors
                      << " avg_us=" << result.averageProcessUs << " worst_us=" << result.worstProcessUs << '\n';
        }

    // Stateful control transition plus a short unvoiced interval. This checks
    // that the standalone renderer never emits invalid samples or loses its
    // delayed passthrough path at a block boundary.
    TDPSOLARenderer renderer;
    renderer.prepare (sampleRate, 256);
    std::vector<float> input (256), output (256);
    auto peak = 0.0;
    auto nonFinite = false;
    for (int start = 0; start < static_cast<int> (sampleRate * 3.0); start += 256)
    {
        const auto ratio = start < 48000 ? 1.0 : std::exp2 (-40.0 / 1200.0);
        const auto voiced = start < 72000 || start >= 76800;
        for (int n = 0; n < 256; ++n)
        {
            const auto t = static_cast<double> (start + n) / sampleRate;
            input[static_cast<size_t> (n)] = static_cast<float> (0.28 * std::sin (2.0 * std::numbers::pi * sourceF0 * t));
        }
        renderer.process (input.data(), output.data(), 256, sourceF0, ratio, voiced);
        for (auto sample : output)
        {
            nonFinite = nonFinite || ! std::isfinite (sample);
            peak = std::max (peak, std::abs (static_cast<double> (sample)));
        }
    }
    std::cout << "tdpsola_state_transition | latency_samples=" << renderer.getLatencySamples()
              << " peak=" << peak << " non_finite=" << nonFinite << '\n';
    require (! nonFinite && peak < 0.9, "ratio or voiced state transition failed");

    TDPSOLARenderer vibratoRenderer;
    constexpr auto vibratoBlock = 256;
    vibratoRenderer.prepare (sampleRate, vibratoBlock, 8192);
    std::vector<float> vibratoInput (vibratoBlock), vibratoOutput (vibratoBlock);
    auto vibratoPhase = 0.0, lastOutput = 0.0, maximumOutputStep = 0.0;
    auto finiteVibrato = true;
    for (int start = 0; start < static_cast<int> (sampleRate * 4.0); start += vibratoBlock)
    {
        for (int n = 0; n < vibratoBlock; ++n)
        {
            const auto time = static_cast<double> (start + n) / sampleRate;
            const auto f0 = 220.0 * std::exp2 ((35.0 * std::sin (2.0 * std::numbers::pi * 6.0 * time)) / 1200.0);
            vibratoInput[static_cast<size_t> (n)] = static_cast<float> (0.28 * std::sin (vibratoPhase)
                + 0.08 * std::sin (2.0 * vibratoPhase));
            vibratoPhase += 2.0 * std::numbers::pi * f0 / sampleRate;
        }
        const auto centreTime = static_cast<double> (start + vibratoBlock / 2) / sampleRate;
        const auto controlF0 = 220.0 * std::exp2 ((35.0 * std::sin (2.0 * std::numbers::pi * 6.0 * centreTime)) / 1200.0);
        // Deliberately step the requested ratio every 1024 samples. The
        // renderer's ratio smoother must turn this into continuous marks.
        const auto requestedRatio = ((start / 1024) % 3 == 0) ? 1.0
                                  : ((start / 1024) % 3 == 1) ? 1.04 : 0.96;
        vibratoRenderer.process (vibratoInput.data(), vibratoOutput.data(), vibratoBlock, controlF0, requestedRatio, true);
        for (int n = 0; n < vibratoBlock; ++n)
        {
            const auto value = vibratoOutput[static_cast<size_t> (n)];
            finiteVibrato = finiteVibrato && std::isfinite (value);
            const auto step = std::abs (static_cast<double> (value) - lastOutput);
            maximumOutputStep = std::max (maximumOutputStep, step);
            if (step > 0.10)
                std::cout << "tdpsola_discontinuity | sample=" << (start + n)
                          << " in_block=" << n
                          << " step=" << step
                          << " requested_ratio=" << requestedRatio
                          << " smoothed_ratio=" << vibratoRenderer.getSmoothedPitchRatio()
                          << " synth_period=" << vibratoRenderer.getLastSynthesisMarkDistance()
                          << " max_period_jump=" << vibratoRenderer.getMaximumSynthesisMarkDistanceJump()
                          << '\n';
            lastOutput = value;
        }
        if ((start + vibratoBlock) % 1024 == 0)
            std::cout << "tdpsola_vibrato_boundary | sample=" << (start + vibratoBlock)
                      << " synth_period=" << vibratoRenderer.getLastSynthesisMarkDistance()
                      << " max_period_jump=" << vibratoRenderer.getMaximumSynthesisMarkDistanceJump() << '\n';
    }
    std::cout << "tdpsola_vibrato_summary | max_output_step=" << maximumOutputStep
              << " max_synth_period_jump=" << vibratoRenderer.getMaximumSynthesisMarkDistanceJump()
              << " finite=" << finiteVibrato << '\n';
    require (finiteVibrato && maximumOutputStep < 0.10, "vibrato created a TD-PSOLA discontinuity");
}
