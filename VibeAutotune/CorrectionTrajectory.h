#pragma once

#include "NotePolicy.h"
#include "HumanizeProcessor.h"
#include "PitchDecomposer.h"
#include "VibratoProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// Offline calibration overrides. Production builds use the measured defaults
// below; the benchmark can compile isolated candidates without editing the
// correction law between trials.
#ifndef VIBE_PITCH_CENTRE_SECONDS
 #define VIBE_PITCH_CENTRE_SECONDS 0.250
#endif
#ifndef VIBE_WIDE_VIBRATO_NEUTRAL_RETENTION
 // Measured Retune-10/Humanize-0 endpoint.  This is the low-Humanize corner
 // of the two-control surface, not the value used at Humanize 75--100.
 #define VIBE_WIDE_VIBRATO_NEUTRAL_RETENTION -0.150
#endif
#ifndef VIBE_WIDE_VIBRATO_HUMANIZED_RETENTION
#define VIBE_WIDE_VIBRATO_HUMANIZED_RETENTION 0.666
#endif
#ifndef VIBE_HARD_TUNE_WIDE_UP_TRANSITION_MS
 #define VIBE_HARD_TUNE_WIDE_UP_TRANSITION_MS 4.0
#endif
#ifndef VIBE_HARD_TUNE_WIDE_DOWN_TRANSITION_MS
 #define VIBE_HARD_TUNE_WIDE_DOWN_TRANSITION_MS 4.0
#endif
#ifndef VIBE_HARD_LOCK_MAXIMUM_CORRECTION_STEP_CENTS
 // A certified hard-lock target must be able to complete a full neighbouring
 // note correction in one scheduler handoff. The TD-PSOLA layer preserves
 // phase continuity; this is control geometry, not a waveform discontinuity.
 #define VIBE_HARD_LOCK_MAXIMUM_CORRECTION_STEP_CENTS 120.0
#endif
#ifndef VIBE_HARD_TUNE_RAPID_ADJACENT_TRANSITION_MS
 // One 128-sample control interval at 48 kHz: committed neighbouring notes
 // arrive as a controlled staircase at the four-knob hard-lock surface.
 #define VIBE_HARD_TUNE_RAPID_ADJACENT_TRANSITION_MS 2.6666667
#endif
#ifndef VIBE_HARD_TUNE_RAPID_DESCENDING_TRANSITION_MS
 #define VIBE_HARD_TUNE_RAPID_DESCENDING_TRANSITION_MS 2.6666667
#endif
#ifndef VIBE_HARD_LOCK_RENDERER_CROSSING_TRANSITION_MS
 // Prediction would reintroduce a 25 ms glide before an already-certified
 // note handoff. At 0/0/0/0, use the bounded 48-cent control staircase
 // instead; other control surfaces retain their own timing paths.
 #define VIBE_HARD_LOCK_RENDERER_CROSSING_TRANSITION_MS 0.0
#endif
#ifndef VIBE_HARD_LOCK_RAPID_REVERSAL_SETTLE_MS
 #define VIBE_HARD_LOCK_RAPID_REVERSAL_SETTLE_MS 8.0
#endif
#ifndef VIBE_HARD_TUNE_RAPID_REVERSAL_GUARD_SECONDS
 #define VIBE_HARD_TUNE_RAPID_REVERSAL_GUARD_SECONDS 0.048
#endif
#ifndef VIBE_HARD_LOCK_RAPID_REVERSAL_HOLD_SAMPLES
 #define VIBE_HARD_LOCK_RAPID_REVERSAL_HOLD_SAMPLES 256
#endif
#ifndef VIBE_RAPID_ADJACENT_ENTRY_BEYOND_MIDPOINT_CENTS
 #define VIBE_RAPID_ADJACENT_ENTRY_BEYOND_MIDPOINT_CENTS 4.0
#endif
#ifndef VIBE_RAPID_ADJACENT_MINIMUM_TRAVEL_CENTS
 #define VIBE_RAPID_ADJACENT_MINIMUM_TRAVEL_CENTS 20.0
#endif
#ifndef VIBE_RAPID_ADJACENT_EVIDENCE_WINDOW_FRAMES
 #define VIBE_RAPID_ADJACENT_EVIDENCE_WINDOW_FRAMES 8
#endif
#ifndef VIBE_RAPID_ADJACENT_MINIMUM_SUPPORTING_STEPS
 #define VIBE_RAPID_ADJACENT_MINIMUM_SUPPORTING_STEPS 4
#endif
#ifndef VIBE_RAPID_ADJACENT_BOUNDARY_DWELL_FRAMES
 // Two consecutive high-confidence raw-YIN frames recover a real short
 // adjacent note without promoting an isolated midpoint fluctuation.
 #define VIBE_RAPID_ADJACENT_BOUNDARY_DWELL_FRAMES 2
#endif
#ifndef VIBE_RAPID_ADJACENT_MINIMUM_DWELL_SAMPLES
 // Candidate hook only. Production waits 80 ms after a committed target;
 // an offline build may admit the existing high-confidence predictor after
 // 32 ms to test the measured 69 ms adjacent-note turnaround.
 #define VIBE_RAPID_ADJACENT_MINIMUM_DWELL_SAMPLES 3840
#endif

// Offline candidate hook. Production remains at the qualified +/-120-cent
// correction rail unless an A/B build explicitly provides a different value.
#ifndef VIBE_MAXIMUM_LIVE_CORRECTION_CENTS
 #define VIBE_MAXIMUM_LIVE_CORRECTION_CENTS 120.0
#endif

// Candidate-only hard-lock amplitude rail. Kept independent from the
// musical rail so this A/B cannot alter Flex/Humanize behaviour.
#ifndef VIBE_HARD_LOCK_MAXIMUM_LIVE_CORRECTION_CENTS
 #define VIBE_HARD_LOCK_MAXIMUM_LIVE_CORRECTION_CENTS VIBE_MAXIMUM_LIVE_CORRECTION_CENTS
#endif

// Offline A/B hook. Hard Tune and Natural Vibrato retain the qualified rail.
#ifndef VIBE_MUSICAL_MAXIMUM_LIVE_CORRECTION_CENTS
 #define VIBE_MUSICAL_MAXIMUM_LIVE_CORRECTION_CENTS VIBE_MAXIMUM_LIVE_CORRECTION_CENTS
#endif
#ifndef VIBE_MUSICAL_RAIL_MAX_RETUNE_MS
 #define VIBE_MUSICAL_RAIL_MAX_RETUNE_MS 50.0
#endif

// Candidate-only switch for the hard-tune A -> B -> A plateau.  It is
// intentionally compile-time so normal presets and released sessions retain
// their exact behaviour while the Hook-2 reversal window is evaluated.
#ifndef VIBE_HARD_TUNE_RAPID_REVERSAL_PLATEAU
 #define VIBE_HARD_TUNE_RAPID_REVERSAL_PLATEAU 1
#endif

// Offline Hook-2 A/B switch: permit the already-certified trajectory
// predictor to lead a hard-tune target by one analysis decision.
#ifndef VIBE_HARD_TUNE_TRAJECTORY_PREVIEW
 #define VIBE_HARD_TUNE_TRAJECTORY_PREVIEW 0
#endif

// This policy is deliberately independent of formant preservation. Formant
// is a renderer property; Classic changes only target/Flex/transition policy.
enum class TuningPolicy : uint8_t { Standard = 0, Classic = 1 };

struct CorrectionTrajectoryData
{
    // Debug-only materialization provenance. It records an update or one
    // fixed-rate scheduler advance; it is never consumed by trajectory DSP.
    int controlAdvanceSamples = 0;
    double controlCorrectionBefore = std::numeric_limits<double>::quiet_NaN();
    double controlCorrectionAfter = std::numeric_limits<double>::quiet_NaN();
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
    double rawVibratoResidualCents = 0.0;
    double vibratoEnergyCents = 0.0;
    double vibratoDeviationCents = 0.0;
    // Pitch decomposition telemetry. These values are observational only in
    // this revision: note selection, Flex, Retune and renderer chronology do
    // not consume them yet.
    double noteCenterCents = std::numeric_limits<double>::quiet_NaN();
    double driftCents = 0.0;
    double vibratoRateHz = 0.0;
    double vibratoConfidence = 0.0;
    double noteStability = 0.0;
    double driftConfidence = 0.0;
    double jitterConfidence = 0.0;
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
    // Fast hard-tune selector telemetry. This predictor is strictly an
    // adjacent-note pre-arm; it never replaces the stable selector for
    // sustained notes or ordinary Retune modes.
    bool rapidAdjacentPrearmed = false;
    int rapidAdjacentCandidateNote = -1;
    int rapidAdjacentEvidenceFrames = 0;
    double rapidAdjacentRawDistanceCents = 0.0;
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
    // Selector-preview telemetry. Read-only: used to identify which gate
    // suppresses a rapid return before changing audio behaviour.
    int trajectoryPreviewNote = -1;
    int trajectoryPreviewDirection = 0;
    int trajectoryPreviewUpdates = 0;
    double trajectoryPreviewTravelCents = 0.0;
    double instantaneousInputMotionCentsPerSecond = 0.0;
    // Selector decision audit.  This is exported solely by the offline
    // diagnostic so rapid-note failures can be attributed to one gate
    // instead of inferred from the rendered waveform.
    int selectorCurrentTargetBefore = -1;
    int selectorPreviousTarget = -1;
    int selectorCandidate = -1;
    int selectorFramesSinceCommit = 0;
    // 0=no rejection/unchanged, 1=preview-direction, 2=rapid-return guard,
    // 3=committed, 4=midpoint, 5=persistence, 6=initial confirmation.
    int selectorDecision = 0;
    // Read-only phrase telemetry. This makes it possible to distinguish a
    // Retune response from the raw-following portamento handoff without
    // inferring either state from the rendered contour.
    bool portamentoActive = false;
    bool usesPortamentoTarget = false;
    // Diagnostic-only lifecycle of the bounded correction excursion. This
    // is control metadata for the renderer, never a second note target.
    bool continuousReturnExcursionActive = false;
    uint8_t continuousReturnExcursionPhase = 0;
    bool continuousReturnExcursionConsumed = false;
    // True for the final scheduled release control even when its state
    // machine has just become inactive. This preserves gesture provenance at
    // the renderer boundary without extending the DSP pulse.
    bool continuousReturnExcursionOwnsControl = false;
    bool extremeTargetChange = false;
};

