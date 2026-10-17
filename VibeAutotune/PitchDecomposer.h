#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// Allocation-free, analysis-hop-rate pitch decomposition. It deliberately
// owns only observation state: it does not select notes, alter a correction
// target, or smooth a renderer control. This keeps source analysis separate
// from Retune/Flex policy and makes its telemetry safe to inspect offline.
struct PitchComponents
{
    double noteCenterCents = 0.0;
    double driftCents = 0.0;
    double vibratoCents = 0.0;
    double residualCents = 0.0;
    double vibratoEnergyCents = 0.0;
    double vibratoRateHz = 0.0;
    double vibratoConfidence = 0.0;
    double driftConfidence = 0.0;
    double jitterConfidence = 0.0;
    double noteStability = 0.0;
    double residualDeviationCents = 0.0;
    bool periodicVibrato = false;
};

class PitchDecomposer
{
public:
    static constexpr int historyLength = 48;

    void reset() noexcept
    {
        noteCenterCents = std::numeric_limits<double>::quiet_NaN();
        slowNoteCenterCents = std::numeric_limits<double>::quiet_NaN();
        driftFollowerCents = std::numeric_limits<double>::quiet_NaN();
        vibratoEnergyCents = 0.0;
        vibratoRecognition = 0.0;
        residualHistory.fill (0.0);
        historyIndex = 0;
        historyCount = 0;
    }

    PitchComponents process (double inputCents, double stableF0Hz,
                             double sampleRate, int hopSamples,
                             double centreSeconds) noexcept
    {
        PitchComponents result;
        if (! std::isfinite (inputCents) || ! std::isfinite (stableF0Hz)
            || sampleRate <= 0.0 || hopSamples <= 0)
            return result;

        if (! std::isfinite (noteCenterCents))
            noteCenterCents = inputCents;
        if (! std::isfinite (slowNoteCenterCents))
            slowNoteCenterCents = inputCents;
        if (! std::isfinite (driftFollowerCents))
            driftFollowerCents = inputCents;

        const auto centreAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                  / (std::max (0.001, centreSeconds) * sampleRate));
        noteCenterCents += (inputCents - noteCenterCents) * centreAlpha;

