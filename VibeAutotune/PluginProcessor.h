#pragma once

#include "CorrectionTrajectory.h"
#include "NoteTransition.h"
#include "NotePolicy.h"
#include "CausalTDPSOLARenderer.h"
#include "SignalsmithPitchRenderer.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <limits>
#include <vector>

class VibeAutotuneProcessor : public juce::AudioProcessor
{
public:
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    struct PitchMonitorData
    {
        juce::String detectedNote;
        juce::String targetNote;
        juce::String outputNote;
        float inputCents = 0.0f;
        float correctionCents = 0.0f;
        bool voiced = false;
        bool bypassed = false;
    };

    struct InputPitchDisplayData
    {
        int midiNote = -1;
        float cents = 0.0f;
    };

    // Numeric tracker snapshot for measurement and non-audio-thread consumers.
    // NaN means that the current analysis block did not produce that value.
    struct PitchTrackingData
    {
        double rawF0Hz = std::numeric_limits<double>::quiet_NaN();
        double stableF0Hz = std::numeric_limits<double>::quiet_NaN();
        double confidence = std::numeric_limits<double>::quiet_NaN();
        bool voiced = false;
        uint64_t timestampSamples = 0;
        int analysisLatencySamples = 0;
        // Atomically-published control state used by a host trace. These are
        // intentionally numeric, so the audio callback can report target
        // ownership without allocating UI strings.
        int detectedMidiNote = -1;
        int targetMidiNote = -1;
        int outputMidiNote = -1;
        float inputCents = 0.0f;
        float correctionCents = 0.0f;
    };

    struct SourceSchedulerDiagnostics
    {
        uint64_t sourceSamplesReceived = 0;
        uint64_t sourceSamplesScheduled = 0;
        uint64_t quantumsScheduled = 0;
        int partialStageFill = 0;
        uint64_t sourceDropCount = 0;
        uint64_t sourceDuplicateCount = 0;
        uint64_t schedulerGapCount = 0;
        uint64_t schedulerOverlapCount = 0;
        uint64_t maximumOutputAheadAtSourcePlanner = 0;
        uint64_t maximumOutputAdvanceBetweenSourcePlanners = 0;
    };

    struct PitchAnalysisPerformanceData
    {
        uint64_t analysisCount = 0;
        double fifoFramePreparationMicroseconds = 0.0;
        double differenceFunctionMicroseconds = 0.0;
        double cmndfMicroseconds = 0.0;
        double candidateSearchMicroseconds = 0.0;
        double parabolicInterpolationMicroseconds = 0.0;
        double normalizedAutocorrelationMicroseconds = 0.0;
        double voicingFeaturesMicroseconds = 0.0;
        double octaveScoringMicroseconds = 0.0;
        double smoothingMicroseconds = 0.0;
        double totalMicroseconds = 0.0;
    };

    // Control-delivery counters are intentionally diagnostic only.  They make
    // target-transition regressions observable without adding logging or work
    // to the audio callback.
    struct CorrectionDeliveryData
    {
        uint64_t targetSwitchCount = 0;
        uint64_t dryBridgeCount = 0;
        float maxTrajectoryStepCents = 0.0f;
        float maxRendererStepCents = 0.0f;
    };

    struct RendererDiscontinuityData
    {
        uint64_t sampleIndex = 0;
        float step = 0.0f;
        double sourceF0Hz = 0.0;
        double pitchRatio = 1.0;
        double synthesisPeriod = 0.0;
        double sourcePeriod = 0.0;
        double sourcePeriodJump = 0.0;
        double tdpsolaOutputStep = 0.0;
        bool tdpsolaFallback = false;
        float dryWetMix = 0.0f;
        bool voiced = false;
    };

