#pragma once

#include <algorithm>
#include <cmath>

// Adds a small, parallel harmonic colour to a stable voiced vowel.  It is
// deliberately independent from pitch detection and TD-PSOLA scheduling:
// this class never changes F0, target notes, grain timing, or dry residuals.
class VoicedColourProcessor
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = std::max (1.0, newSampleRate);
        reset();
    }

    void reset() noexcept { smoothedMix = 0.0; }

    float processWetSample (float wetSample,
                            bool isStableVoiced,
                            double periodicCoherence,
                            double sibilanceResidualMix,
                            float amountPercent) noexcept
    {
        const auto amount = std::clamp (amountPercent * 0.01f, 0.0f, 1.0f);
        const auto isVowelLike = periodicCoherence >= minimumPeriodicCoherence;
        const auto hasCleanHighBand = sibilanceResidualMix <= maximumSibilanceResidualMix;
        const auto canColourThisSample = amount > 0.0f && isStableVoiced && isVowelLike && hasCleanHighBand;

        // Colour must leave immediately when the signal becomes a consonant
        // or breath. This prevents a harmonic processor from roughening /s/
        // and /t/ while the renderer crossfades back to its dry residual.
        if (! canColourThisSample)
        {
            smoothedMix = 0.0;
            return wetSample;
        }

        // The amount is a parallel blend, not a gain control. At 100% the
        // wet vowel contains at most 18% shaped signal, leaving the plugin
        // useful as a pitch corrector rather than turning into a distortion.
        const auto targetMix = amount * maximumParallelMix;
        const auto mixAlpha = 1.0 - std::exp (-1.0 / (mixSmoothingSeconds * sampleRate));
        smoothedMix += (targetMix - smoothedMix) * mixAlpha;

        const auto drive = 1.0f + amount * maximumAdditionalDrive;
        // Divide by drive, not by tanh(drive): quiet material then retains
        // its unity small-signal gain, while louder vowel peaks compress
        // progressively. This makes Colour a timbre control rather than an
        // accidental loudness boost.
        const auto saturatedSample = std::tanh (wetSample * drive) / drive;
        return static_cast<float> (static_cast<double> (wetSample)
                                   + (static_cast<double> (saturatedSample) - wetSample) * smoothedMix);
    }

private:
    static constexpr double minimumPeriodicCoherence = 0.82;
    static constexpr double maximumSibilanceResidualMix = 0.15;
    static constexpr double mixSmoothingSeconds = 0.010;
    static constexpr float maximumParallelMix = 0.18f;
    static constexpr float maximumAdditionalDrive = 2.5f;

    double sampleRate = 48000.0;
    double smoothedMix = 0.0;
};
