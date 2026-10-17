#pragma once

#include "VoicedColourProcessor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#ifndef VIBE_TDPSOLA_CAUSAL_SIBILANCE_ENVELOPE
 #define VIBE_TDPSOLA_CAUSAL_SIBILANCE_ENVELOPE 0
#endif

#ifndef VIBE_TDPSOLA_SIBILANCE_MAX_RESIDUAL_MIX
 #define VIBE_TDPSOLA_SIBILANCE_MAX_RESIDUAL_MIX 1.0
#endif

#ifndef VIBE_TDPSOLA_SIBILANCE_WET_GATE_THRESHOLD
 #define VIBE_TDPSOLA_SIBILANCE_WET_GATE_THRESHOLD 2.0
#endif

#ifndef VIBE_TDPSOLA_SIBILANCE_WET_GATE_LOOKAHEAD_SAMPLES
 #define VIBE_TDPSOLA_SIBILANCE_WET_GATE_LOOKAHEAD_SAMPLES 0
#endif

// Live-only TD-PSOLA research renderer.  It never reads future input.  The
// reported latency leaves enough history for a *centred* analysis grain to be
// complete before that grain is written to the future output FIFO.
class CausalTDPSOLARenderer
{
public:
    // Offline forensic switchboard. Defaults preserve the production signal
    // path exactly; the WAV bench can disable one renderer layer at a time
    // without touching target selection or control timing.
    struct DiagnosticAblation
    {
        bool periodicResidual = true;
        bool correctionWindowBlend = true;
        bool waveformAnchoredSourceMarks = false;
        // The production path uses the phase-continuous renderer. Offline
        // A/B can select this retained TD-PSOLA path explicitly for handoff
        // regression measurements.
        bool phaseContinuousRenderer = true;
        // Offline isolation: retain the production phase renderer but remove
        // its frequency map. This separates resynthesis coloration from
        // note/ratio automation without changing tracker state.
        bool phaseForceUnityRatio = false;
        bool phaseForceStaticUpRatio = false;
        // Trajectory-only forensic switch. It is carried with the offline
        // diagnostic request so a rapid-return candidate cannot silently
        // affect the production selector.
        bool rapidLegatoReturnPreview = false;
        // Offline-only continuous trajectory candidate. It must not change
        // the committed selector note.
        bool continuousRapidReturnExcursion = false;
        // Offline sweep parameters for the trajectory candidate. They are
        // consumed by CorrectionTrajectory; renderer DSP ignores them.
        int continuousReturnMinimumCoherentFrames = 4;
        double continuousReturnMinimumTravelCents = 11.0;
        double continuousReturnHoldMilliseconds = 50.0;
        // Candidate A: a bounded trajectory-only reversal must not steal
        // source-grain ownership from an already coherent OLA handoff.
        // Defaults preserve production behavior exactly.
        bool preserveSourceCursorDuringTrajectoryExcursion = false;
        // A confirmed rapid-return gesture owns its newly scheduled OLA
        // tail. Only future contribution from the preceding gesture is
        // faded; source/synthesis phase and already audible samples remain
        // untouched. This is production behaviour, scoped by the trajectory
        // gate, and can still be disabled for an offline A/B.
        bool trajectoryExcursionTailReplacement = true;
        // Diagnostic cadence-consistency candidate: a grain's source advance
        // follows the synthesis interval that actually spawned it.
        bool cadenceOwnedExcursionRatio = false;
        // During a trajectory-only excursion the committed MIDI target is
        // intentionally unchanged. Let the causal synthesis lattice follow
        // that continuous ratio only for this gesture, preventing a 208-Hz
        // grain from being scheduled on a 220-Hz target lattice.
        bool excursionDrivesSynthesisCadence = false;
        // Offline override for the production slow-Retune input-reference
        // blend. Negative keeps the calibrated production amount; values in
        // [0, 1] support controlled regression renders. CorrectionTrajectory
        // owns the law; the renderer only carries the requested override.
        double slowRetuneInputReferenceAmountOverride = -1.0;
        // Offline A/B: once a committed note has been stable long enough,
        // let the synthesis lattice follow the trajectory residual.  This
        // keeps Humanize/Natural-Vibrato audible while target changes still
        // use the note-owned cadence for a crisp handoff.
        bool stableTargetFollowsTrajectory = true;
        // Offline candidate: attenuate only the still-future OLA tail of a
        // grain owned by the preceding adjacent target.  It never changes
        // target selection, source/synthesis phase, or the production path.
        bool targetAwareTailReplacement = false;
        double targetAwareTailOnsetHalfWindows = 0.0;
        double targetAwareTailFadeHalfWindows = 0.25;
        double targetAwareOldTailGain = 0.65;
        double targetAwareMinimumStepCents = 50.0;
        // A non-zero Hamming shoulder gives the first new target-owned grain
        // usable edge weight while OLA normalisation remains continuous.
        double targetAwareNewGrainHammingBlend = 0.25;
        // Test-only global grain half-window override. Shorter windows reduce
        // old-event OLA persistence; negative preserves the production 0.65.
        double targetAwareWindowHalfFactorOverride = -1.0;
        // Forensic A/B only: bypass deferral and retain the same immediate
        // control fallback used when a candidate cannot be queued.
        bool pendingGrainQueue = true;
        // Offline A/B only: retain every future-audible candidate until its
        // output deadline. Production keeps the proven narrower policy.
        bool pendingQueueAllFutureAudible = false;
        // The processor enables this for the 32-sample live production path.
        // A test can explicitly retain the safe class-local false default.
        bool retimableEpochs = false;
    };

    void setDiagnosticAblation (DiagnosticAblation value) noexcept { diagnosticAblation = value; }
    void setVoicedColourAmountPercent (float newAmountPercent) noexcept
    {
        voicedColourAmountPercent = std::clamp (newAmountPercent, 0.0f, 100.0f);
    }

    struct SourceMark { uint64_t sample = 0; double period = 0.0; };
    struct PublishedControl
    {
        uint64_t timestamp = 0, publicationSample = 0, targetEventId = 0;
        double ratio = 1.0;
        int targetMidi = -1;
    };
    void publishControlFrame (PublishedControl control) noexcept
    {
        publishedControls[publishedControlWrite++ % publishedControls.size()] = control;
        if (diagnosticAblation.retimableEpochs)
        {
            retireRetimableEpochsAtSourceTime (sourceScheduleSample);
            rebuildRetimableEpochChain();
        }
    }
    struct ScheduledGrain
    {
        SourceMark sourceMark {};
        uint64_t synthesisMark = 0;
        double ratioAtStart = 1.0;
        double fractionalPhaseOffset = 0.0;
        int halfWindow = 0;
        // Immutable provenance for diagnostics. It never participates in
        // scheduling, overlap-add, phase, or pitch calculations.
        int targetMidi = -1;
        uint64_t targetEventId = 0;
        uint64_t targetControlTimestamp = 0;
        uint64_t controlEventId = 0;
        // Immutable renderer-side correction generation. Unlike target MIDI,
        // it advances at a signed rapid correction reversal, so a grain
        // which already carries the new target but still carries the old
        // ratio remains distinguishable in the OLA ledger.
        uint64_t correctionGeneration = 0;
        // Unlike targetEventId this marks a trajectory-only gesture.  A
        // rapid excursion deliberately retains its committed target, so its
        // OLA ownership cannot be reconstructed from target metadata alone.
        uint64_t trajectoryExcursionId = 0;
        uint64_t tailReplacementFirstSample = std::numeric_limits<uint64_t>::max();
        uint64_t tailReplacementFadeEndSample = std::numeric_limits<uint64_t>::max();
        double tailReplacementFinalGain = 1.0;
        bool active = false;
    };
    struct PendingGrain
    {
        SourceMark sourceMark {};
        uint64_t synthesisMark = 0, deadline = 0, publicationSample = 0, candidateSourceTime = 0, id = 0, traceSequence = 0;
        double ratio = 1.0, phaseOffset = 0.0, windowFactor = 0.65;
        bool emphasis = false; int targetMidi = -1;
        uint64_t targetEventId = 0, targetTimestamp = 0, controlEventId = 0, generation = 0, excursionId = 0;
        uint64_t provisionalSynthesisMark = 0, chainPredecessorMark = 0, sealSample = 0, ticketIssueSample = 0;
        int64_t retimeSamples = 0;
        bool retimed = false, sealed = false;
        bool active = false;
    };
    // Offline-only epoch ledger. It is never consulted by renderer DSP.
    struct EpochTrace
    {
        uint64_t sequence = 0, decisionSourceSample = 0, sourceScheduleSample = 0;
        uint64_t pendingSynthesisMarkBefore = 0, pendingSynthesisMarkAfter = 0;
        uint64_t sourceMarkCursorBefore = 0, sourceMarkCursorAfter = 0;
        uint64_t selectedMark = 0, synthesisMark = 0, halfWindow = 0, firstAudible = 0;
        uint64_t controlTimestamp = 0, controlPublicationSample = 0, targetEventId = 0;
        uint64_t fallbackControlTimestamp = 0, fallbackControlPublicationSample = 0, fallbackTargetEventId = 0;
        uint64_t pendingCandidateId = 0, pendingFinalizationSample = 0;
        double synthPhaseBefore = 0.0, synthPhaseAfter = 0.0;
        double sourcePeriod = 0.0, effectiveSynthesisPeriod = 0.0, ratio = 1.0, fallbackRatio = 1.0;
        uint64_t sealSample = 0, ticketIssueSample = 0;
        int64_t retimeSamples = 0;
        bool deferred = false, finalized = false, retimed = false;
    };
    static constexpr int epochTraceCapacity = 4096;
    // Exact write-side OLA ledger for a bounded offline output range. Each
    // row mirrors one numerator/denominator addition; it never feeds OLA.
    struct OlaContributionTrace
    {
        uint64_t grainId = 0, targetEventId = 0, controlTimestamp = 0, correctionGeneration = 0;
        uint64_t synthesisMark = 0, halfWindow = 0, destinationSample = 0, sourceMark = 0;
        double ratio = 1.0, numeratorContribution = 0.0, weightContribution = 0.0;
    };
    struct PitchMarkEvidence { double offsetSamples = 0.0; double periodicCorrelation = 0.0; };
    struct DiagnosticState
    {
        enum CorrectionSource : uint8_t
        {
            normalDesired = 0,
            invalidControl,
            dryGuard
        };

        // These expose the hand-off at the renderer boundary.  A dry guard
        // mutes wet output but deliberately does not freeze the source-time
        // control used to keep the PSOLA lattice current for re-entry.
        double rendererPreGuardCorrectionCents = 0.0;
        double rendererPostGuardCorrectionCents = 0.0;
        double rendererInputCorrectionCents = 0.0;
        double requestedRatio = 1.0;
        double activeRatio = 1.0;
        double sourcePhase = 0.0;
        double synthesisPhase = 0.0;
        double currentOverlapWeight = 0.0;
        double audibleRatioAtStart = 1.0;
        double audibleGrainAge = 0.0;
        double audibleReadPosition = 0.0;
        double audibleSourceMarkTime = -1.0;
        double audibleSourceMarkF0 = 0.0;
        double audibleExpectedGrainF0 = 0.0;
        // Strongest individual OLA contributor at the rendered sample.  This
        // is forensic-only ownership data; it never feeds synthesis.
        double dominantGrainWeight = 0.0;
        double dominantGrainEpoch = -1.0;
        double dominantGrainRatio = 1.0;
        double dominantGrainExpectedF0 = 0.0;
        int dominantGrainTargetMidi = -1;
        double dominantGrainTargetEventId = -1.0;
        double dominantGrainControlEventId = -1.0;
        double dominantGrainCorrectionGeneration = -1.0;
        double dominantGrainTargetControlTimestamp = -1.0;
        double dominantGrainTrajectoryExcursionId = 0.0;
        // Debug-only ownership split at the currently rendered OLA sample.
        // These fields never participate in scheduling or synthesis.
        double currentControlEventId = -1.0;
        double currentCorrectionGeneration = -1.0;
        double newestTargetGrainWeight = 0.0;
        double previousTargetGrainWeight = 0.0;
        double newestTargetFirstAudibleSample = -1.0;
        double previousTargetLastAudibleSample = -1.0;
        double targetCandidateWeight = 0.0;
        double targetCandidateEpoch = -1.0;
        double targetCandidateExpectedF0 = 0.0;
        double nextEpochBeforeReversal = -1.0;
        double nextEpochAfterReversal = -1.0;
        double actualSpawnEpoch = -1.0;
        double pendingCandidateId = -1.0;
        double pendingFinalizedAt = -1.0;
        double pendingFinalizationKind = -1.0;
        int pendingGrainCount = 0;
        double grainDecisionInputSample = -1.0;
        double grainFirstAudibleSample = -1.0;
        uint64_t olaDeadlineViolationCount = 0;
        uint64_t pendingDeadlineMissCount = 0;
        uint64_t lateCandidateCreationCount = 0;
        uint64_t lateOlaWriteCount = 0;
        double earliestLateOlaDestination = -1.0;
        double grainHalfWindowAtSpawn = 0.0;
        double grainSourceMarkAtSpawn = -1.0;
        double ratioAtSpawn = 1.0;
        int targetMidiAtSpawn = -1;
        double targetEventIdAtSpawn = -1.0;
        double targetControlTimestampAtSpawn = -1.0;
        double trajectoryExcursionIdAtSpawn = 0.0;
        double sourcePeriodAtSpawn = 0.0;
        double expectedOutputPeriod = 0.0;
        double previousSynthMark = -1.0;
        double currentSynthMark = -1.0;
        double actualSynthPeriod = 0.0;
        double expectedSynthPeriodError = 0.0;
        double actualSourceMarkSpacing = 0.0;
        double sourceSelectionCursor = -1.0;
        double newestCompleteSourceMark = -1.0;
        // Amount of only the non-periodic high-frequency residual restored
        // from the latency-aligned dry signal.  This is deliberately not a
        // global high-band dry mix: vowels should remain entirely wet.
        double sibilanceResidualMix = 0.0;
        double highFrequencyEvidence = 0.0;
        double periodicCoherence = 0.0;
        double pitchMarkCandidateOffset = 0.0;
        double pitchMarkCandidateCorrelation = 0.0;
        double confirmedPitchMarkOffset = 0.0;
        int confirmedPitchMarkRun = 0;
        int queuedMarks = 0;
        int scheduledFutureGrains = 0;
        int shadowFutureSegments = 0;
        int shadowAudibleSegments = 0;
        double shadowFutureSamples = 0.0;
        bool shadowReplacementEligible = false;
        double shadowReplacementFirstSample = -1.0;
        double shadowReplacementLastSample = -1.0;
        bool shadowCandidateRenderApplied = false;
        bool trajectoryExcursionActive = false;
        uint8_t trajectoryExcursionPhase = 0;
        bool strongRatioStep = false;
        bool rapidCorrectionReversal = false;
        bool targetCommitted = false;
        double sourceCursorBeforeStrongStep = -1.0;
        double sourceCursorAfterStrongStep = -1.0;
        bool sourceCursorReanchorSuppressed = false;
        double fractionalPhaseOffsetAtSpawn = 0.0;
        double overlapPhaseOffsetAtSpawn = 0.0;
        double wetMix = 0.0;
        CorrectionSource correctionSource = invalidControl;
        bool latticeReset = false;
        bool dryGuardActive = false;
        bool reentryHoldActive = false;
        bool artifactDryVeto = false;
        bool grainStarved = false;
        bool shouldRenderWet = false;
        int remainingSafetyDrySamples = 0;
    };