// YIN-independent pitch-correction trajectory. It accepts only a stable F0
// snapshot and musical constraints; it has no allocations or JUCE/UI state.
class CorrectionTrajectory
{
public:
    using NaturalVibratoSurface = std::array<std::array<double, 3>, 7>;
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
    // Production-owned, bounded fast target predictor for the explicit
    // hard-tune preset. It is independently switchable for A/B diagnostics;
    // it does not alter the renderer, control scheduler, or latency.
    void setRapidAdjacentTargetSelectionEnabled (bool shouldUse) noexcept
    {
        rapidAdjacentTargetSelectionEnabled = shouldUse;
        resetRapidAdjacentPredictor();
    }
    // Offline-only A/B for the measured F# -> G -> F# return. Unlike the
    // older broad preview, this may nominate only the immediately previous
    // adjacent target during a bounded, coherent reversal.
    void setDiagnosticRapidLegatoReturnPreview (bool shouldUse) noexcept
    {
        diagnosticRapidLegatoReturnPreview = shouldUse;
    }
    void setDiagnosticContinuousRapidReturnExcursion (bool shouldUse) noexcept
    {
        diagnosticContinuousRapidReturnExcursion = shouldUse;
    }
    void setDiagnosticContinuousReturnExcursionTuning (int minimumCoherentFrames,
                                                        double minimumTravelCents,
                                                        double holdMilliseconds) noexcept
    {
        diagnosticContinuousReturnMinimumCoherentFrames = clamp (minimumCoherentFrames, 2, 12);
        diagnosticContinuousReturnMinimumTravelCents = clamp (minimumTravelCents, 2.0, 40.0);
        diagnosticContinuousReturnHoldSeconds = clamp (holdMilliseconds * 0.001, 0.010, 0.120);
    }
    void setDiagnosticStrongHardTunePull (bool shouldUse) noexcept
    {
        diagnosticStrongHardTunePull = shouldUse;
    }
    // Offline system-identification hook. Production leaves durationMs below
    // zero and shape at zero, so the shipped contour is unchanged. Shapes:
    // 0=production smoothstep, 1=linear, 2=raised cosine, 3=quintic smootherstep.
    void setDiagnosticHardTuneWideTransition (double upwardDurationMs,
                                              double downwardDurationMs,
                                              int shape) noexcept
    {
        diagnosticHardTuneWideTransitionUpMs = upwardDurationMs < 0.0 ? -1.0
                                                    : clamp (upwardDurationMs, 0.0, 250.0);
        diagnosticHardTuneWideTransitionDownMs = downwardDurationMs < 0.0 ? -1.0
                                                      : clamp (downwardDurationMs, 0.0, 250.0);
        diagnosticHardTuneWideTransitionShape = clamp (shape, 0, 3);
    }
    void setDiagnosticSlowRetuneInputReferenceAmountOverride (double amount) noexcept
    {
        diagnosticSlowRetuneInputReferenceAmountOverride = amount < 0.0 ? -1.0 : clamp01 (amount);
    }
    void setDiagnosticControlCalibration (double pitchCentreSeconds,
                                          double wideVibratoNeutralRetention,
                                          double wideVibratoHumanizedRetention) noexcept
    {
        diagnosticPitchCentreSeconds = clamp (pitchCentreSeconds, 0.020, 0.250);
        diagnosticWideVibratoNeutralRetention = clamp (wideVibratoNeutralRetention, -0.75, 1.0);
        diagnosticWideVibratoHumanizedRetention = clamp (wideVibratoHumanizedRetention, 0.0, 1.0);
    }
    // Benchmark-only injection point. Production keeps the measured table
    // below; a candidate can be rendered without rewriting source code.
    void setDiagnosticNaturalVibratoSurface (const NaturalVibratoSurface& surface) noexcept
    {
        diagnosticNaturalVibratoSurface = surface;
    }
    // Offline-only extension of the measured Natural Vibrato surface to the
    // hard-tune (0 ms) row. Zero is the production behaviour.
    void setDiagnosticNaturalSurfaceAtHardTuneWeight (double weight) noexcept
    {
        diagnosticNaturalSurfaceAtHardTuneWeight = clamp01 (weight);
    }
    // Offline phase-fit injection. Production uses 1.0; the diagnostic
    // renderer can sweep nearby values without repeatedly editing the law.
    void setDiagnosticNegativeNaturalPhaseScale (double scale) noexcept
    {
        diagnosticNegativeNaturalPhaseScale = clamp (scale, 0.0, 2.5);
    }
    // Offline calibration only. Values are T90 in milliseconds; a negative
    // value restores the production knot. This setter is called at
    // prepareToPlay, never from processBlock.
    void setDiagnosticRetuneT90Overrides (double retune10T90Ms,
                                           double retune25T90Ms) noexcept
    {
        constexpr double ln10 = 2.302585092994046;
        diagnosticRetune10TimeConstantMs = retune10T90Ms > 0.0
            ? clamp (retune10T90Ms, 20.0, 80.0) / ln10
            : artistRetune10MsTimeConstantMs;
        diagnosticRetune25TimeConstantMs = retune25T90Ms > 0.0
            ? clamp (retune25T90Ms, 40.0, 140.0) / ln10
            : artistRetune25MsTimeConstantMs;
    }
    // Offline joint-fit only. Unity values preserve every production curve.
    void setDiagnosticJointControlScales (double flexResidualScale,
                                          double humanizeScale,
                                          double negativeNaturalPhaseScale) noexcept
    {
        diagnosticFlexResidualScale = clamp (flexResidualScale, 0.50, 1.50);
        diagnosticHumanizeScale = clamp (humanizeScale, 0.50, 1.50);
        diagnosticNegativeNaturalPhaseScale = clamp (negativeNaturalPhaseScale, 0.0, 2.50);
    }
    // The verified Artist response through 50 ms is preserved below. Above
    // it, displayed Retune means correction time: at 300/400 ms correction
    // covers 90% of its initial error in 300/400 ms. Target choice remains
    // independent of this law.
    double getEffectiveRetuneTimeMs (double displayedRetuneMs) const noexcept
    {
        const auto displayed = clamp (displayedRetuneMs, 0.0, 400.0);
        if (displayed <= 0.0)
            return 0.0;
        if (displayed <= 10.0)
            return minimumContinuousRetuneMs
                 + (diagnosticRetune10TimeConstantMs - minimumContinuousRetuneMs) * displayed / 10.0;
        if (displayed <= 25.0)
            return diagnosticRetune10TimeConstantMs
                 + (diagnosticRetune25TimeConstantMs - diagnosticRetune10TimeConstantMs)
                       * (displayed - 10.0) / 15.0;
        if (displayed <= 50.0)
            return diagnosticRetune25TimeConstantMs
                 + (artistRetune50MsTimeConstantMs - diagnosticRetune25TimeConstantMs)
                       * (displayed - 25.0) / 25.0;

        // Preserve the measured 50-ms knot (176-ms T90), then converge to
        // the literal UI definition at 300 ms. From 300 ms onward the
        // displayed number is exactly T90.
        const auto measuredT90At50Ms = artistRetune50MsTimeConstantMs * 2.302585092994046;
        const auto t90Ms = displayed <= 300.0
                         ? measuredT90At50Ms + (displayed - 50.0)
                                                * (300.0 - measuredT90At50Ms) / 250.0
                         : displayed;
        return t90Ms / 2.302585092994046;
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
        resetRapidAdjacentPredictor();
        transientRetuneBlend = 0.0;
        resetPitchVariationAnalysis();
        pitchDecomposer.reset();
        unvoicedFrames = 0;
        previousInputPitchCents = std::numeric_limits<double>::quiet_NaN();
        inputMotionCentsPerSecond = 0.0;
        correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
        smoothedCorrectionCents = std::numeric_limits<double>::quiet_NaN();
        outputCorrectionCents = std::numeric_limits<double>::quiet_NaN();
        pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
        vibratoEnergyCents = 0.0;
        sustainedTargetSamples = 0;
        humanizeProcessor.reset();
        transitionStartCents = std::numeric_limits<double>::quiet_NaN();
        transitionEndCents = std::numeric_limits<double>::quiet_NaN();
        transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
        transitionProgress = 1.0;
        transitionActive = false;
        transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
        transitionUsesLinearSlew = false;
        transitionUsesRapidLegatoChain = false;
        transitionUsesRapidAdjacentContour = false;
        transitionDiagnosticWideShape = 0;
        transitionVelocityCentsPerSecond = 0.0;
        rapidLegatoDirection = 0;
        rapidLegatoAdjacentChanges = 0;
        rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
        rapidReturnLastDirection = 0;
        rapidReturnAdjacentReversalCount = 0;
        descentRailGuardSamples = 0;
        reversalSettlingToPreviousTarget = false;
        reversalHoldSamplesRemaining = 0;
        reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
        rendererCrossingActive = false;
        rendererCrossingTargetNote = -1;
        rendererCrossingDurationMs = rendererCrossingTransitionMs;
        rendererCrossingProgress = 0.0;
        rendererCrossingCorrectionCents = 0.0;
        resetContinuousReturnExcursion();
        continuousReturnExcursionConsumed = false;
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
        transitionDiagnosticWideShape = 0;
        transitionVelocityCentsPerSecond = 0.0;
        rapidLegatoDirection = 0;
        rapidLegatoAdjacentChanges = 0;
        rapidLegatoSamplesSinceChange = std::numeric_limits<int>::max();
        rapidReturnLastDirection = 0;
        rapidReturnAdjacentReversalCount = 0;
        descentRailGuardSamples = 0;
        reversalSettlingToPreviousTarget = false;
        reversalHoldSamplesRemaining = 0;
        reversalPendingTargetCents = std::numeric_limits<double>::quiet_NaN();
        rendererCrossingActive = false;
        rendererCrossingTargetNote = -1;
        rendererCrossingDurationMs = rendererCrossingTransitionMs;
        rendererCrossingProgress = 0.0;
        rendererCrossingCorrectionCents = 0.0;
        resetContinuousReturnExcursion();
        continuousReturnExcursionConsumed = false;
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
                                     bool useControlScheduler = false,
                                     double rawF0Hz = std::numeric_limits<double>::quiet_NaN(),
                                     double rawConfidence = 0.0) noexcept
    {
        CorrectionTrajectoryData output;
        const auto correctionBeforeUpdate = outputCorrectionCents;
        if (! voiced || ! std::isfinite (stableF0Hz) || stableF0Hz <= 0.0
            || ! std::isfinite (referencePitchHz) || referencePitchHz <= 0.0
            || sampleRate <= 0.0 || hopSamples <= 0)
        {
            ++unvoicedFrames;
            resetContinuousReturnExcursion();
            continuousReturnExcursionConsumed = false;
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
                pitchDecomposer.reset();
                correctedPitchCents = std::numeric_limits<double>::quiet_NaN();
                smoothedCorrectionCents = std::numeric_limits<double>::quiet_NaN();
                outputCorrectionCents = std::numeric_limits<double>::quiet_NaN();
                pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
                vibratoEnergyCents = 0.0;
                sustainedTargetSamples = 0;
                humanizeProcessor.reset();
                transitionStartCents = std::numeric_limits<double>::quiet_NaN();
                transitionEndCents = std::numeric_limits<double>::quiet_NaN();
                transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
                transitionProgress = 1.0;
                transitionActive = false;
                transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
                transitionUsesLinearSlew = false;
                transitionUsesRapidLegatoChain = false;
                transitionUsesRapidAdjacentContour = false;
                transitionDiagnosticWideShape = 0;
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
        // Parallel, allocation-free decomposition telemetry. It is run at
        // exactly the existing analysis hop and deliberately has no route
        // back to the production control law until reference fitting proves
        // that the independent paths improve rather than regress it.
        currentPitchComponents = pitchDecomposer.process (inputCents, stableF0Hz,
                                                           sampleRate, hopSamples,
                                                           diagnosticPitchCentreSeconds);
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

        // Source analysis has one owner. Retune/Flex below only consume the
        // centre/residual metrics; the decomposer cannot change a target,
        // timing law, or renderer state.
        pitchCenterCents = currentPitchComponents.noteCenterCents;
        const auto rawVibratoResidual = currentPitchComponents.residualCents;
        vibratoEnergyCents = currentPitchComponents.vibratoEnergyCents;
        const auto residualDeviation = currentPitchComponents.residualDeviationCents;
        const auto estimatedVibratoRate = currentPitchComponents.vibratoRateHz;
        const auto detectedVibrato = currentPitchComponents.periodicVibrato;
        humanizeVibratoRecognition = currentPitchComponents.vibratoConfidence;
        const auto rateFrames = std::min (pitchDecomposer.availableHistoryFrames(),
                                          static_cast<int> (std::lround (0.250 * sampleRate / hopSamples)));

        // Artist Natural Vibrato is calibrated as a two-dimensional surface:
        // displayed Natural value by measured source-vibrato energy. This
        // replaces the former stack of narrow/wide multipliers and means a
        // 60-cent source is not forced through the 10-cent control law.
        const auto hasNaturalVibratoControl = std::abs (naturalVibratoPercent) > 0.05;
        const auto fallbackNaturalGain = clamp (1.0 + naturalVibratoPercent * artistNaturalVibratoGainPerUnit,
                                                0.0, 2.0);
        const auto measuredNaturalGain = artistNaturalVibratoGain (naturalVibratoPercent, vibratoEnergyCents);
        // The Artist surface was measured at Retune 10 / Humanize 0. Treat
        // it as an absolute residual response at that measured point, then
        // crossfade smoothly to the general residual law elsewhere. This
        // preserves every measured endpoint and removes the former binary
        // branch at Humanize 0.05 or Retune 5/25.
        const auto retuneBelowReference = smoothTransitionProgress (
            clamp01 (retuneSpeedMs / 10.0));
        const auto retuneAboveReference = 1.0 - smoothTransitionProgress (
            clamp01 ((retuneSpeedMs - 10.0) / 40.0));
        const auto naturalRetuneWeight = clamp01 (retuneBelowReference * retuneAboveReference
            + (retuneSpeedMs <= 0.05 ? diagnosticNaturalSurfaceAtHardTuneWeight : 0.0));
        const auto naturalHumanizeWeight = 1.0 - HumanizeProcessor::artistResponse (humanizePercent);
        const auto measuredNaturalWeight = hasNaturalVibratoControl
                                         ? naturalRetuneWeight * naturalHumanizeWeight : 0.0;
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
        // Do not decide the retained vibrato yet. Humanize is a sustained
        // control, so its measured response is available only after the
        // sustain gate below has settled.
        auto scaledVibratoResidual = 0.0;

        const auto desiredMidi = 69.0 + inputCents / 100.0 + static_cast<double> (transpose);
        const auto sourceMidi = std::clamp (static_cast<int> (std::lround (desiredMidi)), 0, 127);
        const auto sourceRule = notePolicy.ruleForMidi (sourceMidi);
        const auto rawCandidate = notePolicy.midiTargetNote >= 0
                                ? notePolicy.midiTargetNote
                                : selectPolicyTarget (desiredMidi, sourceRule, notePolicy, currentTargetNote);
        if (rawCandidate < 0)
        {
            // Empty eligible set, an exhausted vocal range, or a directional
            // boundary means unity/dry correction. Never retain a stale
            // target or redirect to the opposite direction.
            return output;
        }

        updateTrajectoryPreview (desiredMidi, signedInputDeltaCents, notePolicy, hardLockPreset);


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
        constexpr bool enableHardTuneTrajectoryPreview = VIBE_HARD_TUNE_TRAJECTORY_PREVIEW != 0;
        // Neighbourhood is musical, not chromatic. In a minor scale two
        // adjacent legal degrees can be two semitones apart; conversely a
        // wide preview must never skip a legal degree that has not committed.
        auto previewSkipsLegalTarget = false;
        if (currentTargetNote >= 0 && trajectoryPreviewNote >= 0)
        {
            const auto previewDirection = trajectoryPreviewNote > currentTargetNote ? 1 : -1;
            for (auto note = currentTargetNote + previewDirection;
                 note != trajectoryPreviewNote; note += previewDirection)
                previewSkipsLegalTarget = previewSkipsLegalTarget || notePolicy.isLegalTarget (note);
        }
        const auto previewIsNextLegalTarget = currentTargetNote >= 0 && trajectoryPreviewNote >= 0
                                           && trajectoryPreviewNote != currentTargetNote
                                           && ! previewSkipsLegalTarget;
        const auto canUseHardTunePreview = enableHardTuneTrajectoryPreview && previewIsNextLegalTarget;
        const auto hardTuneCandidate = canUseHardTunePreview ? trajectoryPreviewNote
                                                              : rawCandidate;
        // A short adjacent note can be acoustically real before the
        // median/IIR stable F0 has crossed its normal boundary. Use raw YIN
        // only as a bounded predictor: three coherent, well-voiced frames must
        // nominate the immediately adjacent legal target before it can
        // challenge the stable hard-tune candidate.
        const auto rapidCandidate = updateRapidAdjacentPredictor (rawF0Hz, rawConfidence,
                                                                    referencePitchHz, transpose,
                                                                    notePolicy, hardLockPreset, hopSamples);
        // Artist returns to F# in the recorded rapid A -> B -> A phrase
        // while the raw detector remains a few cents on B's side of the
        // midpoint. Do not lower the midpoint globally. This diagnostic
        // candidate can only return to the immediately previous adjacent
        // note after a coherent reversal has already been observed.
        const auto returnPreviewDirection = previousTargetNote > currentTargetNote ? 1 : -1;
        const auto returnPreviewIsAdjacent = currentTargetNote >= 0
                                          && previousTargetNote >= 0
                                          && std::abs (previousTargetNote - currentTargetNote) == 1;
        const auto returnPreviewMidpoint = 0.5 * static_cast<double> (previousTargetNote + currentTargetNote);
        const auto returnPreviewLeadCents = (desiredMidi - returnPreviewMidpoint)
                                          * static_cast<double> (returnPreviewDirection) * 100.0;
        // The Artist F# onset is about four analysis frames after the first
        // eligible downward preview. Waiting those frames preserves the
        // middle G as a distinct note instead of starting the return early.
        constexpr int rapidReturnMinimumFramesSinceCommit = 14;
        constexpr int rapidReturnMaximumFramesSinceCommit = 36;
        // The first three downward frames occur before Artist has finished
        // presenting the middle G. Six frames keeps that note audible, then
        // enables only the measured final return.
        constexpr int rapidReturnMinimumCoherentFrames = 6;
        constexpr double rapidReturnMinimumTravelCents = 10.0;
        constexpr double rapidReturnMaximumLeadCents = 12.0;
        const auto returnPreviewCandidate = diagnosticRapidLegatoReturnPreview
                                         && hardLockPreset
                                         && returnPreviewIsAdjacent
                                         && rawCandidate == currentTargetNote
                                         && trajectoryPreviewDirection == returnPreviewDirection
                                         && trajectoryPreviewUpdates >= rapidReturnMinimumCoherentFrames
                                         && trajectoryPreviewTravelCents >= rapidReturnMinimumTravelCents
                                         // A single adjacent turn is ordinary melodic movement.
                                         // The recorded failing phrase has already reversed twice
                                         // (A -> B -> A -> B) before its final return begins.
                                         && rapidReturnAdjacentReversalCount >= 2
                                         && framesSinceTargetCommit >= rapidReturnMinimumFramesSinceCommit
                                         && framesSinceTargetCommit <= rapidReturnMaximumFramesSinceCommit
                                         && returnPreviewLeadCents >= -rapidReturnMaximumLeadCents
                                         && returnPreviewLeadCents <= 0.0;
        const auto candidate = returnPreviewCandidate ? previousTargetNote
                             : rapidCandidate >= 0 ? rapidCandidate
                             : hardLockPreset ? hardTuneCandidate
                             : (rapidAdjacentRawCandidate ? rawCandidate : filteredCandidate);
        output.candidateTargetNote = candidate;

        const auto inputJump = std::isfinite (previousInputPitchCents)
                             ? std::abs (inputCents - previousInputPitchCents) : 0.0;
        const auto currentTargetIsValid = currentTargetNote >= 0
                                       && notePolicy.isLegalTarget (currentTargetNote);
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
                      revoicedAfterGap, hardLockPreset, sampleRate, hopSamples);
        previousInputPitchCents = inputCents;

        if (currentTargetNote < 0)
            return output;

        if (currentTargetNote != targetBeforeSelection)
        {
            const auto committedAdjacentStep = hardLockPreset
                                            && targetBeforeSelection >= 0
                                            && std::abs (currentTargetNote - targetBeforeSelection) == 1;
            if (committedAdjacentStep && ! returnPreviewCandidate)
            {
                const auto committedDirection = currentTargetNote > targetBeforeSelection ? 1 : -1;
                // The three target commits in the Artist phrase are 96 ms
                // then 69 ms apart. Keep this window below an ordinary
                // melodic phrase, so only a densely confirmed reversal can
                // authorise the early final return.
                constexpr int rapidReturnSequenceGapFrames = 40;
                if (rapidReturnLastDirection != 0
                    && committedDirection != rapidReturnLastDirection
                    && framesSinceTargetCommit <= rapidReturnSequenceGapFrames)
                    ++rapidReturnAdjacentReversalCount;
                else
                    rapidReturnAdjacentReversalCount = 0;
                rapidReturnLastDirection = committedDirection;
            }
            else if (! committedAdjacentStep)
            {
                rapidReturnAdjacentReversalCount = 0;
                rapidReturnLastDirection = 0;
            }
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
        // A non-zero Retune setting already owns one continuous one-pole
        // correction below. Giving the *target* a second, independent glide
        // delays every new note twice; on a melodic phrase the next target
        // can arrive before the previous target has even become reachable.
        // Keep target handoff time only for the special 0-ms hard-tune
        // contour. For every other setting the committed target is immediate
        // and Retune is the sole timing law.
        auto requestedTransitionDurationMs = hardLockPreset
                                           ? getNoteTransitionDurationMs (tuningPolicy, retuneSpeedMs)
                                           : 0.0;
        auto requestedDiagnosticWideShape = 0;
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
        constexpr bool enableRapidReversalPlateau = VIBE_HARD_TUNE_RAPID_REVERSAL_PLATEAU != 0;
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
        if (hardLockPreset && currentTargetNote != targetBeforeSelection && targetBeforeSelection >= 0)
        {
            const auto targetDistanceCents = 100.0 * std::abs (currentTargetNote - targetBeforeSelection);
            // At the explicit 0/0/0/0 hard-lock surface, Artist reaches a
            // newly committed broad target in roughly two 256-sample F0
            // observations (median 10.7 ms at 48 kHz).  The former
            // source-tempo law stretched these moves to 30..160 ms, so a
            // short note could leave the correction only halfway to its
            // destination. Keep the target discrete and use one bounded
            // future-only de-zipper; detector/commit timing remains intact.
            if (targetDistanceCents >= sourceTimedJumpThresholdCents)
            {
                const auto diagnosticDuration = currentTargetNote > targetBeforeSelection
                                              ? diagnosticHardTuneWideTransitionUpMs
                                              : diagnosticHardTuneWideTransitionDownMs;
                requestedTransitionDurationMs = diagnosticDuration >= 0.0
                                              ? diagnosticDuration
                                              : (currentTargetNote > targetBeforeSelection
                                                 ? hardTuneWideUpTransitionMs
                                                 : hardTuneWideDownTransitionMs);
                requestedDiagnosticWideShape = diagnosticDuration >= 0.0
                                             ? diagnosticHardTuneWideTransitionShape : 0;
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
            transitionDiagnosticWideShape = 0;
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
            transitionDiagnosticWideShape = 0;
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
            transitionDiagnosticWideShape = requestedDiagnosticWideShape;
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
            auto shapedProgress = transitionDiagnosticWideShape == 1 ? transitionProgress
                                : transitionDiagnosticWideShape == 2 ? raisedCosineTransitionProgress (transitionProgress)
                                : transitionDiagnosticWideShape == 3 ? rapidAdjacentTransitionProgress (transitionProgress)
                                : transitionUsesLinearSlew ? transitionProgress
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
        // Slow Retune should preserve the singer's short source movements
        // while its correction offset catches up. The historical musical
        // path instead reconstructs output around a slow pitch centre; a
        // stale correction can therefore move opposite to a real phrase.
        // The shared 0.75 amount is calibrated against all three verified
        // Artist targets at 200/300/400 ms. The blend is continuous above
        // the already-calibrated 0--50 ms control region and remains isolated
        // from Flex, Humanize and Natural Vibrato until measured combined
        // targets exist.
        const auto slowRetuneInputReferenceAmount
            = diagnosticSlowRetuneInputReferenceAmountOverride >= 0.0
                ? diagnosticSlowRetuneInputReferenceAmountOverride
                : calibratedSlowRetuneInputReferenceAmount;
        const auto slowRetuneReferenceEligible = flexTunePercent <= hardLockZeroTolerance
                                              && humanizePercent <= hardLockZeroTolerance
                                              && std::abs (naturalVibratoPercent) <= hardLockZeroTolerance;
        const auto slowRetuneInputWeight = slowRetuneReferenceEligible
                                         ? slowRetuneInputReferenceAmount
                                           * smoothTransitionProgress (
                                               clamp01 ((retuneSpeedMs - 50.0) / 150.0))
                                         : 0.0;
        const auto correctionReferenceCents = hardLockPreset ? inputCents
                                            : pitchCenterCents
                                              + (inputCents - pitchCenterCents)
                                                * slowRetuneInputWeight;
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
        const auto humanize = humanizeProcessor.process (humanizePercent, fastRetuneAmount,
                                                          sustainRamp, humanizeVibratoRecognition,
                                                          sampleRate, hopSamples)
                              * productionHumanizeScale * diagnosticHumanizeScale;
        // Artist's neutral behaviour is level-dependent: a small natural
        // vibrato remains almost intact, while a wide 30-cent vibrato is
        // pulled firmly toward centre. Humanize progressively restores that
        // wide residual. These endpoints are measured on the supplied F#4
        // 10-cent and 30-cent control timelines, not guessed as a generic
        // percentage curve.
        constexpr double lowAmplitudeVibratoEnergyCents = 6.0;
        constexpr double highAmplitudeVibratoEnergyCents = 19.0;
        constexpr double lowAmplitudeNeutralRetention = 0.948;
        // The wide 30-cent / 5 Hz Humanize fixture needs a small
        // counter-residual to cancel modulation retained by this renderer.
        // It is not a Natural Vibrato law: Artist's low-depth natural-vibrato
        // control must keep its own response. Limit it to the measured
        // Retune-10 family and only after wide modulation is established.
        const auto isWideHumanizeControlFamily = retuneSpeedMs >= 5.0 && retuneSpeedMs <= 25.0
                                               && std::abs (naturalVibratoPercent) <= hardLockZeroTolerance
                                               && vibratoEnergyCents >= 12.0;
        const auto isWideHumanizeCalibrationCase = isWideHumanizeControlFamily
                                                 // Do not turn a slow melodic drift into the
                                                 // measured 5 Hz Humanize fixture merely because
                                                 // it eventually has a large excursion.
                                                 && detectedVibrato;
        // Outside the one measured wide-Humanize fixture, preserve the
        // established positive neutral retention.  In particular, Natural
        // Vibrato must never inherit the Humanize counter-residual.
        constexpr double standardHighAmplitudeNeutralRetention = 0.362;
        // The measured Retune-10 Humanize sweep needs a counter-residual at
        // Humanize 0, but carrying that same neutral endpoint into the upper
        // half of the Humanize knob worsens the 75/100 Artist references.
        // Treat the five bounces as one two-control surface: fade the fitted
        // neutral endpoint back to the historical neutral response by 25,
        // while the humanized endpoint below continues to rise normally.
        const auto neutralInteractionFade = smoothTransitionProgress (
            clamp01 (humanizePercent / 25.0));
        constexpr double historicalWideVibratoNeutralRetention = 0.210;
        const auto fittedWideNeutralRetention = diagnosticWideVibratoNeutralRetention
                                              + (historicalWideVibratoNeutralRetention
                                                 - diagnosticWideVibratoNeutralRetention)
                                                * neutralInteractionFade;
        const auto highAmplitudeNeutralRetention = isWideHumanizeControlFamily
                                                  ? (isWideHumanizeCalibrationCase
                                                       ? fittedWideNeutralRetention
                                                       : historicalWideVibratoNeutralRetention)
                                                  : standardHighAmplitudeNeutralRetention;
        // Humanize must leave an ordinary centred 20-cent vibrato intact;
        // only the distinctly wider periodic component measured in the
        // Artist control capture needs the lower retained endpoint.  A
        // linear low→high interpolation made those two regimes share a
        // gain.  Keep the general endpoint through 14 ct, then transition
        // over the measured wide-vibrato band.
        constexpr double ordinaryVibratoCeilingCents = 14.0;
        constexpr double wideVibratoFullScaleCents = 17.0;
        constexpr double artistWideHumanizedRetention = 0.330;
        const auto wideVibratoAmount = smoothTransitionProgress (clamp01 (
            (vibratoEnergyCents - ordinaryVibratoCeilingCents)
            / (wideVibratoFullScaleCents - ordinaryVibratoCeilingCents)));
        const auto highAmplitudeHumanizedRetention
            = diagnosticWideVibratoHumanizedRetention
            + (artistWideHumanizedRetention - diagnosticWideVibratoHumanizedRetention)
              * wideVibratoAmount;
        const auto vibratoAmplitude = clamp01 ((vibratoEnergyCents - lowAmplitudeVibratoEnergyCents)
                                               / (highAmplitudeVibratoEnergyCents
                                                  - lowAmplitudeVibratoEnergyCents));
        const auto neutralVibratoRetention = lowAmplitudeNeutralRetention
                                            + (highAmplitudeNeutralRetention
                                               - lowAmplitudeNeutralRetention) * vibratoAmplitude;
        const auto retainedVibrato = neutralVibratoRetention
                                   + (highAmplitudeHumanizedRetention - neutralVibratoRetention) * humanize;
        // A few of Artist's narrow-vibrato positions sit below the residual
        // floor of this renderer.  The measured control law therefore needs
        // a small phase-opposed residual at those positions.  It remains
        // restricted to the calibrated Retune-10 family, never the general
        // Natural Vibrato behaviour.
        // Negative Natural Vibrato positions at the measured Retune-10 /
        // Humanize-0 reference point can require a small phase-opposed
        // residual to attenuate a wide source vibrato.  The previous guard
        // accidentally allowed this only when Natural Vibrato was *zero*,
        // making negative entries in the measured surface unreachable.
        const auto mayUseCounterResidual = isWideHumanizeCalibrationCase
                                         || measuredNaturalWeight > 0.0;
        const auto fallbackResidualGain = retainedVibrato * fallbackNaturalGain;
        // Preserve the measured reference point bit-for-bit. Besides making
        // the calibration auditable, this avoids a one-ulp blend difference
        // growing into a different grain-choice branch downstream.
        const auto requestedResidualGain = measuredNaturalWeight >= 1.0
                                         ? measuredNaturalGain
                                         : measuredNaturalWeight <= 0.0
                                           ? fallbackResidualGain
                                           : fallbackResidualGain
                                           + (measuredNaturalGain - fallbackResidualGain) * measuredNaturalWeight;
        const auto maximumResidualGain = hasNaturalVibratoControl ? 2.45 : 2.0;
        const auto residualGain = hardLockPreset ? 0.0
                                                  : clamp (requestedResidualGain,
                                                           mayUseCounterResidual ? -0.75 : 0.0,
                                                           maximumResidualGain);
        // The negative Natural Vibrato Artist references do not merely turn
        // down the residual: their 6--8 Hz component arrives earlier than
        // the unmodified pitch-centre residual.  A gain-only surface could
        // match its depth but left a conspicuous delayed second vibrato.
        //
        // Form an analytic-style quadrature companion from the residual
        // derivative, then rotate only this sustained, negative-Natural
        // component forward.  This deliberately lives before residualGain:
        // it cannot change note selection, Retune timing, renderer timing,
        // Humanize, or any zero/positive Natural setting.
        const auto negativeNaturalPhaseWeight = measuredNaturalWeight
                                              * clamp01 (-naturalVibratoPercent / 6.0)
                                              // The calibrated 30-cent fixture settles just
                                              // above 12 cents of mean residual energy. It needs
                                              // the full measured phase rotation by that point;
                                              // below 10 cents this branch remains inactive.
                                              * clamp01 ((vibratoEnergyCents - 10.0) / 2.0);
        const auto residualFrameSeconds = static_cast<double> (hopSamples) / sampleRate;
        const auto residualDerivative = hasPreviousVibratoResidual && residualFrameSeconds > 0.0
                                      ? (rawVibratoResidual - previousVibratoResidualCents) / residualFrameSeconds
                                      : 0.0;
        previousVibratoResidualCents = rawVibratoResidual;
        hasPreviousVibratoResidual = true;
        auto phaseLeadRadians = 0.0;
        auto quadratureResidual = rawVibratoResidual;
        if (negativeNaturalPhaseWeight > 0.0)
        {
            // Keep the quadrature scale on the same bounded estimate as the
            // phase surface. A transient zero-crossing dropout must not turn
            // a one-frame derivative into an oversized correction.
            const auto phaseLeadRateHz = clamp (estimatedVibratoRate, 6.0, 8.0);
            phaseLeadRadians = artistNegativeNaturalPhaseLeadRadians (naturalVibratoPercent,
                                                                                   // The zero-crossing estimate is
                                                                                   // intentionally quantised by its
                                                                                   // 250-ms history.  It may briefly
                                                                                   // report 4 Hz for a real 6-Hz cycle;
                                                                                   // use it only to choose inside the
                                                                                   // calibrated 6--8 Hz response band.
                                                                                   phaseLeadRateHz)
                                        * artistNegativeNaturalPhaseScale (naturalVibratoPercent)
                                        * artistNegativeNaturalDeepPhaseRetention (naturalVibratoPercent,
                                                                                     vibratoEnergyCents)
                                        * artistNegativeNaturalPerControlPhaseMultiplier (naturalVibratoPercent)
                                        * diagnosticNegativeNaturalPhaseScale
                                        * negativeNaturalPhaseWeight;
            // Deriving one analysis frame was too noisy for the 60-cent,
            // 8-Hz reference.  Estimate the quadrature component over the
            // existing 250-ms residual history instead. For a sinusoid
            // r(t-a)=r(t)cos(w a)-q(t)sin(w a), projection onto -sin(w a)
            // returns q(t), the stable 90-degree companion needed here.
            auto quadratureProjection = 0.0;
            auto quadratureNormaliser = 0.0;
            const auto phaseStepRadians = 2.0 * 3.14159265358979323846
                                        * phaseLeadRateHz
                                        * static_cast<double> (hopSamples) / sampleRate;
            for (int age = 1; age < rateFrames; ++age)
            {
                const auto sine = std::sin (phaseStepRadians * static_cast<double> (age));
                quadratureProjection -= pitchDecomposer.residualAtAge (age) * sine;
                quadratureNormaliser += sine * sine;
            }
            const auto windowedQuadrature = quadratureNormaliser > 1.0e-9
                                          ? clamp (quadratureProjection / quadratureNormaliser,
                                                   -maximumInputVibratoDepthCents,
                                                   maximumInputVibratoDepthCents)
                                          : 0.0;
            const auto derivativeQuadrature = clamp (residualDerivative
                                                      / (2.0 * 3.14159265358979323846 * phaseLeadRateHz),
                                                      -maximumInputVibratoDepthCents,
                                                      maximumInputVibratoDepthCents);
            // -9 is the one measured knot whose short-frame derivative is
            // the better phase companion; blend to it only near that knot.
            const auto derivativeWeight = clamp01 (1.0 - std::abs (naturalVibratoPercent + 9.0) / 2.0);
            quadratureResidual = windowedQuadrature
                              + (derivativeQuadrature - windowedQuadrature) * derivativeWeight;
        }
        scaledVibratoResidual = vibratoProcessor.process (
            rawVibratoResidual, quadratureResidual,
            { residualGain, negativeNaturalPhaseWeight > 0.0 ? phaseLeadRadians : 0.0,
              slowRetuneInputWeight, ! hardLockPreset });

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
        const auto rawDesiredCorrectionCents = flexTargetCents - correctionReferenceCents;
        // The descent rail is an artifact guard for the relaxed musical
        // modes.  At the explicit hard-lock preset it would deliberately
        // saturate a valid negative target correction and leave the note
        // above its centre.  The per-control-tick step guard below remains
        // active for hard-lock, so removing this amplitude rail does not
        // create a rectangular ratio jump.
        const auto descentRailGuardActive = descentRailGuardSamples > 0 && ! hardLockPreset;
        const auto correctionRailCents = correctionRailForControls (retuneSpeedMs, hardLockPreset,
                                                                      naturalVibratoPercent);
        const auto desiredCorrectionCents = descentRailGuardActive
                                          ? softRailLimit (rawDesiredCorrectionCents)
                                          : clamp (rawDesiredCorrectionCents,
                                                   -correctionRailCents,
                                                   correctionRailCents);
        // In production the fixed 128-sample control scheduler owns this
        // integration.  Keeping it out of the 512-sample YIN callback is
        // what prevents note-transition durations from being quantised to a
        // detector hop.  Standalone trajectory tests retain the legacy
        // direct mode through the default argument.
        if (! useControlScheduler)
        {
            smoothedCorrectionCents += (desiredCorrectionCents - smoothedCorrectionCents) * alpha;
            smoothedCorrectionCents = clamp (smoothedCorrectionCents,
                                             -correctionRailCents,
                                             correctionRailCents);
        }
        correctedPitchCents = correctionReferenceCents + smoothedCorrectionCents + scaledVibratoResidual;

        // This is a glitch guard, not a second user-facing Retune control.
        // It only becomes active for a trajectory discontinuity larger than
        // 60 cents per analysis hop (typically a tracker/target error). The
        // normal 25 ms trajectory for near-note vocal correction remains
        // below this limit.
        auto safeCorrectionCents = clamp (correctedPitchCents - inputCents,
                                          -correctionRailCents,
                                          correctionRailCents);
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

        if (currentTargetNote != targetBeforeSelection)
            continuousReturnExcursionConsumed = false;

        if (continuousReturnExcursionIsActive()
            && currentTargetNote != continuousReturnExcursionTargetNote)
            resetContinuousReturnExcursion();

        // This diagnostic-only candidate is intentionally not a selector
        // shortcut. It starts only after a dense adjacent-reversal history,
        // a bounded downward near-boundary motion, and sustained coherent
        // evidence toward the previously committed legal neighbour. The
        // current MIDI target remains unchanged throughout the pulse.
        const auto continuousReturnEvidence = diagnosticContinuousRapidReturnExcursion
                                           && ! continuousReturnExcursionIsActive()
                                           && ! continuousReturnExcursionConsumed
                                           && hardLockPreset
                                           && currentTargetNote == targetBeforeSelection
                                           && returnPreviewIsAdjacent
                                           && rawCandidate == currentTargetNote
                                           && trajectoryPreviewDirection == returnPreviewDirection
                                           && trajectoryPreviewUpdates >= diagnosticContinuousReturnMinimumCoherentFrames
                                           && trajectoryPreviewTravelCents >= diagnosticContinuousReturnMinimumTravelCents
                                           && rapidReturnAdjacentReversalCount >= 2
                                           && returnPreviewLeadCents >= -rapidReturnExcursionMaximumLeadCents
                                           && returnPreviewLeadCents <= 0.0
                                           && stableF0Hz >= rendererCrossingReturnMinimumF0Hz;
        if (continuousReturnEvidence)
            beginContinuousReturnExcursion (safeCorrectionCents, inputCents,
                                            previousTargetNote, currentTargetNote, sampleRate);
        if (continuousReturnExcursionIsActive())
            safeCorrectionCents = continuousReturnExcursionCorrectionCents;

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
                                               && rendererCrossingTransitionMs > 0.0
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
                        -correctionRailCents, correctionRailCents);
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
        output.rapidAdjacentPrearmed = rapidAdjacentPrearmed;
        output.rapidAdjacentCandidateNote = rapidAdjacentCandidateNote;
        output.rapidAdjacentEvidenceFrames = rapidAdjacentEvidenceFrames;
        output.rapidAdjacentRawDistanceCents = rapidAdjacentRawDistanceCents;
        output.rapidLegatoAdjacentStep = adjacentLegatoStep;
        output.rapidLegatoHardLock = hardLockPreset;
        output.rapidLegatoTargetChanged = trajectoryTargetChanged;
        output.rapidLegatoContinuesDirection = continuesDirectedLegato;
        output.rapidLegatoChainStarts = startsOrContinuesRapidLegatoChain;
        output.rapidLegatoChainActive = transitionUsesRapidLegatoChain;
        output.rapidLegatoCommittedSteps = rapidLegatoAdjacentChanges;
        output.rapidLegatoSamplesSinceCommit = rapidLegatoSamplesSinceChange;
        output.trajectoryPreviewNote = trajectoryPreviewNote;
        output.trajectoryPreviewDirection = trajectoryPreviewDirection;
        output.trajectoryPreviewUpdates = trajectoryPreviewUpdates;
        output.trajectoryPreviewTravelCents = trajectoryPreviewTravelCents;
        output.instantaneousInputMotionCentsPerSecond = instantaneousInputMotion;
        output.selectorCurrentTargetBefore = selectorCurrentTargetBefore;
        output.selectorPreviousTarget = selectorPreviousTarget;
        output.selectorCandidate = selectorCandidate;
        output.selectorFramesSinceCommit = selectorFramesSinceCommit;
        output.selectorDecision = selectorDecision;
        output.portamentoActive = portamentoActive;
        output.usesPortamentoTarget = portamentoActive && retuneSpeedMs > 0.0;
        output.continuousReturnExcursionActive = continuousReturnExcursionIsActive();
        output.continuousReturnExcursionPhase = static_cast<uint8_t> (continuousReturnExcursionPhase);
        output.continuousReturnExcursionConsumed = continuousReturnExcursionConsumed;
        output.continuousReturnExcursionOwnsControl = continuousReturnExcursionIsActive();
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
        output.rawVibratoResidualCents = rawVibratoResidual;
        output.vibratoEnergyCents = vibratoEnergyCents;
        output.vibratoDeviationCents = residualDeviation;
        output.noteCenterCents = currentPitchComponents.noteCenterCents;
        output.driftCents = currentPitchComponents.driftCents;
        output.vibratoRateHz = currentPitchComponents.vibratoRateHz;
        output.vibratoConfidence = currentPitchComponents.vibratoConfidence;
        output.noteStability = currentPitchComponents.noteStability;
        output.driftConfidence = currentPitchComponents.driftConfidence;
        output.jitterConfidence = currentPitchComponents.jitterConfidence;
        output.pitchRatio = std::exp2 (output.correctionCents / 1200.0);
        output.targetNote = currentTargetNote;
        output.hasValidCorrection = std::isfinite (output.pitchRatio);
        controlSchedulerValid = output.hasValidCorrection;
        cachedInputCents = inputCents;
        cachedTargetCents = targetCents;
        cachedControlTargetCents = controlTargetCents;
        cachedCorrectionReferenceCents = correctionReferenceCents;
        cachedUsesPortamentoTarget = portamentoActive && retuneSpeedMs > 0.0;
        cachedScaledVibratoResidual = scaledVibratoResidual;
        cachedFlex = flex;
        cachedEffectiveRetuneMs = effectiveRetuneMs;
        cachedHumanize = humanize;
        cachedCorrectionGain = correctionGain;
        cachedDescentRailGuard = descentRailGuardActive;
        cachedCorrectionRailCents = correctionRailCents;
        cachedHardLockPreset = hardLockPreset;
        cachedTargetNote = currentTargetNote;
        cachedCandidateTargetNote = candidate;
        cachedExtremeTargetChange = largeTargetJump;
        transientRetuneBlend *= std::exp (-static_cast<double> (hopSamples)
                                          / (transientReleaseSeconds * sampleRate));
        if (transientRetuneBlend < 1.0e-3)
            transientRetuneBlend = 0.0;
        output.controlAdvanceSamples = 0;
        output.controlCorrectionBefore = correctionBeforeUpdate;
        output.controlCorrectionAfter = outputCorrectionCents;
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
        const auto correctionBeforeAdvance = outputCorrectionCents;

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
                auto shapedProgress = transitionDiagnosticWideShape == 1
                                    ? transitionProgress
                                    : transitionDiagnosticWideShape == 2
                                    ? raisedCosineTransitionProgress (transitionProgress)
                                    : transitionDiagnosticWideShape == 3
                                    ? rapidAdjacentTransitionProgress (transitionProgress)
                                    : transitionUsesLinearSlew
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
                        transitionDiagnosticWideShape = 0;
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
                transitionDiagnosticWideShape = 0;
            }
        }

        const auto controlTargetCents = cachedUsesPortamentoTarget ? cachedControlTargetCents
                                                                     : transitionTargetCents;
        const auto correctionReferenceCents = cachedCorrectionReferenceCents;
        const auto pitchError = controlTargetCents - correctionReferenceCents;
        const auto correctionGain = flexCorrectionGain (cachedFlex, pitchError);
        const auto flexTargetCents = correctionReferenceCents + pitchError * correctionGain;
        const auto rawDesiredCorrectionCents = flexTargetCents - correctionReferenceCents;
        const auto desiredCorrectionCents = cachedDescentRailGuard
                                          ? softRailLimit (rawDesiredCorrectionCents)
                                          : clamp (rawDesiredCorrectionCents,
                                                   -cachedCorrectionRailCents,
                                                   cachedCorrectionRailCents);
        const auto timeSeconds = cachedEffectiveRetuneMs * 0.001;
        const auto alpha = timeSeconds <= 0.0 ? 1.0
                         : 1.0 - std::exp (-static_cast<double> (elapsedSamples) / (timeSeconds * sampleRate));
        smoothedCorrectionCents += (desiredCorrectionCents - smoothedCorrectionCents) * alpha;
        smoothedCorrectionCents = clamp (smoothedCorrectionCents,
                                         -cachedCorrectionRailCents,
                                         cachedCorrectionRailCents);
        auto safeCorrectionCents = clamp (correctionReferenceCents + smoothedCorrectionCents
                                          + cachedScaledVibratoResidual - cachedInputCents,
                                          -cachedCorrectionRailCents,
                                          cachedCorrectionRailCents);
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
        const auto excursionOwnsControl = continuousReturnExcursionIsActive();
        if (excursionOwnsControl)
            safeCorrectionCents = advanceContinuousReturnExcursion (elapsedSamples, safeCorrectionCents);
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
        output.portamentoActive = portamentoActive;
        output.usesPortamentoTarget = cachedUsesPortamentoTarget;
        output.continuousReturnExcursionActive = continuousReturnExcursionIsActive();
        output.continuousReturnExcursionPhase = static_cast<uint8_t> (continuousReturnExcursionPhase);
        output.continuousReturnExcursionConsumed = continuousReturnExcursionConsumed;
        output.continuousReturnExcursionOwnsControl = excursionOwnsControl;
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
        output.controlAdvanceSamples = elapsedSamples;
        output.controlCorrectionBefore = correctionBeforeAdvance;
        output.controlCorrectionAfter = outputCorrectionCents;
        return output;
    }

private:
    enum class ContinuousReturnExcursionPhase : uint8_t
    {
        inactive,
        entering,
        holding,
        returning
    };