        // A second, diagnostic-only decomposition separates intentional
        // phrase drift from the existing correction centre. The very-slow
        // baseline follows note intent; the faster follower admits roughly
        // sub-vibrato movement. Their difference is slow drift. Neither
        // state feeds Retune/Flex until the Artist A/B gate proves it.
        const auto slowCenterAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                      / (slowNoteCenterSeconds * sampleRate));
        const auto driftFollowerAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                         / (driftFollowerSeconds * sampleRate));
        slowNoteCenterCents += (inputCents - slowNoteCenterCents) * slowCenterAlpha;
        driftFollowerCents += (inputCents - driftFollowerCents) * driftFollowerAlpha;
        const auto slowDrift = driftFollowerCents - slowNoteCenterCents;
        const auto residual = clamp (inputCents - noteCenterCents,
                                     -maximumInputResidualCents, maximumInputResidualCents);

        const auto energyAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                  / (energyTimeSeconds * sampleRate));
        vibratoEnergyCents += (std::abs (residual) - vibratoEnergyCents) * energyAlpha;

        residualHistory[static_cast<size_t> (historyIndex)] = residual;
        historyIndex = (historyIndex + 1) % historyLength;
        historyCount = std::min (historyCount + 1, historyLength);

        const auto adaptiveWindowSeconds = clamp (0.10 * 220.0 / stableF0Hz, 0.050, 0.200);
        const auto adaptiveWindowFrames = std::clamp (static_cast<int> (std::lround (adaptiveWindowSeconds * sampleRate / hopSamples)),
                                                       5, historyLength);
        const auto varianceFrames = std::min (historyCount, adaptiveWindowFrames);
        auto mean = 0.0;
        for (int age = 0; age < varianceFrames; ++age)
            mean += historyAtAge (age);
        mean /= std::max (1, varianceFrames);

        auto variance = 0.0;
        for (int age = 0; age < varianceFrames; ++age)
        {
            const auto delta = historyAtAge (age) - mean;
            variance += delta * delta;
        }
        const auto deviation = std::sqrt (variance / std::max (1, varianceFrames));

        const auto rateFrames = std::min (historyCount,
                                          static_cast<int> (std::lround (0.250 * sampleRate / hopSamples)));
        auto zeroCrossings = 0;
        auto previousSign = 0;
        for (int age = rateFrames - 1; age >= 0; --age)
        {
            const auto value = historyAtAge (age) - mean;
            const auto sign = value > 0.0 ? 1 : (value < 0.0 ? -1 : 0);
            if (sign != 0 && previousSign != 0 && sign != previousSign)
                ++zeroCrossings;
            if (sign != 0)
                previousSign = sign;
        }
        const auto rateSeconds = static_cast<double> (rateFrames * hopSamples) / sampleRate;
        const auto rateHz = rateSeconds > 0.0 ? static_cast<double> (zeroCrossings) / (2.0 * rateSeconds) : 0.0;
        const auto periodic = deviation >= minimumVibratoDeviationCents
                           && rateHz >= minimumVibratoRateHz && rateHz <= maximumVibratoRateHz;
        const auto recognitionAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                       / (recognitionTimeSeconds * sampleRate));
        vibratoRecognition += ((periodic ? 1.0 : 0.0) - vibratoRecognition) * recognitionAlpha;

        // The production path historically delivers the entire residual to
        // its calibrated renderer law. This is a diagnostic decomposition
        // until the separate paths are fitted against the reference suite.
        const auto confidence = clamp01 (vibratoRecognition);
        result.noteCenterCents = noteCenterCents;
        result.residualCents = residual;
        result.vibratoCents = residual * confidence;
        result.driftCents = slowDrift;
        result.vibratoEnergyCents = vibratoEnergyCents;
        result.vibratoRateHz = rateHz;
        result.vibratoConfidence = confidence;
        // Confidence is intentionally continuous. A residual with coherent
        // low-frequency drift but little periodic evidence is drift; energy
        // left after both components is treated as jitter/noise.
        const auto driftEnergy = std::abs (slowDrift);
        const auto motionEnergy = std::max (1.0e-6, driftEnergy + deviation);
        result.driftConfidence = clamp01 ((driftEnergy / motionEnergy) * (1.0 - confidence));
        result.jitterConfidence = clamp01 ((deviation / motionEnergy)
                                           * (1.0 - confidence)
                                           * (1.0 - result.noteStability));
        result.residualDeviationCents = deviation;
        result.noteStability = clamp01 (1.0 - deviation / 30.0);
        result.periodicVibrato = periodic;
        return result;
    }

    [[nodiscard]] double residualAtAge (int age) const noexcept { return historyAtAge (age); }
    [[nodiscard]] int availableHistoryFrames() const noexcept { return historyCount; }

private:
    static constexpr double maximumInputResidualCents = 60.0;
    static constexpr double energyTimeSeconds = 0.100;
    static constexpr double minimumVibratoDeviationCents = 3.0;
    static constexpr double minimumVibratoRateHz = 4.0;
    static constexpr double maximumVibratoRateHz = 8.0;
    static constexpr double recognitionTimeSeconds = 0.100;
    static constexpr double slowNoteCenterSeconds = 1.20;
    static constexpr double driftFollowerSeconds = 0.055;

    static double clamp (double value, double minimum, double maximum) noexcept
    {
        return std::max (minimum, std::min (maximum, value));
    }
    static double clamp01 (double value) noexcept { return clamp (value, 0.0, 1.0); }
    [[nodiscard]] double historyAtAge (int age) const noexcept
    {
        const auto index = (historyIndex - 1 - age + historyLength) % historyLength;
        return residualHistory[static_cast<size_t> (index)];
    }

    double noteCenterCents = std::numeric_limits<double>::quiet_NaN();
    double slowNoteCenterCents = std::numeric_limits<double>::quiet_NaN();
    double driftFollowerCents = std::numeric_limits<double>::quiet_NaN();
    double vibratoEnergyCents = 0.0;
    double vibratoRecognition = 0.0;
    std::array<double, historyLength> residualHistory {};
    int historyIndex = 0;
    int historyCount = 0;
};