    const DiagnosticState& getDiagnosticState() const noexcept { return diagnosticState; }
    uint64_t getOlaDeadlineViolationCount() const noexcept { return olaDeadlineViolationCount; }
    uint64_t getPendingDeadlineMissCount() const noexcept { return pendingDeadlineMissCount; }
    uint64_t getLateCandidateCreationCount() const noexcept { return lateCandidateCreationCount; }
    uint64_t getLateOlaWriteCount() const noexcept { return lateOlaWriteCount; }
    int getPendingQueuePeakOccupancy() const noexcept { return pendingQueuePeakOccupancy; }
    uint64_t getPendingQueueOverflowCount() const noexcept { return pendingQueueOverflowCount; }
    int getRetimableEpochPeakOccupancy() const noexcept { return retimableEpochPeakOccupancy; }
    uint64_t getEpochRetimeCount() const noexcept { return epochRetimeCount; }
    uint64_t getRetimedSamplesTotal() const noexcept { return retimedSamplesTotal; }
    uint64_t getMaxSingleEpochRetime() const noexcept { return maxSingleEpochRetime; }
    uint64_t getRetimeDeadlineMissCount() const noexcept { return retimeDeadlineMissCount; }
    uint64_t getEpochOrderViolationCount() const noexcept { return epochOrderViolationCount; }
    uint64_t getTicketIssuedPastDeadlineCount() const noexcept { return ticketIssuedPastDeadlineCount; }
    uint64_t getTicketMutationAfterIssueCount() const noexcept { return ticketMutationAfterIssueCount; }
    void setPlannerHandoffMarginSamples (uint64_t samples) noexcept
    {
        plannerHandoffMarginSamples = std::max<uint64_t> (1, samples);
    }
    // Diagnostic-only clock observations.  They describe source-planner
    // opportunities before any output samples of the completing callback are
    // consumed; they do not participate in DSP decisions.
    uint64_t getMaximumOutputAheadAtSourcePlanner() const noexcept { return maximumOutputAheadAtSourcePlanner; }
    uint64_t getMaximumOutputAdvanceBetweenSourcePlanners() const noexcept { return maximumOutputAdvanceBetweenSourcePlanners; }
    void setEpochTraceEnabled (bool shouldEnable) noexcept { epochTraceEnabled = shouldEnable; }
    // Provenance is useful only to signal-chain/live capture. Keep its
    // per-sample ledgers and grain scans out of the ordinary live path.
    void setDiagnosticTrackingEnabled (bool shouldEnable) noexcept { diagnosticTrackingEnabled = shouldEnable; }
    int copyEpochTraces (EpochTrace* destination, int maximumTraces) const noexcept
    {
        if (destination == nullptr || maximumTraces <= 0)
            return 0;
        const auto count = std::min<int> (maximumTraces, epochTraceCount);
        std::copy_n (epochTraces.data(), count, destination);
        return count;
    }
    void setOlaContributionTraceRange (uint64_t firstSample, uint64_t lastSample)
    {
        olaContributionTraceEnabled = firstSample <= lastSample;
        olaContributionTraceFirstSample = firstSample;
        olaContributionTraceLastSample = lastSample;
        olaContributionTraces.clear();
        if (olaContributionTraceEnabled)
            olaContributionTraces.reserve (static_cast<size_t> (std::min<uint64_t> (65536, (lastSample - firstSample + 1) * 8)));
    }
    int copyOlaContributionTraces (OlaContributionTrace* destination, int maximumTraces) const noexcept
    {
        if (destination == nullptr || maximumTraces <= 0)
            return 0;
        const auto count = std::min<int> (maximumTraces, static_cast<int> (olaContributionTraces.size()));
        std::copy_n (olaContributionTraces.data(), count, destination);
        return count;
    }

    void prepare (double newSampleRate, int newMaximumBlock, int newLatencySamples = 256)
    {
        sampleRate = newSampleRate;
        maximumBlock = newMaximumBlock;
        latency = std::max (1, newLatencySamples);
        ringLength = std::max (16384, latency * 8 + maximumBlock * 8);
        sourceHighFrequencyEvidenceRing.assign (static_cast<size_t> (ringLength), 0.0f);
        input.assign (static_cast<size_t> (ringLength), 0.0f);
        outputSum.assign (static_cast<size_t> (ringLength), 0.0f);
        outputWeight.assign (static_cast<size_t> (ringLength), 0.0f);
        outputRatioSum.assign (static_cast<size_t> (ringLength), 0.0);
        outputAgeSum.assign (static_cast<size_t> (ringLength), 0.0);
        outputReadPositionSum.assign (static_cast<size_t> (ringLength), 0.0);
        outputSourceMarkSum.assign (static_cast<size_t> (ringLength), 0.0);
        outputSourceF0Sum.assign (static_cast<size_t> (ringLength), 0.0);
        outputExpectedF0Sum.assign (static_cast<size_t> (ringLength), 0.0);
        outputDominantWeight.assign (static_cast<size_t> (ringLength), 0.0f);
        outputDominantSampleContribution.assign (static_cast<size_t> (ringLength), 0.0f);
        outputDominantEpoch.assign (static_cast<size_t> (ringLength), -1.0);
        outputDominantRatio.assign (static_cast<size_t> (ringLength), 1.0);
        outputDominantExpectedF0.assign (static_cast<size_t> (ringLength), 0.0);
        outputDominantTargetMidi.assign (static_cast<size_t> (ringLength), -1);
        outputDominantTargetEventId.assign (static_cast<size_t> (ringLength), -1.0);
        outputDominantControlEventId.assign (static_cast<size_t> (ringLength), -1.0);
        outputDominantCorrectionGeneration.assign (static_cast<size_t> (ringLength), -1.0);
        outputDominantTargetTimestamp.assign (static_cast<size_t> (ringLength), -1.0);
        outputDominantTrajectoryExcursionId.assign (static_cast<size_t> (ringLength), 0.0);
        marks.assign (2048, {});
        scheduledGrains.fill ({});
        publishedControls.fill ({});
        publishedControlWrite = 0;
        pendingGrains.fill ({});
        pendingGrainCount = 0;
        retimableEpochPlanner.fill ({});
        retimablePlannerCount = 0;
        plannerLastIrreversibleMark = 0;
        pendingQueuePeakOccupancy = 0;
        pendingQueueOverflowCount = 0;
        retimableEpochPeakOccupancy = 0;
        epochRetimeCount = retimedSamplesTotal = maxSingleEpochRetime = 0;
        retimeDeadlineMissCount = epochOrderViolationCount = 0;
        ticketIssuedPastDeadlineCount = ticketMutationAfterIssueCount = 0;
        maximumOutputAheadAtSourcePlanner = 0;
        maximumOutputAdvanceBetweenSourcePlanners = 0;
        hasPreviousSourcePlannerObservation = false;
        previousSourcePlannerSample = 0;
        nextPendingCandidateId = 1;
        epochTraceCount = 0;
        lastPendingCandidateId = lastPendingFinalizedAt = lastPendingFinalizationKind = -1.0;
        scheduledGrainWrite = 0;
        voicedColour.prepare (sampleRate);
        reset();
    }

    void reset() noexcept
    {
        std::fill (sourceHighFrequencyEvidenceRing.begin(), sourceHighFrequencyEvidenceRing.end(), 0.0f);
        std::fill (input.begin(), input.end(), 0.0f);
        std::fill (outputSum.begin(), outputSum.end(), 0.0f);
        std::fill (outputWeight.begin(), outputWeight.end(), 0.0f);
        std::fill (outputRatioSum.begin(), outputRatioSum.end(), 0.0);
        std::fill (outputAgeSum.begin(), outputAgeSum.end(), 0.0);
        std::fill (outputReadPositionSum.begin(), outputReadPositionSum.end(), 0.0);
        std::fill (outputSourceMarkSum.begin(), outputSourceMarkSum.end(), 0.0);
        std::fill (outputSourceF0Sum.begin(), outputSourceF0Sum.end(), 0.0);
        std::fill (outputExpectedF0Sum.begin(), outputExpectedF0Sum.end(), 0.0);
        std::fill (outputDominantWeight.begin(), outputDominantWeight.end(), 0.0f);
        std::fill (outputDominantSampleContribution.begin(), outputDominantSampleContribution.end(), 0.0f);
        std::fill (outputDominantEpoch.begin(), outputDominantEpoch.end(), -1.0);
        std::fill (outputDominantRatio.begin(), outputDominantRatio.end(), 1.0);
        std::fill (outputDominantExpectedF0.begin(), outputDominantExpectedF0.end(), 0.0);
        std::fill (outputDominantTargetMidi.begin(), outputDominantTargetMidi.end(), -1);
        std::fill (outputDominantTargetEventId.begin(), outputDominantTargetEventId.end(), -1.0);
        std::fill (outputDominantControlEventId.begin(), outputDominantControlEventId.end(), -1.0);
        std::fill (outputDominantCorrectionGeneration.begin(), outputDominantCorrectionGeneration.end(), -1.0);
        std::fill (outputDominantTargetTimestamp.begin(), outputDominantTargetTimestamp.end(), -1.0);
        std::fill (outputDominantTrajectoryExcursionId.begin(), outputDominantTrajectoryExcursionId.end(), 0.0);
        sourceScheduleSample = 0;
        outputReadSample = 0;
        olaDeadlineViolationCount = 0;
        pendingDeadlineMissCount = 0;
        lateCandidateCreationCount = 0;
        lateOlaWriteCount = 0;
        pendingQueuePeakOccupancy = 0;
        pendingQueueOverflowCount = 0;
        retimableEpochPlanner.fill ({});
        retimablePlannerCount = 0;
        plannerLastIrreversibleMark = 0;
        retimableEpochPeakOccupancy = 0;
        epochRetimeCount = retimedSamplesTotal = maxSingleEpochRetime = 0;
        retimeDeadlineMissCount = epochOrderViolationCount = 0;
        maximumOutputAheadAtSourcePlanner = 0;
        maximumOutputAdvanceBetweenSourcePlanners = 0;
        hasPreviousSourcePlannerObservation = false;
        previousSourcePlannerSample = 0;
        earliestLateOlaDestination = std::numeric_limits<uint64_t>::max();
        epochTraceCount = 0;
        sourcePhase = synthPhase = 0.0;
        markWrite = markCount = 0;
        hasPreviousEpoch = false;
        previousEpoch = {};
        smoothedRatio = 1.0;
        wetMix = 0.0;
        safetyDrySamples = 0;
        previousControlWasValid = false;
        previousOutputControlWasValid = false;
        previousRequestedCorrectionCents = 0.0;
        hasPreviousRequestedCorrection = false;
        correctionGeneration = 0;
        hasPreviousOutput = false;
        previousOutput = 0.0f;
        lastSpawnEpoch = -1.0;
        lastSpawnDecisionInputSample = -1.0;
        lastSpawnHalfWindow = 0.0;
        lastSpawnSourceMarkSample = -1.0;
        lastSpawnRatio = 1.0;
        lastSpawnTargetMidi = -1;
        lastSpawnTargetEventId = -1.0;
        lastSpawnTargetControlTimestamp = -1.0;
        lastSpawnTrajectoryExcursionId = 0.0;
        previousGrainTargetMidi = -1;
        grainTargetEventId = 0;
        pendingTailReplacementEventId = 0;
        pendingTailReplacementActive = false;
        pendingTrajectoryTailReplacementId = 0;
        pendingTrajectoryTailReplacementActive = false;
        trajectoryExcursionId = 0;
        previousTrajectoryExcursionActive = false;
        hasPreviousCommittedTargetHz = false;
        hasTargetBeforePreviousHz = false;
        previousCommittedTargetHz = 0.0;
        targetBeforePreviousHz = 0.0;
        previousTargetCommitSample = 0;
        pendingSynthesisMark = -1.0;
        previousSynthesisMark = -1.0;
        previousSourceMarkSample = -1.0;
        previousSelectedSourceMark = {};
        hasPreviousSelectedSourceMark = false;
        previousFractionalPhaseOffset = 0.0;
        sourceMarkCursor = 0.0;
        hasSourceMarkCursor = false;
        hasPreviousExcitationEvidence = false;
        excitationEvidenceRun = 0;
        confirmedExcitationOffset = 0.0;
        hasPendingSourceMark = false;
        pendingSourceMarkSample = 0;
        pendingSourceMarkPeriod = 0.0;
        scheduledGrains.fill ({});
        scheduledGrainWrite = 0;
        wetLowState = dryLowState = 0.0;
        sourceHighpassLowState = sourceHighEnergy = sourceWidebandEnergy = 0.0;
        sibilanceResidualMix = 0.0;
        unvoicedBurstBaselineRms = 0.0;
        unvoicedBurstGain = 1.0;
        voicedColour.reset();
    }

    int latencySamples() const noexcept { return latency; }

    struct OutputRenderState
    {
        bool valid = false, forceDry = false;
        double periodicCoherence = 0.0, highFrequencyEvidence = 0.0;
        double sibilanceResidualTarget = 0.0, unvoicedBurstGainTarget = 1.0;
        double mixAlpha = 0.0, releaseMixAlpha = 0.0;
        bool targetIsUsable = false;
        double committedTargetHz = std::numeric_limits<double>::quiet_NaN();
        uint64_t targetControlEventId = 0;
    };

    OutputRenderState prepareOutputRenderState (const float* source, int samples,
                                                double f0Hz, double requestedRatio, bool voiced,
                                                bool forceDry, double committedTargetHz,
                                                uint64_t targetControlEventId) noexcept
    {
        OutputRenderState state {};
        state.valid = voiced && std::isfinite (f0Hz) && f0Hz >= 140.0 && f0Hz <= 700.0
                   && std::isfinite (requestedRatio) && requestedRatio >= 0.90 && requestedRatio <= 1.10;
        state.forceDry = forceDry;
        const auto requestedCorrectionCents = state.valid ? 1200.0 * std::log2 (std::max (requestedRatio, 1.0e-12)) : 0.0;
        const auto wasOutputValid = previousOutputControlWasValid;
        if (state.valid && ! wasOutputValid)
            safetyDrySamples = std::max (safetyDrySamples, static_cast<int> (std::lround (safetyHoldSeconds * sampleRate)));
        double totalEnergy = 1.0e-12, differenceEnergy = 1.0e-12, correlation = 0.0, delayedEnergy = 1.0e-12;
        const auto period = state.valid ? std::clamp (static_cast<int> (std::lround (sampleRate / f0Hz)), 32, 512) : 0;
        // A host may call us with 32 samples.  Measuring high-frequency
        // evidence from just that block misses a real /s/ depending on
        // callback boundaries.  This fixed-state one-pole detector has no
        // look-ahead, allocation, lock, or latency; it only gives the
        // existing dry-high residual path a stable causal envelope.
        constexpr auto highpassCutoffHz = 2800.0;
        constexpr auto highEnergyTimeSeconds = 0.004;
        const auto highpassAlpha = 1.0 - std::exp (-2.0 * 3.14159265358979323846
                                                    * highpassCutoffHz / sampleRate);
        const auto highEnergyAlpha = 1.0 - std::exp (-1.0 / (highEnergyTimeSeconds * sampleRate));
        auto previous = readInput (static_cast<int64_t> (outputReadSample) - 1);
        for (int n=0; n<samples; ++n) { const auto x=static_cast<double>(source[n]); const auto d=x-previous; totalEnergy+=x*x; differenceEnergy+=d*d; previous=source[n]; sourceHighpassLowState+=(x-sourceHighpassLowState)*highpassAlpha; const auto high=x-sourceHighpassLowState; sourceHighEnergy+=(high*high-sourceHighEnergy)*highEnergyAlpha; sourceWidebandEnergy+=(x*x-sourceWidebandEnergy)*highEnergyAlpha;
            const auto alignedHighRatio = sourceHighEnergy / std::max (sourceWidebandEnergy, 1.0e-12);
            const auto alignedHighEvidence = std::clamp ((alignedHighRatio - 0.040) / 0.200, 0.0, 1.0);
            sourceHighFrequencyEvidenceRing[index (sourceScheduleSample + static_cast<uint64_t> (n))]
                = static_cast<float> (alignedHighEvidence);
            if (period>0) { const auto delayed=static_cast<double>(n>=period ? source[n-period] : readInput(static_cast<int64_t>(outputReadSample)+n-period)); correlation+=x*delayed; delayedEnergy+=delayed*delayed; } }
        const auto hfRatio=std::clamp(differenceEnergy/totalEnergy,0.0,4.0);
        state.periodicCoherence=period>0 ? std::abs(correlation)/std::sqrt(totalEnergy*delayedEnergy) : 0.0;
#if VIBE_TDPSOLA_CAUSAL_SIBILANCE_ENVELOPE
        const auto causalHighRatio = sourceHighEnergy / std::max (sourceWidebandEnergy, 1.0e-12);
        state.highFrequencyEvidence = std::clamp ((causalHighRatio - 0.040) / 0.200, 0.0, 1.0);
#else
        state.highFrequencyEvidence=std::clamp((hfRatio-0.38)/0.70,0.0,1.0);
#endif
        const auto nonPeriodic=std::clamp((0.74-state.periodicCoherence)/0.30,0.0,1.0);
        const auto nonPeriodicTarget=state.valid && !forceDry ? std::sqrt(state.highFrequencyEvidence*nonPeriodic) : 0.0;
        const auto texture=std::clamp((std::abs(requestedCorrectionCents)-12.0)/48.0,0.0,1.0);
        const auto periodicTarget=diagnosticAblation.periodicResidual && state.valid && !forceDry && state.periodicCoherence>=periodicResidualMinimumCoherence ? periodicResidualMaximumMix*texture : 0.0;
        state.sibilanceResidualTarget = std::min (std::max (nonPeriodicTarget, periodicTarget),
                                                  static_cast<double> (VIBE_TDPSOLA_SIBILANCE_MAX_RESIDUAL_MIX));
        const auto rms=std::sqrt(totalEnergy/static_cast<double>(samples)); state.unvoicedBurstGainTarget=1.0;
        if (!state.valid && !forceDry) { if (wasOutputValid || unvoicedBurstBaselineRms<=1.0e-8) unvoicedBurstBaselineRms=rms; else { const auto rise=20.0*std::log10((rms+1e-12)/(unvoicedBurstBaselineRms+1e-12)); if(state.highFrequencyEvidence<unvoicedBurstMaximumHighFrequencyEvidence && rise>unvoicedBurstRiseThresholdDb) state.unvoicedBurstGainTarget=std::pow(10.0,-std::clamp((rise-unvoicedBurstRiseThresholdDb)*unvoicedBurstRatio,0.0,unvoicedBurstMaximumAttenuationDb)/20.0); const auto a=rms<unvoicedBurstBaselineRms?unvoicedBurstFloorFallAlpha:unvoicedBurstFloorRiseAlpha; unvoicedBurstBaselineRms+=(rms-unvoicedBurstBaselineRms)*a; } } else unvoicedBurstBaselineRms=0.0;
        previousOutputControlWasValid = state.valid;
        state.mixAlpha=1.0-std::exp(-1.0/(crossfadeSeconds*sampleRate)); state.releaseMixAlpha=1.0-std::exp(-1.0/(releaseCrossfadeSeconds*sampleRate)); state.targetIsUsable=state.valid && std::isfinite(committedTargetHz) && committedTargetHz>=140.0 && committedTargetHz<=700.0; state.committedTargetHz=committedTargetHz; state.targetControlEventId=targetControlEventId;
        return state;
    }