    // Offline diagnostic snapshot of the production signal chain. No strings,
    // allocations, locks, or callbacks are used on the audio thread.
    struct SignalChainDiagnosticFrame
    {
        uint64_t sampleIndex = 0;
        double rawF0Hz = std::numeric_limits<double>::quiet_NaN();
        double stableF0Hz = std::numeric_limits<double>::quiet_NaN();
        double pitchConfidence = std::numeric_limits<double>::quiet_NaN();
        double inputPitchCents = std::numeric_limits<double>::quiet_NaN();
        double targetPitchCents = std::numeric_limits<double>::quiet_NaN();
        double candidateTargetCents = std::numeric_limits<double>::quiet_NaN();
        double requestedRendererRatio = 1.0;
        float rendererPreGuardCorrectionCents = 0.0f;
        float rendererPostGuardCorrectionCents = 0.0f;
        float deliveredRendererCorrectionCents = 0.0f;
        float rendererInputCorrectionCents = 0.0f;
        uint8_t rendererCorrectionSource = 1;
        float rendererStepCents = 0.0f;
        float dryWetMix = 0.0f;
        // Store the control value itself, rather than inferring it from the
        // current trajectory. A CSV must state which Retune setting produced
        // a target event before it can be compared with Auto-Tune.
        float retuneSpeedMs = 0.0f;
        float flexTunePercent = 0.0f;
        float humanizePercent = 0.0f;
        float naturalVibratoPercent = 0.0f;
        float trackingValue = 0.0f;
        float flexCorrectionGain = 0.0f;
        float desiredCorrectionCents = 0.0f;
        float retuneCorrectionCents = 0.0f;
        float humanizeWeight = 0.0f;
        float vibratoContributionCents = 0.0f;
        float rawVibratoResidualCents = 0.0f;
        float vibratoEnergyCents = 0.0f;
        float vibratoDeviationCents = 0.0f;
        float pitchNoteCenterCents = 0.0f;
        float pitchDriftCents = 0.0f;
        float pitchVibratoRateHz = 0.0f;
        float pitchVibratoConfidence = 0.0f;
        float pitchNoteStability = 0.0f;
        float pitchDriftConfidence = 0.0f;
        float pitchJitterConfidence = 0.0f;
        float referenceHz = 440.0f;
        int voiceRangeMinHz = 0;
        int voiceRangeMaxHz = 0;
        int rawMidi = -1;
        int candidateTargetMidi = -1;
        int committedTargetMidi = -1;
        int key = 0;
        int scale = 0;
        int transpose = 0;
        bool voiced = false;
        bool targetChanged = false;
        bool correctionIsActive = false;
        bool targetBridge = false;
        bool transientBridge = false;
        bool trajectoryTransitionActive = false;
        float trajectoryTransitionProgress = 1.0f;
        double trajectoryTransitionStartCents = std::numeric_limits<double>::quiet_NaN();
        double trajectoryTransitionEndCents = std::numeric_limits<double>::quiet_NaN();
        float trajectoryTransitionDurationMs = 0.0f;
        bool trajectoryRapidAdjacentContour = false;
        bool trajectoryRendererCrossingActive = false;
        int trajectoryRendererCrossingTargetMidi = -1;
        float trajectoryRendererCrossingProgress = 1.0f;
        bool trajectoryExcursionActive = false;
        uint8_t trajectoryExcursionPhase = 0;
        bool trajectoryExcursionConsumed = false;
        bool trajectoryExcursionOwnsControl = false;
        double targetDecisionMidpointCents = std::numeric_limits<double>::quiet_NaN();
        float targetDecisionExcessCents = 0.0f;
        float targetHysteresisCents = 0.0f;
        int pendingTargetMidi = -1;
        int pendingTargetFrames = 0;
        bool targetDecisionBeyondHysteresis = false;
        bool rapidAdjacentRawCandidate = false;
        bool rapidAdjacentPrearmed = false;
        int rapidAdjacentCandidateMidi = -1;
        int rapidAdjacentEvidenceFrames = 0;
        double rapidAdjacentRawDistanceCents = 0.0;
        bool rapidLegatoAdjacentStep = false;
        bool rapidLegatoHardLock = false;
        bool rapidLegatoTargetChanged = false;
        bool rapidLegatoContinuesDirection = false;
        bool rapidLegatoChainStarts = false;
        bool rapidLegatoChainActive = false;
        int rapidLegatoCommittedSteps = 0;
        int rapidLegatoSamplesSinceCommit = 0;
        double rapidLegatoDistanceCents = 0.0;
        double rapidLegatoInputMotionCentsPerSecond = 0.0;
        int trajectoryPreviewNote = -1;
        int trajectoryPreviewDirection = 0;
        int trajectoryPreviewUpdates = 0;
        double trajectoryPreviewTravelCents = 0.0;
        double instantaneousInputMotionCentsPerSecond = 0.0;
        int selectorCurrentTargetBefore = -1;
        int selectorPreviousTarget = -1;
        int selectorCandidate = -1;
        int selectorFramesSinceCommit = 0;
        int selectorDecision = 0;
        bool trajectoryPortamentoActive = false;
        bool trajectoryUsesPortamentoTarget = false;
        bool latticeReset = false;
        bool dryGuardActive = false;
        bool reentryHoldActive = false;
        // Exact source-time control frame consumed by TD-PSOLA. These avoid
        // comparing a renderer interval with a later analysis snapshot.
        uint64_t sourceControlTimestamp = 0;
        // The latency-delayed control used by the output-stage ratio path.
        // Keep it separate from sourceControl*, which belongs to TD-PSOLA.
        uint64_t outputRendererControlTimestamp = 0;
        double outputRendererControlPitchRatio = 1.0;
        int outputRendererControlTargetMidi = -1;
        double sourceControlF0Hz = std::numeric_limits<double>::quiet_NaN();
        double sourceControlTargetCents = std::numeric_limits<double>::quiet_NaN();
        int sourceControlTargetMidi = -1;
        bool sourceControlValid = false;
        bool sourceControlHoldDry = false;
        double sourceControlPitchRatio = 1.0;
        int sourceControlMaterializationAdvanceSamples = 0;
        double sourceControlCorrectionBefore = std::numeric_limits<double>::quiet_NaN();
        double sourceControlCorrectionAfter = std::numeric_limits<double>::quiet_NaN();
        bool sourceControlTrajectoryExcursionActive = false;
        uint8_t sourceControlTrajectoryExcursionPhase = 0;
        bool sourceControlTrajectoryExcursionOwnsControl = false;
        // Debug-only comparison of caller and renderer source-control lookups.
        uint64_t callerControlLookupTime = 0;
        uint64_t rendererControlLookupTime = 0;
        int64_t controlLookupDeltaSamples = 0;
        uint64_t callerControlTimestamp = 0;
        uint64_t rendererControlTimestamp = 0;
        uint64_t callerControlEventId = 0;
        uint64_t rendererControlEventId = 0;
        double callerControlPitchRatio = 1.0;
        double rendererControlPitchRatio = 1.0;
        bool rendererStrongRatioStep = false;
        bool rendererRapidCorrectionReversal = false;
        bool rendererTargetCommitted = false;
        double sourceCursorBeforeStrongStep = -1.0;
        double sourceCursorAfterStrongStep = -1.0;
        bool sourceCursorReanchorSuppressed = false;
        double fractionalPhaseOffsetAtSpawn = 0.0;
        double overlapPhaseOffsetAtSpawn = 0.0;
        float olaOverlapWeight = 0.0f;
        double olaAudibleRatioAtStart = 1.0;
        double olaAudibleGrainAge = 0.0;
        double olaAudibleReadPosition = 0.0;
        double olaDominantGrainWeight = 0.0;
        double olaDominantGrainEpoch = -1.0;
        double olaDominantGrainRatio = 1.0;
        double olaDominantGrainExpectedF0 = 0.0;
        int olaDominantGrainTargetMidi = -1;
        double olaDominantGrainTargetEventId = -1.0;
        double olaDominantGrainControlEventId = -1.0;
        double olaDominantGrainTargetControlTimestamp = -1.0;
        double olaDominantGrainTrajectoryExcursionId = 0.0;
        double olaNewestTargetGrainWeight = 0.0;
        double olaPreviousTargetGrainWeight = 0.0;
        double olaNewestTargetFirstAudibleSample = -1.0;
        double olaPreviousTargetLastAudibleSample = -1.0;
        double olaTargetCandidateWeight = 0.0;
        double olaTargetCandidateEpoch = -1.0;
        double olaTargetCandidateExpectedF0 = 0.0;
        int olaShadowFutureSegments = 0;
        int olaShadowAudibleSegments = 0;
        double olaShadowFutureSamples = 0.0;
        bool olaShadowReplacementEligible = false;
        double olaShadowReplacementFirstSample = -1.0;
        double olaShadowReplacementLastSample = -1.0;
        bool olaShadowCandidateRenderApplied = false;
        double nextEpochBeforeReversal = -1.0;
        double nextEpochAfterReversal = -1.0;
        double actualSpawnEpoch = -1.0;
        double grainDecisionInputSample = -1.0;
        double grainFirstAudibleSample = -1.0;
        double grainHalfWindowAtSpawn = 0.0;
        double grainSourceMarkAtSpawn = -1.0;
        double ratioAtSpawn = 1.0;
        int spawnTargetMidi = -1;
        double spawnTargetEventId = -1.0;
        double spawnTargetControlTimestamp = -1.0;
        double spawnTrajectoryExcursionId = 0.0;
        double spawnSourceMarkF0 = 0.0;
        double spawnExpectedF0 = 0.0;
        double olaSourceMarkTime = -1.0;
        double olaSourceMarkF0 = 0.0;
        double olaExpectedGrainF0 = 0.0;
        double synthExpectedPeriod = 0.0;
        double synthActualPeriod = 0.0;
        double synthPeriodError = 0.0;
        double synthSourceSpacing = 0.0;
        double sourceSelectionCursor = -1.0;
        double newestCompleteSourceMark = -1.0;
        double sibilanceResidualMix = 0.0;
        double rendererHighFrequencyEvidence = 0.0;
        double rendererPeriodicCoherence = 0.0;
        double pitchMarkCandidateOffset = 0.0;
        double pitchMarkCandidateCorrelation = 0.0;
        double confirmedPitchMarkOffset = 0.0;
        int confirmedPitchMarkRun = 0;
        bool rendererArtifactDryVeto = false;
        bool rendererGrainStarved = false;
        bool rendererShouldRenderWet = false;
        int rendererSafetyDrySamples = 0;
    };

