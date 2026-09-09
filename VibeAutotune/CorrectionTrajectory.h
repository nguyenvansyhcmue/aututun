#pragma once

#include "NotePolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// This policy is deliberately independent of formant preservation. Formant
// is a renderer property; Classic changes only target/Flex/transition policy.
enum class TuningPolicy : uint8_t { Standard = 0, Classic = 1 };

struct CorrectionTrajectoryData
{
    double inputPitchCents = std::numeric_limits<double>::quiet_NaN();
    double targetPitchCents = std::numeric_limits<double>::quiet_NaN();
    double correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
    double correctionCents = 0.0;
    double pitchRatio = 1.0;
    int targetNote = -1;
    bool hasValidCorrection = false;
    bool targetChanged = false;
    // Read-only diagnostic values. They mirror existing calculations and do
    // not feed back into target selection or correction.
    double flexCorrectionGain = 0.0;
    double desiredCorrectionCents = 0.0;
    double retuneCorrectionCents = 0.0;
    double humanizeWeight = 0.0;
    double vibratoContributionCents = 0.0;
    // Audit-only: candidate after the three-frame median and before target
    // commitment. It is not consumed by the correction path.
    int candidateTargetNote = -1;
    // The target presented to Flex/Retune. Unlike targetPitchCents it is
    // continuous across an ordinary committed note change.
    double transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
    // The active trajectory endpoints are exported so a rapid multi-note
    // passage can be audited for a re-anchor rather than guessed from audio.
    double transitionStartCents = std::numeric_limits<double>::quiet_NaN();
    double transitionEndCents = std::numeric_limits<double>::quiet_NaN();
    double transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
    double transitionProgress = 1.0;
    bool transitionActive = false;
    bool transitionUsesRapidAdjacentContour = false;
    bool rendererCrossingActive = false;
    int rendererCrossingTargetNote = -1;
    double rendererCrossingProgress = 1.0;
    // Read-only target-selector audit. These values explain why a candidate
    // was allowed to cross a neighbouring-note boundary.
    double targetDecisionMidpointCents = std::numeric_limits<double>::quiet_NaN();
    double targetDecisionExcessCents = 0.0;
    double targetHysteresisCents = 0.0;
    int pendingTargetNote = -1;
    int pendingTargetFrames = 0;
    bool targetDecisionBeyondHysteresis = false;
    bool rapidAdjacentRawCandidate = false;
    bool rapidLegatoAdjacentStep = false;
    bool rapidLegatoHardLock = false;
    bool rapidLegatoTargetChanged = false;
    bool rapidLegatoContinuesDirection = false;
    bool rapidLegatoChainStarts = false;
    bool rapidLegatoChainActive = false;
    int rapidLegatoCommittedSteps = 0;
    int rapidLegatoSamplesSinceCommit = std::numeric_limits<int>::max();
    double rapidLegatoDistanceCents = 0.0;
    double rapidLegatoInputMotionCentsPerSecond = 0.0;
    bool extremeTargetChange = false;
};

// YIN-independent pitch-correction trajectory. It accepts only a stable F0
// snapshot and musical constraints; it has no allocations or JUCE/UI state.
class CorrectionTrajectory
{
public:
    // Offline candidate only. The shipped hard-tune path remains the
    // historical linear legato contour unless explicitly enabled by the
    // diagnostic renderer switch.
    void setDiagnosticRaisedCosineHardTuneLegato (bool shouldUse) noexcept
    {
        diagnosticRaisedCosineHardTuneLegato = shouldUse;
    }
    void setDiagnosticRapidLegatoPreview (bool shouldUse) noexcept
    {
        diagnosticRapidLegatoPreview = shouldUse;
    }
    void setDiagnosticStrongHardTunePull (bool shouldUse) noexcept
    {
        diagnosticStrongHardTunePull = shouldUse;
    }
    // Retune is an Artist-calibrated one-pole time constant, not the literal
    // number printed on the control. Offline measurements using the C5->B4
    // stimulus and Auto-Tune Artist (Alto/Tenor, C minor, Flex/Humanize/
    // Vibrato 0) reached 90% recovery in 53.3/96.0/176.0 ms at displayed
    // 10/25/50 ms. Dividing those figures by ln(10) gives the matching
    // one-pole constants below. This keeps the knob's musical response
    // comparable without baking host latency into the correction path.
    static double getEffectiveRetuneTimeMs (double displayedRetuneMs) noexcept
    {
        const auto displayed = clamp (displayedRetuneMs, 0.0, 400.0);
        if (displayed <= 0.0)
            return 0.0;
        // Keep a short continuous floor below the first measured knot so
        // 0 remains the only literal snap setting.
        if (displayed <= 10.0)
            return minimumContinuousRetuneMs
                 + (artistRetune10MsTimeConstantMs - minimumContinuousRetuneMs) * displayed / 10.0;
        if (displayed <= 25.0)
            return artistRetune10MsTimeConstantMs
                 + (artistRetune25MsTimeConstantMs - artistRetune10MsTimeConstantMs)
                       * (displayed - 10.0) / 15.0;
        if (displayed <= 50.0)
            return artistRetune25MsTimeConstantMs
                 + (artistRetune50MsTimeConstantMs - artistRetune25MsTimeConstantMs)
                       * (displayed - 25.0) / 25.0;
        return artistRetune50MsTimeConstantMs
             + artistRetuneAbove50Slope * (displayed - 50.0);
    }

    static double getNoteTransitionDurationMs (TuningPolicy policy, double retuneSpeedMs) noexcept
    {
        const auto displayedRetune = clamp (retuneSpeedMs, 0.0, 400.0);
        if (displayedRetune <= 0.0)
            return 0.0;

        // This is only the de-zippered target handoff. It remains shorter
        // than Retune response; otherwise a 100 ms setting visibly drags
        // the entire note transition instead of naturally correcting it.
        if (policy == TuningPolicy::Standard)
            return clamp (20.0 + displayedRetune * 0.40, 20.0, 180.0);
        return clamp (18.0 + displayedRetune * 0.35, 18.0, 120.0);
    }

    // Cubic smoothstep gives a zero slope at both ends of a target segment.
    // Unlike a linear ramp, a new note cannot inject an instantaneous control
    // velocity into the Retune one-pole or phase-vocoder dezipper.
    static double smoothTransitionProgress (double progress) noexcept
    {
        const auto x = clamp01 (progress);
        return x * x * (3.0 - 2.0 * x);
    }

    static double raisedCosineTransitionProgress (double progress) noexcept
    {
        const auto x = clamp01 (progress);
        return 0.5 - 0.5 * std::cos (3.14159265358979323846 * x);
    }

    // A rapid neighbouring-note reversal needs a little more movement in
    // the centre than the ordinary cubic curve, while still arriving at the
    // exact target with zero endpoint velocity.  The quintic curve raises
    // central slew by 25% (1.875 vs 1.5) without changing duration or either
    // note centre.
    static double rapidAdjacentTransitionProgress (double progress) noexcept
    {
        const auto x = clamp01 (progress);
        return x * x * x * (10.0 + x * (-15.0 + 6.0 * x));
    }

    void reset() noexcept
    {
        currentTargetNote = -1;
        pendingTargetNote = -1;
        pendingTargetFrames = 0;
        previousTargetNote = -1;
        framesSinceTargetCommit = 0;
        targetCandidateHistory.fill (-1);
        targetCandidateHistoryIndex = 0;
        targetCandidateHistoryCount = 0;
        resetTrajectoryPreview();
        transientRetuneBlend = 0.0;
        resetPitchVariationAnalysis();
        unvoicedFrames = 0;
        previousInputPitchCents = std::numeric_limits<double>::quiet_NaN();
        inputMotionCentsPerSecond = 0.0;
        correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
        smoothedCorrectionCents = std::numeric_limits<double>::quiet_NaN();
        outputCorrectionCents = std::numeric_limits<double>::quiet_NaN();
        pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
        vibratoEnergyCents = 0.0;
        sustainedTargetSamples = 0;
        humanizeSustainWeight = 0.0;
        transitionStartCents = std::numeric_limits<double>::quiet_NaN();
        transitionEndCents = std::numeric_limits<double>::quiet_NaN();
        transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
        transitionProgress = 1.0;
        transitionActive = false;
        transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
        transitionUsesLinearSlew = false;
        transitionUsesRapidLegatoChain = false;
        transitionUsesRapidAdjacentContour = false;
        transitionVelocityCentsPerSecond = 0.0;
        rapidLegatoDirection = 0;
        rapidLegatoAdjacentChanges = 0;
        rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
        descentRailGuardSamples = 0;
        reversalSettlingToPreviousTarget = false;
        reversalHoldSamplesRemaining = 0;
        reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
        rendererCrossingActive = false;
        rendererCrossingTargetNote = -1;
        rendererCrossingDurationMs = rendererCrossingTransitionMs;
        rendererCrossingProgress = 0.0;
        rendererCrossingCorrectionCents = 0.0;
        resetPortamentoTracking();
        resetControlSchedulerCache();
    }

    void resetTargetSelection() noexcept
    {
        currentTargetNote = -1;
        pendingTargetNote = -1;
        pendingTargetFrames = 0;
        previousTargetNote = -1;
        framesSinceTargetCommit = 0;
        targetCandidateHistory.fill (-1);
        targetCandidateHistoryIndex = 0;
        targetCandidateHistoryCount = 0;
        resetTrajectoryPreview();
        sustainedTargetSamples = 0;
        transitionStartCents = std::numeric_limits<double>::quiet_NaN();
        transitionEndCents = std::numeric_limits<double>::quiet_NaN();
        transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
        transitionProgress = 1.0;
        transitionActive = false;
        transitionUsesLinearSlew = false;
        transitionUsesRapidLegatoChain = false;
        transitionUsesRapidAdjacentContour = false;
        transitionVelocityCentsPerSecond = 0.0;
        rapidLegatoDirection = 0;
        rapidLegatoAdjacentChanges = 0;
        rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
        descentRailGuardSamples = 0;
        reversalSettlingToPreviousTarget = false;
        reversalHoldSamplesRemaining = 0;
        reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
        rendererCrossingActive = false;
        rendererCrossingTargetNote = -1;
        rendererCrossingDurationMs = rendererCrossingTransitionMs;
        rendererCrossingProgress = 0.0;
        rendererCrossingCorrectionCents = 0.0;
        resetPortamentoTracking();
        resetControlSchedulerCache();
    }

