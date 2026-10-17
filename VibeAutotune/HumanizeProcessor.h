#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Sustain-only residual policy. This processor is intentionally unaware of
// note selection, renderer state and Natural Vibrato shaping: it returns a
// bounded retention weight from already-classified analysis features.
class HumanizeProcessor
{
public:
    void reset() noexcept { sustainWeight = 0.0; }

    [[nodiscard]] static double artistResponse (double humanizePercent) noexcept
    {
        constexpr std::array<double, 5> positions {{ 0.0, 25.0, 50.0, 75.0, 100.0 }};
        constexpr std::array<double, 5> responses {{ 0.0, 0.680, 0.878, 0.943, 1.0 }};
        const auto value = clamp (humanizePercent, positions.front(), positions.back());
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                const auto amount = (value - positions[index - 1]) / (positions[index] - positions[index - 1]);
                return responses[index - 1] + (responses[index] - responses[index - 1]) * amount;
            }
        return responses.back();
    }

    double process (double humanizePercent, double fastRetuneAmount,
                    double sustainRamp, double vibratoConfidence,
                    double sampleRate, int hopSamples) noexcept
    {
        if (sampleRate <= 0.0 || hopSamples <= 0)
            return clamp01 (sustainWeight);
        const auto requested = artistResponse (humanizePercent)
                             * clamp01 (fastRetuneAmount)
                             * clamp01 (sustainRamp)
                             * clamp01 (vibratoConfidence);
        const auto alpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                            / (weightTimeSeconds * sampleRate));
        sustainWeight += (requested - sustainWeight) * alpha;
        return clamp01 (sustainWeight);
    }

private:
    static constexpr double weightTimeSeconds = 0.040;

    static double clamp (double value, double minimum, double maximum) noexcept
    {
        return std::max (minimum, std::min (maximum, value));
    }
    static double clamp01 (double value) noexcept { return clamp (value, 0.0, 1.0); }

    double sustainWeight = 0.0;
};