    // Debug-only record of an individual write into the renderer control
    // ring. Unlike SignalChainDiagnosticFrame (which is sampled at output
    // time), this preserves the exact update/advance -> push ordering.
    struct ControlMaterializationEvent
    {
        uint64_t sequence = 0;
        uint64_t timestampSamples = 0;
        uint64_t targetEventId = 0;
        uint64_t targetEventTimestampSamples = 0;
        uint64_t publishedAtInputSample = 0;
        uint64_t hostBlockStartSample = 0;
        uint64_t hostBlockEndSample = 0;
        bool publishedToPsolaRing = false;
        int controlAdvanceSamples = 0;
        double transitionStartCents = std::numeric_limits<double>::quiet_NaN();
        double transitionTargetCents = std::numeric_limits<double>::quiet_NaN();
        double transitionProgress = std::numeric_limits<double>::quiet_NaN();
        double transitionDurationMs = std::numeric_limits<double>::quiet_NaN();
        double correctionBeforeAdvance = std::numeric_limits<double>::quiet_NaN();
        double correctionAfterAdvance = std::numeric_limits<double>::quiet_NaN();
        double scheduledRatio = 1.0;
        double scheduledCorrectionCents = 0.0;
    };

    struct RendererControlRingEntry
    {
        int ringIndex = -1;
        bool written = false;
        uint64_t insertionSequence = 0;
        uint64_t timestampSamples = 0;
        double pitchRatio = 1.0;
        double correctionCents = 0.0;
        uint64_t targetEventId = 0;
        bool valid = false;
        bool holdDry = false;
    };