    void ingestAndSchedule (const float* source, int samples,
                  double f0Hz, double requestedRatio, bool voiced, bool forceDry = false,
                  double committedTargetHz = std::numeric_limits<double>::quiet_NaN(),
                  double targetResidualCents = 0.0,
                  bool trajectoryExcursionActive = false,
                  uint8_t trajectoryExcursionPhase = 0,
                  int committedTargetMidi = -1,
                  uint64_t targetControlTimestamp = 0,
                  uint64_t targetControlEventId = 0,
                  uint64_t controlPublicationSample = 0) noexcept
    {
        if (source == nullptr || samples <= 0 || samples > maximumBlock)
            return;

        observeSourcePlannerClock (sourceScheduleSample);

        // Source-time alone retires planner membership.  In particular this
        // never observes pending OLA ticket completion from readOutput().
        retireRetimableEpochsAtSourceTime (sourceScheduleSample);

        diagnosticState = {};
        const auto sourceQuantumStart = sourceScheduleSample;
        const auto valid = voiced && std::isfinite (f0Hz) && f0Hz >= 140.0 && f0Hz <= 700.0
                        && std::isfinite (requestedRatio) && requestedRatio >= 0.90 && requestedRatio <= 1.10;
        if (! valid)
        {
            hasSourceMarkCursor = false;
            hasPreviousSelectedSourceMark = false;
            previousFractionalPhaseOffset = 0.0;
            hasPreviousExcitationEvidence = false;
            excitationEvidenceRun = 0;
            confirmedExcitationOffset = 0.0;
            hasPendingSourceMark = false;
        }
        const auto requestedCorrectionCents = valid
                                             ? 1200.0 * std::log2 (std::max (requestedRatio, 1.0e-12)) : 0.0;
        diagnosticState.rendererPreGuardCorrectionCents = requestedCorrectionCents;
        diagnosticState.rendererPostGuardCorrectionCents = requestedCorrectionCents;
        diagnosticState.rendererInputCorrectionCents = requestedCorrectionCents;
        diagnosticState.requestedRatio = requestedRatio;
        diagnosticState.correctionSource = ! valid ? DiagnosticState::invalidControl
                                                   : forceDry ? DiagnosticState::dryGuard
                                                              : DiagnosticState::normalDesired;
        // A source-time target can legitimately reverse while the previous
        // PSOLA grains are still queued for the causal output latency. This
        // is a frequency-control event, never permission to reset the epoch
        // lattice or either phase accumulator: such a reset teleports the
        // read/synthesis trajectory and creates an audible click. Ratio
        // smoothing below carries the reversal into newly scheduled grains
        // while the existing overlap-add tail remains phase-continuous.
        const auto rapidCorrectionReversal = valid && hasPreviousRequestedCorrection
                                          && requestedCorrectionCents * previousRequestedCorrectionCents < 0.0
                                          && std::abs (requestedCorrectionCents - previousRequestedCorrectionCents)
                                               >= rapidReversalThresholdCents;
        const auto strongRatioStep = valid && hasPreviousRequestedCorrection
                                  && std::abs (requestedCorrectionCents - previousRequestedCorrectionCents)
                                       >= schedulerTraceThresholdCents;
        const auto risingCorrectionStep = strongRatioStep
                                       && requestedCorrectionCents > previousRequestedCorrectionCents;
        const auto targetIsUsable = valid && std::isfinite (committedTargetHz)
                                 && committedTargetHz >= 140.0 && committedTargetHz <= 700.0;
        if (targetIsUsable && committedTargetMidi >= 0
            && committedTargetMidi != previousGrainTargetMidi)
        {
            previousGrainTargetMidi = committedTargetMidi;
            ++grainTargetEventId;
        }
        const auto targetStepCents = targetIsUsable && hasPreviousCommittedTargetHz
                                   ? std::abs (1200.0 * std::log2 (committedTargetHz / previousCommittedTargetHz))
                                   : 0.0;
        const auto targetCommitted = targetStepCents >= 50.0;
        if (diagnosticAblation.targetAwareTailReplacement && targetCommitted)
        {
            pendingTailReplacementEventId = grainTargetEventId;
            pendingTailReplacementActive = true;
        }
        const auto preserveSourceCursor = diagnosticAblation.preserveSourceCursorDuringTrajectoryExcursion
                                       && trajectoryExcursionActive
                                       && ! targetCommitted;
        if (trajectoryExcursionActive && ! previousTrajectoryExcursionActive)
        {
            ++trajectoryExcursionId;
            if (diagnosticAblation.trajectoryExcursionTailReplacement)
            {
                pendingTrajectoryTailReplacementId = trajectoryExcursionId;
                pendingTrajectoryTailReplacementActive = true;
                // The new gesture has been confirmed, while the first grain
                // it owns may still be one synthesis period away. Rewrite
                // only queued (therefore inaudible) tail from the preceding
                // event now; waiting for that first grain was too late for
                // a fast A->B->A return.
                applyTargetAwareTailReplacement (-1);
            }
        }
        previousTrajectoryExcursionActive = trajectoryExcursionActive;
        const auto grainTrajectoryExcursionId = trajectoryExcursionActive ? trajectoryExcursionId : 0;
        diagnosticState.trajectoryExcursionActive = trajectoryExcursionActive;
        diagnosticState.trajectoryExcursionPhase = trajectoryExcursionPhase;
        diagnosticState.strongRatioStep = strongRatioStep;
        diagnosticState.rapidCorrectionReversal = rapidCorrectionReversal;
        diagnosticState.targetCommitted = targetCommitted;
        diagnosticState.currentControlEventId = static_cast<double> (targetControlEventId);
        if (valid && correctionGeneration == 0)
            correctionGeneration = 1;
        if (rapidCorrectionReversal)
            ++correctionGeneration;
        diagnosticState.currentCorrectionGeneration = static_cast<double> (correctionGeneration);
        const auto adjacentTargetCommit = targetCommitted && targetStepCents <= 250.0;
        if (targetIsUsable)
        {
            // A target commit can arrive part-way through the old synthesis
            // period.  Preserve the phase history, but retime the one
            // pending epoch to the period of the newly committed neighbour.
            // This avoids carrying 5–10 ms of the old note into A->B->A
            // returns without inserting an extra grain or rewriting OLA.
            const auto returnsToRecentTarget = adjacentTargetCommit && hasTargetBeforePreviousHz
                                             && std::abs (1200.0 * std::log2 (committedTargetHz
                                                                              / targetBeforePreviousHz)) < 25.0
                                             && f0Hz >= rapidReturnMinimumF0Hz
                                             && sourceScheduleSample - previousTargetCommitSample
                                                    <= rapidReturnMaximumSamples;
            if (returnsToRecentTarget && previousSynthesisMark >= 0.0)
            {
                const auto firstMarkThisCall = static_cast<double> (sourceScheduleSample
                                                                      + static_cast<uint64_t> (latency));
                const auto lastMarkThisCall = firstMarkThisCall + static_cast<double> (samples - 1);
                const auto desiredMark = previousSynthesisMark + sampleRate / committedTargetHz;
                if (desiredMark >= firstMarkThisCall && desiredMark <= lastMarkThisCall)
                    pendingSynthesisMark = desiredMark;
            }
            if (targetCommitted)
            {
                targetBeforePreviousHz = previousCommittedTargetHz;
                hasTargetBeforePreviousHz = hasPreviousCommittedTargetHz;
                previousCommittedTargetHz = committedTargetHz;
                previousTargetCommitSample = sourceScheduleSample;
            }
            else if (! hasPreviousCommittedTargetHz)
            {
                previousCommittedTargetHz = committedTargetHz;
                hasPreviousCommittedTargetHz = true;
            }
        }
        else
        {
            hasPreviousCommittedTargetHz = false;
            hasTargetBeforePreviousHz = false;
            pendingSynthesisMark = -1.0;
        }
        constexpr double stableTargetTrajectoryDelaySeconds = 0.120;
        const auto targetHasSettled = targetIsUsable
                                   && sourceScheduleSample >= previousTargetCommitSample
                                   && sourceScheduleSample - previousTargetCommitSample
                                      >= static_cast<uint64_t> (std::lround (stableTargetTrajectoryDelaySeconds * sampleRate));
        const auto residualRatio = std::exp2 (std::clamp (targetResidualCents, -120.0, 120.0) / 1200.0);
        // requestedRatio contains both base correction and the intentional
        // Humanize/Natural-Vibrato residual.  Only snap the lattice to the
        // committed note after the *base* path has arrived. This preserves a
        // slow Retune move, while avoiding cancellation of a residual on a
        // held note (source F0 * total ratio would otherwise remove it).
        const auto trajectoryFrequency = f0Hz * smoothedRatio;
        const auto baseTrajectoryFrequency = trajectoryFrequency / residualRatio;
        const auto baseCorrectionSettled = targetHasSettled
                                        && std::abs (1200.0 * std::log2 (baseTrajectoryFrequency / committedTargetHz)) <= 2.0;
        const auto synthesisFrequency = targetIsUsable
                                     && !(diagnosticAblation.excursionDrivesSynthesisCadence
                                          && trajectoryExcursionActive)
                                     && (! diagnosticAblation.stableTargetFollowsTrajectory || baseCorrectionSettled)
                                     ? committedTargetHz * (diagnosticAblation.stableTargetFollowsTrajectory
                                                                 ? residualRatio : 1.0)
                                     : trajectoryFrequency;
        const auto samplesToNextBefore = valid ? (1.0 - synthPhase) * sampleRate / synthesisFrequency : -1.0;
        if (strongRatioStep)
        {
            diagnosticState.nextEpochBeforeReversal = static_cast<double> (sourceScheduleSample) + samplesToNextBefore;
            diagnosticState.nextEpochAfterReversal = static_cast<double> (sourceScheduleSample)
                                                   + (1.0 - synthPhase) * sampleRate
                                                       / (targetIsUsable ? committedTargetHz : f0Hz * requestedRatio);
        }
        if (valid)
        {
            previousRequestedCorrectionCents = requestedCorrectionCents;
            hasPreviousRequestedCorrection = true;
        }
        else
        {
            hasPreviousRequestedCorrection = false;
        }
        const auto ratioTarget = valid ? requestedRatio : 1.0;
        // Hard-tune controls arrive at 128-sample cadence.  A shorter
        // scheduler-only time constant lets a confirmed target land before
        // the next vowel transition while still avoiding a period step.
        // Retune 0 must arrive at the committed ratio within one short
        // control interval. This smooths frequency only; source/synthesis
        // phase remains integrated and is never re-anchored on a target
        // change.
        constexpr auto ratioSmoothingSeconds = 0.0;
        const auto ratioAlpha = ratioSmoothingSeconds <= 0.0 ? 1.0
                              : 1.0 - std::exp (-1.0 / (ratioSmoothingSeconds * sampleRate));
        for (int n = 0; n < samples; ++n, ++sourceScheduleSample)
        {
            input[index (sourceScheduleSample)] = source[n];

            if (! valid)
                continue;

            smoothedRatio += (ratioTarget - smoothedRatio) * ratioAlpha;
            // Confirm a predicted source epoch only after a small part of
            // the following cycle is safely in history. This lets one
            // continuous tracker compare both sides of the candidate rather
            // than independently anchoring every grain to a formant peak.
            if (hasPendingSourceMark
                && sourceScheduleSample >= pendingSourceMarkSample
                                     + static_cast<uint64_t> (pitchMarkSearchRadius (pendingSourceMarkPeriod)))
            {
                pushMark ({ refineCompletedEpoch (pendingSourceMarkSample, pendingSourceMarkPeriod),
                            pendingSourceMarkPeriod });
                hasPendingSourceMark = false;
            }
            sourcePhase += f0Hz / sampleRate;
            // A committed note owns synthesis cadence. Source F0/ratio still
            // choose the grain sequence, but cannot bend the output lattice
            // away from this note between adjacent grains.
            const auto activeSynthesisFrequency = targetIsUsable
                                                 && !(diagnosticAblation.excursionDrivesSynthesisCadence
                                                      && trajectoryExcursionActive)
                                                 && (! diagnosticAblation.stableTargetFollowsTrajectory || baseCorrectionSettled)
                                                 ? committedTargetHz * (diagnosticAblation.stableTargetFollowsTrajectory
                                                                            ? residualRatio : 1.0)
                                                 : trajectoryFrequency;
            const auto synthPhaseBeforeUpdate = synthPhase;
            const auto pendingSynthesisMarkBefore = pendingSynthesisMark;
            synthPhase += activeSynthesisFrequency / sampleRate;
            const auto synthPhaseAfterUpdate = synthPhase;
            if (sourcePhase >= 1.0)
            {
                sourcePhase -= std::floor (sourcePhase);
                const auto period = sampleRate / f0Hz;
                pendingSourceMarkSample = sourceScheduleSample;
                pendingSourceMarkPeriod = period;
                hasPendingSourceMark = true;
            }
            const auto reachedRetimedEpoch = pendingSynthesisMark >= 0.0
                                          && static_cast<double> (sourceScheduleSample
                                                                   + static_cast<uint64_t> (latency))
                                               >= pendingSynthesisMark;
            if ((synthPhase >= 1.0 || reachedRetimedEpoch) && markCount > 0)
            {
                if (reachedRetimedEpoch)
                {
                    // The retimed epoch replaces the old-rate pending one;
                    // start the following period from this exact mark.
                    synthPhase = 0.0;
                    pendingSynthesisMark = -1.0;
                }
                else
                {
                    synthPhase -= std::floor (synthPhase);
                    pendingSynthesisMark = -1.0;
                }
                SourceMark safeMark {};
                SourceMark newestCompleteMark {};
                const auto hasNewestCompleteMark = findLatestCompleteMark (sourceScheduleSample, newestCompleteMark);
                if (hasNewestCompleteMark)
                    diagnosticState.newestCompleteSourceMark = static_cast<double> (newestCompleteMark.sample);
                if (! hasSourceMarkCursor)
                {
                    if (hasNewestCompleteMark)
                    {
                        sourceMarkCursor = static_cast<double> (newestCompleteMark.sample);
                        hasSourceMarkCursor = true;
                    }
                }
                else if (hasNewestCompleteMark)
                {
                    // Source selection may fall behind after a voiced F0
                    // change: a stale 160 Hz grain paired with a current
                    // 220 Hz synthesis lattice creates the metallic buzz
                    // heard on vocal.  Re-align only this *selection cursor*
                    // once it is more than 1.5 current periods late.  The
                    // synthesis phase and queued OLA grains remain intact.
                    // A pitch-down output has a longer synthesis period, so
                    // its source cursor naturally trails by more than one
                    // input cycle. Treating that expected lag as stale makes
                    // the selector periodically jump forward and biases the
                    // rendered pitch back upward.
                    const auto maximumSelectionLag = 1.5
                                                   * std::max (newestCompleteMark.period, sampleRate / f0Hz);
                    if (sourceMarkCursor < static_cast<double> (newestCompleteMark.sample)
                                          - maximumSelectionLag)
                        sourceMarkCursor = static_cast<double> (newestCompleteMark.sample);

                    // At a certified control step, start the new grain from
                    // the newest complete epoch. The outgoing grain remains
                    // in OLA, so this updates source ownership without
                    // cutting its audible tail.
                    if (strongRatioStep)
                    {
                        diagnosticState.sourceCursorBeforeStrongStep = sourceMarkCursor;
                        diagnosticState.sourceCursorReanchorSuppressed = preserveSourceCursor;
                    }
                    if (strongRatioStep && ! preserveSourceCursor)
                        sourceMarkCursor = static_cast<double> (newestCompleteMark.sample);
                    if (strongRatioStep)
                        diagnosticState.sourceCursorAfterStrongStep = sourceMarkCursor;

                }
                const auto sourceMarkCursorBeforeSelection = sourceMarkCursor;
                if (hasSourceMarkCursor
                    && findCompleteMarkNearest (sourceMarkCursor, sourceScheduleSample, safeMark))
                {
                    // Candidate forensic mode: the target/synthesis clock is
                    // deliberately untouched.  Only the already-complete
                    // source grain is centred on a repeatable vocal epoch.
                    // The bound rejects any correlation solution that would
                    // turn a formant peak into a different cycle.
                    if (diagnosticAblation.waveformAnchoredSourceMarks)
                    {
                        const auto latticeMark = safeMark;
                        const auto resolvedMark = resolvePitchSynchronousMark (safeMark);
                        const auto maximumAnchorOffset = 0.08 * safeMark.period;
                        if (std::abs (static_cast<double> (resolvedMark.sample)
                                      - static_cast<double> (latticeMark.sample)) <= maximumAnchorOffset)
                            safeMark = resolvedMark;
                    }
                    // Evidence only. This may never move a mark until its
                    // confidence and temporal continuity are validated over
                    // real vocal passages; formant peaks are not pitch marks.
                    const auto pitchMarkEvidence = measureExcitationEpoch (safeMark);
                    diagnosticState.pitchMarkCandidateOffset = pitchMarkEvidence.offsetSamples;
                    diagnosticState.pitchMarkCandidateCorrelation = pitchMarkEvidence.periodicCorrelation;
                    // A raw local offset can be a formant maximum. It becomes
                    // usable only after repeated, high-correlation agreement
                    // across adjacent cycles, then it is allowed to move the
                    // read phase gradually. This never changes the synthesis
                    // cadence or target pitch.
                    const auto agreesWithPrevious = hasPreviousExcitationEvidence
                                                 && std::abs (pitchMarkEvidence.offsetSamples
                                                              - previousExcitationEvidenceOffset) <= 2.0;
                    if (pitchMarkEvidence.periodicCorrelation >= 0.88 && agreesWithPrevious)
                    {
                        excitationEvidenceRun = std::min (excitationEvidenceRun + 1, 16);
                        if (excitationEvidenceRun >= 3)
                            confirmedExcitationOffset += std::clamp (pitchMarkEvidence.offsetSamples
                                                                      - confirmedExcitationOffset,
                                                                      -0.40, 0.40);
                    }
                    else
                    {
                        excitationEvidenceRun = 0;
                        // Let a formerly confirmed offset return to the
                        // lattice rather than holding it through a formant or
                        // voiced transition.
                        confirmedExcitationOffset *= 0.75;
                    }
                    previousExcitationEvidenceOffset = pitchMarkEvidence.offsetSamples;
                    hasPreviousExcitationEvidence = pitchMarkEvidence.periodicCorrelation >= 0.88;
                    const auto synthesisMark = sourceScheduleSample + static_cast<uint64_t> (latency);
                    // Steady vowels deliberately retain the deterministic
                    // source lattice: that is what gives a Retune-0 note its
                    // firm, flat plateau.  During a real correction step,
                    // however, repeating the exact same source alignment in
                    // every overlapping grain makes the handoff unnaturally
                    // static/dull.  Use the already-queued OLA waveform as
                    // the phase reference only for that local handoff.  The
                    // estimate is bounded to +/- 4 samples and accepted only
                    // on a high correlation, so it never resets a phase,
                    // changes pitch cadence, or creates a second wet voice.
                    auto fractionalPhaseOffset = estimateFractionalPhaseOffset (safeMark);
                    auto overlapPhaseOffset = 0.0;
                    // A rising return must establish a new note rather than
                    // be phase-matched into the tail of the preceding lower
                    // note. Keep overlap matching on descending hand-offs,
                    // where it has already proved beneficial for continuity.
                    if (strongRatioStep && ! risingCorrectionStep)
                    {
                        overlapPhaseOffset = estimateOverlapPhaseOffset (safeMark, synthesisMark);
                        fractionalPhaseOffset += overlapPhaseOffset;
                    }
                    diagnosticState.fractionalPhaseOffsetAtSpawn = fractionalPhaseOffset;
                    diagnosticState.overlapPhaseOffsetAtSpawn = overlapPhaseOffset;
                    auto ratioAtGrainStart = smoothedRatio;
                    if (diagnosticAblation.cadenceOwnedExcursionRatio
                        && trajectoryExcursionActive && previousSynthesisMark >= 0.0)
                    {
                        const auto completedInterval = static_cast<double> (synthesisMark) - previousSynthesisMark;
                        if (completedInterval > 1.0)
                            ratioAtGrainStart = std::clamp (safeMark.period / completedInterval, 0.50, 2.00);
                    }
                    const auto windowHalfFactor = diagnosticAblation.targetAwareTailReplacement
                                                && diagnosticAblation.targetAwareWindowHalfFactorOverride > 0.0
                                                ? diagnosticAblation.targetAwareWindowHalfFactorOverride
                                                : analysisHalfWindowFactor;
                    const auto halfWindow = static_cast<uint64_t> (std::clamp (static_cast<int> (std::lround (safeMark.period * windowHalfFactor)), 32, 512));
                    const auto deadline = synthesisMark > halfWindow ? synthesisMark - halfWindow : 0;
                    // The source-time ledger is initially provisional in
                    // retimable mode; the planner below may revise its
                    // future entries, never output-time finalization.
                    recordGrainTiming (safeMark, synthesisMark, ratioAtGrainStart);
                    const auto deferred = diagnosticAblation.pendingGrainQueue
                                       && (diagnosticAblation.retimableEpochs
                                           || diagnosticAblation.pendingQueueAllFutureAudible
                                           || (controlPublicationSample > sourceScheduleSample
                                               && controlPublicationSample < deadline));
                    const auto pendingCandidateId = deferred ? nextPendingCandidateId : 0;
                    if (deferred)
                    {
                        PendingGrain pending { safeMark, synthesisMark, deadline, controlPublicationSample,
                                               sourceScheduleSample, nextPendingCandidateId++, 0, ratioAtGrainStart, fractionalPhaseOffset,
                                               windowHalfFactor, strongRatioStep, committedTargetMidi,
                                               grainTargetEventId, targetControlTimestamp, targetControlEventId,
                                               correctionGeneration, grainTrajectoryExcursionId,
                                               synthesisMark, 0, 0, 0, 0, false, false, true };
                        if (outputReadSample > deadline)
                            ++lateCandidateCreationCount;
                        if (diagnosticAblation.retimableEpochs)
                        {
                            if (! enqueueRetimableEpoch (pending))
                                ++pendingQueueOverflowCount;
                            rebuildRetimableEpochChain();
                        }
                        else if (! enqueuePendingGrain (pending))
                        {
                            ++pendingQueueOverflowCount;
                            finalizePendingGrain (pending, sourceScheduleSample);
                        }
                    }
                    else
                        addCentredGrain (safeMark, synthesisMark, ratioAtGrainStart,
                                         fractionalPhaseOffset, windowHalfFactor,
                                         strongRatioStep, committedTargetMidi,
                                         grainTargetEventId, targetControlTimestamp, targetControlEventId,
                                         correctionGeneration, grainTrajectoryExcursionId);
                    if (epochTraceEnabled && epochTraceCount < epochTraceCapacity)
                    {
                        auto& trace = epochTraces[static_cast<size_t> (epochTraceCount++)];
                        trace = {};
                        trace.sequence = static_cast<uint64_t> (epochTraceCount);
                        trace.decisionSourceSample = trace.sourceScheduleSample = sourceScheduleSample;
                        trace.pendingSynthesisMarkBefore = pendingSynthesisMarkBefore >= 0.0 ? static_cast<uint64_t> (pendingSynthesisMarkBefore) : 0;
                        trace.pendingSynthesisMarkAfter = pendingSynthesisMark >= 0.0 ? static_cast<uint64_t> (pendingSynthesisMark) : 0;
                        trace.sourceMarkCursorBefore = static_cast<uint64_t> (std::max (0.0, sourceMarkCursorBeforeSelection));
                        trace.sourceMarkCursorAfter = static_cast<uint64_t> (std::max (0.0, sourceMarkCursor + safeMark.period / ratioAtGrainStart));
                        trace.selectedMark = safeMark.sample;
                        trace.synthesisMark = synthesisMark; trace.halfWindow = halfWindow; trace.firstAudible = deadline;
                        trace.controlTimestamp = targetControlTimestamp; trace.controlPublicationSample = controlPublicationSample;
                        trace.targetEventId = grainTargetEventId; trace.pendingCandidateId = pendingCandidateId;
                        trace.fallbackControlTimestamp = targetControlTimestamp;
                        trace.fallbackControlPublicationSample = controlPublicationSample;
                        trace.fallbackTargetEventId = grainTargetEventId;
                        trace.synthPhaseBefore = synthPhaseBeforeUpdate; trace.synthPhaseAfter = synthPhaseAfterUpdate;
                        trace.sourcePeriod = safeMark.period;
                        trace.effectiveSynthesisPeriod = activeSynthesisFrequency > 0.0 ? sampleRate / activeSynthesisFrequency : 0.0;
                        trace.ratio = ratioAtGrainStart; trace.fallbackRatio = ratioAtGrainStart; trace.deferred = deferred;
                        if (diagnosticAblation.retimableEpochs && pendingCandidateId != 0)
                            for (const auto& pending : pendingGrains)
                                if (pending.active && pending.id == pendingCandidateId)
                                {
                                    trace.synthesisMark = pending.synthesisMark;
                                    trace.firstAudible = pending.deadline;
                                    trace.controlTimestamp = pending.targetTimestamp;
                                    trace.controlPublicationSample = pending.publicationSample;
                                    trace.targetEventId = pending.targetEventId;
                                    trace.ratio = pending.ratio;
                                    trace.retimed = pending.retimed;
                                    trace.retimeSamples = pending.retimeSamples;
                                    break;
                                }
                    }
                    lastSpawnDecisionInputSample = static_cast<double> (sourceScheduleSample);
                    lastSpawnHalfWindow = std::clamp (static_cast<int> (std::lround (safeMark.period * windowHalfFactor)), 32, 512);
                    lastSpawnSourceMarkSample = static_cast<double> (safeMark.sample);
                    lastSpawnEpoch = static_cast<double> (synthesisMark);
                    lastSpawnRatio = ratioAtGrainStart;
                    lastSpawnTargetMidi = committedTargetMidi;
                    lastSpawnTargetEventId = static_cast<double> (grainTargetEventId);
                    lastSpawnTargetControlTimestamp = static_cast<double> (targetControlTimestamp);
                    lastSpawnTrajectoryExcursionId = static_cast<double> (grainTrajectoryExcursionId);
                    // TD-PSOLA changes pitch by moving synthesis marks.
                    // Consecutive source grains must remain one source period
                    // apart: advancing them by sourcePeriod / ratio makes the
                    // source phase track the output lattice and mathematically
                    // cancels the pitch shift (e.g. C# remains C# despite a
                    // correct C target).  Ratio remains immutable grain
                    // provenance; it does not own source-mark spacing.
                    sourceMarkCursor += safeMark.period;
                    previousSelectedSourceMark = safeMark;
                    hasPreviousSelectedSourceMark = true;
                    previousFractionalPhaseOffset = fractionalPhaseOffset;
                }
            }
        }
        if (diagnosticAblation.retimableEpochs)
        {
            rebuildRetimableEpochChain();
            sealRetimableEpochsAtSourceTime (sourceQuantumStart);
        }
        diagnosticState.activeRatio = smoothedRatio;
        diagnosticState.sourcePhase = sourcePhase;
        diagnosticState.synthesisPhase = synthPhase;
        diagnosticState.queuedMarks = static_cast<int> (markCount);
        diagnosticState.scheduledFutureGrains = countScheduledFutureGrains();
        diagnosticState.wetMix = wetMix;
        diagnosticState.actualSpawnEpoch = lastSpawnEpoch;
        diagnosticState.pendingCandidateId = lastPendingCandidateId;
        diagnosticState.pendingFinalizedAt = lastPendingFinalizedAt;
        diagnosticState.pendingFinalizationKind = lastPendingFinalizationKind;
        diagnosticState.pendingGrainCount = pendingGrainCount;
        diagnosticState.grainDecisionInputSample = lastSpawnDecisionInputSample;
        diagnosticState.grainHalfWindowAtSpawn = lastSpawnHalfWindow;
        diagnosticState.grainFirstAudibleSample = lastSpawnEpoch >= 0.0
                                                 ? lastSpawnEpoch - lastSpawnHalfWindow : -1.0;
        diagnosticState.olaDeadlineViolationCount = olaDeadlineViolationCount;
        diagnosticState.grainSourceMarkAtSpawn = lastSpawnSourceMarkSample;
        diagnosticState.ratioAtSpawn = lastSpawnRatio;
        diagnosticState.targetMidiAtSpawn = lastSpawnTargetMidi;
        diagnosticState.targetEventIdAtSpawn = lastSpawnTargetEventId;
        diagnosticState.targetControlTimestampAtSpawn = lastSpawnTargetControlTimestamp;
        diagnosticState.trajectoryExcursionIdAtSpawn = lastSpawnTrajectoryExcursionId;
        diagnosticState.sourcePeriodAtSpawn = lastSpawnSourcePeriod;
        diagnosticState.expectedOutputPeriod = lastExpectedOutputPeriod;
        diagnosticState.previousSynthMark = lastPreviousSynthesisMark;
        diagnosticState.currentSynthMark = lastSpawnEpoch;
        diagnosticState.actualSynthPeriod = lastActualSynthesisPeriod;
        diagnosticState.expectedSynthPeriodError = lastExpectedSynthesisPeriodError;
        diagnosticState.actualSourceMarkSpacing = lastSourceMarkSpacing;
        diagnosticState.sourceSelectionCursor = sourceMarkCursor;
        diagnosticState.confirmedPitchMarkOffset = confirmedExcitationOffset;
        diagnosticState.confirmedPitchMarkRun = excitationEvidenceRun;
        diagnosticState.sibilanceResidualMix = sibilanceResidualMix;
    }

public:
    size_t index (uint64_t sample) const noexcept { return static_cast<size_t> (sample % static_cast<uint64_t> (ringLength)); }
    void readOutput (float* destination, int samples, const OutputRenderState& outputState) noexcept
    {
        if (destination == nullptr || samples <= 0 || samples > maximumBlock)
            return;

        const auto valid = outputState.valid;
        const auto forceDry = outputState.forceDry;
        const auto periodicCoherence = outputState.periodicCoherence;
        const auto sibilanceResidualTarget = outputState.sibilanceResidualTarget;
        const auto unvoicedBurstGainTarget = outputState.unvoicedBurstGainTarget;
        const auto mixAlpha = outputState.mixAlpha;
        const auto releaseMixAlpha = outputState.releaseMixAlpha;
        const auto targetIsUsable = outputState.targetIsUsable;
        const auto committedTargetHz = outputState.committedTargetHz;
        const auto targetControlEventId = outputState.targetControlEventId;
        diagnosticState.highFrequencyEvidence = outputState.highFrequencyEvidence;
        diagnosticState.periodicCoherence = outputState.periodicCoherence;

        for (int n = 0; n < samples; ++n, ++outputReadSample)
        {
            servicePendingGrainsBeforeOutputSample (outputReadSample);
            diagnosticState.pendingDeadlineMissCount = pendingDeadlineMissCount;
            diagnosticState.lateCandidateCreationCount = lateCandidateCreationCount;
            diagnosticState.lateOlaWriteCount = lateOlaWriteCount;
            diagnosticState.earliestLateOlaDestination = earliestLateOlaDestination == std::numeric_limits<uint64_t>::max()
                                                        ? -1.0 : static_cast<double> (earliestLateOlaDestination);
            diagnosticState.newestTargetGrainWeight = 0.0;
            diagnosticState.previousTargetGrainWeight = 0.0;
            diagnosticState.newestTargetFirstAudibleSample = -1.0;
            diagnosticState.previousTargetLastAudibleSample = -1.0;
            const auto outIndex = index (outputReadSample);
            const auto weight = outputWeight[outIndex];
            diagnosticState.currentOverlapWeight = weight;
            if (diagnosticTrackingEnabled && weight > 1.0e-6f)
            {
                diagnosticState.audibleRatioAtStart = outputRatioSum[outIndex] / weight;
                diagnosticState.audibleGrainAge = outputAgeSum[outIndex] / weight;
                diagnosticState.audibleReadPosition = outputReadPositionSum[outIndex] / weight;
                diagnosticState.audibleSourceMarkTime = outputSourceMarkSum[outIndex] / weight;
                diagnosticState.audibleSourceMarkF0 = outputSourceF0Sum[outIndex] / weight;
                diagnosticState.audibleExpectedGrainF0 = outputExpectedF0Sum[outIndex] / weight;
                diagnosticState.dominantGrainWeight = outputDominantWeight[outIndex];
                diagnosticState.dominantGrainEpoch = outputDominantEpoch[outIndex];
                diagnosticState.dominantGrainRatio = outputDominantRatio[outIndex];
                diagnosticState.dominantGrainExpectedF0 = outputDominantExpectedF0[outIndex];
                diagnosticState.dominantGrainTargetMidi = outputDominantTargetMidi[outIndex];
                diagnosticState.dominantGrainTargetEventId = outputDominantTargetEventId[outIndex];
                diagnosticState.dominantGrainControlEventId = outputDominantControlEventId[outIndex];
                diagnosticState.dominantGrainCorrectionGeneration = outputDominantCorrectionGeneration[outIndex];
                diagnosticState.dominantGrainTargetControlTimestamp = outputDominantTargetTimestamp[outIndex];
                diagnosticState.dominantGrainTrajectoryExcursionId = outputDominantTrajectoryExcursionId[outIndex];
                for (const auto& grain : scheduledGrains)
                {
                    if (! grain.active || outputReadSample + static_cast<uint64_t> (grain.halfWindow) < grain.synthesisMark
                        || outputReadSample > grain.synthesisMark + static_cast<uint64_t> (grain.halfWindow))
                        continue;
                    const auto offset = static_cast<int> (static_cast<int64_t> (outputReadSample)
                                                          - static_cast<int64_t> (grain.synthesisMark));
                    const auto phase = static_cast<float> (offset) / static_cast<float> (grain.halfWindow);
                    const auto contributionWeight = 0.5 * (1.0 + std::cos (3.14159265358979323846 * phase));
                    if (grain.controlEventId == targetControlEventId)
                    {
                        diagnosticState.newestTargetGrainWeight += contributionWeight;
                        diagnosticState.newestTargetFirstAudibleSample = static_cast<double> (outputReadSample);
                    }
                    else if (grain.controlEventId < targetControlEventId)
                    {
                        diagnosticState.previousTargetGrainWeight += contributionWeight;
                        diagnosticState.previousTargetLastAudibleSample = static_cast<double> (outputReadSample);
                    }
                }
                if (targetIsUsable)
                {
                    for (const auto& grain : scheduledGrains)
                    {
                        const auto grainHalfWindow = static_cast<uint64_t> (grain.halfWindow);
                        if (! grain.active || outputReadSample + grainHalfWindow < grain.synthesisMark
                            || outputReadSample > grain.synthesisMark + grainHalfWindow)
                            continue;
                        const auto expectedHz = (sampleRate / grain.sourceMark.period) * grain.ratioAtStart;
                        if (std::abs (1200.0 * std::log2 (expectedHz / committedTargetHz)) > 25.0)
                            continue;
                        const auto offset = static_cast<int> (static_cast<int64_t> (outputReadSample)
                                                              - static_cast<int64_t> (grain.synthesisMark));
                        const auto phase = static_cast<float> (offset) / static_cast<float> (grain.halfWindow);
                        const auto candidateWeight = 0.5 * (1.0 + std::cos (3.14159265358979323846 * phase));
                        if (candidateWeight > diagnosticState.targetCandidateWeight)
                        {
                            diagnosticState.targetCandidateWeight = candidateWeight;
                            diagnosticState.targetCandidateEpoch = static_cast<double> (grain.synthesisMark);
                            diagnosticState.targetCandidateExpectedF0 = expectedHz;
                        }
                    }
                }
            }
            const auto dry = readInput (static_cast<int64_t> (outputReadSample) - latency);
            const auto grainAvailable = weight > 1.0e-6f;
            auto wet = grainAvailable ? outputSum[outIndex] / weight : dry;
            outputSum[outIndex] = 0.0f;
            outputWeight[outIndex] = 0.0f;
            if (diagnosticTrackingEnabled)
            {
                outputRatioSum[outIndex] = outputAgeSum[outIndex] = outputReadPositionSum[outIndex] = 0.0;
                outputSourceMarkSum[outIndex] = outputSourceF0Sum[outIndex] = outputExpectedF0Sum[outIndex] = 0.0;
                outputDominantWeight[outIndex] = 0.0f;
                outputDominantSampleContribution[outIndex] = 0.0f;
                outputDominantEpoch[outIndex] = -1.0;
                outputDominantRatio[outIndex] = 1.0;
                outputDominantExpectedF0[outIndex] = 0.0;
                outputDominantTargetMidi[outIndex] = -1;
                outputDominantTargetEventId[outIndex] = -1.0;
                outputDominantControlEventId[outIndex] = -1.0;
                outputDominantCorrectionGeneration[outIndex] = -1.0;
                outputDominantTargetTimestamp[outIndex] = -1.0;
                outputDominantTrajectoryExcursionId[outIndex] = 0.0;
            }

            // Colour only the already-pitch-shifted, periodic wet vowel.
            // This leaves target selection, grain cadence, and the later
            // dry high-band residual completely untouched.
            wet = voicedColour.processWetSample (wet, valid, periodicCoherence,
                                                  sibilanceResidualMix, voicedColourAmountPercent);

            // A voiced signal stays in one continuous processed path.  A
            // note already at target uses ratio == 1, it does not bypass to
            // a separate dry signal.  This keeps the renderer's phase and
            // spectral state continuous as correction starts and stops.
            // A transient may remain formally voiced for a few milliseconds.
            // Do not let its PSOLA grain become audible as a false bright
            // consonant: leave through the existing equal-power dry handoff.
            const auto sibilanceEvidenceSample = static_cast<int64_t> (outputReadSample) - latency
                                               + VIBE_TDPSOLA_SIBILANCE_WET_GATE_LOOKAHEAD_SAMPLES;
            const auto alignedSibilanceEvidence = readSourceHighFrequencyEvidence (
                sibilanceEvidenceSample);
            diagnosticState.highFrequencyEvidence = alignedSibilanceEvidence;
            const auto sibilanceForcesDry = alignedSibilanceEvidence
                                         >= static_cast<double> (VIBE_TDPSOLA_SIBILANCE_WET_GATE_THRESHOLD);
            auto shouldRenderWet = valid && ! forceDry && ! sibilanceForcesDry;
            if (safetyDrySamples > 0)
            {
                diagnosticState.reentryHoldActive = true;
                diagnosticState.dryGuardActive = true;
                --safetyDrySamples;
                shouldRenderWet = false;
            }
            if (forceDry)
                diagnosticState.dryGuardActive = true;

            if (shouldRenderWet && wetMix >= artifactVetoMinimumWetMix && hasPreviousOutput
                && std::abs (wet - previousOutput) > artifactStepThreshold
                && std::abs (dry - previousOutput) < dryContinuityThreshold)
            {
                diagnosticState.artifactDryVeto = true;
            }

            // A voiced stream remains in the processed path, but a wet fade
            // cannot start before an actual OLA grain exists. Otherwise the
            // fade reaches 50–70% while wet == dry, then the first grain
            // enters abruptly and clicks. This is a grain-availability gate
            // for the entry fade, not a near-unity dry bypass.
            const auto grainStarved = shouldRenderWet && ! grainAvailable;
            diagnosticState.grainStarved = grainStarved;
            const auto mixTarget = shouldRenderWet && grainAvailable ? 1.0 : 0.0;
            diagnosticState.shouldRenderWet = shouldRenderWet;
            diagnosticState.remainingSafetyDrySamples = safetyDrySamples;
            // At an invalid/transient boundary the scheduled grain belongs
            // to the preceding vowel.  Leave it quickly for aligned dry;
            // wet re-entry remains the longer equal-power fade.
            wetMix += (mixTarget - wetMix) * (valid ? mixAlpha : releaseMixAlpha);
            const auto equalPowerWet = std::sin (0.5 * 3.14159265358979323846 * wetMix);
            // Attack faster than release: protect the first samples of a
            // consonant, then return gently to the wet vowel path without a
            // treble zipper/click.
            const auto residualSeconds = sibilanceResidualTarget > sibilanceResidualMix
                                       ? sibilanceResidualAttackSeconds
                                       : sibilanceResidualReleaseSeconds;
            const auto residualMixAlpha = 1.0 - std::exp (-1.0 / (residualSeconds * sampleRate));
            sibilanceResidualMix += (sibilanceResidualTarget - sibilanceResidualMix) * residualMixAlpha;
            // The matching low-pass states make this a continuous high-band
            // crossover, rather than summing a full dry vocal over the wet
            // signal.  Its periodic contribution is deliberately capped at
            // a few percent; it restores edge, not source pitch.
            constexpr auto crossoverHz = 2800.0;
            const auto lowAlpha = 1.0 - std::exp (-2.0 * 3.14159265358979323846 * crossoverHz / sampleRate);
            wetLowState += (static_cast<double> (wet) - wetLowState) * lowAlpha;
            dryLowState += (static_cast<double> (dry) - dryLowState) * lowAlpha;
            const auto hybridWet = static_cast<double> (wet)
                                 + (static_cast<double> (dry) - dryLowState - (static_cast<double> (wet) - wetLowState))
                                   * sibilanceResidualMix;
            const auto burstGainSeconds = unvoicedBurstGainTarget < unvoicedBurstGain
                                        ? unvoicedBurstAttackSeconds
                                        : unvoicedBurstReleaseSeconds;
            const auto burstGainAlpha = 1.0 - std::exp (-1.0 / (burstGainSeconds * sampleRate));
            unvoicedBurstGain += (unvoicedBurstGainTarget - unvoicedBurstGain) * burstGainAlpha;
            destination[n] = static_cast<float> ((dry + (hybridWet - dry) * equalPowerWet) * unvoicedBurstGain);
            previousOutput = destination[n];
            hasPreviousOutput = true;

            if (! valid)
                continue;

        }
    }