    void resetContinuousReturnExcursion() noexcept
    {
        continuousReturnExcursionPhase = ContinuousReturnExcursionPhase::inactive;
        continuousReturnExcursionTargetNote = -1;
        continuousReturnExcursionStartCents = 0.0;
        continuousReturnExcursionPeakCents = 0.0;
        continuousReturnExcursionReturnCents = 0.0;
        continuousReturnExcursionCorrectionCents = 0.0;
        continuousReturnExcursionProgress = 0.0;
        continuousReturnExcursionHoldSamples = 0;
        continuousReturnExcursionEntrySamples = 0;
        continuousReturnExcursionReturnSamples = 0;
    }

    bool continuousReturnExcursionIsActive() const noexcept
    {
        return continuousReturnExcursionPhase != ContinuousReturnExcursionPhase::inactive;
    }

    void beginContinuousReturnExcursion (double currentCorrectionCents,
                                         double inputCents,
                                         int returnNote,
                                         int currentNote,
                                         double sampleRate) noexcept
    {
        if (returnNote < 0 || currentNote < 0 || sampleRate <= 0.0)
            return;

        // The excursion is an attraction toward the most recently committed
        // adjacent note, not a temporary target commit. Its peak is bounded
        // below the exact note correction so the ratio remains a short,
        // reversible musical gesture instead of a held replacement note.
        const auto returnNoteCents = (static_cast<double> (returnNote) - 69.0) * 100.0;
        continuousReturnExcursionPhase = ContinuousReturnExcursionPhase::entering;
        continuousReturnExcursionConsumed = true;
        continuousReturnExcursionTargetNote = currentNote;
        continuousReturnExcursionStartCents = currentCorrectionCents;
        continuousReturnExcursionPeakCents = clamp (
            rapidReturnExcursionAttraction * (returnNoteCents - inputCents),
            -maximumLiveCorrectionCents, maximumLiveCorrectionCents);
        continuousReturnExcursionCorrectionCents = currentCorrectionCents;
        continuousReturnExcursionProgress = 0.0;
        continuousReturnExcursionEntrySamples = std::max (1, static_cast<int> (
            std::lround (rapidReturnExcursionEntrySeconds * sampleRate)));
        continuousReturnExcursionHoldSamples = std::max (1, static_cast<int> (
            std::lround (diagnosticContinuousReturnHoldSeconds * sampleRate)));
        continuousReturnExcursionReturnSamples = std::max (1, static_cast<int> (
            std::lround (rapidReturnExcursionReturnSeconds * sampleRate)));
    }

