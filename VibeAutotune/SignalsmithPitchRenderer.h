#pragma once

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

// Candidate hook only. The production limits below preserve the qualified
// renderer behaviour; offline A/B builds may widen them without editing DSP.
#ifndef VIBE_SPECTRAL_MIN_RATIO
 #define VIBE_SPECTRAL_MIN_RATIO 0.90
#endif
#ifndef VIBE_SPECTRAL_MAX_RATIO
 #define VIBE_SPECTRAL_MAX_RATIO 1.10
#endif

// Offline A/B hooks. The production cadence remains 1024/128 unless an
// explicitly configured candidate build proves a better vocal transition.
#ifndef VIBE_SPECTRAL_WINDOW_SAMPLES
 #define VIBE_SPECTRAL_WINDOW_SAMPLES 1024
#endif
#ifndef VIBE_SPECTRAL_HOP_SAMPLES
 #define VIBE_SPECTRAL_HOP_SAMPLES 128
#endif

// Fixed-latency spectral pitch renderer used for voiced correction.  It owns
// no note selection: the existing trajectory remains the sole control source.
class SignalsmithPitchRenderer
{
public:
    void prepare (double newSampleRate, int maximumBlock)
    {
        sampleRate = newSampleRate;
        stretch.configure (1, VIBE_SPECTRAL_WINDOW_SAMPLES,
                           VIBE_SPECTRAL_HOP_SAMPLES, true);
        stretch.setFormantBase (static_cast<float> (200.0 / sampleRate));
        stretch.reset();
        dryDelay.assign (static_cast<size_t> (latencySamples() + maximumBlock + 1), 0.0f);
        sibilanceTargetDelay.assign (dryDelay.size(), 0.0f);
        dryWrite = 0;
        wetGate = 0.0f;
        sibilanceResidualMix = 0.0f;
        maximumSibilanceResidualMix = 0.0f;
        wetLowState = dryLowState = sourceLowState = 0.0f;
        sourceEnergy = highEnergy = delayedEnergy = 1.0e-12;
        correlation = 0.0;
        sourceEvidenceAlpha = 1.0 - std::exp (-1.0 / (sourceEvidenceSeconds * sampleRate));
        sourceCrossoverAlpha = 1.0 - std::exp (-2.0 * std::numbers::pi_v<double>
                                               * sourceAnalysisCrossoverHz / sampleRate);
        // This is a lifecycle gate, not a musical envelope or a noise gate.
        // It only prevents an instantaneous spectral-wet -> delayed-dry
        // splice when voicing certification changes at a control boundary.
        wetGateAttack = onePoleCoefficient (0.0015);
        wetGateRelease = onePoleCoefficient (0.0040);
    }
    void reset()
    {
        stretch.reset();
        std::fill (dryDelay.begin(), dryDelay.end(), 0.0f);
        std::fill (sibilanceTargetDelay.begin(), sibilanceTargetDelay.end(), 0.0f);
        dryWrite = 0;
        wetGate = 0.0f;
        sibilanceResidualMix = 0.0f;
        maximumSibilanceResidualMix = 0.0f;
        wetLowState = dryLowState = sourceLowState = 0.0f;
        sourceEnergy = highEnergy = delayedEnergy = 1.0e-12;
        correlation = 0.0;
    }
    int latencySamples() const noexcept { return stretch.inputLatency() + stretch.outputLatency(); }
    double getMaximumSibilanceResidualMix() const noexcept { return maximumSibilanceResidualMix; }