    // Compatibility contract for the existing equal-span caller. Phase 2 may
    // drive these two responsibilities independently; Phase 1 does not.
    void process (const float* source, float* destination, int samples,
                  double f0Hz, double requestedRatio, bool voiced, bool forceDry = false,
                  double committedTargetHz = std::numeric_limits<double>::quiet_NaN(),
                  double targetResidualCents = 0.0,
                  bool trajectoryExcursionActive = false,
                  uint8_t trajectoryExcursionPhase = 0,
                  int committedTargetMidi = -1,
                  uint64_t targetControlTimestamp = 0,
                  uint64_t targetControlEventId = 0,
                  uint64_t controlPublicationSample = 0) noexcept
    {
        const auto outputState = prepareOutputRenderState (source, samples, f0Hz, requestedRatio, voiced,
                                                           forceDry, committedTargetHz, targetControlEventId);
        ingestAndSchedule (source, samples, f0Hz, requestedRatio, voiced, forceDry,
                           committedTargetHz, targetResidualCents, trajectoryExcursionActive,
                           trajectoryExcursionPhase, committedTargetMidi, targetControlTimestamp,
                           targetControlEventId, controlPublicationSample);
        readOutput (destination, samples, outputState);
    }

private:
    float readInput (int64_t sample) const noexcept
    {
        if (sample < 0 || sample >= static_cast<int64_t> (sourceScheduleSample + 1)
            || sample < static_cast<int64_t> (sourceScheduleSample) - ringLength)
            return 0.0f;
        return input[index (static_cast<uint64_t> (sample))];
    }
    float readSourceHighFrequencyEvidence (int64_t sample) const noexcept
    {
        if (sample < 0 || sample >= static_cast<int64_t> (sourceScheduleSample + 1)
            || sample < static_cast<int64_t> (sourceScheduleSample) - ringLength)
            return 0.0f;
        return sourceHighFrequencyEvidenceRing[index (static_cast<uint64_t> (sample))];
    }
    void pushMark (SourceMark mark) noexcept
    {
        marks[markWrite] = mark;
        markWrite = (markWrite + 1) % marks.size();
        markCount = std::min (markCount + 1, marks.size());
        previousEpoch = mark;
        hasPreviousEpoch = true;
    }
    const SourceMark& latestMark() const noexcept
    {
        return marks[(markWrite + marks.size() - 1) % marks.size()];
    }
    bool findLatestCompleteMark (uint64_t now, SourceMark& result) const noexcept
    {
        // A centred grain has one source period on either side of its mark.
        // Search newest-first so the source trajectory stays close to the
        // current vocal while remaining strictly causal.
        for (size_t age = 0; age < markCount; ++age)
        {
            const auto slot = (markWrite + marks.size() - 1 - age) % marks.size();
            const auto& candidate = marks[slot];
            const auto halfWindow = static_cast<uint64_t> (std::clamp (
                static_cast<int> (std::lround (candidate.period * analysisHalfWindowFactor)), 32, 512));
            if (candidate.sample >= halfWindow && candidate.sample + halfWindow <= now)
            {
                result = candidate;
                return true;
            }
        }
        return false;
    }
    bool findCompleteMarkNearest (double desiredSample, uint64_t now, SourceMark& result) const noexcept
    {
        auto bestDistance = std::numeric_limits<double>::infinity();
        auto found = false;
        for (size_t age = 0; age < markCount; ++age)
        {
            const auto slot = (markWrite + marks.size() - 1 - age) % marks.size();
            const auto& candidate = marks[slot];
            const auto halfWindow = static_cast<uint64_t> (std::clamp (
                static_cast<int> (std::lround (candidate.period * analysisHalfWindowFactor)), 32, 512));
            if (candidate.sample < halfWindow || candidate.sample + halfWindow > now)
                continue;
            const auto distance = std::abs (static_cast<double> (candidate.sample) - desiredSample);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                result = candidate;
                found = true;
            }
        }
        return found;
    }
    static int pitchMarkSearchRadius (double period) noexcept
    {
        return std::clamp (static_cast<int> (std::lround (period * 0.14)), 4, 48);
    }
    uint64_t refineCompletedEpoch (uint64_t predicted, double period) const noexcept
    {
        if (! hasPreviousEpoch)
            return predicted;
        // Select a completed cycle by waveform similarity to the preceding
        // epoch. Unlike a raw zero crossing, this rejects formant-driven
        // polarity changes that would move a TD-PSOLA grain into a wrong
        // cycle. The search is strictly history-only and adds no latency.
        const auto comparisonLength = std::clamp (static_cast<int> (std::lround (std::min (period, previousEpoch.period))),
                                                  32, 192);
        const auto radius = pitchMarkSearchRadius (period);
        auto best = predicted;
        auto bestScore = -2.0;
        // The caller waits until predicted + radius is in history, so both
        // sides of this narrow search are available without extra latency.
        for (int offset = -radius; offset <= radius; ++offset)
        {
            const auto candidate = static_cast<int64_t> (predicted) + offset;
            double dot = 0.0, candidateEnergy = 0.0, previousEnergy = 0.0;
            for (int i = 0; i < comparisonLength; ++i)
            {
                const auto a = static_cast<double> (readInput (candidate - comparisonLength + 1 + i));
                const auto b = static_cast<double> (readInput (static_cast<int64_t> (previousEpoch.sample)
                                                                 - comparisonLength + 1 + i));
                dot += a * b;
                candidateEnergy += a * a;
                previousEnergy += b * b;
            }
            const auto score = dot / std::sqrt (candidateEnergy * previousEnergy + 1.0e-12);
            if (score > bestScore) { bestScore = score; best = static_cast<uint64_t> (candidate); }
        }
        return best;
    }
    SourceMark resolvePitchSynchronousMark (SourceMark mark) const noexcept
    {
        // The F0 lattice tells us the expected cycle; the source waveform
        // tells us where the actual pitch epoch is.  Resolve only after the
        // mark is safely in history, by maximizing one-cycle normalized
        // correlation in a narrow window.  This is phase/pulse detection,
        // not a target or retune change.
        const auto sourcePeriod = std::clamp (static_cast<int> (std::lround (mark.period)), 48, 512);
        const auto searchRadius = std::clamp (static_cast<int> (std::lround (mark.period * 0.16)), 6, 48);
        const auto comparisonLength = std::clamp (sourcePeriod, 48, 192);
        auto bestOffset = 0;
        auto bestScore = -2.0;
        for (int offset = -searchRadius; offset <= searchRadius; ++offset)
        {
            const auto centre = static_cast<int64_t> (mark.sample) + offset;
            double dot = 0.0, currentEnergy = 0.0, priorEnergy = 0.0;
            for (int i = 0; i < comparisonLength; ++i)
            {
                const auto relative = i - comparisonLength / 2;
                const auto current = static_cast<double> (readInput (centre + relative));
                const auto prior = static_cast<double> (readInput (centre - sourcePeriod + relative));
                dot += current * prior;
                currentEnergy += current * current;
                priorEnergy += prior * prior;
            }
            const auto score = dot / std::sqrt (currentEnergy * priorEnergy + 1.0e-12);
            if (score > bestScore) { bestScore = score; bestOffset = offset; }
        }
        // Do not let a breath/formant frame invent a new epoch.  Low
        // confidence stays on the continuous F0 lattice instead.
        if (bestScore >= waveformEpochAnchorMinimumCorrelation)
            mark.sample = static_cast<uint64_t> (std::max<int64_t> (0, static_cast<int64_t> (mark.sample) + bestOffset));
        return mark;
    }
    PitchMarkEvidence measureExcitationEpoch (const SourceMark& mark) const noexcept
    {
        // Use a first-difference excitation proxy, not raw waveform height.
        // Formants can dominate raw correlation, whereas a glottal pulse is
        // more visible in the local excitation slope.  This is measurement
        // only: no audio timing is changed by this function.
        const auto sourcePeriod = std::clamp (static_cast<int> (std::lround (mark.period)), 48, 512);
        const auto searchRadius = std::clamp (static_cast<int> (std::lround (mark.period * 0.12)), 4, 32);
        const auto comparisonLength = std::clamp (sourcePeriod, 48, 160);
        PitchMarkEvidence result {};
        auto bestScore = -2.0;
        for (int offset = -searchRadius; offset <= searchRadius; ++offset)
        {
            const auto centre = static_cast<int64_t> (mark.sample) + offset;
            double dot = 0.0, currentEnergy = 0.0, previousEnergy = 0.0;
            for (int i = 0; i < comparisonLength; ++i)
            {
                const auto relative = i - comparisonLength / 2;
                const auto currentIndex = centre + relative;
                const auto previousIndex = currentIndex - sourcePeriod;
                const auto current = static_cast<double> (readInput (currentIndex) - readInput (currentIndex - 1));
                const auto previous = static_cast<double> (readInput (previousIndex) - readInput (previousIndex - 1));
                dot += current * previous;
                currentEnergy += current * current;
                previousEnergy += previous * previous;
            }
            const auto score = dot / std::sqrt (currentEnergy * previousEnergy + 1.0e-12);
            if (score > bestScore)
            {
                bestScore = score;
                result.offsetSamples = static_cast<double> (offset);
            }
        }
        result.periodicCorrelation = bestScore;
        return result;
    }
    double estimateFractionalPhaseOffset (const SourceMark& mark) const noexcept
    {
        if (! hasPreviousSelectedSourceMark)
            return 0.0;
        const auto rawDistance = static_cast<double> (mark.sample)
                               - static_cast<double> (previousSelectedSourceMark.sample);
        if (std::abs (rawDistance) < mark.period * 0.30)
            return previousFractionalPhaseOffset;
        const auto comparisonLength = std::clamp (static_cast<int> (std::lround (
            std::min (mark.period, previousSelectedSourceMark.period))), 48, 192);
        constexpr auto phaseSearchRadius = 0;
        std::array<double, 2 * phaseSearchRadius + 1> scores {};
        auto bestIndex = phaseSearchRadius;
        auto bestScore = -2.0;
        for (int offset = -phaseSearchRadius; offset <= phaseSearchRadius; ++offset)
        {
            double dot = 0.0, candidateEnergy = 0.0, previousEnergy = 0.0;
            for (int i = 0; i < comparisonLength; ++i)
            {
                const auto a = static_cast<double> (readInput (static_cast<int64_t> (mark.sample) + offset
                                                                 - comparisonLength / 2 + i));
                const auto b = static_cast<double> (readInput (static_cast<int64_t> (previousSelectedSourceMark.sample)
                                                                 - comparisonLength / 2 + i));
                dot += a * b;
                candidateEnergy += a * a;
                previousEnergy += b * b;
            }
            const auto score = dot / std::sqrt (candidateEnergy * previousEnergy + 1.0e-12);
            scores[static_cast<size_t> (offset + phaseSearchRadius)] = score;
            if (score > bestScore) { bestScore = score; bestIndex = offset + phaseSearchRadius; }
        }
        auto fractional = 0.0;
        if (bestIndex > 0 && bestIndex + 1 < static_cast<int> (scores.size()))
        {
            const auto left = scores[static_cast<size_t> (bestIndex - 1)];
            const auto centre = scores[static_cast<size_t> (bestIndex)];
            const auto right = scores[static_cast<size_t> (bestIndex + 1)];
            const auto denominator = left - 2.0 * centre + right;
            if (std::abs (denominator) > 1.0e-9)
                fractional = std::clamp (0.5 * (left - right) / denominator, -0.5, 0.5);
        }
        return static_cast<double> (bestIndex - phaseSearchRadius) + fractional;
    }
    double estimateOverlapPhaseOffset (const SourceMark& mark, uint64_t synthesisMark) const noexcept
    {
        // Align a new grain to the actual waveform already scheduled in its
        // OLA overlap. Unlike source-to-source correlation, this measures the
        // signal the listener will receive and cannot alter output cadence.
        constexpr int searchRadius = 4;
        const auto halfWindow = std::clamp (static_cast<int> (std::lround (mark.period * analysisHalfWindowFactor)), 32, 512);
        auto bestOffset = 0;
        auto bestScore = -2.0;
        for (int sourceOffset = -searchRadius; sourceOffset <= searchRadius; ++sourceOffset)
        {
            double dot = 0.0, candidateEnergy = 0.0, existingEnergy = 0.0;
            auto count = 0;
            for (int relative = -halfWindow + searchRadius; relative <= halfWindow - searchRadius; relative += 4)
            {
                const auto destination = static_cast<int64_t> (synthesisMark) + relative;
                if (destination < static_cast<int64_t> (sourceScheduleSample) + 1)
                    continue;
                const auto destinationIndex = index (static_cast<uint64_t> (destination));
                const auto existingWeight = outputWeight[destinationIndex];
                if (existingWeight < 0.10f)
                    continue;
                const auto phase = static_cast<float> (relative) / static_cast<float> (halfWindow);
                const auto window = 0.5f * (1.0f + std::cos (3.14159265358979323846f * phase));
                const auto candidate = static_cast<double> (readInputInterpolated (
                    static_cast<double> (static_cast<int64_t> (mark.sample) + relative) + sourceOffset)) * window;
                const auto existing = static_cast<double> (outputSum[destinationIndex] / existingWeight);
                dot += candidate * existing;
                candidateEnergy += candidate * candidate;
                existingEnergy += existing * existing;
                ++count;
            }
            if (count < 16)
                continue;
            const auto score = dot / std::sqrt (candidateEnergy * existingEnergy + 1.0e-12);
            if (score > bestScore)
            {
                bestScore = score;
                bestOffset = sourceOffset;
            }
        }
        // Do not invent alignment where the future OLA contains no reliable
        // comparison signal (such as the first grain after re-entry).
        return bestScore >= 0.72 ? static_cast<double> (bestOffset) : 0.0;
    }
    float readInputInterpolated (double sample) const noexcept
    {
        const auto first = static_cast<int64_t> (std::floor (sample));
        const auto fraction = static_cast<float> (sample - static_cast<double> (first));
        return readInput (first) + (readInput (first + 1) - readInput (first)) * fraction;
    }

    double tailReplacementGain (const ScheduledGrain& grain, uint64_t destination) const noexcept
    {
        if (destination < grain.tailReplacementFirstSample)
            return 1.0;
        if (destination >= grain.tailReplacementFadeEndSample)
            return grain.tailReplacementFinalGain;
        const auto length = std::max<uint64_t> (1, grain.tailReplacementFadeEndSample
                                                    - grain.tailReplacementFirstSample);
        const auto phase = static_cast<double> (destination - grain.tailReplacementFirstSample)
                         / static_cast<double> (length);
        // Equal-power descent keeps the old component continuous while the
        // new target-owned grain takes over in the ordinary OLA schedule.
        return 1.0 + (grain.tailReplacementFinalGain - 1.0) * std::sin (0.5 * 3.14159265358979323846 * phase);
    }

    void applyTargetAwareTailReplacement (int newTargetMidi) noexcept
    {
        const auto now = sourceScheduleSample + 1;
        for (auto& grain : scheduledGrains)
        {
            if (! grain.active || grain.targetMidi < 0 || grain.targetMidi == newTargetMidi)
                continue;
            const auto grainEnd = grain.synthesisMark + static_cast<uint64_t> (grain.halfWindow);
            if (grainEnd <= now)
                continue;
            const auto oldFirst = grain.tailReplacementFirstSample;
            const auto oldFadeEnd = grain.tailReplacementFadeEndSample;
            const auto oldFinalGain = grain.tailReplacementFinalGain;
            const auto onset = static_cast<uint64_t> (std::lround (
                std::max (0.0, diagnosticAblation.targetAwareTailOnsetHalfWindows) * grain.halfWindow));
            const auto first = std::min (grainEnd, now + onset);
            const auto fade = std::max<uint64_t> (1, static_cast<uint64_t> (std::lround (
                std::max (0.0, diagnosticAblation.targetAwareTailFadeHalfWindows) * grain.halfWindow)));
            grain.tailReplacementFirstSample = first;
            grain.tailReplacementFadeEndSample = std::min (grainEnd, first + fade);
            grain.tailReplacementFinalGain = std::clamp (diagnosticAblation.targetAwareOldTailGain, 0.0, 1.0);
            // Contributions were queued at gain 1.  Rewrite only samples
            // that are still in the future; source and synthesis phases are
            // left untouched.
            for (auto destination = first; destination <= grainEnd; ++destination)
            {
                const auto offset = static_cast<int> (static_cast<int64_t> (destination)
                                                     - static_cast<int64_t> (grain.synthesisMark));
                const auto phase = static_cast<float> (offset) / static_cast<float> (grain.halfWindow);
                const auto weight = 0.5 * (1.0 + std::cos (3.14159265358979323846 * phase));
                const auto previousGain = oldFirst == std::numeric_limits<uint64_t>::max() || destination < oldFirst
                                            ? 1.0
                                            : destination >= oldFadeEnd ? oldFinalGain
                                            : 1.0 + (oldFinalGain - 1.0) * std::sin (0.5 * 3.14159265358979323846
                                                * static_cast<double> (destination - oldFirst)
                                                / static_cast<double> (std::max<uint64_t> (1, oldFadeEnd - oldFirst)));
                const auto gainDelta = tailReplacementGain (grain, destination) - previousGain;
                if (std::abs (gainDelta) < 1.0e-12)
                    continue;
                const auto slot = index (destination);
                const auto sample = readInputInterpolated (static_cast<double> (static_cast<int64_t> (grain.sourceMark.sample)
                                                                                 + offset) + grain.fractionalPhaseOffset);
                // OLA's denominator is an availability/normalisation term,
                // not an artistic gain.  Reducing it created artificial
                // grain starvation in the first experiment.  Keep it and
                // all ownership moments intact; this candidate only removes
                // the old waveform from the numerator while a new-target
                // grain is already entering the ordinary OLA schedule.
                outputSum[slot] += static_cast<float> (sample * weight * gainDelta);
            }
            diagnosticState.shadowCandidateRenderApplied = true;
            diagnosticState.shadowReplacementEligible = true;
            diagnosticState.shadowReplacementFirstSample = static_cast<double> (first);
            diagnosticState.shadowReplacementLastSample = static_cast<double> (grainEnd);
        }
    }

    void crossReplaceOldTailWithNewGrain (const ScheduledGrain& fresh) noexcept
    {
        const auto targetTransaction = diagnosticAblation.targetAwareTailReplacement && fresh.targetMidi >= 0
                                    && pendingTailReplacementActive && fresh.targetEventId == pendingTailReplacementEventId;
        const auto trajectoryTransaction = diagnosticAblation.trajectoryExcursionTailReplacement
                                        && fresh.trajectoryExcursionId != 0
                                        && pendingTrajectoryTailReplacementActive
                                        && fresh.trajectoryExcursionId == pendingTrajectoryTailReplacementId;
        if (! targetTransaction && ! trajectoryTransaction)
            return;
        const auto first = std::max<uint64_t> (sourceScheduleSample + 1,
                                               fresh.synthesisMark > static_cast<uint64_t> (fresh.halfWindow)
                                                 ? fresh.synthesisMark - fresh.halfWindow : 0);
        const auto last = fresh.synthesisMark + static_cast<uint64_t> (fresh.halfWindow);
        for (auto destination = first; destination <= last; ++destination)
        {
            const auto slot = index (destination);
            const auto oldEvent = outputDominantTargetEventId[slot];
            const auto oldWeight = static_cast<double> (outputDominantWeight[slot]);
            const auto oldTrajectory = outputDominantTrajectoryExcursionId[slot];
            if (oldWeight <= 1.0e-9
                || (targetTransaction && (oldEvent < 0.0 || static_cast<uint64_t> (oldEvent) == fresh.targetEventId))
                || (trajectoryTransaction && oldTrajectory == static_cast<double> (fresh.trajectoryExcursionId)))
                continue;
            const auto onset = static_cast<uint64_t> (std::lround (std::max (0.0,
                diagnosticAblation.targetAwareTailOnsetHalfWindows) * fresh.halfWindow));
            const auto fade = std::max<uint64_t> (1, static_cast<uint64_t> (std::lround (std::max (0.0,
                diagnosticAblation.targetAwareTailFadeHalfWindows) * fresh.halfWindow)));
            const auto fadeFirst = std::min (last, first + onset);
            const auto fadeLast = std::min (last, fadeFirst + fade);
            if (destination < fadeFirst)
                continue;
            const auto progress = destination >= fadeLast ? 1.0 : static_cast<double> (destination - fadeFirst)
                    / static_cast<double> (std::max<uint64_t> (1, fadeLast - fadeFirst));
            const auto transferShape = std::sin (0.5 * 3.14159265358979323846 * progress);
            const auto freshOffset = static_cast<int> (static_cast<int64_t> (destination) - static_cast<int64_t> (fresh.synthesisMark));
            const auto freshWeight = 0.5 * (1.0 + std::cos (3.14159265358979323846 * freshOffset / fresh.halfWindow));
                // Transfer a bounded share of existing old weight to the
                // new grain. Denominator stays invariant exactly.
            const auto transfer = std::min (oldWeight, freshWeight) * transferShape
                                    * (1.0 - std::clamp (diagnosticAblation.targetAwareOldTailGain, 0.0, 1.0));
            if (transfer <= 1.0e-9)
                continue;
            const auto oldSample = outputDominantSampleContribution[slot] / oldWeight;
            const auto freshSample = readInputInterpolated (static_cast<double> (static_cast<int64_t> (fresh.sourceMark.sample) + freshOffset)
                                                                 + fresh.fractionalPhaseOffset);
            outputSum[slot] += static_cast<float> ((freshSample - oldSample) * transfer);
            diagnosticState.shadowCandidateRenderApplied = true;
            diagnosticState.shadowReplacementEligible = true;
            diagnosticState.shadowReplacementFirstSample = static_cast<double> (fadeFirst);
            diagnosticState.shadowReplacementLastSample = static_cast<double> (last);
        }
        // One first-new grain owns one replacement transaction. Subsequent
        // grains of the same target retain normal OLA behaviour.
        if (targetTransaction)
            pendingTailReplacementActive = false;
        if (trajectoryTransaction)
            pendingTrajectoryTailReplacementActive = false;
    }

    // Planner ownership is source-time only. Render tickets are a separate
    // bounded queue: output may consume a ticket but cannot remove or alter
    // an epoch from this array.
    bool enqueueRetimableEpoch (const PendingGrain& epoch) noexcept
    {
        for (auto& slot : retimableEpochPlanner)
            if (! slot.active)
            {
                slot = epoch;
                ++retimablePlannerCount;
                retimableEpochPeakOccupancy = std::max (retimableEpochPeakOccupancy, retimablePlannerCount);
                return true;
            }
        return false;
    }

    void observeSourcePlannerClock (uint64_t sourceNow) noexcept
    {
        if (outputReadSample >= sourceNow)
            maximumOutputAheadAtSourcePlanner = std::max (maximumOutputAheadAtSourcePlanner,
                                                           outputReadSample - sourceNow);
        if (hasPreviousSourcePlannerObservation && outputReadSample >= previousSourcePlannerSample)
            maximumOutputAdvanceBetweenSourcePlanners = std::max (
                maximumOutputAdvanceBetweenSourcePlanners,
                outputReadSample - previousSourcePlannerSample);
        previousSourcePlannerSample = sourceNow;
        hasPreviousSourcePlannerObservation = true;
    }

    void retireRetimableEpochsAtSourceTime (uint64_t now) noexcept
    {
        if (! diagnosticAblation.retimableEpochs)
            return;
        for (auto& epoch : retimableEpochPlanner)
            if (epoch.active && epoch.sealed && epoch.deadline < now)
            {
                plannerLastIrreversibleMark = std::max (plannerLastIrreversibleMark, epoch.synthesisMark);
                epoch.active = false;
                --retimablePlannerCount;
            }
    }

    void sealRetimableEpochsAtSourceTime (uint64_t sourceNow) noexcept
    {
        for (auto& epoch : retimableEpochPlanner)
            if (epoch.active && ! epoch.sealed && epoch.deadline <= sourceNow + plannerHandoffMarginSamples)
            {
                epoch.sealed = true;
                epoch.sealSample = sourceNow;
                epoch.ticketIssueSample = sourceNow;
                auto ticket = epoch; // immutable timing/control snapshot
                if (ticket.deadline < outputReadSample)
                    ++ticketIssuedPastDeadlineCount;
                if (! enqueuePendingGrain (ticket))
                {
                    ++pendingQueueOverflowCount;
                    continue;
                }
                if (epochTraceEnabled)
                    for (int i = 0; i < epochTraceCount; ++i)
                        if (epochTraces[static_cast<size_t> (i)].pendingCandidateId == epoch.id)
                        {
                            auto& trace = epochTraces[static_cast<size_t> (i)];
                            trace.sealSample = epoch.sealSample;
                            trace.ticketIssueSample = epoch.ticketIssueSample;
                            trace.synthesisMark = epoch.synthesisMark;
                            trace.firstAudible = epoch.deadline;
                            trace.controlTimestamp = epoch.targetTimestamp;
                            trace.controlPublicationSample = epoch.publicationSample;
                            trace.targetEventId = epoch.targetEventId;
                            trace.ratio = epoch.ratio;
                            trace.retimed = epoch.retimed;
                            trace.retimeSamples = epoch.retimeSamples;
                            break;
                        }
            }
    }

    // Rebuilds only the not-yet-audible portion of the epoch chain.  The
    // queued grain owns no OLA samples until finalization, so moving this
    // mark is a metadata operation rather than an OLA rewrite.
    void rebuildRetimableEpochChain() noexcept
    {
        if (! diagnosticAblation.retimableEpochs)
            return;

        std::array<int, pendingGrainCapacity> order {};
        int count = 0;
        for (int i = 0; i < pendingGrainCapacity; ++i)
            if (retimableEpochPlanner[static_cast<size_t> (i)].active)
                order[static_cast<size_t> (count++)] = i;
        std::sort (order.begin(), order.begin() + count, [this] (int a, int b)
        {
            return retimableEpochPlanner[static_cast<size_t> (a)].candidateSourceTime
                 < retimableEpochPlanner[static_cast<size_t> (b)].candidateSourceTime;
        });
        if (count == 0)
            return;

        auto& first = retimableEpochPlanner[static_cast<size_t> (order[0])];
        uint64_t previousMark = first.chainPredecessorMark;
        if (previousMark == 0)
            previousMark = plannerLastIrreversibleMark;
        if (previousMark == 0)
            previousMark = first.provisionalSynthesisMark - static_cast<uint64_t> (std::llround (
                std::max (1.0, first.sourceMark.period / std::max (0.50, first.ratio))));
        for (int pass = 0; pass < 2; ++pass)
        {
            uint64_t chainMark = previousMark;
            for (int n = 0; n < count; ++n)
            {
                auto& epoch = retimableEpochPlanner[static_cast<size_t> (order[static_cast<size_t> (n)])];
                epoch.chainPredecessorMark = chainMark;
                if (epoch.sealed)
                {
                    chainMark = epoch.synthesisMark;
                    continue;
                }
                const auto halfWindow = static_cast<uint64_t> (std::clamp (
                    static_cast<int> (std::lround (epoch.sourceMark.period * epoch.windowFactor)), 32, 512));
                PublishedControl best {};
                auto found = false;
                // Eligibility is evaluated at this epoch's audible edge,
                // not at the callback or candidate decision sample.
                const auto currentDeadline = epoch.synthesisMark > halfWindow ? epoch.synthesisMark - halfWindow : 0;
                for (const auto& control : publishedControls)
                    if (control.timestamp <= epoch.candidateSourceTime
                        && control.publicationSample <= currentDeadline
                        && (! found || control.timestamp > best.timestamp))
                    { best = control; found = true; }
                if (found)
                {
                    epoch.ratio = best.ratio;
                    epoch.targetTimestamp = best.timestamp;
                    epoch.publicationSample = best.publicationSample;
                    epoch.targetEventId = best.targetEventId;
                    epoch.targetMidi = best.targetMidi;
                }

                uint64_t proposed = epoch.synthesisMark;
                if (chainMark != 0)
                {
                    const auto period = std::max (1.0, epoch.sourceMark.period / std::max (0.50, epoch.ratio));
                    proposed = chainMark + static_cast<uint64_t> (std::llround (period));
                }
                // The source-to-output latency mapping is immutable.  A
                // retime may delay a future epoch to preserve its rebuilt
                // lattice, but may never pull it ahead of the provisional
                // `candidate source + latency` mark.
                proposed = std::max (proposed, epoch.provisionalSynthesisMark);
                if (chainMark != 0 && proposed <= chainMark)
                {
                    ++epochOrderViolationCount;
                    proposed = chainMark + 1;
                }
                chainMark = proposed;
                if (pass == 1 && proposed != epoch.synthesisMark)
                {
                    const auto delta = static_cast<int64_t> (proposed) - static_cast<int64_t> (epoch.synthesisMark);
                    epoch.synthesisMark = proposed;
                    epoch.deadline = proposed - halfWindow;
                    epoch.retimed = true;
                    epoch.retimeSamples += delta;
                    ++epochRetimeCount;
                    const auto magnitude = static_cast<uint64_t> (delta < 0 ? -delta : delta);
                    retimedSamplesTotal += magnitude;
                    maxSingleEpochRetime = std::max (maxSingleEpochRetime, magnitude);
                    updateEpochTraceForPending (epoch);
                }
            }
        }
    }

    void updateEpochTraceForPending (const PendingGrain& pending) noexcept
    {
        if (! epochTraceEnabled)
            return;
        for (int i = 0; i < epochTraceCount; ++i)
            if (epochTraces[static_cast<size_t> (i)].pendingCandidateId == pending.id)
            {
                auto& trace = epochTraces[static_cast<size_t> (i)];
                trace.synthesisMark = pending.synthesisMark;
                trace.firstAudible = pending.deadline;
                trace.controlTimestamp = pending.targetTimestamp;
                trace.controlPublicationSample = pending.publicationSample;
                trace.targetEventId = pending.targetEventId;
                trace.ratio = pending.ratio;
                trace.retimed = pending.retimed;
                trace.retimeSamples = pending.retimeSamples;
                break;
            }
    }

    bool enqueuePendingGrain (const PendingGrain& pending) noexcept
    {
        for (auto& slot : pendingGrains)
            if (! slot.active)
            {
                slot = pending;
                ++pendingGrainCount;
                pendingQueuePeakOccupancy = std::max (pendingQueuePeakOccupancy, pendingGrainCount);
                return true;
            }
        return false;
    }

    void finalizePendingGrain (const PendingGrain& pending, uint64_t now) noexcept
    {
        if (diagnosticAblation.retimableEpochs && pending.deadline < now)
            ++retimeDeadlineMissCount;
        addCentredGrain (pending.sourceMark, pending.synthesisMark, pending.ratio,
                         pending.phaseOffset, pending.windowFactor, pending.emphasis,
                         pending.targetMidi, pending.targetEventId, pending.targetTimestamp,
                         pending.controlEventId, pending.generation, pending.excursionId);
        lastPendingCandidateId = static_cast<double> (pending.id);
        lastPendingFinalizedAt = static_cast<double> (now);
        lastPendingFinalizationKind = now >= pending.publicationSample && now < pending.deadline ? 1.0 : 2.0;
        if (epochTraceEnabled)
            for (int i = 0; i < epochTraceCount; ++i)
                if (epochTraces[static_cast<size_t> (i)].pendingCandidateId == pending.id)
                {
                    auto& trace = epochTraces[static_cast<size_t> (i)];
                    trace.pendingFinalizationSample = now;
                    trace.controlTimestamp = pending.targetTimestamp;
                    trace.controlPublicationSample = pending.publicationSample;
                    trace.targetEventId = pending.targetEventId;
                    trace.ratio = pending.ratio;
                    trace.retimed = pending.retimed;
                    trace.retimeSamples = pending.retimeSamples;
                    trace.finalized = true;
                    break;
                }
    }

    // Source scheduling creates candidates, but cannot own an audible OLA
    // deadline once input and output clocks are decoupled.  This bounded
    // service runs immediately before the current output slot is consumed.
    void servicePendingGrainsBeforeOutputSample (uint64_t now) noexcept
    {
        for (auto& slot : pendingGrains)
        {
            if (! slot.active)
                continue;
            // Re-resolve from all materialized controls. Timestamp alone is
            // insufficient: a frame is eligible only once its causal
            // publication point has passed.
            if (! diagnosticAblation.retimableEpochs)
            {
                PublishedControl best {};
                auto found = false;
                for (const auto& control : publishedControls)
                    if (control.publicationSample <= now && control.timestamp <= slot.candidateSourceTime
                        && (! found || control.timestamp > best.timestamp))
                    { best = control; found = true; }
                if (found && best.timestamp > slot.targetTimestamp)
                {
                    slot.ratio = best.ratio;
                    slot.targetTimestamp = best.timestamp;
                    slot.targetEventId = best.targetEventId;
                    slot.targetMidi = best.targetMidi;
                    slot.publicationSample = best.publicationSample;
                }
            }
            if (slot.deadline < now)
            {
                ++pendingDeadlineMissCount;
                finalizePendingGrain (slot, now);
                slot.active = false;
                --pendingGrainCount;
                continue;
            }
            if (slot.deadline == now)
            {
                // This is the latest legal instant: add the contribution
                // before readOutput normalizes and clears this exact slot.
                finalizePendingGrain (slot, now);
                slot.active = false;
                --pendingGrainCount;
            }
        }
    }

    // Source scheduling owns this ledger at candidate creation.  In
    // particular it may not be updated by pending-grain finalization from
    // the output clock, because a host may consume an output-only partial
    // callback before the next 128-sample source quantum is ingested.
    void recordGrainTiming (const SourceMark& mark, uint64_t synthesisMark, double ratioAtStart) noexcept
    {
        const auto currentSynthesisMark = static_cast<double> (synthesisMark);
        const auto expectedPeriod = mark.period / ratioAtStart;
        lastPreviousSynthesisMark = previousSynthesisMark;
        lastActualSynthesisPeriod = previousSynthesisMark >= 0.0 ? currentSynthesisMark - previousSynthesisMark : 0.0;
        lastExpectedSynthesisPeriodError = previousSynthesisMark >= 0.0 ? lastActualSynthesisPeriod - expectedPeriod : 0.0;
        lastSourceMarkSpacing = previousSourceMarkSample >= 0.0 ? static_cast<double> (mark.sample) - previousSourceMarkSample : 0.0;
        previousSynthesisMark = currentSynthesisMark;
        previousSourceMarkSample = static_cast<double> (mark.sample);
        lastSpawnSourcePeriod = mark.period;
        lastExpectedOutputPeriod = expectedPeriod;
    }

    void addCentredGrain (const SourceMark& mark, uint64_t synthesisMark, double ratioAtStart,
                          double fractionalPhaseOffset, double windowHalfFactor,
                          bool transitionBodyEmphasis, int targetMidi,
                          uint64_t targetEventId, uint64_t targetControlTimestamp,
                          uint64_t controlEventId,
                          uint64_t grainCorrectionGeneration,
                          uint64_t trajectoryExcursionId) noexcept
    {
        const auto halfWindow = std::clamp (static_cast<int> (std::lround (mark.period * windowHalfFactor)), 32, 512);
        // A staged source quantum must still arrive before the first sample
        // of its grain becomes audible.  Count rather than compensate: a
        // latency increase would conceal a broken causal schedule.
        if (synthesisMark < outputReadSample + static_cast<uint64_t> (halfWindow))
            ++olaDeadlineViolationCount;
        ScheduledGrain grain { mark, synthesisMark, ratioAtStart, fractionalPhaseOffset, halfWindow,
                              targetMidi, targetEventId, targetControlTimestamp, controlEventId,
                              grainCorrectionGeneration,
                              trajectoryExcursionId,
                              std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), 1.0, true };
        // Keep the transparent Hann window for unshifted/near-unity audio.
        // In a real hard correction, blend only a small Hamming shoulder into
        // the new grain.  Its non-zero edge retains more of the grain body
        // (therefore harmonic density) while OLA normalisation below still
        // makes the sum continuous.  This is deliberately a per-grain,
        // ratio-at-spawn decision: no window is changing underneath an
        // audible grain and no EQ/dry copy is added.
        rememberScheduledGrain (grain);
        // Read the ledger before this grain overwrites the previous event's
        // dominant contribution at overlapping future samples.
        crossReplaceOldTailWithNewGrain (grain);
        for (int offset = -halfWindow; offset <= halfWindow; ++offset)
            addGrainSampleContribution (grain, offset, 1.0, transitionBodyEmphasis);
    }

    void addGrainSampleContribution (const ScheduledGrain& grain, int offset, double gain,
                                     bool transitionBodyEmphasis = false) noexcept
    {
        const auto destination = static_cast<int64_t> (grain.synthesisMark) + offset;
        if (destination < static_cast<int64_t> (outputReadSample))
        {
            ++lateOlaWriteCount;
            if (earliestLateOlaDestination == std::numeric_limits<uint64_t>::max())
                earliestLateOlaDestination = destination >= 0 ? static_cast<uint64_t> (destination) : 0;
            return;
        }
        const auto phase = static_cast<float> (offset) / static_cast<float> (grain.halfWindow);
        const auto cosine = std::cos (3.14159265358979323846f * phase);
        const auto hann = 0.5f * (1.0f + cosine);
        const auto correctionCents = std::abs (1200.0 * std::log2 (std::max (grain.ratioAtStart, 1.0e-12)));
        const auto hammingBlend = diagnosticAblation.correctionWindowBlend
                                ? std::clamp ((correctionCents - 6.0) / 30.0,
                                              0.0, transitionBodyEmphasis
                                                        ? rapidTransitionWindowMaximumHammingBlend
                                                        : hardTuneWindowMaximumHammingBlend)
                                : 0.0;
        const auto targetAwareBlend = diagnosticAblation.targetAwareTailReplacement && transitionBodyEmphasis
                                    ? std::clamp (diagnosticAblation.targetAwareNewGrainHammingBlend, 0.0, 1.0)
                                    : 0.0;
        const auto hamming = 0.54f + 0.46f * cosine;
        const auto weight = static_cast<double> (hann + (hamming - hann) * std::max (hammingBlend, targetAwareBlend)) * gain;
        const auto markF0 = sampleRate / grain.sourceMark.period;
        const auto sample = readInputInterpolated (static_cast<double> (static_cast<int64_t> (grain.sourceMark.sample)
                                                                        + offset)
                                                   + grain.fractionalPhaseOffset);
        const auto destinationIndex = index (static_cast<uint64_t> (destination));
        const auto numeratorContribution = static_cast<double> (sample) * weight;
        outputSum[destinationIndex] += static_cast<float> (numeratorContribution);
        outputWeight[destinationIndex] += static_cast<float> (weight);
        if (olaContributionTraceEnabled
            && destination >= static_cast<int64_t> (olaContributionTraceFirstSample)
            && destination <= static_cast<int64_t> (olaContributionTraceLastSample)
            && olaContributionTraces.size() < olaContributionTraces.capacity())
        {
            olaContributionTraces.push_back ({ grain.synthesisMark, grain.targetEventId,
                                               grain.targetControlTimestamp, grain.correctionGeneration,
                                               grain.synthesisMark, static_cast<uint64_t> (grain.halfWindow),
                                               static_cast<uint64_t> (destination), grain.sourceMark.sample,
                                               grain.ratioAtStart, numeratorContribution, weight });
        }
        if (diagnosticTrackingEnabled)
        {
            outputRatioSum[destinationIndex] += grain.ratioAtStart * weight;
            outputAgeSum[destinationIndex] += static_cast<double> (offset + grain.halfWindow) * weight;
            outputReadPositionSum[destinationIndex]
                += static_cast<double> (static_cast<int64_t> (grain.sourceMark.sample) + offset) * weight;
            outputSourceMarkSum[destinationIndex] += static_cast<double> (grain.sourceMark.sample) * weight;
            outputSourceF0Sum[destinationIndex] += markF0 * weight;
            outputExpectedF0Sum[destinationIndex] += markF0 * grain.ratioAtStart * weight;
            if (weight > outputDominantWeight[destinationIndex])
            {
                outputDominantWeight[destinationIndex] = static_cast<float> (weight);
                outputDominantSampleContribution[destinationIndex] = sample * static_cast<float> (weight);
                outputDominantEpoch[destinationIndex] = static_cast<double> (grain.synthesisMark);
                outputDominantRatio[destinationIndex] = grain.ratioAtStart;
                outputDominantExpectedF0[destinationIndex] = markF0 * grain.ratioAtStart;
                outputDominantTargetMidi[destinationIndex] = grain.targetMidi;
                outputDominantTargetEventId[destinationIndex] = static_cast<double> (grain.targetEventId);
                outputDominantControlEventId[destinationIndex] = static_cast<double> (grain.controlEventId);
                outputDominantCorrectionGeneration[destinationIndex] = static_cast<double> (grain.correctionGeneration);
                outputDominantTargetTimestamp[destinationIndex] = static_cast<double> (grain.targetControlTimestamp);
                outputDominantTrajectoryExcursionId[destinationIndex] = static_cast<double> (grain.trajectoryExcursionId);
            }
        }
    }


    void rememberScheduledGrain (const ScheduledGrain& grain) noexcept
    {
        // This is a future-event queue, not merely diagnostic history. An
        // event remains addressable until every one of its contributions has
        // become audible, which is the prerequisite for a later, safe
        // source/synthesis reschedule.
        for (size_t attempt = 0; attempt < scheduledGrains.size(); ++attempt)
        {
            auto& slot = scheduledGrains[scheduledGrainWrite];
            scheduledGrainWrite = (scheduledGrainWrite + 1) % scheduledGrains.size();
            if (! slot.active)
            {
                slot = grain;
                return;
            }
        }
    }

    void retireAudibleScheduledGrains() noexcept
    {
        for (auto& grain : scheduledGrains)
            if (grain.active
                && grain.synthesisMark + static_cast<uint64_t> (grain.halfWindow) < outputReadSample)
                grain.active = false;
    }

    int countScheduledFutureGrains() const noexcept
    {
        auto count = 0;
        for (const auto& grain : scheduledGrains)
            if (grain.active && grain.synthesisMark > sourceScheduleSample)
                ++count;
        return count;
    }

    // Keep the existing guard; give the first certified wet grain a longer
    // equal-power entry so its phase settles under the dry vocal instead of
    // exposing a single onset tick at the end of the fade.
    static constexpr double crossfadeSeconds = 0.010;
    // A voiced-to-dry exit can cut across the tail of a PSOLA grain. Keep
    // this independent from the 10.7 ms wet entry: 7 ms removes the end-word
    // tick without delaying re-entry or changing control semantics.
    static constexpr double releaseCrossfadeSeconds = 0.007;
    // Golden baseline profile: this is the renderer behaviour verified by
    // VibeAutotune-Live-Preview-hook2-preset.wav for the 0/0/0 preset.
    static constexpr double safetyHoldSeconds = 0.0120;
    static constexpr double rapidReversalThresholdCents = 40.0;
    static constexpr int rapidTakeoverLeadSamples = 8;
    static constexpr double rapidReturnMinimumF0Hz = 180.0;
    static constexpr uint64_t rapidReturnMaximumSamples = 9600; // 200 ms at 48 kHz
    static constexpr double schedulerTraceThresholdCents = 18.0;
    static constexpr float artifactStepThreshold = 0.040f;
    static constexpr float dryContinuityThreshold = 0.060f;
    // Never veto the initial dry -> wet entry against a previous dry sample.
    // That comparison is structurally phase-different and otherwise traps
    // the renderer in an endless safety-hold loop.
    static constexpr double artifactVetoMinimumWetMix = 0.35;
    static constexpr double minimumPitchMarkCorrelation = 0.55;
    static constexpr double waveformEpochAnchorMinimumCorrelation = 0.82;
    static constexpr double sibilanceResidualAttackSeconds = 0.0005;
    static constexpr double sibilanceResidualReleaseSeconds = 0.004;
    static constexpr double periodicResidualMinimumCoherence = 0.90;
    static constexpr double periodicResidualMaximumMix = 0.0;
    static constexpr double unvoicedBurstRiseThresholdDb = 3.0;
    static constexpr double unvoicedBurstRatio = 1.5;
    static constexpr double unvoicedBurstMaximumAttenuationDb = 6.0;
    static constexpr double unvoicedBurstMaximumHighFrequencyEvidence = 0.25;
    static constexpr double unvoicedBurstFloorFallAlpha = 0.35;
    static constexpr double unvoicedBurstFloorRiseAlpha = 0.01;
    static constexpr double unvoicedBurstAttackSeconds = 0.001;
    static constexpr double unvoicedBurstReleaseSeconds = 0.020;
    // A little under one period on each side keeps the current ratio from
    // bleeding too far into an adjacent hard-tune target, while retaining
    // enough overlap for a continuous voiced waveform.