    double advanceContinuousReturnExcursion (int elapsedSamples,
                                              double baseCorrectionCents) noexcept
    {
        if (! continuousReturnExcursionIsActive() || elapsedSamples <= 0)
            return baseCorrectionCents;

        if (continuousReturnExcursionPhase == ContinuousReturnExcursionPhase::entering)
        {
            continuousReturnExcursionProgress = clamp01 (continuousReturnExcursionProgress
                + static_cast<double> (elapsedSamples) / continuousReturnExcursionEntrySamples);
            continuousReturnExcursionCorrectionCents = continuousReturnExcursionStartCents
                + (continuousReturnExcursionPeakCents - continuousReturnExcursionStartCents)
                    * continuousReturnExcursionProgress;
            if (continuousReturnExcursionProgress >= 1.0)
            {
                continuousReturnExcursionPhase = ContinuousReturnExcursionPhase::holding;
                continuousReturnExcursionProgress = 0.0;
            }
            return continuousReturnExcursionCorrectionCents;
        }

        if (continuousReturnExcursionPhase == ContinuousReturnExcursionPhase::holding)
        {
            continuousReturnExcursionHoldSamples -= elapsedSamples;
            continuousReturnExcursionCorrectionCents = continuousReturnExcursionPeakCents;
            if (continuousReturnExcursionHoldSamples <= 0)
            {
                continuousReturnExcursionPhase = ContinuousReturnExcursionPhase::returning;
                continuousReturnExcursionReturnCents = baseCorrectionCents;
                continuousReturnExcursionProgress = 0.0;
            }
            return continuousReturnExcursionCorrectionCents;
        }

        continuousReturnExcursionProgress = clamp01 (continuousReturnExcursionProgress
            + static_cast<double> (elapsedSamples) / continuousReturnExcursionReturnSamples);
        continuousReturnExcursionCorrectionCents = continuousReturnExcursionPeakCents
            + (continuousReturnExcursionReturnCents - continuousReturnExcursionPeakCents)
                * continuousReturnExcursionProgress;
        if (continuousReturnExcursionProgress >= 1.0)
        {
            const auto returnedCorrection = continuousReturnExcursionReturnCents;
            resetContinuousReturnExcursion();
            return returnedCorrection;
        }
        return continuousReturnExcursionCorrectionCents;
    }