    CorrectionTrajectoryData update (double stableF0Hz, double referencePitchHz,
                                     int transpose, const NotePolicySnapshot& notePolicy,
                                     double retuneSpeedMs, double flexTunePercent,
                                     double humanizePercent, double naturalVibratoPercent,
                                     double sampleRate, int hopSamples, bool voiced,
                                     bool audioTransient = false,
                                     TuningPolicy tuningPolicy = TuningPolicy::Standard,
                                     bool useControlScheduler = false) noexcept
    {
        CorrectionTrajectoryData output;
        if (! voiced || ! std::isfinite (stableF0Hz) || stableF0Hz <= 0.0
            || ! std::isfinite (referencePitchHz) || referencePitchHz <= 0.0
            || sampleRate <= 0.0 || hopSamples <= 0)
        {
            ++unvoicedFrames;
            pendingTargetNote = -1;
            pendingTargetFrames = 0;
            // A short hold prevents a target-state click at a voiced boundary.
            // It is intentionally not published as a valid correction.
            if (unvoicedFrames >= 3)
            {
                currentTargetNote = -1;
                targetCandidateHistory.fill (-1);
                targetCandidateHistoryIndex = 0;
                targetCandidateHistoryCount = 0;
        transientRetuneBlend = 0.0;
                resetPitchVariationAnalysis();
                correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
                smoothedCorrectionCents = std::numeric_limits<double>::quiet_NaN();
                outputCorrectionCents = std::numeric_limits<double>::quiet_NaN();
                pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
                vibratoEnergyCents = 0.0;
                sustainedTargetSamples = 0;
                humanizeSustainWeight = 0.0;
                transitionStartCents = std::numeric_limits<double>::quiet_NaN();
                transitionEndCents = std::numeric_limits<double>::quiet_NaN();
                transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
                transitionProgress = 1.0;
                transitionActive = false;
                transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
                transitionUsesLinearSlew = false;
                transitionUsesRapidLegatoChain = false;
                transitionUsesRapidAdjacentContour = false;
                transitionVelocityCentsPerSecond = 0.0;
                rapidLegatoDirection = 0;
                rapidLegatoAdjacentChanges = 0;
                rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
                descentRailGuardSamples = 0;
                reversalSettlingToPreviousTarget = false;
                reversalHoldSamplesRemaining = 0;
                reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
                inputMotionCentsPerSecond = 0.0;
                resetPortamentoTracking();
                resetControlSchedulerCache();
            }
            return output;
        }

        // A consonant/gap can be only one analysis frame long.  Retaining
        // the prior note through that gap made the next syllable spend its
        // first 100+ ms correcting toward the *previous* target (for example
        // A2 while the recovered F0 was already G3/B3).  Treat re-voicing as
        // a fresh target onset: the detector must establish the new note
        // before any old target can affect it.
        const auto revoicedAfterGap = unvoicedFrames > 0;
        unvoicedFrames = 0;
        if (revoicedAfterGap)
            resetTargetSelection();
        const auto inputCents = 1200.0 * std::log2 (stableF0Hz / referencePitchHz);
        // Retune controls correction response, but a wide melodic movement
        // must not be overtaken by a fixed target ramp.  Track the recent F0
        // motion rate so broad, slow portamento retains the singer's tempo.
        const auto signedInputDeltaCents = std::isfinite (previousInputPitchCents)
                                         ? inputCents - previousInputPitchCents : 0.0;
        const auto inputDeltaCents = std::abs (signedInputDeltaCents);
        const auto instantaneousInputMotion = inputDeltaCents * sampleRate
                                             / static_cast<double> (hopSamples);
        const auto motionAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                 / (inputMotionTimeSeconds * sampleRate));
        inputMotionCentsPerSecond += (instantaneousInputMotion - inputMotionCentsPerSecond) * motionAlpha;
        updatePortamentoTracking (inputCents, retuneSpeedMs);

        // A slow centre leaves only existing periodic pitch movement in the
        // residual. It is evaluated per analysis hop, not per host block.
        if (! std::isfinite (pitchCenterCents))
            pitchCenterCents = inputCents;