#ifndef VIBE_TDPSOLA_ANALYSIS_HALF_WINDOW_FACTOR
#define VIBE_TDPSOLA_ANALYSIS_HALF_WINDOW_FACTOR 0.65
#endif
    static constexpr double analysisHalfWindowFactor = VIBE_TDPSOLA_ANALYSIS_HALF_WINDOW_FACTOR;
    // The candidate remains predominantly Hann; this is enough to test the
    // wet-body hypothesis without creating a hard window edge.
    static constexpr double hardTuneWindowMaximumHammingBlend = 0.0;
    // Leave the window neutral: rapid-note definition is handled by fresh
    // source-grain ownership, not by changing the spectral window shape.
    static constexpr double rapidTransitionWindowMaximumHammingBlend = 0.0;

    double sampleRate = 48000.0, sourcePhase = 0.0, synthPhase = 0.0, smoothedRatio = 1.0, wetMix = 0.0;
    DiagnosticAblation diagnosticAblation {};
    int maximumBlock = 0, latency = 256, ringLength = 0;
    uint64_t sourceScheduleSample = 0;
    uint64_t outputReadSample = 0;
    uint64_t olaDeadlineViolationCount = 0;
    uint64_t pendingDeadlineMissCount = 0;
    uint64_t lateCandidateCreationCount = 0;
    uint64_t lateOlaWriteCount = 0;
    uint64_t earliestLateOlaDestination = std::numeric_limits<uint64_t>::max();
    std::vector<float> sourceHighFrequencyEvidenceRing, input, outputSum, outputWeight;
    std::vector<double> outputRatioSum, outputAgeSum, outputReadPositionSum, outputSourceMarkSum, outputSourceF0Sum, outputExpectedF0Sum;
    std::vector<float> outputDominantWeight;
    std::vector<float> outputDominantSampleContribution;
    std::vector<double> outputDominantEpoch, outputDominantRatio, outputDominantExpectedF0;
    std::vector<int> outputDominantTargetMidi;
    std::vector<double> outputDominantTargetEventId;
    std::vector<double> outputDominantControlEventId;
    std::vector<double> outputDominantCorrectionGeneration;
    std::vector<double> outputDominantTargetTimestamp;
    std::vector<double> outputDominantTrajectoryExcursionId;
    std::vector<SourceMark> marks;
    std::array<ScheduledGrain, 256> scheduledGrains {};
    std::array<PublishedControl, 64> publishedControls {};
    size_t publishedControlWrite = 0;
    static constexpr int pendingGrainCapacity = 16;
    std::array<PendingGrain, pendingGrainCapacity> pendingGrains {};
    std::array<PendingGrain, pendingGrainCapacity> retimableEpochPlanner {};
    int pendingGrainCount = 0;
    int retimablePlannerCount = 0;
    uint64_t plannerLastIrreversibleMark = 0;
    int pendingQueuePeakOccupancy = 0;
    uint64_t pendingQueueOverflowCount = 0;
    int retimableEpochPeakOccupancy = 0;
    uint64_t epochRetimeCount = 0;
    uint64_t retimedSamplesTotal = 0;
    uint64_t maxSingleEpochRetime = 0;
    uint64_t retimeDeadlineMissCount = 0;
    uint64_t epochOrderViolationCount = 0;
    uint64_t ticketIssuedPastDeadlineCount = 0;
    uint64_t ticketMutationAfterIssueCount = 0;
    uint64_t plannerHandoffMarginSamples = 128;
    uint64_t maximumOutputAheadAtSourcePlanner = 0;
    uint64_t maximumOutputAdvanceBetweenSourcePlanners = 0;
    uint64_t previousSourcePlannerSample = 0;
    bool hasPreviousSourcePlannerObservation = false;
    uint64_t nextPendingCandidateId = 1;
    bool epochTraceEnabled = false;
    bool diagnosticTrackingEnabled = false;
    int epochTraceCount = 0;
    std::array<EpochTrace, epochTraceCapacity> epochTraces {};
    bool olaContributionTraceEnabled = false;
    uint64_t olaContributionTraceFirstSample = 0;
    uint64_t olaContributionTraceLastSample = 0;
    std::vector<OlaContributionTrace> olaContributionTraces;
    double lastPendingCandidateId = -1.0, lastPendingFinalizedAt = -1.0, lastPendingFinalizationKind = -1.0;
    size_t scheduledGrainWrite = 0;
    size_t markWrite = 0, markCount = 0;
    SourceMark previousEpoch {};
    bool hasPreviousEpoch = false;
    int safetyDrySamples = 0;
    bool previousControlWasValid = false;
    bool previousOutputControlWasValid = false;
    double previousRequestedCorrectionCents = 0.0;
    bool hasPreviousRequestedCorrection = false;
    uint64_t correctionGeneration = 0;
    float previousOutput = 0.0f;
    bool hasPreviousOutput = false;
    double lastSpawnEpoch = -1.0;
    double lastSpawnDecisionInputSample = -1.0;
    double lastSpawnHalfWindow = 0.0;
    double lastSpawnSourceMarkSample = -1.0;
    double lastSpawnRatio = 1.0;
    double previousSynthesisMark = -1.0;
    double previousSourceMarkSample = -1.0;
    SourceMark previousSelectedSourceMark {};
    bool hasPreviousSelectedSourceMark = false;
    double previousFractionalPhaseOffset = 0.0;
    double lastPreviousSynthesisMark = -1.0;
    double lastActualSynthesisPeriod = 0.0;
    double lastExpectedSynthesisPeriodError = 0.0;
    double lastSourceMarkSpacing = 0.0;
    double sourceMarkCursor = 0.0;
    bool hasSourceMarkCursor = false;
    double lastSpawnSourcePeriod = 0.0;
    double lastExpectedOutputPeriod = 0.0;
    int lastSpawnTargetMidi = -1;
    double lastSpawnTargetEventId = -1.0;
    double lastSpawnTargetControlTimestamp = -1.0;
    double lastSpawnTrajectoryExcursionId = 0.0;
    int previousGrainTargetMidi = -1;
    uint64_t grainTargetEventId = 0;
    uint64_t pendingTailReplacementEventId = 0;
    bool pendingTailReplacementActive = false;
    uint64_t pendingTrajectoryTailReplacementId = 0;
    bool pendingTrajectoryTailReplacementActive = false;
    uint64_t trajectoryExcursionId = 0;
    bool previousTrajectoryExcursionActive = false;
    bool hasPreviousCommittedTargetHz = false;
    bool hasTargetBeforePreviousHz = false;
    double previousCommittedTargetHz = 0.0;
    double targetBeforePreviousHz = 0.0;
    uint64_t previousTargetCommitSample = 0;
    double pendingSynthesisMark = -1.0;
    double previousExcitationEvidenceOffset = 0.0;
    double confirmedExcitationOffset = 0.0;
    int excitationEvidenceRun = 0;
    bool hasPreviousExcitationEvidence = false;
    bool hasPendingSourceMark = false;
    uint64_t pendingSourceMarkSample = 0;
    double pendingSourceMarkPeriod = 0.0;
    double wetLowState = 0.0;
    double dryLowState = 0.0;
    double sibilanceResidualMix = 0.0;
    double sourceHighpassLowState = 0.0;
    double sourceHighEnergy = 0.0;
    double sourceWidebandEnergy = 0.0;
    double unvoicedBurstBaselineRms = 0.0;
    double unvoicedBurstGain = 1.0;
    VoicedColourProcessor voicedColour;
    float voicedColourAmountPercent = 0.0f;
    DiagnosticState diagnosticState {};
};