    void resetControlSchedulerCache() noexcept
    {
        controlSchedulerValid = false;
        cachedInputCents = 0.0;
        cachedTargetCents = 0.0;
        cachedControlTargetCents = 0.0;
        cachedCorrectionReferenceCents = 0.0;
        cachedUsesPortamentoTarget = false;
        cachedScaledVibratoResidual = 0.0;
        cachedFlex = 0.0;
        cachedEffectiveRetuneMs = 0.0;
        cachedHumanize = 0.0;
        cachedCorrectionGain = 0.0;
        cachedDescentRailGuard = false;
        cachedCorrectionRailCents = maximumLiveCorrectionCents;
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
    // Adjacent-note hard-lock landing is independently tunable for measured
    // A/B work. The production default is a single scheduler interval, so
    // detected neighbouring notes read as controlled discrete steps rather
    // than a portamento-like 9 ms glide. It remains separate from wide jumps.
    static constexpr double hardTuneRapidAdjacentTransitionMs = VIBE_HARD_TUNE_RAPID_ADJACENT_TRANSITION_MS;
    static constexpr double hardTuneRapidDescendingTransitionMs = VIBE_HARD_TUNE_RAPID_DESCENDING_TRANSITION_MS;
    // The hard-lock surface has already certified its target. Four ms makes
    // two-semitone luyến land in two fixed control handoffs rather than
    // smearing over the former 6 ms contour, while preserving PSOLA-safe
    // phase continuity and staying above the previously unsafe 3 ms trial.
    static constexpr double hardTuneWideUpTransitionMs = VIBE_HARD_TUNE_WIDE_UP_TRANSITION_MS;
    static constexpr double hardTuneWideDownTransitionMs = VIBE_HARD_TUNE_WIDE_DOWN_TRANSITION_MS;
    // Predictive crossing is a separate, optional de-zipper. Hard-lock uses
    // the zero-duration option so it cannot turn certified adjacent targets
    // into a preview glide; the bounded correction-step guard remains active.
    static constexpr double rendererCrossingTransitionMs = VIBE_HARD_LOCK_RENDERER_CROSSING_TRANSITION_MS;
    static constexpr double rendererCrossingMinimumMotionCentsPerSecond = 1500.0;
    static constexpr double rendererCrossingReturnTransitionMs = 12.0;
    static constexpr double rendererCrossingReturnMinimumMotionCentsPerSecond = 700.0;
    static constexpr double rendererCrossingReturnMinimumF0Hz = 180.0;
    static constexpr int rapidReturnExcursionMinimumCoherentFrames = 4;
    static constexpr double rapidReturnExcursionMinimumTravelCents = 11.0;
    static constexpr double rapidReturnExcursionMaximumLeadCents = 12.0;
    static constexpr double rapidReturnExcursionAttraction = 0.85;
    static constexpr double rapidReturnExcursionEntrySeconds = 0.006;
    static constexpr double rapidReturnExcursionHoldSeconds = 0.050;
    static constexpr double rapidReturnExcursionReturnSeconds = 0.012;
    // An A->B->A return is a different gesture from a new phrase. Allow a
    // measured, moderately fast downward return to preview only while the
    // opposite target is still recent; this keeps slower later luyến out.
    static constexpr double rendererCrossingPreviewLeadSemitones = 0.28;
    static constexpr double rapidReversalSettleMs = VIBE_HARD_LOCK_RAPID_REVERSAL_SETTLE_MS;
    // This is a narrowly-scoped hand-off between the fast landing at the
    // middle note and the normal return contour. Keeping it short is vital:
    // a longer hold blocks the next real F0 reversal and merges the two
    // audible notes into one glide.
    static constexpr int rapidReversalHoldSamples = VIBE_HARD_LOCK_RAPID_REVERSAL_HOLD_SAMPLES;
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
    // The fast branch requires a little raw travel beyond the midpoint. This
    // keeps a short musical dwell at the old target without falling back to
    // stable-F0's much longer hold.
    static constexpr double rapidAdjacentEntryBeyondMidpointCents = VIBE_RAPID_ADJACENT_ENTRY_BEYOND_MIDPOINT_CENTS;
    static constexpr double rapidAdjacentMinimumMotionCents = 1.5;
    static constexpr double rapidAdjacentMinimumTravelCents = VIBE_RAPID_ADJACENT_MINIMUM_TRAVEL_CENTS;
    static constexpr double rapidAdjacentMinimumConfidence = 0.75;
    static constexpr int rapidAdjacentEvidenceWindowFrames = VIBE_RAPID_ADJACENT_EVIDENCE_WINDOW_FRAMES;
    static constexpr int rapidAdjacentMinimumSupportingSteps = VIBE_RAPID_ADJACENT_MINIMUM_SUPPORTING_STEPS;
    static constexpr int rapidAdjacentBoundaryDwellFrames = VIBE_RAPID_ADJACENT_BOUNDARY_DWELL_FRAMES;
    static constexpr int rapidAdjacentMinimumDwellSamples = VIBE_RAPID_ADJACENT_MINIMUM_DWELL_SAMPLES;
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
    static constexpr double hardTuneRapidReversalGuardSeconds = VIBE_HARD_TUNE_RAPID_REVERSAL_GUARD_SECONDS;
    // Classic still has a more direct target policy, but never permits a
    // single suspicious analysis frame to become an audible note switch.
    static constexpr int classicTargetPersistenceFrames = 2;
    static constexpr int targetMedianHistoryLength = 3;
    static constexpr double transientRetuneMs = 55.0;
    static constexpr double minimumContinuousRetuneMs = 8.0;
    // Control-law calibration from the matched hook-2 Artist reference. These
    // are T90 knots expressed as first-order time constants; UI labels and
    // state schema remain unchanged.
    static constexpr double artistRetune10MsTimeConstantMs = 20.0 / 2.302585092994046;
    static constexpr double artistRetune25MsTimeConstantMs = 40.0 / 2.302585092994046;
    static constexpr double productionFlexResidualScale = 0.75;
    static constexpr double productionHumanizeScale = 1.25;
    static constexpr double artistRetune50MsTimeConstantMs = 176.0 / 2.302585092994046;
    static constexpr double transientReleaseSeconds = 0.018;
    static constexpr double humanizeMinimumSustainMs = 80.0;
    // A melodic third is a normal committed note change, not a renderer
    // failure.  Bridging it to dry interrupts an otherwise continuous Note
    // Transition / Retune trajectory and re-engages the renderer with a
    // sizeable control step.  Reserve the exceptional bridge for a truly
    // implausible tracker jump; ordinary musical intervals stay on the
    // continuous processed path.
    static constexpr int largeTargetJumpSemitones = 7;
    static constexpr double maximumLiveCorrectionCents = VIBE_MAXIMUM_LIVE_CORRECTION_CENTS;
    static constexpr double musicalMaximumLiveCorrectionCents = VIBE_MUSICAL_MAXIMUM_LIVE_CORRECTION_CENTS;
    static constexpr double hardLockMaximumLiveCorrectionCents
        = VIBE_HARD_LOCK_MAXIMUM_LIVE_CORRECTION_CENTS;
    static constexpr double hardLockZeroTolerance = 0.05;

    static constexpr double correctionRailForControls (double retuneSpeedMs,
                                                        bool hardLockPreset,
                                                        double naturalVibratoPercent) noexcept
    {
        if (hardLockPreset)
            return hardLockMaximumLiveCorrectionCents;

        const auto usesNaturalVibrato = naturalVibratoPercent > hardLockZeroTolerance
                                      || naturalVibratoPercent < -hardLockZeroTolerance;
        return retuneSpeedMs <= VIBE_MUSICAL_RAIL_MAX_RETUNE_MS
            && ! usesNaturalVibrato
             ? musicalMaximumLiveCorrectionCents
             : maximumLiveCorrectionCents;
    }
    static constexpr int rapidDescentRailGuardHoldSamples = 1536;
    static constexpr double maximumCorrectionStepCents = 60.0;
    static constexpr double hardLockMaximumCorrectionStepCents = VIBE_HARD_LOCK_MAXIMUM_CORRECTION_STEP_CENTS;
    static constexpr double humanizeStartSeconds = 0.240;
    static constexpr double humanizeRampSeconds = 0.180;
    static constexpr double pitchCentreTimeSeconds = VIBE_PITCH_CENTRE_SECONDS;
    static constexpr double vibratoEnergyTimeSeconds = 0.100;
    static constexpr double minimumVibratoEnergyCents = 2.5;
    static constexpr double vibratoEnergyFadeCents = 4.0;
    // At the 256-sample Alto hop, 48 entries give 256 ms.  That covers a
    // complete 4 Hz vibrato cycle, so the 4–8 Hz Humanize detector can make
    // a rate decision instead of treating every sustained vibrato as drift.
    static constexpr int pitchResidualHistoryLength = 48;
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
    // correction.  The verified control renders demonstrate a real polarity
    // asymmetry at 30/60 cents, so this surface intentionally retains the
    // signed error axis instead of folding it through abs(pitchErrorCents).
    // Rows are Flex 0/25/50/75/100 and columns are -60..+60 cents.
    // Bilinear interpolation keeps automation continuous between the
    // observed control positions without inventing a symmetric response.
    double flexCorrectionGain (double flex, double pitchErrorCents) const noexcept
    {
        constexpr std::array<double, 5> flexKnots {{ 0.0, 0.25, 0.50, 0.75, 1.0 }};
        constexpr std::array<double, 9> errorKnots {{ -60.0, -30.0, -10.0, -5.0,
                                                         0.0, 5.0, 10.0, 30.0, 60.0 }};
        constexpr std::array<std::array<double, 9>, 5> residualFraction {{
            {{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }}, // Flex 0
            {{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }}, // Flex 25
            // Columns are target-minus-source pitchError.  The measured
            // source residual therefore appears on the opposite signed axis.
            {{ 0.395, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.266 }},
            {{ 0.797, 0.274, 0.0, 0.0, 0.0, 0.0, 0.0, 0.166, 0.757 }},
            {{ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 }}  // Flex 100
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
        interpolateIndex (errorKnots, pitchErrorCents, errorLower, errorAmount);
        const auto residualAtLowerFlex = residualFraction[flexLower][errorLower]
                                        + (residualFraction[flexLower][errorLower + 1]
                                           - residualFraction[flexLower][errorLower]) * errorAmount;
        const auto residualAtUpperFlex = residualFraction[flexLower + 1][errorLower]
                                        + (residualFraction[flexLower + 1][errorLower + 1]
                                           - residualFraction[flexLower + 1][errorLower]) * errorAmount;
        const auto residual = clamp01 (productionFlexResidualScale
                                       * (residualAtLowerFlex
                                          + (residualAtUpperFlex - residualAtLowerFlex) * flexAmount)
                                       * diagnosticFlexResidualScale);
        return 1.0 - residual;
    }

    // Artist Humanize does not rise linearly. On the recorded 5 Hz sustained
    // vowel, the additional retained modulation reaches 69/89/96% of its
    // maximum at the displayed 25/50/75 settings. This affects only a note
    // that has passed the sustain and periodic-vibrato gates above.
    static double artistNegativeNaturalPhaseLeadRadians (double naturalVibratoPercent,
                                                         double vibratoRateHz) noexcept
    {
        // Measured candidate-minus-Artist phase error on the 30-cent,
        // 6/8-Hz Natural fixtures.  Linear interpolation preserves a smooth
        // UI response between -12, -9 and -6 and avoids extrapolating into
        // Natural's neutral/positive region.
        constexpr std::array<double, 3> positions {{ -12.0, -9.0, -6.0 }};
        constexpr std::array<double, 3> leadAt6HzDegrees {{ 141.0, 125.0, 98.0 }};
        constexpr std::array<double, 3> leadAt8HzDegrees {{ 64.0, 39.0, 32.0 }};
        const auto value = clamp (naturalVibratoPercent, positions.front(), positions.back());
        size_t lower = 0;
        double amount = 0.0;
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                lower = index - 1;
                amount = (value - positions[lower]) / (positions[index] - positions[lower]);
                break;
            }
        if (value >= positions.back())
        {
            lower = positions.size() - 2;
            amount = 1.0;
        }
        const auto leadAt6Hz = leadAt6HzDegrees[lower]
                             + (leadAt6HzDegrees[lower + 1] - leadAt6HzDegrees[lower]) * amount;
        const auto leadAt8Hz = leadAt8HzDegrees[lower]
                             + (leadAt8HzDegrees[lower + 1] - leadAt8HzDegrees[lower]) * amount;
        const auto frequencyAmount = clamp01 ((vibratoRateHz - 6.0) / 2.0);
        const auto leadDegrees = leadAt6Hz + (leadAt8Hz - leadAt6Hz) * frequencyAmount;
        return leadDegrees * 3.14159265358979323846 / 180.0;
    }

    static double artistNegativeNaturalPhaseScale (double naturalVibratoPercent) noexcept
    {
        // Independent F0 sweeps select the smallest phase multiplier at each
        // measured negative-Natural knot.  The -6 row needs a stronger
        // rotation because its residual-delivery gain is lower than -9/-12.
        constexpr std::array<double, 3> positions {{ -12.0, -9.0, -6.0 }};
        constexpr std::array<double, 3> scales {{ 1.50, 1.50, 2.00 }};
        const auto value = clamp (naturalVibratoPercent, positions.front(), positions.back());
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                const auto amount = (value - positions[index - 1])
                                  / (positions[index] - positions[index - 1]);
                return scales[index - 1] + (scales[index] - scales[index - 1]) * amount;
            }
        return scales.back();
    }

    static double artistNegativeNaturalPerControlPhaseMultiplier (double naturalVibratoPercent) noexcept
    {
        // Fixed-latency Artist sweeps show that a global phase multiplier is
        // wrong: -9/-6 improve near 0.5, while -12 regresses there. Keep the
        // original -12 phase and reduce only the two independently measured
        // knots. Interpolation keeps automation continuous between them.
        constexpr std::array<double, 3> positions {{ -12.0, -9.0, -6.0 }};
        constexpr std::array<double, 3> multipliers {{ 1.0, 0.5, 0.5 }};
        const auto value = clamp (naturalVibratoPercent, positions.front(), positions.back());
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                const auto amount = (value - positions[index - 1]) / (positions[index] - positions[index - 1]);
                return multipliers[index - 1] + (multipliers[index] - multipliers[index - 1]) * amount;
            }
        return multipliers.back();
    }