    // `sourceF0Hz` is advisory analysis metadata from the already committed
    // control stream. It never selects a target; it only avoids treating a
    // periodic vowel harmonic as the non-periodic tail of an /s/.
    void process (const float* source, float* destination, int samples, double ratio, bool voiced,
                  double sourceF0Hz = std::numeric_limits<double>::quiet_NaN()) noexcept
    {
        if (source == nullptr || destination == nullptr || samples <= 0 || dryDelay.empty()
            || sibilanceTargetDelay.size() != dryDelay.size()) return;
        const auto safeRatio = static_cast<float> (std::clamp (ratio, static_cast<double> (VIBE_SPECTRAL_MIN_RATIO),
                                                               static_cast<double> (VIBE_SPECTRAL_MAX_RATIO)));
        stretch.setTransposeFactor (safeRatio);
        struct Input { const float* p; const float* operator[] (int) const { return p; } } input { source };
        struct Output { float* p; float* operator[] (int) const { return p; } } output { destination };
        stretch.process (input, samples, output, samples);
        for (int n = 0; n < samples; ++n)
        {
            dryDelay[dryWrite] = source[n];
            const auto read = (dryWrite + dryDelay.size() - static_cast<size_t> (latencySamples())) % dryDelay.size();
            const auto dry = dryDelay[read];

            // Keep the unpitched high-frequency residual from the original
            // voice. Its control cue is delayed by exactly the renderer
            // latency, preventing a dry/wet time mismatch or broad blend.
            const auto x = static_cast<double> (source[n]);
            const auto period = std::isfinite (sourceF0Hz) && sourceF0Hz >= 80.0
                              ? std::clamp (static_cast<int> (std::lround (sampleRate / sourceF0Hz)), 16,
                                            static_cast<int> (dryDelay.size() - 1))
                              : 0;
            const auto delayed = period > 0
                               ? static_cast<double> (dryDelay[(dryWrite + dryDelay.size()
                                                          - static_cast<size_t> (period)) % dryDelay.size()])
                               : 0.0;
            // A true high-band estimate catches sustained /s/ and /sh/
            // energy which a one-sample derivative can miss at 48 kHz.
            // This remains a short causal one-pole calculation, not an FFT.
            sourceLowState += (source[n] - sourceLowState) * sourceCrossoverAlpha;
            const auto sourceHigh = x - static_cast<double> (sourceLowState);
            sourceEnergy += (x * x - sourceEnergy) * sourceEvidenceAlpha;
            highEnergy += (sourceHigh * sourceHigh - highEnergy) * sourceEvidenceAlpha;
            correlation += (x * delayed - correlation) * sourceEvidenceAlpha;
            delayedEnergy += (delayed * delayed - delayedEnergy) * sourceEvidenceAlpha;
            const auto highFrequencyEvidence = std::clamp (
                (highEnergy / std::max (sourceEnergy, 1.0e-12) - 0.10) / 0.25, 0.0, 1.0);
            const auto periodicCoherence = period > 0
                ? std::abs (correlation) / std::sqrt (std::max (sourceEnergy * delayedEnergy, 1.0e-18)) : 0.0;
            const auto nonPeriodic = std::clamp ((0.74 - periodicCoherence) / 0.30, 0.0, 1.0);
            sibilanceTargetDelay[dryWrite] = voiced
                ? static_cast<float> (std::sqrt (highFrequencyEvidence * nonPeriodic)) : 0.0f;
            const auto sibilanceTarget = sibilanceTargetDelay[read];
            // A direct wet/dry switch can create a one-sample click because
            // the spectral output and its latency-aligned dry counterpart
            // are not sample-identical around a pitch event.  Smooth only
            // that renderer lifecycle edge.  The 1.5/4 ms time constants
            // are shorter than a vocal note transition, so this cannot turn
            // a discrete target note into a glide.
            const auto targetWet = voiced ? 1.0f : 0.0f;
            const auto coefficient = targetWet > wetGate ? wetGateAttack : wetGateRelease;
            wetGate += (targetWet - wetGate) * coefficient;

            const auto residualSeconds = sibilanceTarget > sibilanceResidualMix
                                       ? sibilanceAttackSeconds : sibilanceReleaseSeconds;
            sibilanceResidualMix += (sibilanceTarget - sibilanceResidualMix)
                                * onePoleCoefficient (residualSeconds);
            maximumSibilanceResidualMix = std::max (maximumSibilanceResidualMix, sibilanceResidualMix);
            const auto crossoverAlpha = static_cast<float> (1.0 - std::exp (
                -2.0 * std::numbers::pi_v<double> * crossoverHz / sampleRate));
            wetLowState += (destination[n] - wetLowState) * crossoverAlpha;
            dryLowState += (dry - dryLowState) * crossoverAlpha;
            const auto hybridWet = destination[n]
                + ((dry - dryLowState) - (destination[n] - wetLowState)) * sibilanceResidualMix;
            destination[n] = dry + (hybridWet - dry) * wetGate;
            dryWrite = (dryWrite + 1) % dryDelay.size();
        }
    }
private:
    float onePoleCoefficient (double seconds) const noexcept
    {
        return static_cast<float> (1.0 - std::exp (-1.0 / std::max (1.0, seconds * sampleRate)));
    }

    double sampleRate = 48000.0;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::vector<float> dryDelay;
    std::vector<float> sibilanceTargetDelay;
    size_t dryWrite = 0;
    float wetGate = 0.0f;
    float wetGateAttack = 1.0f;
    float wetGateRelease = 1.0f;
    float sibilanceResidualMix = 0.0f;
    float maximumSibilanceResidualMix = 0.0f;
    float wetLowState = 0.0f, dryLowState = 0.0f, sourceLowState = 0.0f;
    double sourceEnergy = 1.0e-12, highEnergy = 1.0e-12;
    double correlation = 0.0, delayedEnergy = 1.0e-12;

    static constexpr double crossoverHz = 2800.0;
    static constexpr double sibilanceAttackSeconds = 0.0005;
    static constexpr double sibilanceReleaseSeconds = 0.004;
    static constexpr double sourceEvidenceSeconds = 128.0 / 48000.0;
    static constexpr double sourceAnalysisCrossoverHz = 3500.0;
    double sourceEvidenceAlpha = 1.0;
    double sourceCrossoverAlpha = 1.0;
};