        const auto centreAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                  / (pitchCentreTimeSeconds * sampleRate));
        pitchCenterCents += (inputCents - pitchCenterCents) * centreAlpha;
        const auto rawVibratoResidual = clamp (inputCents - pitchCenterCents,
                                               -maximumInputVibratoDepthCents,
                                               maximumInputVibratoDepthCents);
        const auto energyAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                  / (vibratoEnergyTimeSeconds * sampleRate));
        vibratoEnergyCents += (std::abs (rawVibratoResidual) - vibratoEnergyCents) * energyAlpha;

        // Humanize analyses pitch variance over a window that scales with F0:
        // low voices use up to 200 ms; high voices use down to 50 ms. A
        // second fixed 250 ms sign-history estimates the 4–8 Hz vibrato rate,
        // so a short high-voice variance window cannot mistake pitch drift for
        // periodic vibrato. Both histories are fixed-size and allocation-free.
        pitchResidualHistory[static_cast<size_t> (pitchResidualHistoryIndex)] = rawVibratoResidual;
        pitchResidualHistoryIndex = (pitchResidualHistoryIndex + 1) % pitchResidualHistoryLength;
        pitchResidualHistoryCount = std::min (pitchResidualHistoryCount + 1, pitchResidualHistoryLength);
        const auto adaptiveWindowSeconds = clamp (0.10 * 220.0 / stableF0Hz, 0.050, 0.200);
        const auto adaptiveWindowFrames = std::clamp (static_cast<int> (std::lround (adaptiveWindowSeconds * sampleRate / hopSamples)),
                                                       5, pitchResidualHistoryLength);
        const auto varianceFrames = std::min (pitchResidualHistoryCount, adaptiveWindowFrames);
        auto residualMean = 0.0;
        for (int index = 0; index < varianceFrames; ++index)
        {
            const auto historyIndex = (pitchResidualHistoryIndex - 1 - index + pitchResidualHistoryLength)
                                    % pitchResidualHistoryLength;
            residualMean += pitchResidualHistory[static_cast<size_t> (historyIndex)];
        }
        residualMean /= std::max (1, varianceFrames);
        auto residualVariance = 0.0;
        for (int index = 0; index < varianceFrames; ++index)
        {
            const auto historyIndex = (pitchResidualHistoryIndex - 1 - index + pitchResidualHistoryLength)
                                    % pitchResidualHistoryLength;
            const auto delta = pitchResidualHistory[static_cast<size_t> (historyIndex)] - residualMean;
            residualVariance += delta * delta;
        }
        const auto residualDeviation = std::sqrt (residualVariance / std::max (1, varianceFrames));

        const auto rateFrames = std::min (pitchResidualHistoryCount,
                                          static_cast<int> (std::lround (0.250 * sampleRate / hopSamples)));
        auto zeroCrossings = 0;
        auto previousSign = 0;
        for (int reverseIndex = rateFrames - 1; reverseIndex >= 0; --reverseIndex)
        {
            const auto historyIndex = (pitchResidualHistoryIndex - 1 - reverseIndex + pitchResidualHistoryLength)
                                    % pitchResidualHistoryLength;
            const auto value = pitchResidualHistory[static_cast<size_t> (historyIndex)] - residualMean;
            const auto sign = value > 0.0 ? 1 : (value < 0.0 ? -1 : 0);
            if (sign != 0 && previousSign != 0 && sign != previousSign)
                ++zeroCrossings;
            if (sign != 0)
                previousSign = sign;
        }
        const auto rateSeconds = static_cast<double> (rateFrames * hopSamples) / sampleRate;
        const auto estimatedVibratoRate = rateSeconds > 0.0 ? static_cast<double> (zeroCrossings) / (2.0 * rateSeconds) : 0.0;
        const auto detectedVibrato = residualDeviation >= minimumHumanizeVibratoDeviationCents
                                  && estimatedVibratoRate >= minimumHumanizeVibratoRateHz
                                  && estimatedVibratoRate <= maximumHumanizeVibratoRateHz;
        const auto vibratoRecognitionAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                              / (vibratoRecognitionTimeSeconds * sampleRate));
        humanizeVibratoRecognition += ((detectedVibrato ? 1.0 : 0.0) - humanizeVibratoRecognition)
                                    * vibratoRecognitionAlpha;

        // Artist exposes Natural Vibrato in a compact -12 to +12 range.
        // The recorded Artist bounces give 5.67 ct RMS at -12 and 7.76 ct at
        // +12 around a 6.7 ct neutral response, so one displayed unit is a
        // 1.3% change in retained source modulation. The explicit 0/0/0
        // hard-tune preset is a distinct case: it
        // must land on the target centre, not preserve the input residual.
        // Otherwise Retune 0 can still sit tens of cents off-note even after
        // the target is committed. This does not synthesize modulation; it
        // merely prevents source vibrato/drift from bypassing hard-lock.
        const auto vibratoPresence = clamp01 ((vibratoEnergyCents - minimumVibratoEnergyCents)
                                              / vibratoEnergyFadeCents);
        const auto vibratoGain = clamp (1.0 + naturalVibratoPercent * artistNaturalVibratoGainPerUnit, 0.0, 2.0);
        // APVTS parameters are floats and arrive through a normalised
        // conversion. A UI value displayed as exactly 0 can retain a tiny
        // non-zero binary residue, so equality-to-zero silently disabled
        // hard-lock. The UI resolution is 0.1; this half-step tolerance
        // recognises only the displayed zero position, never 0.1.
        constexpr auto hardLockZeroTolerance = 0.05;
        const auto hardLockPreset = retuneSpeedMs <= hardLockZeroTolerance
                                 && flexTunePercent <= hardLockZeroTolerance
                                 && humanizePercent <= hardLockZeroTolerance
                                 && std::abs (naturalVibratoPercent) <= hardLockZeroTolerance;
        const auto residualGain = hardLockPreset ? 0.0
                                                  : 1.0 + vibratoPresence * (vibratoGain - 1.0);
        const auto scaledVibratoResidual = clamp (rawVibratoResidual * residualGain,
                                                   -maximumOutputVibratoDepthCents,
                                                   maximumOutputVibratoDepthCents);

        const auto desiredMidi = 69.0 + inputCents / 100.0 + static_cast<double> (transpose);
        const auto sourceMidi = std::clamp (static_cast<int> (std::lround (desiredMidi)), 0, 127);
        const auto sourceRule = notePolicy.ruleForMidi (sourceMidi);
        const auto rawCandidate = selectPolicyTarget (desiredMidi, sourceRule, notePolicy, currentTargetNote);
        if (rawCandidate < 0)
            return output;

        updateTrajectoryPreview (rawCandidate, desiredMidi, signedInputDeltaCents, notePolicy, hardLockPreset);


        // A three-frame median rejects single-frame target outliers before
        // they can reach the Retune one-pole. It adds no allocation and is
        // deliberately kept in the trajectory, not in YIN.
        const auto filteredCandidate = filterTargetCandidate (rawCandidate);
        // The normal three-frame median is deliberately conservative: it
        // rejects isolated tracker mistakes before they can retune a vowel.
        // At the explicit hard-lock preset, however, that same 21.3 ms
        // wait consumes an entire real adjacent-note luyến before its
        // correction contour is even allowed to start.  Let a *single*
        // neighbouring raw target through only while the detector is moving
        // faster than vibrato and the input has crossed the target midpoint.
        // Target notes remain discrete. A quick return still goes through
        // selectTarget's two-frame reversal confirmation below, so this does
        // not turn ordinary one-frame tracker jitter into chatter.
        const auto rapidAdjacentRawCandidate = enableRapidAdjacentRawCandidate
                                             && hardLockPreset
                                             && currentTargetNote >= 0
                                             && std::abs (rawCandidate - currentTargetNote) == 1
                                             // The 40 ms moving average is useful for
                                             // broad portamento decisions, but it hides
                                             // the leading edge of a 20–33 ms luyến.
                                             // This is a one-frame rapid-event gate, so
                                             // use the measured frame velocity here.
                                             && instantaneousInputMotion >= rapidLegatoMinimumMotionCentsPerSecond
                                             && std::abs (desiredMidi
                                                          - 0.5 * static_cast<double> (rawCandidate + currentTargetNote))
                                                    > midpointHysteresisSemitones;
        // The explicit 0/0/50/0/0 hard-tune preset uses the detector's
        // immediate scale candidate. Other presets retain the median path.
        // A confirmed directional preview reaches the next legal note while
        // the F0 is still just before its midpoint.  It is produced only
        // after two coherent moving frames, so let it remove one analysis
        // hop of hard-tune commit latency without promoting isolated YIN
        // jitter into a target change.
        // Direct hard-tune already receives the immediate legal candidate.
        // The old early-preview override could invent a neighbouring note
        // before the raw candidate confirmed it, then force a return one
        // frame later (for example C# -> D -> C# on a stable vowel).
        // Keep the preview telemetry, but do not let it become an audible
        // target until it has a separately validated renderer use.
        constexpr bool enableHardTuneTrajectoryPreview = false;
        const auto canUseHardTunePreview = enableHardTuneTrajectoryPreview && trajectoryPreviewNote >= 0;
        const auto hardTuneCandidate = canUseHardTunePreview ? trajectoryPreviewNote
                                                              : rawCandidate;
        const auto candidate = hardLockPreset ? hardTuneCandidate
                                              : (rapidAdjacentRawCandidate ? rawCandidate : filteredCandidate);
        output.candidateTargetNote = candidate;

        const auto inputJump = std::isfinite (previousInputPitchCents)
                             ? std::abs (inputCents - previousInputPitchCents) : 0.0;
        const auto currentTargetIsValid = currentTargetNote >= 0
                                       && isLegalNoteRule (notePolicy.ruleForMidi (currentTargetNote));
        const auto targetBeforeSelection = currentTargetNote;
        auto targetDecisionMidpointCents = std::numeric_limits<double>::quiet_NaN();
        auto targetDecisionExcessCents = 0.0;
        auto targetDecisionBeyondHysteresis = false;
        if (candidate >= 0 && targetBeforeSelection >= 0 && candidate != targetBeforeSelection)
        {
            const auto direction = candidate > targetBeforeSelection ? 1.0 : -1.0;
            const auto midpointMidi = 0.5 * static_cast<double> (candidate + targetBeforeSelection);
            targetDecisionMidpointCents = (midpointMidi - 69.0) * 100.0;
            targetDecisionExcessCents = (desiredMidi - midpointMidi) * direction * 100.0;
            targetDecisionBeyondHysteresis = targetDecisionExcessCents > midpointHysteresisSemitones * 100.0;
        }
        const auto candidateReturnsToPreviousTarget = candidate >= 0
                                                   && candidate == previousTargetNote;
        const auto targetOnset = inputJump > onsetJumpCents;
        // A fast F0 movement is evidence for target selection, not by
        // itself an audio transient.  At Retune 0, treating every >95 ct
        // luyến as a transient silently replaced the requested hard-tune
        // response with the 55 ms safety response, causing the old-target
        // hold that users hear before a note switch. Real audio transients
        // still enable this guard below.
        selectTarget (candidate, desiredMidi, targetOnset || ! currentTargetIsValid, tuningPolicy,
                      revoicedAfterGap, hardLockPreset);
        previousInputPitchCents = inputCents;

        if (currentTargetNote < 0)
            return output;

        if (currentTargetNote != targetBeforeSelection)
        {
            output.targetChanged = targetBeforeSelection >= 0;
            sustainedTargetSamples = hopSamples;
            if (hardLockPreset && targetBeforeSelection >= 0
                && currentTargetNote == targetBeforeSelection - 1)
                descentRailGuardSamples = rapidDescentRailGuardHoldSamples;
            // Keep the in-flight correction and the slow pitch centre. A
            // pitch track may legitimately alternate scale targets during a
            // luyến láy/vibrato. Resetting to unity on every target update
            // made the control repeatedly restart, while Retune must instead
            // interpolate continuously between those targets.
        }
        else
        {
            sustainedTargetSamples += hopSamples;
            descentRailGuardSamples = std::max (0, descentRailGuardSamples - hopSamples);
        }

        // The bridge path may protect an implausibly large target jump, but
        // it must not reset the Retune state. This makes target changes and
        // their timing observable without injecting a new correction jump.
        const auto largeTargetJump = targetBeforeSelection >= 0
                                  && std::abs (currentTargetNote - targetBeforeSelection) >= largeTargetJumpSemitones;

        const auto targetCents = (static_cast<double> (currentTargetNote) - 69.0) * 100.0;
        // At Retune 0 the audible trajectory must follow only a committed
        // MIDI target. Previewing a pending neighbour makes the correction
        // start toward an undecided note and can leave a false half-semitone
        // shelf during rapid A/B alternation. Pending state remains useful
        // for selector telemetry, but never becomes an audio destination.
        const auto trajectoryTargetNote = currentTargetNote;
        const auto trajectoryTargetCents = (static_cast<double> (trajectoryTargetNote) - 69.0) * 100.0;
        const auto trajectoryTargetChanged = std::isfinite (transitionEndCents)
                                          && std::abs (trajectoryTargetCents - transitionEndCents) > 0.01;
        auto requestedTransitionDurationMs = getNoteTransitionDurationMs (tuningPolicy, retuneSpeedMs);
        const auto hardTuneLegatoDistance = std::isfinite (transitionEndCents)
                                         ? std::abs (trajectoryTargetCents - transitionEndCents) : 0.0;
        const auto hardTuneLegatoTransition = retuneSpeedMs <= 0.0 && trajectoryTargetChanged
                                            && hardTuneLegatoDistance <= hardTuneLegatoMaximumCents;
        // This path is entered only after the normal median/hysteresis
        // selector has committed a neighbouring note. It deliberately does
        // not promote raw F0 candidates: the musical target remains an
        // exact MIDI note, while only the correction contour is continuous.
        const auto confirmedRapidAdjacentTransition = hardTuneLegatoTransition
                                                    && std::abs (hardTuneLegatoDistance - 100.0) < 0.01;
        constexpr bool enableRapidReversalPlateau = true;
        const auto rapidAdjacentReversal = enableRapidReversalPlateau
                                        && hardLockPreset
                                        && currentTargetNote != targetBeforeSelection
                                        && candidateReturnsToPreviousTarget
                                        && std::abs (currentTargetNote - targetBeforeSelection) == 1
                                        && transitionActive;
        // A rapid A -> B -> A event deliberately has two destinations:
        // first land B, then return to A after its tiny plateau.  The normal
        // "trajectory target changed" path below must not overwrite B with A
        // in the same analysis frame (or while the plateau is pending), or
        // B is never audible at all.
        const auto rapidReversalSequenceOwnsTarget = rapidAdjacentReversal
                                                  || reversalSettlingToPreviousTarget
                                                  || reversalHoldSamplesRemaining > 0;
        const auto legatoStepDirection = trajectoryTargetChanged
                                      ? (trajectoryTargetCents > transitionEndCents ? 1 : -1) : 0;
        const auto adjacentLegatoStep = enableRapidLegatoChain && hardLockPreset && trajectoryTargetChanged
                                     && std::abs (hardTuneLegatoDistance - 100.0) < 0.01
                                     && inputMotionCentsPerSecond >= rapidLegatoMinimumMotionCentsPerSecond;
        const auto continuesDirectedLegato = adjacentLegatoStep
                                          && legatoStepDirection == rapidLegatoDirection
                                          && rapidLegatoSamplesSinceChange <= rapidLegatoChainGapSamples;
        if (adjacentLegatoStep)
        {
            rapidLegatoAdjacentChanges = continuesDirectedLegato ? rapidLegatoAdjacentChanges + 1 : 1;
            rapidLegatoDirection = legatoStepDirection;
            rapidLegatoSamplesSinceChange = 0;
        }
        else
        {
            rapidLegatoSamplesSinceChange = std::min (std::numeric_limits<int>::max() - hopSamples,
                                                       rapidLegatoSamplesSinceChange + hopSamples);
            if (trajectoryTargetChanged)
            {
                rapidLegatoDirection = 0;
                rapidLegatoAdjacentChanges = 0;
            }
        }
        const auto startsOrContinuesRapidLegatoChain = adjacentLegatoStep
                                                     && rapidLegatoAdjacentChanges >= 2;
        if (hardTuneLegatoTransition)
        {
            // A short legato movement is one continuous cents ramp. Thus an
            // A -> B move passes through A# at the midpoint rather than
            // snapping over it. Extra semitones receive a small amount of
            // time, preserving a firm 0-ms landing without a rectangular
            // multi-semitone control step.
            // Land rapidly on an arriving higher adjacent target, but give
            // the audible return to the lower neighbour a short continuous
            // descent. The target remains discrete in both directions.
            const auto rapidLegatoIsDescending = trajectoryTargetChanged
                                              && trajectoryTargetCents < transitionEndCents;
            requestedTransitionDurationMs = diagnosticStrongHardTunePull ? 3.0
                                          : confirmedRapidAdjacentTransition
                                          ? (rapidLegatoIsDescending
                                             ? hardTuneRapidDescendingTransitionMs
                                             : hardTuneRapidAdjacentTransitionMs)
                                          : hardTuneAdjacentTransitionMs;
        }
        if (currentTargetNote != targetBeforeSelection && targetBeforeSelection >= 0 && retuneSpeedMs > 0.0)
        {
            const auto targetDistanceCents = 100.0 * std::abs (currentTargetNote - targetBeforeSelection);
            // Adjacent-note luyến stays responsive.  Only broad movements
            // inherit a bounded portion of the observed vocal travel time.
            if (targetDistanceCents >= sourceTimedJumpThresholdCents)
            {
                const auto sourceTravelMs = 1000.0 * targetDistanceCents
                                          / std::fmax (minimumSourceMotionCentsPerSecond,
                                                       inputMotionCentsPerSecond);
                const auto sourceTimedMs = clamp (sourceTravelMs * sourceTempoFollowRatio,
                                                   requestedTransitionDurationMs,
                                                   clamp (160.0 + retuneSpeedMs * 1.5, 160.0, 520.0));
                requestedTransitionDurationMs = std::fmax (requestedTransitionDurationMs, sourceTimedMs);
            }
        }
        if (! std::isfinite (transitionDurationMs))
            transitionDurationMs = requestedTransitionDurationMs;
        else if (rapidAdjacentReversal)
        {
            // A fast B -> A return should not re-anchor immediately from an
            // in-flight glide. Land B, let its centre be audible for two
            // control ticks, then begin the normal continuous A transition.
            // This branch is limited to an adjacent confirmed reversal at
            // the 0 ms hard-lock preset; it never affects one-way luyến.
            reversalPendingTargetCents = trajectoryTargetCents;
            reversalHoldSamplesRemaining = 0;
            reversalSettlingToPreviousTarget = true;
            transitionStartCents = transitionTargetCents;
            transitionEndCents = (static_cast<double> (targetBeforeSelection) - 69.0) * 100.0;
            transitionDurationMs = rapidReversalSettleMs;
            transitionProgress = 0.0;
            transitionActive = true;
            // Retune 0 is deliberately a hard robotic contour: the musical
            // target remains discrete and the cents path is a constant-rate
            // line, not an eased S-curve.  Audio phase remains continuous in
            // the renderer; this changes only the correction geometry.
            transitionUsesLinearSlew = true;
            transitionUsesRapidLegatoChain = false;
            transitionUsesRapidAdjacentContour = true;
            transitionVelocityCentsPerSecond = 0.0;
        }
        else if (startsOrContinuesRapidLegatoChain)
        {
            transitionEndCents = trajectoryTargetCents;
            transitionActive = true;
            if (! transitionUsesRapidLegatoChain)
                transitionVelocityCentsPerSecond = static_cast<double> (legatoStepDirection)
                                                  * hardTuneLegatoChainRateCentsPerSecond;
            transitionUsesRapidLegatoChain = true;
            transitionUsesLinearSlew = true;
            transitionUsesRapidAdjacentContour = false;
        }
        else if (trajectoryTargetChanged && ! rapidReversalSequenceOwnsTarget)
            // Apply the musical duration at the start of this event. If it
            // were filtered from the prior duration, the first part would
            // still rush before the source-tempo safeguard took effect.
            transitionDurationMs = requestedTransitionDurationMs;
        else if (! transitionActive || ! transitionUsesRapidAdjacentContour)
        {
            const auto alpha = 1.0 - std::exp (-static_cast<double> (hopSamples) / (0.020 * sampleRate));
            transitionDurationMs += (requestedTransitionDurationMs - transitionDurationMs) * alpha;
        }
        if (! std::isfinite (transitionTargetCents))
        {
            transitionStartCents = trajectoryTargetCents;
            transitionEndCents = trajectoryTargetCents;
            transitionTargetCents = trajectoryTargetCents;
            transitionProgress = 1.0;
            transitionActive = false;
        }
        else if (trajectoryTargetChanged && ! rapidReversalSequenceOwnsTarget)
        {
            // Re-anchor to the current in-flight target, never to a stale
            // discrete note or to unity. This keeps successive commits
            // continuous even if they arrive before a transition completes.
            transitionStartCents = transitionTargetCents;
            transitionEndCents = trajectoryTargetCents;
            transitionProgress = 0.0;
            transitionActive = true;
            // A low-slope linear ramp reaches the landing note sooner than a
            // long S-curve while remaining smoother at its steepest point.
            // It is restricted to short hard-tune legato moves; all other
            // transitions retain the zero-slope S-curve.
            // A committed neighbouring target at Retune 0 must not spend
            // its first/last control ticks flattening an ease curve.  A
            // constant-rate cents ramp gives the intended sharp, segmented
            // hard-tune step while the renderer retains phase continuity.
            transitionUsesLinearSlew = hardTuneLegatoTransition && ! diagnosticRaisedCosineHardTuneLegato;
            transitionUsesRapidLegatoChain = false;
            transitionUsesRapidAdjacentContour = confirmedRapidAdjacentTransition;
            transitionVelocityCentsPerSecond = 0.0;

        }

        if (transitionActive && ! useControlScheduler)
        {
            // Mode policy is shorter than Retune but separate from it; Retune
            // remains the user-facing musical time constant.
            const auto transitionSeconds = transitionDurationMs * 0.001;
            const auto progressStep = transitionSeconds > 0.0
                                    ? static_cast<double> (hopSamples) / (transitionSeconds * sampleRate)
                                    : 1.0;
            transitionProgress = clamp01 (transitionProgress + progressStep);
            auto shapedProgress = transitionUsesLinearSlew ? transitionProgress
                                : (diagnosticRaisedCosineHardTuneLegato && hardTuneLegatoTransition
                                   ? raisedCosineTransitionProgress (transitionProgress)
                                : (transitionUsesRapidAdjacentContour
                                   ? rapidAdjacentTransitionProgress (transitionProgress)
                                   : smoothTransitionProgress (transitionProgress)));
            transitionTargetCents = transitionStartCents
                                  + (transitionEndCents - transitionStartCents) * shapedProgress;
            if (transitionProgress >= 1.0)
            {
                transitionTargetCents = transitionEndCents;
                transitionActive = false;
                transitionUsesRapidAdjacentContour = false;
            }
        }
        // BypassLegal is a musical exception for the normal correction
        // modes: an explicitly bypassed legal note is allowed to pass its
        // source contour untouched.  It must not be allowed to escape the
        // 0/0/50/0/0 hard-lock policy, though.  In that preset the plug-in
        // remains fully wet even when the detector classifies the source as
        // an already-legal note; otherwise source vibrato/drift reaches the
        // renderer with a zero ratio correction and the audible plateau can
        // never become flat.
        if (! hardLockPreset
            && sourceRule == NoteRule::BypassLegal
            && currentTargetNote == sourceMidi)
        {
            // BypassLegal remains a valid landing note for nearby illegal
            // pitches, but an input classified as this note passes unchanged.
            smoothedCorrectionCents = 0.0;
            outputCorrectionCents = 0.0;
            correctedPitchCents = inputCents;
            output.inputPitchCents = inputCents;
            output.targetPitchCents = targetCents;
            output.correctedPitchCents = inputCents;
            output.correctionCents = 0.0;
            output.pitchRatio = 1.0;
            output.targetNote = currentTargetNote;
            output.hasValidCorrection = true;
            return output;
        }
        if (! std::isfinite (smoothedCorrectionCents))
            smoothedCorrectionCents = 0.0;

        // Flex-Tune attenuates the pitch error close to a selected target,
        // without changing the target selector or mixing waveforms. The
        // Gaussian-shaped hold term is continuous, bounded and monotonic in
        // |error|: large deviations still receive near-full correction.
        // Selection remains on raw detector pitch so this residual cannot
        // perturb target hysteresis. Correction follows the slow centre and
        // the scaled source residual is restored only at the output pitch.
        // The eventual scale target remains committed above, but during a
        // continuous glide the Retune controller follows the F0 contour
        // instead of re-starting a discrete target ramp at every semitone.
        const auto controlTargetCents = portamentoActive && retuneSpeedMs > 0.0
                                      ? inputCents : transitionTargetCents;
        // The slow pitch centre is useful for Flex/Humanize, where it
        // separates a sung residual from deliberate correction. It must not
        // become the reference of the explicit 0/0/50/0/0 hard-lock preset:
        // doing so compares a new target to ~120 ms old F0 and asks for an
        // excessive correction (glide then overshoot) even though the target
        // and current detected F0 are both already valid. Hard-lock needs
        // the literal musical delta: target - current input pitch.
        const auto correctionReferenceCents = hardLockPreset ? inputCents : pitchCenterCents;
        const auto pitchError = controlTargetCents - correctionReferenceCents;
        const auto flex = tuningPolicy == TuningPolicy::Classic ? 0.0
                        : std::fmax (0.0, std::fmin (1.0, flexTunePercent * 0.01));
        // Flex is a near-target relaxation, never a correction bypass. Even
        // at 100% it retains a meaningful pull toward the selected note;
        // otherwise a held note is left entirely untreated and Flex becomes
        // indistinguishable from turning the plug-in off.
        // Calibrated against matched Auto-Tune Pro vocal references at
        // Flex 10/20/50, Retune 0. The former linear 0.65*Flex curve pulled
        // too hard near a target (especially around Flex 50), making the
        // Live path sound more forced than the reference. This is only the
        // near-target relaxation; large pitch errors still receive full pull.
        const auto correctionGain = flexCorrectionGain (flex, pitchError);
        const auto flexTargetCents = correctionReferenceCents + pitchError * correctionGain;

        // A one-pole response in cents. Its alpha is evaluated per analysis
        // hop, so the trajectory is invariant to host block size.
        // Humanize does not touch attacks: it starts only after a target has
        // remained stable for 240 ms and ramps over the following 180 ms.
        // At 100% it limits the sustained-note retune time to 4x its base.
        const auto heldSeconds = static_cast<double> (sustainedTargetSamples) / sampleRate;
        const auto sustainRamp = clamp01 ((heldSeconds - humanizeStartSeconds) / humanizeRampSeconds);
        // Humanize belongs to the hard-tune use case: it opens only on a
        // sustained note while the base Retune setting is still fast. Slower
        // Retune values already provide their own natural glide.
        const auto effectiveBaseRetuneMs = getEffectiveRetuneTimeMs (retuneSpeedMs);
        // Humanize's eligibility remains expressed in the user's knob
        // domain: low displayed settings are the deliberate hard-tune use
        // case, even though the calibrated DSP time is longer internally.
        const auto fastRetuneAmount = clamp01 ((45.0 - retuneSpeedMs) / 25.0);
        // Humanize relaxes a sustained, *periodic* vocal residual only.  The
        // correction path itself remains centred on pitchCenterCents, so a
        // stable pitch drift is still corrected rather than being mistaken
        // for vibrato.  There is no cents-size cut-off here: periodicity and
        // variance determine whether a residual is vibrato.
        const auto requestedHumanizeWeight = artistHumanizeResponse (humanizePercent)
                                           * fastRetuneAmount
                                           * sustainRamp
                                           * clamp01 (humanizeVibratoRecognition);
        const auto humanizeWeightAlpha = 1.0 - std::exp (-static_cast<double> (hopSamples)
                                                          / (humanizeWeightTimeSeconds * sampleRate));
        humanizeSustainWeight += (requestedHumanizeWeight - humanizeSustainWeight) * humanizeWeightAlpha;
        const auto humanize = clamp01 (humanizeSustainWeight);
        if (audioTransient)
            transientRetuneBlend = 1.0;

        // Humanize is strictly sustain-only. Its ceiling has a non-zero
        // value so a 0 ms hard-tune can still relax a *long* held note while
        // its attack and all target changes remain an immediate 0 ms snap.
        const auto humanizeCeilingMs = std::fmax (humanizeMinimumSustainMs, effectiveBaseRetuneMs * 2.0);
        const auto sustainRetuneMs = effectiveBaseRetuneMs
                                   + (humanizeCeilingMs - effectiveBaseRetuneMs) * humanize;

        // Protect a detected consonant/onset for one analysis frame only.
        // The blend decays exponentially afterwards, so returning to the
        // user's Retune setting cannot create a step in the trajectory.
        const auto onsetRetuneMs = effectiveBaseRetuneMs
                                 + (std::fmax (transientRetuneMs, effectiveBaseRetuneMs) - effectiveBaseRetuneMs)
                                       * transientRetuneBlend;
        const auto effectiveRetuneMs = std::fmax (sustainRetuneMs, onsetRetuneMs);
        const auto timeSeconds = effectiveRetuneMs * 0.001;
        const auto alpha = timeSeconds <= 0.0 ? 1.0
                         : 1.0 - std::exp (-static_cast<double> (hopSamples) / (timeSeconds * sampleRate));
        // The renderer is intentionally used only for musical, near-note
        // retuning. Limiting this control signal prevents a transient YIN or
        // target error from becoming an audible octave dive or a clipped
        // renderer frame. This is not a Dry/Wet control and does not alter
        // normal chromatic corrections (which are within +/-50 cents).
        // The hard-lock path has deliberately chosen the instantaneous
        // detected pitch as its correction reference above.  It must use
        // that *same* reference here.  Subtracting the 120 ms pitch centre
        // while later applying the result to inputCents leaves a residual
        // equal to (input - centre): the note bends in the right direction
        // but continues to sound almost raw instead of landing on its exact
        // MIDI centre.  Musical/Flex presets retain the slow-centre law.
        const auto rawDesiredCorrectionCents = flexTargetCents
                                              - (hardLockPreset ? correctionReferenceCents
                                                                : pitchCenterCents);
        // The descent rail is an artifact guard for the relaxed musical
        // modes.  At the explicit hard-lock preset it would deliberately
        // saturate a valid negative target correction and leave the note
        // above its centre.  The per-control-tick step guard below remains
        // active for hard-lock, so removing this amplitude rail does not
        // create a rectangular ratio jump.
        const auto descentRailGuardActive = descentRailGuardSamples > 0 && ! hardLockPreset;
        const auto desiredCorrectionCents = descentRailGuardActive
                                          ? softRailLimit (rawDesiredCorrectionCents)
                                          : clamp (rawDesiredCorrectionCents,
                                                   -maximumLiveCorrectionCents,
                                                   maximumLiveCorrectionCents);
        // In production the fixed 128-sample control scheduler owns this
        // integration.  Keeping it out of the 512-sample YIN callback is
        // what prevents note-transition durations from being quantised to a
        // detector hop.  Standalone trajectory tests retain the legacy
        // direct mode through the default argument.
        if (! useControlScheduler)
        {
            smoothedCorrectionCents += (desiredCorrectionCents - smoothedCorrectionCents) * alpha;
            smoothedCorrectionCents = clamp (smoothedCorrectionCents,
                                             -maximumLiveCorrectionCents,
                                             maximumLiveCorrectionCents);
        }
        correctedPitchCents = correctionReferenceCents + smoothedCorrectionCents + scaledVibratoResidual;

        // This is a glitch guard, not a second user-facing Retune control.
        // It only becomes active for a trajectory discontinuity larger than
        // 60 cents per analysis hop (typically a tracker/target error). The
        // normal 25 ms trajectory for near-note vocal correction remains
        // below this limit.
        auto safeCorrectionCents = clamp (correctedPitchCents - inputCents,
                                          -maximumLiveCorrectionCents,
                                          maximumLiveCorrectionCents);
        const auto maximumStepCents = hardLockPreset ? hardLockMaximumCorrectionStepCents
                                                      : maximumCorrectionStepCents;
        if (std::isfinite (outputCorrectionCents))
            safeCorrectionCents = clamp (safeCorrectionCents,
                                         outputCorrectionCents - maximumStepCents,
                                         outputCorrectionCents + maximumStepCents);
        else
            safeCorrectionCents = clamp (safeCorrectionCents,
                                         -maximumStepCents,
                                         maximumStepCents);

        // This predictor feeds only a temporary renderer-correction contour.
        // It deliberately never changes the selector's candidate or MIDI
        // target.  The normal target path remains the sole authority for a
        // committed note.
        if (rendererCrossingActive && currentTargetNote == rendererCrossingTargetNote)
        {
            rendererCrossingActive = false;
            rendererCrossingTargetNote = -1;
            smoothedCorrectionCents = rendererCrossingCorrectionCents;
            safeCorrectionCents = rendererCrossingCorrectionCents;
        }
        const auto canPredictRendererCrossing = hardLockPreset
                                               && ! rendererCrossingActive
                                               && currentTargetNote >= 0
                                               && trajectoryPreviewUpdates >= 2
                                               && trajectoryPreviewTravelCents >= 20.0;
        if (canPredictRendererCrossing)
        {
            const auto predictedTarget = directedLegalNote (desiredMidi, notePolicy,
                                                              trajectoryPreviewDirection > 0);
            const auto voicedDescendingReturn = predictedTarget >= 0
                                              && predictedTarget < currentTargetNote
                                              && predictedTarget == previousTargetNote
                                              && stableF0Hz >= rendererCrossingReturnMinimumF0Hz;
            const auto crossingMotionThreshold = voicedDescendingReturn
                                               ? rendererCrossingReturnMinimumMotionCentsPerSecond
                                               : rendererCrossingMinimumMotionCentsPerSecond;
            if (predictedTarget >= 0 && predictedTarget != currentTargetNote
                && std::abs (predictedTarget - currentTargetNote) <= 2)
            {
                const auto direction = predictedTarget > currentTargetNote ? 1.0 : -1.0;
                const auto midpoint = 0.5 * static_cast<double> (predictedTarget + currentTargetNote);
                if (instantaneousInputMotion >= crossingMotionThreshold
                    && (desiredMidi - midpoint) * direction >= -rendererCrossingPreviewLeadSemitones)
                {
                    rendererCrossingActive = true;
                    rendererCrossingTargetNote = predictedTarget;
                    rendererCrossingStartCorrectionCents = safeCorrectionCents;
                    rendererCrossingEndCorrectionCents = clamp (
                        (static_cast<double> (predictedTarget) - 69.0) * 100.0 - inputCents,
                        -maximumLiveCorrectionCents, maximumLiveCorrectionCents);
                    rendererCrossingCorrectionCents = safeCorrectionCents;
                    rendererCrossingDurationMs = voicedDescendingReturn
                                               ? rendererCrossingReturnTransitionMs
                                               : rendererCrossingTransitionMs;
                    rendererCrossingProgress = 0.0;
                }
            }
        }
        outputCorrectionCents = safeCorrectionCents;
        correctedPitchCents = inputCents + safeCorrectionCents;

        output.inputPitchCents = inputCents;
        output.targetPitchCents = targetCents;
        output.transitionTargetCents = controlTargetCents;
        output.transitionStartCents = transitionStartCents;
        output.transitionEndCents = transitionEndCents;
        output.transitionDurationMs = transitionDurationMs;
        output.transitionProgress = transitionProgress;
        output.transitionActive = transitionActive;
        output.transitionUsesRapidAdjacentContour = transitionUsesRapidAdjacentContour;
        output.rendererCrossingActive = rendererCrossingActive;
        output.rendererCrossingTargetNote = rendererCrossingTargetNote;
        output.rendererCrossingProgress = rendererCrossingProgress;
        output.targetDecisionMidpointCents = targetDecisionMidpointCents;
        output.targetDecisionExcessCents = targetDecisionExcessCents;
        output.targetHysteresisCents = midpointHysteresisSemitones * 100.0;
        output.pendingTargetNote = pendingTargetNote;
        output.pendingTargetFrames = pendingTargetFrames;
        output.targetDecisionBeyondHysteresis = targetDecisionBeyondHysteresis;
        output.rapidAdjacentRawCandidate = rapidAdjacentRawCandidate;
        output.rapidLegatoAdjacentStep = adjacentLegatoStep;
        output.rapidLegatoHardLock = hardLockPreset;
        output.rapidLegatoTargetChanged = trajectoryTargetChanged;
        output.rapidLegatoContinuesDirection = continuesDirectedLegato;
        output.rapidLegatoChainStarts = startsOrContinuesRapidLegatoChain;
        output.rapidLegatoChainActive = transitionUsesRapidLegatoChain;
        output.rapidLegatoCommittedSteps = rapidLegatoAdjacentChanges;
        output.rapidLegatoSamplesSinceCommit = rapidLegatoSamplesSinceChange;
        output.rapidLegatoDistanceCents = hardTuneLegatoDistance;
        output.rapidLegatoInputMotionCentsPerSecond = inputMotionCentsPerSecond;
        output.extremeTargetChange = largeTargetJump;
        output.correctedPitchCents = correctedPitchCents;
        output.correctionCents = safeCorrectionCents;
        output.flexCorrectionGain = correctionGain;
        output.desiredCorrectionCents = desiredCorrectionCents;
        output.retuneCorrectionCents = smoothedCorrectionCents;
        output.humanizeWeight = humanize;
        output.vibratoContributionCents = scaledVibratoResidual;
        output.pitchRatio = std::exp2 (output.correctionCents / 1200.0);
        output.targetNote = currentTargetNote;
        output.hasValidCorrection = std::isfinite (output.pitchRatio);
        controlSchedulerValid = output.hasValidCorrection;
        cachedInputCents = inputCents;
        cachedTargetCents = targetCents;
        cachedControlTargetCents = controlTargetCents;
        cachedUsesPortamentoTarget = portamentoActive && retuneSpeedMs > 0.0;
        cachedScaledVibratoResidual = scaledVibratoResidual;
        cachedFlex = flex;
        cachedEffectiveRetuneMs = effectiveRetuneMs;
        cachedHumanize = humanize;
        cachedCorrectionGain = correctionGain;
        cachedDescentRailGuard = descentRailGuardActive;
        cachedHardLockPreset = hardLockPreset;
        cachedTargetNote = currentTargetNote;
        cachedCandidateTargetNote = candidate;
        cachedExtremeTargetChange = largeTargetJump;
        transientRetuneBlend *= std::exp (-static_cast<double> (hopSamples)
                                          / (transientReleaseSeconds * sampleRate));
        if (transientRetuneBlend < 1.0e-3)
            transientRetuneBlend = 0.0;
        return output;
    }

    // Called only by the fixed-rate renderer-control scheduler. It never
    // selects a target or reads YIN state; it advances the already committed
    // target segment and evaluates the existing correction equations for the
    // elapsed control interval.
    CorrectionTrajectoryData advanceScheduledTransition (int elapsedSamples,
                                                          double sampleRate) noexcept
    {
        CorrectionTrajectoryData output;
        if (! controlSchedulerValid || elapsedSamples <= 0 || sampleRate <= 0.0)
            return output;

        if (transitionActive)
        {
            if (transitionUsesRapidLegatoChain)
            {
                const auto dt = static_cast<double> (elapsedSamples) / sampleRate;
                const auto next = transitionTargetCents + transitionVelocityCentsPerSecond * dt;
                const auto remaining = transitionEndCents - transitionTargetCents;
                const auto reachedEndpoint = remaining == 0.0
                                         || (transitionEndCents - next) * remaining <= 0.0;
                transitionTargetCents = reachedEndpoint ? transitionEndCents : next;
                if (reachedEndpoint)
                {
                    transitionActive = false;
                    transitionUsesRapidLegatoChain = false;
                    transitionUsesRapidAdjacentContour = false;
                    transitionVelocityCentsPerSecond = 0.0;
                    transitionProgress = 1.0;
                }
            }
            else
            {
                const auto durationSamples = std::fmax (1.0, transitionDurationMs * 0.001 * sampleRate);
                transitionProgress = clamp01 (transitionProgress + static_cast<double> (elapsedSamples) / durationSamples);
                auto shapedProgress = transitionUsesLinearSlew
                                    ? transitionProgress
                                    : (transitionUsesRapidAdjacentContour
                                       ? rapidAdjacentTransitionProgress (transitionProgress)
                                       : smoothTransitionProgress (transitionProgress));
                transitionTargetCents = transitionStartCents
                                      + (transitionEndCents - transitionStartCents) * shapedProgress;
                if (transitionProgress >= 1.0)
                {
                    transitionTargetCents = transitionEndCents;
                    if (reversalSettlingToPreviousTarget)
                    {
                        reversalSettlingToPreviousTarget = false;
                        reversalHoldSamplesRemaining = rapidReversalHoldSamples;
                    }
                    else
                    {
                        transitionActive = false;
                        transitionUsesRapidAdjacentContour = false;
                    }
                }
            }
        }

        if (reversalHoldSamplesRemaining > 0)
        {
            reversalHoldSamplesRemaining -= elapsedSamples;
            if (reversalHoldSamplesRemaining <= 0 && std::isfinite (reversalPendingTargetCents))
            {
                const auto returnTargetCents = reversalPendingTargetCents;
                transitionStartCents = transitionTargetCents;
                transitionEndCents = returnTargetCents;
                reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
                // This is the second half of the same rapid adjacent-note
                // reversal.  Sending it through the 42 ms general adjacent
                // transition smears the just-audible middle note into the
                // return. Keep the target plateau short, then use the same
                // direction-aware rapid contour as the first leg.
                const auto returnIsDescending = returnTargetCents < transitionTargetCents;
                transitionDurationMs = returnIsDescending ? hardTuneRapidDescendingTransitionMs
                                                           : hardTuneRapidAdjacentTransitionMs;
                transitionProgress = 0.0;
                transitionActive = true;
                // Second half of a rapid reversal has the same hard,
                // constant-rate geometry as the first half.  Do not bring
                // back the quintic easing here or A -> B -> A becomes a
                // rounded glide at the return.
                transitionUsesLinearSlew = true;
                transitionUsesRapidAdjacentContour = true;
            }
        }

        const auto controlTargetCents = cachedUsesPortamentoTarget ? cachedControlTargetCents
                                                                     : transitionTargetCents;
        const auto correctionReferenceCents = cachedHardLockPreset ? cachedInputCents : pitchCenterCents;
        const auto pitchError = controlTargetCents - correctionReferenceCents;
        const auto correctionGain = flexCorrectionGain (cachedFlex, pitchError);
        const auto flexTargetCents = correctionReferenceCents + pitchError * correctionGain;
        const auto rawDesiredCorrectionCents = flexTargetCents - correctionReferenceCents;
        const auto desiredCorrectionCents = cachedDescentRailGuard
                                          ? softRailLimit (rawDesiredCorrectionCents)
                                          : clamp (rawDesiredCorrectionCents,
                                                   -maximumLiveCorrectionCents,
                                                   maximumLiveCorrectionCents);
        const auto timeSeconds = cachedEffectiveRetuneMs * 0.001;
        const auto alpha = timeSeconds <= 0.0 ? 1.0
                         : 1.0 - std::exp (-static_cast<double> (elapsedSamples) / (timeSeconds * sampleRate));
        smoothedCorrectionCents += (desiredCorrectionCents - smoothedCorrectionCents) * alpha;
        smoothedCorrectionCents = clamp (smoothedCorrectionCents,
                                         -maximumLiveCorrectionCents,
                                         maximumLiveCorrectionCents);
        auto safeCorrectionCents = clamp (correctionReferenceCents + smoothedCorrectionCents
                                          + cachedScaledVibratoResidual - cachedInputCents,
                                          -maximumLiveCorrectionCents,
                                          maximumLiveCorrectionCents);
        if (rendererCrossingActive)
        {
            const auto durationSamples = std::fmax (1.0, rendererCrossingDurationMs * 0.001 * sampleRate);
            rendererCrossingProgress = clamp01 (rendererCrossingProgress
                                               + static_cast<double> (elapsedSamples) / durationSamples);
            rendererCrossingCorrectionCents = rendererCrossingStartCorrectionCents
                                             + (rendererCrossingEndCorrectionCents
                                                - rendererCrossingStartCorrectionCents)
                                               * rendererCrossingProgress;
            // Keep the predicted endpoint until the selector confirms it;
            // otherwise the old target would pull correction backward for a
            // control tick immediately after this continuous crossing.
            safeCorrectionCents = rendererCrossingCorrectionCents;

            // A renderer crossing is a short prediction, never a new
            // musical target. If the selector has not confirmed that exact
            // note by the end of the 25 ms preview, stop it immediately.
            // Previously a rejected preview stayed at its endpoint until a
            // coincidental later commit, which could hold a wrong correction
            // for seconds even though currentTargetNote was already valid.
            if (rendererCrossingProgress >= 1.0
                && currentTargetNote != rendererCrossingTargetNote)
            {
                rendererCrossingActive = false;
                rendererCrossingTargetNote = -1;
            }
        }
        const auto maximumStepCents = cachedHardLockPreset ? hardLockMaximumCorrectionStepCents
                                                            : maximumCorrectionStepCents;
        safeCorrectionCents = clamp (safeCorrectionCents,
                                     outputCorrectionCents - maximumStepCents,
                                     outputCorrectionCents + maximumStepCents);
        outputCorrectionCents = safeCorrectionCents;

        output.inputPitchCents = cachedInputCents;
        output.targetPitchCents = cachedTargetCents;
        output.transitionTargetCents = controlTargetCents;
        output.transitionStartCents = transitionStartCents;
        output.transitionEndCents = transitionEndCents;
        output.transitionDurationMs = transitionDurationMs;
        output.transitionProgress = transitionProgress;
        output.transitionActive = transitionActive;
        output.transitionUsesRapidAdjacentContour = transitionUsesRapidAdjacentContour;
        output.rendererCrossingActive = rendererCrossingActive;
        output.rendererCrossingTargetNote = rendererCrossingTargetNote;
        output.rendererCrossingProgress = rendererCrossingProgress;
        output.extremeTargetChange = cachedExtremeTargetChange;
        output.correctedPitchCents = cachedInputCents + safeCorrectionCents;
        output.correctionCents = safeCorrectionCents;
        output.flexCorrectionGain = correctionGain;
        output.desiredCorrectionCents = desiredCorrectionCents;
        output.retuneCorrectionCents = smoothedCorrectionCents;
        output.humanizeWeight = cachedHumanize;
        output.vibratoContributionCents = cachedScaledVibratoResidual;
        output.pitchRatio = std::exp2 (safeCorrectionCents / 1200.0);
        output.targetNote = cachedTargetNote;
        output.candidateTargetNote = cachedCandidateTargetNote;
        output.hasValidCorrection = std::isfinite (output.pitchRatio);
        return output;
    }