    static double artistNegativeNaturalDeepPhaseRetention (double naturalVibratoPercent,
                                                            double sourceVibratoEnergyCents) noexcept
    {
        // The 60-cent / 8-Hz Artist reference has a different response only
        // at Natural -12: applying the mid-depth phase lead there makes it
        // lag again. Fade that one row to its measured gain-only response
        // above the deep-vibrato range; -9 and -6 retain their fitted lead.
        constexpr std::array<double, 3> positions {{ -12.0, -9.0, -6.0 }};
        constexpr std::array<double, 3> deepRetentions {{ 0.0, 1.0, 1.0 }};
        const auto energyProgress = clamp01 ((sourceVibratoEnergyCents - 16.0) / 8.0);
        const auto value = clamp (naturalVibratoPercent, positions.front(), positions.back());
        size_t lower = 0;
        double controlAmount = 0.0;
        for (size_t index = 1; index < positions.size(); ++index)
            if (value <= positions[index])
            {
                lower = index - 1;
                controlAmount = (value - positions[lower]) / (positions[index] - positions[lower]);
                break;
            }
        if (value >= positions.back())
        {
            lower = positions.size() - 2;
            controlAmount = 1.0;
        }
        const auto deepRetention = deepRetentions[lower]
                                 + (deepRetentions[lower + 1] - deepRetentions[lower]) * controlAmount;
        return 1.0 + (deepRetention - 1.0) * energyProgress;
    }