    // Debug-only, exact-at-lookup capture of the private PSOLA control ring.
    struct RendererControlLookupTrace
    {
        uint64_t queryTimestampSamples = 0;
        uint8_t lookupRole = 0; // 1=wet delayed, 2=PSOLA renderer, 3=PSOLA preflight
        int psolaControlWriteIndex = 0;
        bool foundBefore = false;
        bool foundAfter = false;
        bool interpolationEligible = false;
        bool interpolationApplied = false;
        double interpolationT = 0.0;
        RendererControlRingEntry before;
        RendererControlRingEntry after;
        RendererControlRingEntry returned;
        std::array<RendererControlRingEntry, 64> ring {};
    };

    // Compact realtime UI scope.  Each entry is an input envelope bucket, not
    // raw audio: fixed memory, no allocation and no work on the UI thread's
    // audio path.
    struct WaveformScopeFrame
    {
        uint64_t sampleIndex = 0;
        float minimum = 0.0f;
        float maximum = 0.0f;
        float inputPitchCents = std::numeric_limits<float>::quiet_NaN();
        float targetPitchCents = std::numeric_limits<float>::quiet_NaN();
        float deliveredCorrectionCents = 0.0f;
        bool voiced = false;
    };

    // The editor reads the live scope while the audio callback publishes it.
    // Keep the transport atomic: a normal struct ring creates a C++ data race
    // even when the published write index itself is atomic.
    struct WaveformScopeSlot
    {
        std::atomic<uint64_t> sampleIndex { 0 };
        std::atomic<float> minimum { 0.0f };
        std::atomic<float> maximum { 0.0f };
        std::atomic<float> inputPitchCents { 0.0f };
        std::atomic<float> targetPitchCents { 0.0f };
        std::atomic<float> deliveredCorrectionCents { 0.0f };
        std::atomic<bool> voiced { false };
    };
    static constexpr int waveformScopeCapacity = 1024;