private:
    void resetControlSchedulerCache() noexcept
    {
        controlSchedulerValid = false;
        cachedInputCents = 0.0;
        cachedTargetCents = 0.0;
        cachedControlTargetCents = 0.0;
        cachedUsesPortamentoTarget = false;
        cachedScaledVibratoResidual = 0.0;
        cachedFlex = 0.0;
        cachedEffectiveRetuneMs = 0.0;
        cachedHumanize = 0.0;
        cachedCorrectionGain = 0.0;
        cachedDescentRailGuard = false;
        cachedHardLockPreset = false;
        cachedTargetNote = -1;
        cachedCandidateTargetNote = -1;
        cachedExtremeTargetChange = false;
    }

    // This is target *observation*, not output smoothing. Keep it responsive
    // enough to expose real adjacent target changes; Retune trajectory then
    // determines how fast the rendered pitch follows them.
    static constexpr double midpointHysteresisSemitones = 0.04; // 4 cents
    static constexpr double onsetJumpCents = 95.0;
    static constexpr double inputMotionTimeSeconds = 0.040;
    static constexpr double sourceTimedJumpThresholdCents = 150.0;
    static constexpr double minimumSourceMotionCentsPerSecond = 180.0;
    static constexpr double sourceTempoFollowRatio = 0.65;
    // At explicit 0 ms this is only a de-zipper between two confirmed
    // adjacent targets, not an extra musical glide. 44 ms made hard-tune
    // luyến leave an audible slow-motion tail.
    // Retune 0 remains an immediate landing on a sustained target.  This
    // separate, short contour is only for a confirmed adjacent target switch
    // while the vowel is moving. Nine milliseconds is the shortest measured
    // landing that improves the reference transition score without increasing
    // the rendered sample-step bound.
    // Preserve the hard-tune destination, but give a legato adjacent-note
    // move enough control frames to avoid a single grain being retuned across
    // the whole interval.
    // The renderer needs a short certified contour during a legato move;
    // this does not alter the hard-tune destination on sustained vowels.
    static constexpr double hardTuneAdjacentTransitionMs = 6.0;
    // This is used only after a neighbouring target was normally committed.
    // In a tightly spaced A <-> A# figure, a longer upward ramp overlaps
    // the following reversal and turns the distinct notes into a glide.
    // Keep this landing short so each committed target remains audible.
    static constexpr double hardTuneRapidAdjacentTransitionMs = 9.0;
    static constexpr double hardTuneRapidDescendingTransitionMs = 9.0;
    static constexpr double rendererCrossingTransitionMs = 25.0;
    static constexpr double rendererCrossingMinimumMotionCentsPerSecond = 1500.0;
    static constexpr double rendererCrossingReturnTransitionMs = 12.0;
    static constexpr double rendererCrossingReturnMinimumMotionCentsPerSecond = 700.0;
    static constexpr double rendererCrossingReturnMinimumF0Hz = 180.0;
    // An A->B->A return is a different gesture from a new phrase. Allow a
    // measured, moderately fast downward return to preview only while the
    // opposite target is still recent; this keeps slower later luyến out.
    static constexpr double rendererCrossingPreviewLeadSemitones = 0.28;
    static constexpr double rapidReversalSettleMs = 8.0;
    // This is a narrowly-scoped hand-off between the fast landing at the
    // middle note and the normal return contour. Keeping it short is vital:
    // a longer hold blocks the next real F0 reversal and merges the two
    // audible notes into one glide.
    static constexpr int rapidReversalHoldSamples = 256;
    static constexpr double hardTuneLegatoMaximumCents = 300.01;
    static constexpr int rapidLegatoChainGapSamples = 1536;
    // This is above ordinary vibrato motion and is used only together with a
    // discrete adjacent target crossing. It never promotes a broad jump or
    // an unvoiced re-entry into rapid-legato.
    static constexpr double rapidLegatoMinimumMotionCentsPerSecond = 2000.0;
    // Keep rapid luyến on the same median + hysteresis target path as every
    // other note change. The trajectory itself supplies the one smooth,
    // fixed-duration movement; bypassing confirmation here made short notes
    // too easy to overwrite before they became audible.
    static constexpr bool enableRapidAdjacentRawCandidate = false;
    static constexpr bool enableRapidLegatoChain = false;
    static constexpr double hardTuneLegatoChainRateCentsPerSecond = 100000.0
                                                               / hardTuneAdjacentTransitionMs;
    static constexpr double portamentoMinimumFrameDeltaCents = 3.0;
    // Wider than the 40 ct vibrato regression envelope (80 ct peak-to-peak),
    // so natural vibrato never enters the glide path.
    static constexpr double portamentoActivationTravelCents = 120.0;
    // A single wide note jump is a landing event, not a portamento. Require
    // a short same-direction F0 run before bypassing intermediate targets.
    static constexpr int portamentoMinimumMotionFrames = 3;
    static constexpr int portamentoSettleFrames = 5;
    // Target candidates are already median-filtered over three frames.  A
    // All candidate changes use the same sustained confirmation after the
    // median/hysteresis. This keeps the committed note stable during glides.
    static constexpr int targetPersistenceFrames = 1;
    static constexpr int onsetTargetPersistenceFrames = 1;
    // A new target after an unvoiced gap must not be created from the first
    // recovered frame. Breath tails and consonant bursts often produce one
    // plausible F0 frame; requiring three existing analysis hops prevents
    // that burst from ever entering the pitch renderer without adding audio
    // buffering or plugin latency.
    static constexpr int initialTargetPersistenceFrames = 3;
    // The median already rejects a one-frame detector outlier. Do not block
    // a genuine A -> B -> A melodic/vibrato target sequence here: this state
    // must be visible to the Retune trajectory at every speed setting.
    // A genuine luyến can continue in one direction immediately. Only an
    // A→B→A reversal inside four analysis hops needs a second confirmation;
    // this prevents tracker chatter from restarting hard-tune repeatedly.
    static constexpr int targetReversalGuardFrames = 4;
    static constexpr int targetReversalPersistenceFrames = 2;
    // Alto analyses every 256 samples, so nine frames preserve a rapid
    // returned note for 48 ms without changing ordinary note commits.
    static constexpr int hardTuneRapidReversalGuardFrames = 9;
    // Classic still has a more direct target policy, but never permits a
    // single suspicious analysis frame to become an audible note switch.
    static constexpr int classicTargetPersistenceFrames = 2;
    static constexpr int targetMedianHistoryLength = 3;
    static constexpr double transientRetuneMs = 55.0;
    static constexpr double minimumContinuousRetuneMs = 8.0;
    static constexpr double artistRetune10MsTimeConstantMs = 53.333 / 2.302585092994046;
    static constexpr double artistRetune25MsTimeConstantMs = 96.0 / 2.302585092994046;
    static constexpr double artistRetune50MsTimeConstantMs = 176.0 / 2.302585092994046;
    static constexpr double artistRetuneAbove50Slope = (artistRetune50MsTimeConstantMs
                                                       - artistRetune25MsTimeConstantMs) / 25.0;
    static constexpr double transientReleaseSeconds = 0.018;
    static constexpr double humanizeMinimumSustainMs = 80.0;
    // A melodic third is a normal committed note change, not a renderer
    // failure.  Bridging it to dry interrupts an otherwise continuous Note
    // Transition / Retune trajectory and re-engages the renderer with a
    // sizeable control step.  Reserve the exceptional bridge for a truly
    // implausible tracker jump; ordinary musical intervals stay on the
    // continuous processed path.
    static constexpr int largeTargetJumpSemitones = 7;
    static constexpr double maximumLiveCorrectionCents = 120.0;
    static constexpr int rapidDescentRailGuardHoldSamples = 1536;
    static constexpr double maximumCorrectionStepCents = 60.0;
    static constexpr double hardLockMaximumCorrectionStepCents = 36.0;
    static constexpr double humanizeStartSeconds = 0.240;
    static constexpr double humanizeRampSeconds = 0.180;
    static constexpr double humanizeWeightTimeSeconds = 0.040;
    static constexpr double pitchCentreTimeSeconds = 0.120;
    static constexpr double vibratoEnergyTimeSeconds = 0.100;
    static constexpr double minimumVibratoEnergyCents = 2.5;
    static constexpr double vibratoEnergyFadeCents = 4.0;
    static constexpr int pitchResidualHistoryLength = 24;
    static constexpr double minimumHumanizeVibratoDeviationCents = 3.0;
    static constexpr double minimumHumanizeVibratoRateHz = 4.0;
    static constexpr double maximumHumanizeVibratoRateHz = 8.0;
    static constexpr double vibratoRecognitionTimeSeconds = 0.100;
    static constexpr double maximumInputVibratoDepthCents = 60.0;
    static constexpr double maximumOutputVibratoDepthCents = 80.0;
    static constexpr double artistNaturalVibratoGainPerUnit = 0.013;

    static double clamp01 (double value) noexcept { return std::fmax (0.0, std::fmin (1.0, value)); }
    static double clamp (double value, double minimum, double maximum) noexcept
    {
        return std::fmax (minimum, std::fmin (maximum, value));
    }

    static double softRailLimit (double value) noexcept
    {
        constexpr auto knee = 88.0;
        constexpr auto ceiling = 108.0;
        constexpr auto range = ceiling - knee;
        const auto magnitude = std::abs (value);
        if (magnitude <= knee)
            return value;
        const auto limited = knee + range * (1.0 - std::exp (-(magnitude - knee) / range));
        return std::copysign (limited, value);
    }

    // Artist calibration: residual pitch fraction after a settled F#-minor
    // correction, measured at input errors 0/5/10/30/60 cents. Rows are the
    // Flex positions 0/25/50/75/100. Linear interpolation keeps knob motion
    // continuous while retaining the measured hard pull at low Flex and the
    // genuine bypass at Flex 100.
    static double flexCorrectionGain (double flex, double pitchErrorCents) noexcept
    {
        constexpr std::array<double, 5> flexKnots {{ 0.0, 0.25, 0.50, 0.75, 1.0 }};
        constexpr std::array<double, 5> errorKnots {{ 0.0, 5.0, 10.0, 30.0, 60.0 }};
        constexpr std::array<std::array<double, 5>, 5> residualFraction {{
            {{ 0.0, 0.0, 0.0, 0.0, 0.0 }}, // Flex 0
            {{ 0.0, 0.0, 0.0, 0.0, 0.0 }}, // Flex 25
            {{ 0.0, 0.0, 0.0, 0.0, 0.33 }},
            {{ 0.0, 0.0, 0.0, 0.22, 0.78 }},
            {{ 1.0, 1.0, 1.0, 1.0, 1.0 }}  // Flex 100
        }};

        const auto interpolateIndex = [] (const auto& knots, double value, size_t& lower, double& amount)
        {
            const auto clampedValue = clamp (value, knots.front(), knots.back());
            lower = knots.size() - 2;
            amount = 1.0;
            for (size_t index = 1; index < knots.size(); ++index)
                if (clampedValue <= knots[index])
                {
                    lower = index - 1;
                    amount = (clampedValue - knots[lower]) / (knots[index] - knots[lower]);
                    return;
                }
        };

        size_t flexLower = 0, errorLower = 0;
        double flexAmount = 0.0, errorAmount = 0.0;
        interpolateIndex (flexKnots, clamp01 (flex), flexLower, flexAmount);
        interpolateIndex (errorKnots, std::abs (pitchErrorCents), errorLower, errorAmount);
        const auto residualAtLowerFlex = residualFraction[flexLower][errorLower]
                                        + (residualFraction[flexLower][errorLower + 1]
                                           - residualFraction[flexLower][errorLower]) * errorAmount;
        const auto residualAtUpperFlex = residualFraction[flexLower + 1][errorLower]
                                        + (residualFraction[flexLower + 1][errorLower + 1]
                                           - residualFraction[flexLower + 1][errorLower]) * errorAmount;
        const auto residual = residualAtLowerFlex + (residualAtUpperFlex - residualAtLowerFlex) * flexAmount;
        return 1.0 - residual;
    }

    // Artist Humanize does not rise linearly. On the recorded 5 Hz sustained
    // vowel, the additional retained modulation reaches 69/89/96% of its
    // maximum at the displayed 25/50/75 settings. This affects only a note
    // that has passed the sustain and periodic-vibrato gates above.
    static double artistHumanizeResponse (double humanizePercent) noexcept
    {
        constexpr std::array<double, 5> positions {{ 0.0, 25.0, 50.0, 75.0, 100.0 }};
        constexpr std::array<double, 5> responses {{ 0.0, 0.69, 0.89, 0.96, 1.0 }};
        const auto value = clamp (humanizePercent, positions.front(), positions.back());
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                const auto amount = (value - positions[index - 1]) / (positions[index] - positions[index - 1]);
                return responses[index - 1] + (responses[index] - responses[index - 1]) * amount;
            }
        return responses.back();
    }

    void resetPitchVariationAnalysis() noexcept
    {
        pitchResidualHistory.fill (0.0);
        pitchResidualHistoryIndex = 0;
        pitchResidualHistoryCount = 0;
        humanizeVibratoRecognition = 0.0;
        humanizeSustainWeight = 0.0;
    }

    void resetPortamentoTracking() noexcept
    {
        portamentoActive = false;
        portamentoDirection = 0;
        portamentoTravelCents = 0.0;
        portamentoStillFrames = 0;
        portamentoMotionFrames = 0;
    }

    void updatePortamentoTracking (double inputCents, double retuneSpeedMs) noexcept
    {
        if (! std::isfinite (previousInputPitchCents) || retuneSpeedMs <= 0.0)
        {
            if (retuneSpeedMs <= 0.0)
                resetPortamentoTracking();
            return;
        }

        const auto delta = inputCents - previousInputPitchCents;
        if (std::abs (delta) < portamentoMinimumFrameDeltaCents)
        {
            if (++portamentoStillFrames >= portamentoSettleFrames)
                resetPortamentoTracking();
            return;
        }

        const auto direction = delta > 0.0 ? 1 : -1;
        if (direction != portamentoDirection)
        {
            portamentoDirection = direction;
            portamentoTravelCents = std::abs (delta);
            portamentoMotionFrames = 1;
            portamentoActive = false;
        }
        else
        {
            portamentoTravelCents += std::abs (delta);
            ++portamentoMotionFrames;
        }
        portamentoStillFrames = 0;

        // Wide, monotonic F0 travel is a sung glide, not a chain of faulty
        // semitones. The modest threshold rejects ordinary vibrato while
        // starting before successive scale targets can make a staircase.
        if (portamentoTravelCents >= portamentoActivationTravelCents
            && portamentoMotionFrames >= portamentoMinimumMotionFrames
            && inputMotionCentsPerSecond >= minimumSourceMotionCentsPerSecond)
            portamentoActive = true;
    }

    static int nearestLegalNote (double desiredMidi, const NotePolicySnapshot& policy,
                                 int preferredNote) noexcept
    {
        auto bestNote = -1;
        auto bestDistance = std::numeric_limits<double>::infinity();
        for (int note = 0; note <= 127; ++note)
        {
            if (! isLegalNoteRule (policy.ruleForMidi (note)))
                continue;

            const auto distance = std::abs (static_cast<double> (note) - desiredMidi);
            if (distance < bestDistance
                || (distance == bestDistance && note == preferredNote)
                || (distance == bestDistance && bestNote != preferredNote && note < bestNote))
            {
                bestNote = note;
                bestDistance = distance;
            }
        }
        return bestNote;
    }

    static int directedLegalNote (double desiredMidi, const NotePolicySnapshot& policy,
                                  bool upward) noexcept
    {
        const auto initial = upward ? static_cast<int> (std::floor (desiredMidi)) + 1
                                    : static_cast<int> (std::ceil (desiredMidi)) - 1;
        for (int note = initial; note >= 0 && note <= 127; note += upward ? 1 : -1)
            if (isLegalNoteRule (policy.ruleForMidi (note)))
                return note;
        return -1;
    }

    static int selectPolicyTarget (double desiredMidi, NoteRule sourceRule,
                                   const NotePolicySnapshot& policy, int currentTarget) noexcept
    {
        if (sourceRule == NoteRule::IllegalUp)
            return directedLegalNote (desiredMidi, policy, true);
        if (sourceRule == NoteRule::IllegalDown)
            return directedLegalNote (desiredMidi, policy, false);
        return nearestLegalNote (desiredMidi, policy, currentTarget);
    }

    void resetTrajectoryPreview() noexcept
    {
        trajectoryPreviewNote = -1;
        trajectoryPreviewDirection = 0;
        trajectoryPreviewUpdates = 0;
        trajectoryPreviewTravelCents = 0.0;
        diagnosticPreviewPreviousDeltaCents = 0.0;
    }

    void updateTrajectoryPreview (int nearestCandidate, double desiredMidi,
                                  double signedInputDeltaCents,
                                  const NotePolicySnapshot& policy, bool hardLock) noexcept
    {
        const auto previousDelta = diagnosticPreviewPreviousDeltaCents;
        diagnosticPreviewPreviousDeltaCents = signedInputDeltaCents;
        if (! hardLock || currentTargetNote < 0)
        {
            resetTrajectoryPreview();
            return;
        }
        if (std::abs (signedInputDeltaCents) >= 2.0)
        {
            const auto direction = signedInputDeltaCents > 0.0 ? 1 : -1;
            if (direction != trajectoryPreviewDirection)
            {
                trajectoryPreviewDirection = direction;
                trajectoryPreviewUpdates = 1;
                trajectoryPreviewTravelCents = std::abs (signedInputDeltaCents);
                trajectoryPreviewNote = -1;
            }
            else
            {
                ++trajectoryPreviewUpdates;
                trajectoryPreviewTravelCents += std::abs (signedInputDeltaCents);
            }
        }
        if (trajectoryPreviewNote >= 0)
        {
            if (currentTargetNote == trajectoryPreviewNote)
                resetTrajectoryPreview();
            return;
        }
        const auto previewTravelThreshold = diagnosticRapidLegatoPreview ? 10.0 : 20.0;
        const auto previewFramesRequired = 2;
        const auto accelerationSupportsDirection = ! diagnosticRapidLegatoPreview
                                                || (signedInputDeltaCents * trajectoryPreviewDirection > 0.0
                                                    && std::abs (signedInputDeltaCents)
                                                       >= std::abs (previousDelta) + 0.5);
        // Nominate the directed adjacent legal note *before* raw
        // quantisation crosses its midpoint. The prior current-candidate
        // veto made the preview inert in production: by the time it ran,
        // direct hard-tune had already committed the same raw candidate.
        // Two coherent moving frames and the narrow lead below remain
        // mandatory, so normal vibrato cannot promote a neighbour.
        if (trajectoryPreviewUpdates < previewFramesRequired
            || trajectoryPreviewTravelCents < previewTravelThreshold
            || ! accelerationSupportsDirection)
            return;
        const auto adjacent = directedLegalNote (desiredMidi, policy, trajectoryPreviewDirection > 0);
        if (adjacent < 0 || adjacent == currentTargetNote)
            return;
        const auto direction = adjacent > currentTargetNote ? 1.0 : -1.0;
        const auto midpoint = 0.5 * static_cast<double> (adjacent + currentTargetNote);
        // Four cents is enough to win part of an analysis hop while the
        // coherent-direction gate prevents normal vibrato from becoming an
        // extra target commit.
        const auto previewLeadSemitones = diagnosticRapidLegatoPreview ? 0.30 : 0.04;
        if ((desiredMidi - midpoint) * direction >= -previewLeadSemitones)
            trajectoryPreviewNote = adjacent;
    }

    int filterTargetCandidate (int candidate) noexcept
    {
        targetCandidateHistory[static_cast<size_t> (targetCandidateHistoryIndex)] = candidate;
        targetCandidateHistoryIndex = (targetCandidateHistoryIndex + 1) % targetMedianHistoryLength;
        targetCandidateHistoryCount = std::min (targetCandidateHistoryCount + 1, targetMedianHistoryLength);

        std::array<int, targetMedianHistoryLength> sorted {};
        for (int index = 0; index < targetCandidateHistoryCount; ++index)
            sorted[static_cast<size_t> (index)] = targetCandidateHistory[static_cast<size_t> (index)];
        std::sort (sorted.begin(), sorted.begin() + targetCandidateHistoryCount);
        return sorted[static_cast<size_t> (targetCandidateHistoryCount / 2)];
    }

    void selectTarget (int candidate, double desiredMidi, bool onset, TuningPolicy tuningPolicy,
                       bool requiresInitialConfirmation, bool directHardTune) noexcept
    {
        if (diagnosticRapidLegatoPreview)
            diagnosticCommitLockSamples = std::max (0, diagnosticCommitLockSamples - diagnosticControlHopSamples);
        ++framesSinceTargetCommit;

        if (currentTargetNote < 0)
        {
            if (! requiresInitialConfirmation)
            {
                currentTargetNote = candidate;
                previousTargetNote = -1;
                framesSinceTargetCommit = 0;
                pendingTargetNote = -1;
                pendingTargetFrames = 0;
                return;
            }
            pendingTargetFrames = pendingTargetNote == candidate ? pendingTargetFrames + 1 : 1;
            pendingTargetNote = candidate;
            if (pendingTargetFrames >= initialTargetPersistenceFrames)
            {
                currentTargetNote = candidate;
                previousTargetNote = -1;
                framesSinceTargetCommit = 0;
                pendingTargetNote = -1;
                pendingTargetFrames = 0;
            }
            return;
        }

        if (candidate == currentTargetNote)
        {
            pendingTargetNote = -1;
            pendingTargetFrames = 0;
            return;
        }

        if (directHardTune)
        {
            if (diagnosticRapidLegatoPreview && diagnosticCommitLockSamples > 0)
                return;
            const auto candidateDirection = candidate > currentTargetNote ? 1 : -1;
            // The fast Alto analysis may briefly quantise a mixed boundary
            // frame to the opposite neighbour. Do not let that frame reverse
            // the musical target unless the measured F0 is moving with it.
            if (trajectoryPreviewDirection != 0 && candidateDirection != trajectoryPreviewDirection)
                return;
            // Retune 0 normally commits immediately. The exception is an
            // immediate return to the note that was active before this one:
            // A -> B -> A -> B chatter gives the returned A only one
            // analysis frame, so no audible grain can establish its pitch.
            // Keep that returned note for 48 ms; an ordinary adjacent step
            // or a sustained reversal is unaffected.
            const auto immediateReturnToPrevious = candidate == previousTargetNote
                                                 && framesSinceTargetCommit < hardTuneRapidReversalGuardFrames;
            if (immediateReturnToPrevious)
                return;
            previousTargetNote = currentTargetNote;
            currentTargetNote = candidate;
            framesSinceTargetCommit = 0;
            pendingTargetNote = -1;
            pendingTargetFrames = 0;
            if (diagnosticRapidLegatoPreview)
                diagnosticCommitLockSamples = diagnosticRapidLegatoCommitLockSamples;
            return;
        }

        const auto direction = candidate > currentTargetNote ? 1.0 : -1.0;
        const auto midpoint = 0.5 * static_cast<double> (candidate + currentTargetNote);
        const auto beyondMidpoint = (desiredMidi - midpoint) * direction > midpointHysteresisSemitones;
        if (! onset && ! beyondMidpoint)
            return;

        pendingTargetFrames = pendingTargetNote == candidate ? pendingTargetFrames + 1 : 1;
        pendingTargetNote = candidate;
        const auto requiredFrames = onset ? onsetTargetPersistenceFrames
                                  : tuningPolicy == TuningPolicy::Classic ? classicTargetPersistenceFrames
                                                                        : targetPersistenceFrames;
        const auto isQuickReversal = tuningPolicy == TuningPolicy::Standard
                                  && candidate == previousTargetNote
                                  && framesSinceTargetCommit < targetReversalGuardFrames;
        const auto guardedRequiredFrames = isQuickReversal
                                         ? targetReversalPersistenceFrames : requiredFrames;
        if (pendingTargetFrames >= guardedRequiredFrames)
        {
            previousTargetNote = currentTargetNote;
            currentTargetNote = candidate;
            framesSinceTargetCommit = 0;
            pendingTargetNote = -1;
            pendingTargetFrames = 0;
        }
    }

    int currentTargetNote = -1;
    int trajectoryPreviewNote = -1;
    int trajectoryPreviewDirection = 0;
    int trajectoryPreviewUpdates = 0;
    double trajectoryPreviewTravelCents = 0.0;
    double diagnosticPreviewPreviousDeltaCents = 0.0;
    int pendingTargetNote = -1;
    int pendingTargetFrames = 0;
    int previousTargetNote = -1;
    int framesSinceTargetCommit = 0;
    std::array<int, targetMedianHistoryLength> targetCandidateHistory {};
    int targetCandidateHistoryIndex = 0;
    int targetCandidateHistoryCount = 0;
    double transientRetuneBlend = 0.0;
    int unvoicedFrames = 0;
    int64_t sustainedTargetSamples = 0;
    double previousInputPitchCents = std::numeric_limits<double>::quiet_NaN();
    double inputMotionCentsPerSecond = 0.0;
    bool portamentoActive = false;
    int portamentoDirection = 0;
    double portamentoTravelCents = 0.0;
    int portamentoStillFrames = 0;
    int portamentoMotionFrames = 0;
    double correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
    double smoothedCorrectionCents = std::numeric_limits<double>::quiet_NaN();
    double outputCorrectionCents = std::numeric_limits<double>::quiet_NaN();
    double pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
    double vibratoEnergyCents = 0.0;
    std::array<double, pitchResidualHistoryLength> pitchResidualHistory {};
    int pitchResidualHistoryIndex = 0;
    int pitchResidualHistoryCount = 0;
    double humanizeVibratoRecognition = 0.0;
    double humanizeSustainWeight = 0.0;
    double transitionStartCents = std::numeric_limits<double>::quiet_NaN();
    double transitionEndCents = std::numeric_limits<double>::quiet_NaN();
    double transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
    double transitionProgress = 1.0;
    bool transitionActive = false;
    double transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
    bool transitionUsesLinearSlew = false;
    bool transitionUsesRapidLegatoChain = false;
    bool transitionUsesRapidAdjacentContour = false;
    bool diagnosticRaisedCosineHardTuneLegato = false;
    bool diagnosticRapidLegatoPreview = false;
    bool diagnosticStrongHardTunePull = false;
    int diagnosticCommitLockSamples = 0;
    // A genuine rapid luyến in the reference holds each scale note roughly
    // 80 ms. Keep the early adjacent decision through that interval so a
    // residual vibrato half-cycle cannot immediately reclaim the old note.
    static constexpr int diagnosticRapidLegatoCommitLockSamples = 3840;
    static constexpr int diagnosticControlHopSamples = 256;
    double transitionVelocityCentsPerSecond = 0.0;
    int rapidLegatoDirection = 0;
    int rapidLegatoAdjacentChanges = 0;
    int rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
    int descentRailGuardSamples = 0;
    bool reversalSettlingToPreviousTarget = false;
    int reversalHoldSamplesRemaining = 0;
    double reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
    bool rendererCrossingActive = false;
    int rendererCrossingTargetNote = -1;
    double rendererCrossingDurationMs = rendererCrossingTransitionMs;
    double rendererCrossingStartCorrectionCents = 0.0;
    double rendererCrossingEndCorrectionCents = 0.0;
    double rendererCrossingCorrectionCents = 0.0;
    double rendererCrossingProgress = 0.0;
    bool controlSchedulerValid = false;
    double cachedInputCents = 0.0;
    double cachedTargetCents = 0.0;
    double cachedControlTargetCents = 0.0;
    bool cachedUsesPortamentoTarget = false;
    double cachedScaledVibratoResidual = 0.0;
    double cachedFlex = 0.0;
    double cachedEffectiveRetuneMs = 0.0;
    double cachedHumanize = 0.0;
    double cachedCorrectionGain = 0.0;
    bool cachedDescentRailGuard = false;
    bool cachedHardLockPreset = false;
    int cachedTargetNote = -1;
    int cachedCandidateTargetNote = -1;
    bool cachedExtremeTargetChange = false;
};