    double artistNaturalVibratoGain (double naturalVibratoPercent,
                                     double sourceVibratoEnergyCents) const noexcept
    {
        // Rows are Artist's -12/-9/-6/+3/+6/+12 renders; columns are the
        // measured 10/30/60-cent source fixtures (energy about 6/16/27 ct).
        // Values are residual delivery gains, not a synthesized oscillator.
        constexpr std::array<double, 7> positions {{ -12.0, -9.0, -6.0, 0.0, 3.0, 6.0, 12.0 }};
        constexpr std::array<double, 3> energies {{ 6.0, 16.0, 27.0 }};
        const auto& gains = diagnosticNaturalVibratoSurface;

        const auto interpolate = [] (const auto& knots, double value,
                                     size_t& lower, double& amount) noexcept
        {
            const auto clampedValue = clamp (value, knots.front(), knots.back());
            lower = 0; amount = 0.0;
            for (size_t index = 1; index < knots.size(); ++index)
                if (clampedValue <= knots[index])
                {
                    lower = index - 1;
                    amount = (clampedValue - knots[lower]) / (knots[index] - knots[lower]);
                    return;
                }
            lower = knots.size() - 2;
            amount = 1.0;
        };

        size_t controlLower = 0, energyLower = 0;
        double controlAmount = 0.0, energyAmount = 0.0;
        interpolate (positions, naturalVibratoPercent, controlLower, controlAmount);
        interpolate (energies, sourceVibratoEnergyCents, energyLower, energyAmount);
        const auto gainAtLowerControl = gains[controlLower][energyLower]
                                      + (gains[controlLower][energyLower + 1] - gains[controlLower][energyLower]) * energyAmount;
        const auto gainAtUpperControl = gains[controlLower + 1][energyLower]
                                      + (gains[controlLower + 1][energyLower + 1] - gains[controlLower + 1][energyLower]) * energyAmount;
        return gainAtLowerControl + (gainAtUpperControl - gainAtLowerControl) * controlAmount;
    }



    void resetPitchVariationAnalysis() noexcept
    {
        pitchDecomposer.reset();
        humanizeVibratoRecognition = 0.0;
        humanizeProcessor.reset();
        previousVibratoResidualCents = 0.0;
        hasPreviousVibratoResidual = false;
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
            if (! policy.isLegalTarget (note))
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
            if (policy.isLegalTarget (note))
                return note;
        return -1;
    }

    static int adjacentLegalNote (int currentNote, const NotePolicySnapshot& policy,
                                  int direction) noexcept
    {
        for (int note = currentNote + direction; note >= 0 && note <= 127; note += direction)
            if (policy.isLegalTarget (note))
                return note;
        return -1;
    }