    VibeAutotuneProcessor();
    ~VibeAutotuneProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midiMessages) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer,
                               juce::MidiBuffer& midiMessages) override;

    PitchMonitorData getPitchMonitorData() const;
    static InputPitchDisplayData calculateInputPitchDisplay (double stableF0Hz,
                                                              double referenceA4Hz,
                                                              bool voiced) noexcept;
    PitchTrackingData getPitchTrackingData() const noexcept;
    SourceSchedulerDiagnostics getSourceSchedulerDiagnostics() const noexcept;
    uint64_t getRendererOlaDeadlineViolationCount() const noexcept;
    uint64_t getRendererPendingDeadlineMissCount() const noexcept;
    uint64_t getRendererLateCandidateCreationCount() const noexcept;
    uint64_t getRendererLateOlaWriteCount() const noexcept;
    int getRendererPendingQueuePeakOccupancy() const noexcept;
    uint64_t getRendererPendingQueueOverflowCount() const noexcept;
    int getRendererRetimableEpochPeakOccupancy() const noexcept;
    uint64_t getRendererEpochRetimeCount() const noexcept;
    uint64_t getRendererRetimedSamplesTotal() const noexcept;
    uint64_t getRendererMaxSingleEpochRetime() const noexcept;
    uint64_t getRendererRetimeDeadlineMissCount() const noexcept;
    uint64_t getRendererEpochOrderViolationCount() const noexcept;
    uint64_t getRendererTicketIssuedPastDeadlineCount() const noexcept;
    uint64_t getRendererTicketMutationAfterIssueCount() const noexcept;
    void setRendererEpochTraceEnabled (bool shouldEnable) noexcept;
    int copyRendererEpochTraces (CausalTDPSOLARenderer::EpochTrace* destination, int maximumTraces) const noexcept;
    void setRendererOlaContributionTraceRange (uint64_t firstSample, uint64_t lastSample);
    int copyRendererOlaContributionTraces (CausalTDPSOLARenderer::OlaContributionTrace* destination,
                                           int maximumTraces) const noexcept;
    PitchAnalysisPerformanceData getPitchAnalysisPerformanceData() const noexcept;
    CorrectionDeliveryData getCorrectionDeliveryData() const noexcept;
    RendererDiscontinuityData getRendererDiscontinuityData() const noexcept;
    void setSignalChainDiagnosticEnabled (bool shouldEnable) noexcept;
    void setRapidAdjacentTargetSelectionEnabled (bool shouldEnable) noexcept;
    void setRendererDiagnosticAblation (CausalTDPSOLARenderer::DiagnosticAblation value) noexcept;
    void setControlCalibrationDiagnostic (double pitchCentreSeconds,
                                          double wideVibratoNeutralRetention,
                                          double wideVibratoHumanizedRetention) noexcept;
    // Offline benchmark hook only. It lets the control-surface optimiser
    // render a candidate without making that candidate the plug-in default.
    void setNaturalVibratoSurfaceDiagnostic (
        const CorrectionTrajectory::NaturalVibratoSurface& surface) noexcept;
    void setNegativeNaturalPhaseScaleDiagnostic (double scale) noexcept;
    void setHardTuneWideTransitionDiagnostic (double upwardDurationMs,
                                              double downwardDurationMs,
                                              int shape) noexcept;
    int copySignalChainDiagnosticFrames (SignalChainDiagnosticFrame* destination, int maximumFrames) const noexcept;
    static constexpr int controlMaterializationEventCapacity = 4096;
    int copyControlMaterializationEvents (ControlMaterializationEvent* destination,
                                          int maximumEvents) const noexcept;
    static constexpr int rendererControlLookupTraceCapacity = 16;
    int copyRendererControlLookupTraces (RendererControlLookupTrace* destination,
                                         int maximumTraces) const noexcept;
    // Live capture is intentionally separate from the short offline test
    // snapshot above. It is a lock-free, allocation-free audio-thread trace
    // intended for diagnosing the exact control stream a DAW session heard.
    static constexpr int liveDiagnosticCapacity = 65536;
    void setLiveDiagnosticCaptureEnabled (bool shouldEnable) noexcept;
    bool isLiveDiagnosticCaptureEnabled() const noexcept;
    int copyLiveDiagnosticFrames (SignalChainDiagnosticFrame* destination, int maximumFrames) const noexcept;
    int copyWaveformScopeFrames (WaveformScopeFrame* destination, int maximumFrames) const noexcept;
    void setPitchAnalysisProfilingEnabled (bool shouldEnable) noexcept;
    juce::String getLatestDetectedNoteName() const;
    int getLatestDetectedMidiNote() const noexcept;
    int getLatestTargetMidiNote() const noexcept;
    void setMidiTargetNoteFromUi (int midiNote) noexcept;
    int getMidiTargetNote() const noexcept;
    uint16_t getEnabledNoteMask() const noexcept;
    void setEnabledNoteMask (uint16_t newMask) noexcept;
    uint16_t getBaseScaleMask() const noexcept;
    bool hasCustomNotePolicy() const noexcept;
    NoteRule getNoteRuleForPitchClass (int pitchClass) const noexcept;
    NoteRule getNoteRuleForMidiNote (int midiNote) const noexcept;
    void setNoteRuleForPitchClass (int pitchClass, NoteRule rule) noexcept;
    void setNoteRuleForMidiNote (int midiNote, NoteRule rule) noexcept;
    void resetNoteRuleForPitchClass (int pitchClass) noexcept;
    void resetNoteRuleForMidiNote (int midiNote) noexcept;
    void resetNotePolicyToCurrentScale() noexcept;
    bool getGroupOctaves() const noexcept;
    void setGroupOctaves (bool shouldGroup) noexcept;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "VibeAutotune"; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState parameters;