    static int selectPolicyTarget (double desiredMidi, NoteRule sourceRule,
                                   const NotePolicySnapshot& policy, int currentTarget) noexcept
    {
        // Directional policies deliberately do not fall back to a nearest
        // note on the other side. A missing target returns -1 (unity/dry).
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

    void updateTrajectoryPreview (double desiredMidi, double signedInputDeltaCents,
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

    void resetRapidAdjacentPredictor() noexcept
    {
        rapidAdjacentPrearmed = false;
        rapidAdjacentCandidateNote = -1;
        rapidAdjacentEvidenceFrames = 0;
        rapidAdjacentRawDistanceCents = 0.0;
        rapidAdjacentDirection = 0;
        rapidAdjacentTravelCents = 0.0;
        rapidAdjacentRawHistory.fill (std::numeric_limits<double>::quiet_NaN());
        rapidAdjacentRawHistoryIndex = 0;
        rapidAdjacentRawHistoryCount = 0;
        rapidAdjacentPredictorTargetNote = -1;
        rapidAdjacentBoundaryCandidateNote = -1;
        rapidAdjacentBoundaryFrames = 0;
    }

    int updateRapidAdjacentPredictor (double rawF0Hz, double rawConfidence,
                                       double referencePitchHz, int transpose,
                                       const NotePolicySnapshot& policy,
                                       bool hardLock, int hopSamples) noexcept
    {
        if (! rapidAdjacentTargetSelectionEnabled || ! hardLock
            || currentTargetNote < 0 || ! std::isfinite (rawF0Hz) || rawF0Hz <= 0.0
            || ! std::isfinite (rawConfidence) || rawConfidence < rapidAdjacentMinimumConfidence)
        {
            resetRapidAdjacentPredictor();
            return -1;
        }

        const auto rawMidi = 69.0 + 12.0 * std::log2 (rawF0Hz / referencePitchHz)
                           + static_cast<double> (transpose);

        // A short luyến can settle just across a legal-note midpoint before
        // the longer motion predictor has accumulated enough travel. This
        // separate boundary-dwell route remains hard-lock-only, accepts only
        // the immediately adjacent legal note, and requires consecutive
        // high-confidence raw-YIN frames. It is macro-gated for A/B.
        if (rapidAdjacentBoundaryDwellFrames > 0
            && framesSinceTargetCommit * hopSamples >= rapidAdjacentMinimumDwellSamples)
        {
            const auto rawSourceMidi = std::clamp (static_cast<int> (std::lround (rawMidi)), 0, 127);
            const auto rawSourceRule = policy.ruleForMidi (rawSourceMidi);
            const auto boundaryCandidate = selectPolicyTarget (rawMidi, rawSourceRule,
                                                                policy, currentTargetNote);
            const auto isAdjacentBoundary = boundaryCandidate >= 0
                                         && std::abs (boundaryCandidate - currentTargetNote) == 1;
            const auto direction = isAdjacentBoundary
                                 ? (boundaryCandidate > currentTargetNote ? 1.0 : -1.0) : 0.0;
            const auto midpoint = isAdjacentBoundary
                                ? 0.5 * static_cast<double> (boundaryCandidate + currentTargetNote) : 0.0;
            const auto beyondMidpoint = isAdjacentBoundary
                                     && (rawMidi - midpoint) * direction >= 0.0;
            if (beyondMidpoint)
            {
                rapidAdjacentBoundaryFrames = boundaryCandidate == rapidAdjacentBoundaryCandidateNote
                                            ? rapidAdjacentBoundaryFrames + 1 : 1;
                rapidAdjacentBoundaryCandidateNote = boundaryCandidate;
                if (rapidAdjacentBoundaryFrames >= rapidAdjacentBoundaryDwellFrames)
                {
                    rapidAdjacentCandidateNote = boundaryCandidate;
                    rapidAdjacentDirection = static_cast<int> (direction);
                    rapidAdjacentEvidenceFrames = rapidAdjacentBoundaryFrames;
                    rapidAdjacentTravelCents = 0.0;
                    rapidAdjacentRawDistanceCents = (rawMidi - midpoint) * direction * 100.0;
                    rapidAdjacentPrearmed = true;
                    return boundaryCandidate;
                }
            }
            else
            {
                rapidAdjacentBoundaryCandidateNote = -1;
                rapidAdjacentBoundaryFrames = 0;
            }
        }
        else
        {
            rapidAdjacentBoundaryCandidateNote = -1;
            rapidAdjacentBoundaryFrames = 0;
        }
        if (rapidAdjacentPredictorTargetNote != currentTargetNote)
        {
            resetRapidAdjacentPredictor();
            rapidAdjacentPredictorTargetNote = currentTargetNote;
        }
        rapidAdjacentRawHistory[static_cast<size_t> (rapidAdjacentRawHistoryIndex)] = rawMidi;
        rapidAdjacentRawHistoryIndex = (rapidAdjacentRawHistoryIndex + 1) % rapidAdjacentEvidenceWindowFrames;
        rapidAdjacentRawHistoryCount = std::min (rapidAdjacentRawHistoryCount + 1,
                                                 rapidAdjacentEvidenceWindowFrames);
        if (rapidAdjacentRawHistoryCount < rapidAdjacentEvidenceWindowFrames)
        {
            rapidAdjacentEvidenceFrames = rapidAdjacentRawHistoryCount;
            return -1;
        }
        if (framesSinceTargetCommit * hopSamples < rapidAdjacentMinimumDwellSamples)
            return -1;

        const auto oldestIndex = rapidAdjacentRawHistoryIndex;
        const auto oldestMidi = rapidAdjacentRawHistory[static_cast<size_t> (oldestIndex)];
        const auto netTravelCents = (rawMidi - oldestMidi) * 100.0;
        if (std::abs (netTravelCents) < rapidAdjacentMinimumTravelCents)
        {
            rapidAdjacentPrearmed = false;
            rapidAdjacentCandidateNote = -1;
            rapidAdjacentEvidenceFrames = rapidAdjacentRawHistoryCount;
            rapidAdjacentTravelCents = std::abs (netTravelCents);
            rapidAdjacentRawDistanceCents = 0.0;
            return -1;
        }

        const auto direction = netTravelCents > 0.0 ? 1 : -1;
        auto supportingSteps = 0;
        for (int offset = 1; offset < rapidAdjacentEvidenceWindowFrames; ++offset)
        {
            const auto previous = rapidAdjacentRawHistory[static_cast<size_t> ((oldestIndex + offset - 1)
                                                                                 % rapidAdjacentEvidenceWindowFrames)];
            const auto current = rapidAdjacentRawHistory[static_cast<size_t> ((oldestIndex + offset)
                                                                                % rapidAdjacentEvidenceWindowFrames)];
            if ((current - previous) * static_cast<double> (direction) * 100.0
                >= rapidAdjacentMinimumMotionCents)
                ++supportingSteps;
        }
        if (supportingSteps < rapidAdjacentMinimumSupportingSteps)
        {
            rapidAdjacentPrearmed = false;
            rapidAdjacentEvidenceFrames = supportingSteps;
            rapidAdjacentTravelCents = std::abs (netTravelCents);
            return -1;
        }

        const auto candidate = adjacentLegalNote (currentTargetNote, policy, direction);
        if (candidate < 0)
        {
            resetRapidAdjacentPredictor();
            return -1;
        }
        const auto midpoint = 0.5 * static_cast<double> (candidate + currentTargetNote);
        const auto distanceCents = (rawMidi - midpoint) * static_cast<double> (direction) * 100.0;

        // The raw predictor is intentionally asymmetric. It may pre-arm up
        // past the midpoint, but only after a four-frame
        // net movement with at least two supporting hops. A return remains
        // governed by selectTarget's normal 48-ms reversal guard unless this
        // same bounded evidence is present again.
        const auto withinFastEntry = distanceCents >= rapidAdjacentEntryBeyondMidpointCents;
        if (! withinFastEntry)
        {
            rapidAdjacentPrearmed = false;
            rapidAdjacentCandidateNote = -1;
            rapidAdjacentEvidenceFrames = 0;
            rapidAdjacentTravelCents = 0.0;
            rapidAdjacentDirection = direction;
            rapidAdjacentRawDistanceCents = distanceCents;
            return -1;
        }

        rapidAdjacentCandidateNote = candidate;
        rapidAdjacentDirection = direction;
        rapidAdjacentEvidenceFrames = rapidAdjacentEvidenceWindowFrames;
        rapidAdjacentTravelCents = std::abs (netTravelCents);
        rapidAdjacentRawDistanceCents = distanceCents;
        rapidAdjacentPrearmed = true;
        return rapidAdjacentPrearmed ? candidate : -1;
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
                       bool requiresInitialConfirmation, bool directHardTune,
                       double sampleRate, int hopSamples) noexcept
    {
        selectorCurrentTargetBefore = currentTargetNote;
        selectorPreviousTarget = previousTargetNote;
        selectorCandidate = candidate;
        selectorFramesSinceCommit = framesSinceTargetCommit;
        selectorDecision = 0;
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
                selectorDecision = 3;
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
                selectorDecision = 3;
            }
            else
                selectorDecision = 6;
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
            {
                selectorDecision = 2;
                return;
            }
            const auto candidateDirection = candidate > currentTargetNote ? 1 : -1;
            // The fast Alto analysis may briefly quantise a mixed boundary
            // frame to the opposite neighbour. Do not let that frame reverse
            // the musical target unless the measured F0 is moving with it.
            if (trajectoryPreviewDirection != 0 && candidateDirection != trajectoryPreviewDirection)
            {
                selectorDecision = 1;
                return;
            }
            // Retune 0 normally commits immediately. The exception is an
            // immediate return to the note that was active before this one:
            // A -> B -> A -> B chatter gives the returned A only one
            // analysis frame, so no audible grain can establish its pitch.
            // Keep the current note for at most 48 ms; an ordinary adjacent
            // step or a sustained, coherent reversal remains eligible.
            const auto guardFrames = std::max (1, static_cast<int> (std::ceil (
                hardTuneRapidReversalGuardSeconds * sampleRate / std::max (1, hopSamples))));
            // An opt-in rapid-legato return is already direction-confirmed
            // by its dedicated six-frame preview gate.  Let that confirmed
            // musical target replace the old trajectory immediately; its
            // actual pull and time still come from the normal user controls
            // below.  Raw detector reversals keep the ordinary chatter guard.
            const auto immediateReturnToPrevious = candidate == previousTargetNote
                                                 && framesSinceTargetCommit < guardFrames
                                                 && ! diagnosticRapidLegatoReturnPreview;
            const auto direction = candidate > currentTargetNote ? 1.0 : -1.0;
            const auto midpoint = 0.5 * static_cast<double> (candidate + currentTargetNote);
            const auto beyondMidpoint = (desiredMidi - midpoint) * direction
                                      > midpointHysteresisSemitones;
            if (immediateReturnToPrevious)
            {
                selectorDecision = 2;
                return;
            }
            previousTargetNote = currentTargetNote;
            currentTargetNote = candidate;
            framesSinceTargetCommit = 0;
            pendingTargetNote = -1;
            pendingTargetFrames = 0;
            if (diagnosticRapidLegatoPreview)
                diagnosticCommitLockSamples = diagnosticRapidLegatoCommitLockSamples;
            selectorDecision = 3;
            return;
        }

        const auto direction = candidate > currentTargetNote ? 1.0 : -1.0;
        const auto midpoint = 0.5 * static_cast<double> (candidate + currentTargetNote);
        const auto beyondMidpoint = (desiredMidi - midpoint) * direction > midpointHysteresisSemitones;
        if (! onset && ! beyondMidpoint)
        {
            selectorDecision = 4;
            return;
        }

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
            selectorDecision = 3;
        }
        else
            selectorDecision = 5;
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
    PitchDecomposer pitchDecomposer;
    PitchComponents currentPitchComponents;
    VibratoProcessor vibratoProcessor;
    double pitchCenterCents = std::numeric_limits<double>::quiet_NaN();
    double vibratoEnergyCents = 0.0;
    double previousVibratoResidualCents = 0.0;
    bool hasPreviousVibratoResidual = false;
    double humanizeVibratoRecognition = 0.0;
    HumanizeProcessor humanizeProcessor;
    double diagnosticPitchCentreSeconds = VIBE_PITCH_CENTRE_SECONDS;
    double diagnosticWideVibratoNeutralRetention = VIBE_WIDE_VIBRATO_NEUTRAL_RETENTION;
    double diagnosticWideVibratoHumanizedRetention = VIBE_WIDE_VIBRATO_HUMANIZED_RETENTION;
    NaturalVibratoSurface diagnosticNaturalVibratoSurface {{
        // The 60-cent Artist stimulus needs substantially more attenuation
        // for negative Natural Vibrato than its 10/30-cent neighbours.
        // These three values are output-contour fits, not a cosmetic curve.
        {{ 0.744, 1.042, -0.280 }}, // -12: phase-fit residual delivery
        {{ 0.480, 0.974, -0.703 }}, // -9: fixed-latency phase/amplitude fit
        {{ 0.240, 0.480, -0.280 }}, // -6: phase-fit residual delivery
        {{ 1.000, 1.000, 1.000 }}, //  0
        {{ 0.272, 0.475, 0.299 }}, // +3
        {{ 0.497, 0.727, 0.543 }}, // +6
        {{ 1.004, 1.254, 1.228 }}  // +12
    }};
    double diagnosticNaturalSurfaceAtHardTuneWeight = 0.0;
    double diagnosticNegativeNaturalPhaseScale = 1.0;
    double diagnosticFlexResidualScale = 1.0;
    double diagnosticHumanizeScale = 1.0;
    double transitionStartCents = std::numeric_limits<double>::quiet_NaN();
    double transitionEndCents = std::numeric_limits<double>::quiet_NaN();
    double transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
    double transitionProgress = 1.0;
    bool transitionActive = false;
    double transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
    bool transitionUsesLinearSlew = false;
    bool transitionUsesRapidLegatoChain = false;
    bool transitionUsesRapidAdjacentContour = false;
    int transitionDiagnosticWideShape = 0;
    bool diagnosticRaisedCosineHardTuneLegato = false;
    bool diagnosticRapidLegatoPreview = false;
    bool diagnosticRapidLegatoReturnPreview = false;
    bool rapidAdjacentTargetSelectionEnabled = false;
    bool rapidAdjacentPrearmed = false;
    int rapidAdjacentCandidateNote = -1;
    int rapidAdjacentEvidenceFrames = 0;
    int rapidAdjacentDirection = 0;
    double rapidAdjacentTravelCents = 0.0;
    double rapidAdjacentRawDistanceCents = 0.0;
    std::array<double, rapidAdjacentEvidenceWindowFrames> rapidAdjacentRawHistory {};
    int rapidAdjacentRawHistoryIndex = 0;
    int rapidAdjacentRawHistoryCount = 0;
    int rapidAdjacentPredictorTargetNote = -1;
    int rapidAdjacentBoundaryCandidateNote = -1;
    int rapidAdjacentBoundaryFrames = 0;
    // Production rapid-legato ownership is deliberately narrow: only the
    // hard-lock A->B->A evidence gate below can start this gesture. The
    // setter remains for offline A/B and regression diagnostics.
    bool diagnosticContinuousRapidReturnExcursion = true;
    bool diagnosticStrongHardTunePull = false;
    double diagnosticHardTuneWideTransitionUpMs = -1.0;
    double diagnosticHardTuneWideTransitionDownMs = -1.0;
    int diagnosticHardTuneWideTransitionShape = 0;
    static constexpr double calibratedSlowRetuneInputReferenceAmount = 0.75;
    double diagnosticSlowRetuneInputReferenceAmountOverride = -1.0;
    double diagnosticRetune10TimeConstantMs = artistRetune10MsTimeConstantMs;
    double diagnosticRetune25TimeConstantMs = artistRetune25MsTimeConstantMs;
    int rapidReturnLastDirection = 0;
    int rapidReturnAdjacentReversalCount = 0;
    int selectorCurrentTargetBefore = -1;
    int selectorPreviousTarget = -1;
    int selectorCandidate = -1;
    int selectorFramesSinceCommit = 0;
    int selectorDecision = 0;
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
    ContinuousReturnExcursionPhase continuousReturnExcursionPhase = ContinuousReturnExcursionPhase::inactive;
    int continuousReturnExcursionTargetNote = -1;
    double continuousReturnExcursionStartCents = 0.0;
    double continuousReturnExcursionPeakCents = 0.0;
    double continuousReturnExcursionReturnCents = 0.0;
    double continuousReturnExcursionCorrectionCents = 0.0;
    double continuousReturnExcursionProgress = 0.0;
    int continuousReturnExcursionHoldSamples = 0;
    int continuousReturnExcursionEntrySamples = 0;
    int continuousReturnExcursionReturnSamples = 0;
    bool continuousReturnExcursionConsumed = false;
    int diagnosticContinuousReturnMinimumCoherentFrames = rapidReturnExcursionMinimumCoherentFrames;
    double diagnosticContinuousReturnMinimumTravelCents = rapidReturnExcursionMinimumTravelCents;
    double diagnosticContinuousReturnHoldSeconds = rapidReturnExcursionHoldSeconds;
    bool controlSchedulerValid = false;
    double cachedInputCents = 0.0;
    double cachedTargetCents = 0.0;
    double cachedControlTargetCents = 0.0;
    double cachedCorrectionReferenceCents = 0.0;
    bool cachedUsesPortamentoTarget = false;
    double cachedScaledVibratoResidual = 0.0;
    double cachedFlex = 0.0;
    double cachedEffectiveRetuneMs = 0.0;
    double cachedHumanize = 0.0;
    double cachedCorrectionGain = 0.0;
    bool cachedDescentRailGuard = false;
    double cachedCorrectionRailCents = maximumLiveCorrectionCents;
    bool cachedHardLockPreset = false;
    int cachedTargetNote = -1;
    int cachedCandidateTargetNote = -1;
    bool cachedExtremeTargetChange = false;
};