private:
    // Grants the deterministic conformance test access to the real control
    // ring without adding a runtime diagnostic or production API.
    friend class ControlHandoffTestAccess;
    struct PitchAnalysisConfig
    {
        int frameSize = 1024;
        int hopSize = 256;
        double minimumFrequency = 110.0;
        double maximumFrequency = 700.0;
        int inputType = 1;
    };

    struct PitchCandidate
    {
        int period = 0;
        double frequency = 0.0;
        double periodicity = 0.0;
        double correlation = 0.0;
        double valleyProminence = 0.0;
        double quality = 0.0;
    };

    struct PsolaControlFrame
    {
        uint64_t timestampSamples = 0;
        double stableF0Hz = std::numeric_limits<double>::quiet_NaN();
        double pitchRatio = 1.0;
        // The intentional Humanize/Natural-Vibrato departure is carried
        // separately from the base note-correction ratio.  A PSOLA lattice
        // can then keep a slow Retune move in flight without cancelling the
        // residual once the base correction has reached its note.
        double residualPitchCents = 0.0;
        bool valid = false;
        bool holdDry = false;
        bool targetBridge = false;
        bool transientBridge = false;
        int targetMidi = -1;
        double targetPitchCents = std::numeric_limits<double>::quiet_NaN();
        uint64_t targetEventId = 0;
        uint64_t targetEventTimestampSamples = 0;
        int rendererCrossingTargetMidi = -1;
        double rendererCrossingProgress = 1.0;
        bool continuousReturnExcursionActive = false;
        uint8_t continuousReturnExcursionPhase = 0;
        bool continuousReturnExcursionConsumed = false;
        bool continuousReturnExcursionOwnsControl = false;
        bool debugWritten = false;
        uint64_t debugInsertionSequence = 0;
        uint64_t debugPublicationSample = 0;
        // Debug-only snapshot of the exact update/scheduler action that
        // materialized this queued control frame.
        int debugControlAdvanceSamples = 0;
        double debugControlCorrectionBefore = std::numeric_limits<double>::quiet_NaN();
        double debugControlCorrectionAfter = std::numeric_limits<double>::quiet_NaN();
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    static juce::String noteNameForMidi (int midiNote);
    void clearPitchMonitor (bool bypassed);
    void publishPitchTrackingData (double rawF0Hz, double stableF0Hz,
                                   double confidence, bool voiced) noexcept;
    PitchAnalysisConfig getPitchAnalysisConfig (int inputType) const noexcept;
    void resetPitchAnalysisState (bool clearMonitor);
    void synchroniseNotePolicyToScale() noexcept;
    void resetNotePolicyToScale (int key, int scale) noexcept;
    NotePolicySnapshot makeNotePolicySnapshot() const noexcept;
    static bool scaleContainsPitchClass (int root, int scale, int pitchClass) noexcept;
    void pushPitchAnalysisSamples (const float* samples, int numberOfSamples, uint64_t blockStartSample);
    void analysePitchFrame (uint64_t frameEndSample);
    bool findPitchCandidate (int expectedPeriod, int minimumLag, int maximumLag,
                             PitchCandidate& candidate) const noexcept;
    void publishUnvoicedFrame (uint64_t frameEndSample);
    void updateCorrectionTrajectory (double stableFrequency, bool voiced,
                                     uint64_t frameEndSample,
                                     double rawFrequency = std::numeric_limits<double>::quiet_NaN(),
                                     double rawConfidence = 0.0) noexcept;
    void resetPsolaRenderer() noexcept;
    void pushPsolaControlFrame (uint64_t timestampSamples, double stableF0Hz, double pitchRatio,
                                double residualPitchCents,
                                bool valid, bool holdDry = false, bool targetBridge = false,
                                bool transientBridge = false, int targetMidi = -1,
                                double targetPitchCents = std::numeric_limits<double>::quiet_NaN(),
                                uint64_t targetEventId = 0,
                                uint64_t targetEventTimestampSamples = 0,
                                int rendererCrossingTargetMidi = -1,
                                double rendererCrossingProgress = 1.0,
                                bool continuousReturnExcursionActive = false,
                                uint8_t continuousReturnExcursionPhase = 0,
                                bool continuousReturnExcursionConsumed = false,
                                bool continuousReturnExcursionOwnsControl = false,
                                const CorrectionTrajectoryData* materialization = nullptr) noexcept;
    void recordControlMaterializationEvent (uint64_t timestampSamples, uint64_t targetEventId,
                                            uint64_t targetEventTimestampSamples, double pitchRatio,
                                            const CorrectionTrajectoryData& materialization,
                                            bool publishedToPsolaRing) noexcept;
    void captureRendererControlLookupTrace (uint64_t queryTimestampSamples, uint8_t lookupRole,
                                            const PsolaControlFrame& rawBefore, bool foundBefore,
                                            const PsolaControlFrame& rawAfter, bool foundAfter,
                                            const PsolaControlFrame& returned,
                                            bool interpolationEligible, bool interpolationApplied,
                                            double interpolationT) const noexcept;
    PsolaControlFrame getRendererControlFrame (uint64_t outputTimelineSample,
                                                uint8_t lookupRole = 0) const noexcept;
    void processPitchRenderer (juce::AudioBuffer<float>& buffer, int startSample, int sampleCount,
                               bool outputCorrectionActive, double outputPitchRatio,
                               double outputStableF0Hz,
                               bool inputPsolaVoiced, double inputPitchRatio,
                               double inputStableF0Hz,
                               double inputCommittedTargetHz = std::numeric_limits<double>::quiet_NaN(),
                               uint64_t canonicalSourceControlTime = 0,
                               uint64_t outputControlTime = 0,
                               const float* const* stagedSource = nullptr,
                               int stagedSourceSamples = -1) noexcept;
    void resetPitchRenderer() noexcept;
    void captureLiveDiagnosticFrame (const SignalChainDiagnosticFrame& frame) noexcept;
    void captureWaveformScope (const float* input, int sampleCount, uint64_t startSample) noexcept;

    double analysisSampleRate = 44100.0;
    std::vector<double> yinDifference;
    std::vector<double> yinCumulativeMean;
    std::vector<double> yinCrossCorrelation;
    std::vector<double> pitchFrameEnergyPrefix;
    std::vector<double> pitchHistory;
    std::vector<float> pitchAnalysisFifo;
    std::vector<float> pitchAnalysisFrame;
    std::vector<float> rawPitchAnalysisFrame;
    std::vector<float> rendererInput;
    std::vector<float> phaseContinuousRendererOutput;
    // Source/control time is intentionally quantised independently of the
    // host callback.  This fixed storage is the only place a short callback
    // may wait; output is always rendered by the callback that owns it.
    // Source scheduling is deliberately finer than control publication.  A
    // completed quantum is the only unit allowed to advance source-time
    // state; arbitrary host callback tails remain in this fixed stage.
    static constexpr int sourceStageStorageSamples = 64;
    // Shipped default is 64.  The 32-sample value is an explicitly opt-in
    // diagnostic lattice selected only at prepare time.
    int activeSourceSchedulerQuantumSamples = 64;
    std::array<float, sourceStageStorageSamples * 2> sourceStage {};
    int sourceStageFill = 0;
    uint64_t sourceStageStartSample = 0;
    uint64_t sourceSamplesReceived = 0;
    uint64_t sourceSamplesScheduled = 0;
    uint64_t sourceQuantumsScheduled = 0;
    uint64_t sourceDropCount = 0;
    uint64_t sourceDuplicateCount = 0;
    uint64_t schedulerGapCount = 0;
    uint64_t schedulerOverlapCount = 0;
    uint64_t nextScheduledSourceSample = 0;
    int pitchHistoryIndex = 0;
    int pitchHistoryCount = 0;
    int pitchAnalysisWriteIndex = 0;
    int pitchAnalysisSamplesAvailable = 0;
    int pitchAnalysisSamplesSinceLastFrame = 0;
    int configuredInputType = -1;
    int voicingAttackFrames = 0;
    int voicingReleaseFrames = 0;
    int pendingOctaveFrames = 0;
    double pendingOctaveFrequency = std::numeric_limits<double>::quiet_NaN();
    int pendingPitchJumpFrames = 0;
    double pendingPitchJumpFrequency = std::numeric_limits<double>::quiet_NaN();
    int pendingSubharmonicFrames = 0;
    double pendingSubharmonicFrequency = std::numeric_limits<double>::quiet_NaN();
    double stableLog2F0 = std::numeric_limits<double>::quiet_NaN();
    double previousAnalysisFrameRms = 0.0;
    // Slowly decaying vowel-level reference. It lets the voicing gate reject
    // low-level breath/consonant fragments inside an otherwise voiced phrase.
    double voicedRmsEnvelope = 0.0;
    int audioTransientFramesRemaining = 0;
    CorrectionTrajectory correctionTrajectory;
    NoteTransition noteTransition;
    CorrectionTrajectoryData latestCorrectionTrajectory;
    double noiseFloorRms = 1.0e-5;
    PitchAnalysisConfig pitchAnalysisConfig;
    int psolaMaximumBlockSize = 0;
    int psolaMaximumChannels = 0;
    std::array<CausalTDPSOLARenderer, 2> tdpsolaRenderers;
    std::array<SignalsmithPitchRenderer, 2> phaseContinuousRenderers;
    // The spectral renderer is the production path because it is the
    // closest pitch/timbre match to the Artist reference on sustained vocal.
    // TD-PSOLA remains available for diagnostic comparisons.
    // These can be changed by an offline diagnostic controller while the
    // processor is alive. They are sampled by the audio callback, so plain
    // bools would create a data race even though the production values rarely
    // change. The normal live path always selects the phase-continuous one.
    std::atomic<bool> usePhaseContinuousRenderer { false };
    std::atomic<bool> phaseForceUnityRatioForDiagnostic { false };
    std::atomic<bool> phaseForceStaticUpRatioForDiagnostic { false };
    int rendererLatencySamples = 0;
    int rendererControlDelaySamples = 0;
    bool rendererCorrectionEngaged = false;
    bool rendererReleasingToUnity = false;
    // Consecutive certified control ticks required before the transform is
    // audible. This rejects a single false F0 without adding audio latency.
    int rendererEngageControlFrames = 0;
    // Counts consecutive low-correction renderer-control ticks before
    // returning to delayed dry.  This is deliberately control-domain state:
    // it must never act as an audio noise gate.
    int rendererReleaseControlFrames = 0;
    // A short control-rate dezipper for renderer automation only. This does
    // not replace CorrectionTrajectory's Retune response; it prevents a
    // new YIN/control frame from becoming a discontinuous pitch-ratio
    // update at an arbitrary host block boundary.
    double renderedPitchRatio = 1.0;
    // Last certified control is retained only for the short voiced/unvoiced
    // crossfade.  It prevents replacing a phase-synchronous signal with dry
    // at full wet gain on the first breath/consonant sample.
    double lastValidRendererF0Hz = 0.0;
    double lastValidRendererPitchRatio = 1.0;
    bool hasLastValidRendererControl = false;
    int rendererInvalidControlSamples = 0;
    // TD-PSOLA keeps its grain scheduler warm through a transient, but the
    // audible wet path must wait for a fresh stable control run.
    int tdpsolaReentryDrySamples = 0;
    std::array<PsolaControlFrame, 64> psolaControlFrames {};
    int psolaControlWriteIndex = 0;
    uint64_t psolaControlInsertionSequence = 0;
    // Diagnostic publication context; set by analysis while consuming the
    // current host block and never read by DSP decisions.
    uint64_t debugControlPublicationInputSample = 0;
    uint64_t debugControlPublicationBlockStartSample = 0;
    uint64_t debugControlPublicationBlockEndSample = 0;
    std::array<ControlMaterializationEvent, controlMaterializationEventCapacity> controlMaterializationEvents {};
    uint64_t controlMaterializationEventCount = 0;
    uint64_t psolaTargetEventId = 0;
    uint64_t psolaTargetEventTimestampSamples = 0;
    bool pitchHistoryInitialised = false;
    double smoothedPitchRatio = 1.0;
    double smoothedCorrectionCents = 0.0;
    double previousTrajectoryCorrectionCents = 0.0;
    double previousRendererCorrectionCents = 0.0;
    bool hadValidTrajectoryCorrection = false;
    bool hadActiveRendererControl = false;
    std::atomic<uint64_t> committedTargetSwitchCount { 0 };
    std::atomic<uint64_t> dryBridgeActivationCount { 0 };
    std::atomic<float> maximumTrajectoryStepCents { 0.0f };
    std::atomic<float> maximumRendererStepCents { 0.0f };
    RendererDiscontinuityData rendererDiscontinuityData;
    float previousRendererOutputSample = 0.0f;
    bool hasPreviousRendererOutputSample = false;
    // Diagnostic-only snapshots of the two independently queried source
    // control timelines. They do not feed renderer control or DSP.
    uint64_t debugCallerControlLookupTime = 0;
    PsolaControlFrame debugCallerControl {};
    uint64_t debugRendererControlLookupTime = 0;
    PsolaControlFrame debugRendererControl {};
    std::atomic<bool> signalChainDiagnosticEnabled { false };
    mutable std::array<RendererControlLookupTrace, rendererControlLookupTraceCapacity> rendererControlLookupTraces {};
    mutable int rendererControlLookupTraceCount = 0;
    std::array<SignalChainDiagnosticFrame, 128> signalChainDiagnosticFrames {};
    int signalChainDiagnosticFrameCount = 0;
    std::unique_ptr<std::array<SignalChainDiagnosticFrame, liveDiagnosticCapacity>> liveDiagnosticFrames;
    // Producer state is exclusively owned by the audio thread. The published
    // count is release/acquire synchronised with the UI/export thread.
    uint64_t liveDiagnosticProducerCount = 0;
    std::atomic<uint64_t> liveDiagnosticPublishedCount { 0 };
    std::atomic<bool> liveDiagnosticCaptureEnabled { false };
    std::atomic<bool> liveDiagnosticClearRequested { false };
    std::array<WaveformScopeSlot, waveformScopeCapacity> waveformScopeFrames {};
    uint64_t waveformScopeProducerCount = 0;
    std::atomic<uint64_t> waveformScopePublishedCount { 0 };
    uint64_t processedInputSamples = 0;
    int currentAnalysisLatencySamples = 0;

    std::atomic<int> latestDetectedMidiNote { -1 };
    std::atomic<int> latestTargetMidiNote { -1 };
    std::atomic<int> midiTargetNote { -1 };
    std::array<bool, 128> midiHeldNotes {};
    std::array<uint64_t, 128> midiNoteOrder {};
    uint64_t midiOrderCounter = 0;
    std::atomic<int> latestOutputMidiNote { -1 };
    std::atomic<float> latestInputCents { 0.0f };
    std::atomic<float> latestCentsDeviation { 0.0f };
    std::atomic<bool> latestVoiced { false };
    std::atomic<bool> latestBypassed { false };
    std::atomic<double> latestRawF0Hz { std::numeric_limits<double>::quiet_NaN() };
    std::atomic<double> latestStableF0Hz { std::numeric_limits<double>::quiet_NaN() };
    std::atomic<double> latestPitchConfidence { std::numeric_limits<double>::quiet_NaN() };
    std::atomic<uint64_t> latestPitchTimestampSamples { 0 };
    std::atomic<int> latestAnalysisLatencySamples { 0 };
    // enabledNoteMask remains as a backwards-compatible pitch-class view of
    // Legal/BypassLegal notes. The authoritative data is NoteRule.
    std::atomic<uint16_t> enabledNoteMask { 0x0fff };
    std::atomic<uint16_t> baseScaleMask { 0x0fff };
    // Note-map source of truth: the selected Key/Scale creates the twelve
    // defaults; non-grouped keyboard edits create explicit MIDI overrides.
    std::array<std::atomic<uint8_t>, 12> pitchClassDefaultPolicy;
    std::array<std::atomic<uint8_t>, 128> midiNoteOverridePolicy;
    std::array<std::atomic<bool>, 128> midiNoteHasOverride;
    std::atomic<bool> groupOctaves { true };
    std::atomic<uint32_t> notePolicyRevision { 0 };
    uint32_t appliedNotePolicyRevision = 0;
    int synchronisedPolicyKey = -1;
    int synchronisedPolicyScale = -1;
    std::atomic<bool> pitchAnalysisProfilingEnabled { false };
    uint64_t profiledAnalysisCount = 0;
    uint64_t profiledFifoFrameTicks = 0;
    uint64_t profiledDifferenceTicks = 0;
    uint64_t profiledCmndfTicks = 0;
    uint64_t profiledCandidateSearchTicks = 0;
    uint64_t profiledParabolicTicks = 0;
    uint64_t profiledAutocorrelationTicks = 0;
    uint64_t profiledVoicingTicks = 0;
    uint64_t profiledOctaveTicks = 0;
    uint64_t profiledSmoothingTicks = 0;
    uint64_t profiledTotalTicks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeAutotuneProcessor)
};
