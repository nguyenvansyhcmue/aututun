#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>

namespace
{
constexpr double yinThreshold = 0.15;
constexpr float silenceThreshold = 0.0001f;
constexpr int maximumPitchAnalysisFrameSize = 2048;
// The shipped renderer keeps its 128-sample cadence. A separate offline
// build may override this compile-time value to measure control bandwidth;
// it is not a public parameter and it never changes the released binary.
#ifndef VIBE_RENDERER_CONTROL_INTERVAL_SAMPLES
 #define VIBE_RENDERER_CONTROL_INTERVAL_SAMPLES 128
#endif
constexpr int rendererControlIntervalSamples = VIBE_RENDERER_CONTROL_INTERVAL_SAMPLES;
// Offline renderer A/B only. The released component remains spectral.
#ifndef VIBE_USE_CAUSAL_TDPSOLA_RENDERER_CANDIDATE
 #define VIBE_USE_CAUSAL_TDPSOLA_RENDERER_CANDIDATE 0
#endif

// Offline A/B only. -1 preserves the released dynamic margin (two source
// quanta); non-negative values control when a future TD-PSOLA grain becomes
// immutable. This never changes selector or public parameter behaviour.
#ifndef VIBE_TDPSOLA_PLANNER_HANDOFF_MARGIN_SAMPLES
 #define VIBE_TDPSOLA_PLANNER_HANDOFF_MARGIN_SAMPLES -1
#endif

// Offline Studio-quality A/B only. The normal component retains 1152
// samples; a larger value gives retimable TD-PSOLA epochs real future
// control evidence and is reported to the host for correct PDC.
#ifndef VIBE_TDPSOLA_LATENCY_SAMPLES
 #define VIBE_TDPSOLA_LATENCY_SAMPLES 1152
#endif

// Candidate-only hook.  The shipped value remains two control ticks; an
// offline build may measure whether a single certified hard-tune tick closes
// short-note misses without changing the selector or any public control.
#ifndef VIBE_HARD_TUNE_ENGAGE_CONTROL_FRAMES
 #define VIBE_HARD_TUNE_ENGAGE_CONTROL_FRAMES 2
#endif

constexpr double rendererControlDezipperFastSeconds = 0.006;
constexpr double rendererControlDezipperSlowSeconds = 0.032;
// Measured against the Artist F#4 control fixture. The spectral renderer
// needs 9.6% extra control cents to deliver the requested steady correction.
constexpr double rendererDeliveryGain = 1.096;
// Offline calibration hook for the causal TD-PSOLA candidate. Production
// remains unity until Artist-reference and realtime qualification pass.
#ifndef VIBE_TDPSOLA_DELIVERY_GAIN
 #define VIBE_TDPSOLA_DELIVERY_GAIN 1.0
#endif

// Exact 0 ms has its own faster renderer automation path. It remains
// de-zipped (rather than a discontinuous ratio write) but reaches a target
// perceptibly sooner than the normal continuous Retune range.
// Retune 0 is a full correction-strength mode. Phase continuity belongs to
// TD-PSOLA's integrated synthesis phase, not to a second control dezipper.
constexpr double rendererControlDezipperHardTuneSeconds = 0.0;
// The phase-vocoder path is deliberately reserved for a correction that is
// musically audible. This protects an otherwise-natural vocal from needless
// transform coloration around an in-tune note. The release threshold is lower
// than the engage threshold so it cannot chatter on a vibrato boundary.
// Keep a short hysteretic tail after a real correction.  It avoids a wet/dry
// handoff on every small vibrato movement, while allowing the renderer to get
// out of the signal path once a normal Retune setting has genuinely settled.
constexpr double rendererReleaseHoldSeconds = 0.120;
// The renderer is updated every 128 samples.  Even after the trajectory and
// dezipper, a detector recovery can move its ratio by 10+ cents in one tick;
// phase-vocoder formants make that audible as a bubble/click.  Normal Retune
// settings therefore get a final control-slew ceiling.  Explicit 0 ms hard
// tune remains on its own direct path.
constexpr double rendererMaximumContinuousStepCents = 6.0;
// 0 ms selects hard-tune target/trajectory behaviour, not discontinuous
// phase-vocoder automation.  This faster ceiling keeps the intentional
// robotic snap while preventing a 40+ cent one-tick jump from sounding like
// a broken/bouncing note.
// Once the target is confirmed, hard-tune must arrive decisively. 28 cents
// per 128-sample control tick still de-zips the vocoder but avoids turning a
// one-semitone 0 ms landing into a 20+ ms pitch ramp.
constexpr double rendererMaximumHardTuneStepCents = 120.0;

double rendererDeliveryRailCents (double retuneSpeedMs, double flexTunePercent,
                                  double humanizePercent, double naturalVibratoPercent) noexcept
{
    constexpr auto zeroTolerance = 0.05;
    const auto hardTune = retuneSpeedMs <= zeroTolerance
                       && flexTunePercent <= zeroTolerance
                       && humanizePercent <= zeroTolerance
                       && std::abs (naturalVibratoPercent) <= zeroTolerance;
    const auto hasNaturalVibrato = std::abs (naturalVibratoPercent) > zeroTolerance;
    if (hardTune)
        return VIBE_HARD_LOCK_MAXIMUM_LIVE_CORRECTION_CENTS;

    return retuneSpeedMs <= VIBE_MUSICAL_RAIL_MAX_RETUNE_MS
        && ! hasNaturalVibrato
        ? VIBE_MUSICAL_MAXIMUM_LIVE_CORRECTION_CENTS
        : 120.0;
}

constexpr std::array<std::array<bool, 12>, 14> scaleIntervals {{
    {{ true, true, true, true, true, true, true, true, true, true, true, true }},   // Chromatic
    {{ true, false, true, false, true, true, false, true, false, true, false, true }}, // Major
    {{ true, false, true, true, false, true, false, true, true, false, true, false }}, // Natural minor
    {{ true, false, true, false, true, false, false, true, false, true, false, false }}, // Major pentatonic
    {{ true, false, false, true, false, true, false, true, false, false, true, false }}, // Minor pentatonic
    {{ true, false, true, true, false, true, false, true, true, false, false, true }}, // Harmonic minor
    {{ true, false, true, true, false, true, false, true, false, true, false, true }}, // Melodic minor (ascending)
    {{ true, false, true, false, true, true, false, true, false, true, true, false }}, // Dorian
    {{ true, true, false, true, false, true, false, true, true, false, true, false }}, // Phrygian
    {{ true, false, true, false, true, false, true, true, false, true, false, true }}, // Lydian
    {{ true, false, true, false, true, true, false, true, false, true, true, false }}, // Mixolydian
    {{ true, true, false, true, false, true, true, false, true, false, true, false }}, // Locrian
    {{ true, false, false, true, false, true, true, true, false, false, true, false }}, // Minor blues
    {{ true, false, true, false, true, false, true, false, true, false, true, false }}  // Whole tone
}};
constexpr int userScaleIndex = static_cast<int> (scaleIntervals.size());

constexpr uint8_t noteRuleValue (NoteRule rule) noexcept { return static_cast<uint8_t> (rule); }
constexpr NoteRule noteRuleFromValue (uint8_t value) noexcept
{
    return static_cast<NoteRule> (juce::jlimit (0, 4, static_cast<int> (value)));
}
}

VibeAutotuneProcessor::VibeAutotuneProcessor()
    : AudioProcessor (BusesProperties()
                        // Preserve the original Logic-compatible stereo
                        // channel-strip layout. Pitch tracking remains one
                        // vocal trajectory; this declares host I/O only.
                        .withInput  ("Input", juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "VibeAutotuneState", createParameterLayout()),
      pitchHistory (5, 0.0),
      liveDiagnosticFrames (std::make_unique<std::array<SignalChainDiagnosticFrame, liveDiagnosticCapacity>>())
{
    for (auto& rule : pitchClassDefaultPolicy)
        rule.store (noteRuleValue (NoteRule::Legal), std::memory_order_relaxed);
    for (auto& rule : midiNoteOverridePolicy)
        rule.store (noteRuleValue (NoteRule::Legal), std::memory_order_relaxed);
    for (auto& hasOverride : midiNoteHasOverride)
        hasOverride.store (false, std::memory_order_relaxed);
    resetNotePolicyToScale (0, 0);
#if VIBE_TARGET_AWARE_TAIL_TEST
    // A/B component only. The public four controls and selector are kept
    // untouched; this turns on one fixed renderer candidate so Logic capture
    // compares the same preset against the ordinary component.
    CausalTDPSOLARenderer::DiagnosticAblation tailTest;
    tailTest.phaseContinuousRenderer = false;
    tailTest.targetAwareTailReplacement = true;
    tailTest.targetAwareTailOnsetHalfWindows = 0.0;
    tailTest.targetAwareTailFadeHalfWindows = 0.25;
    tailTest.targetAwareOldTailGain = 1.0;
    tailTest.targetAwareMinimumStepCents = 50.0;
    tailTest.targetAwareNewGrainHammingBlend = 0.0;
    tailTest.targetAwareWindowHalfFactorOverride = 0.60;
    setRendererDiagnosticAblation (tailTest);
#endif
}

bool VibeAutotuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& input = layouts.getMainInputChannelSet();
    const auto& output = layouts.getMainOutputChannelSet();
    return input == output
        && (input == juce::AudioChannelSet::mono()
            || input == juce::AudioChannelSet::stereo());
}

juce::AudioProcessorValueTreeState::ParameterLayout VibeAutotuneProcessor::createParameterLayout()
{
    using Parameter = juce::AudioParameterFloat;
    using Choice = juce::AudioParameterChoice;
    using Bool = juce::AudioParameterBool;
    using Range = juce::NormalisableRange<float>;
    using FloatAttributes = juce::AudioParameterFloatAttributes;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<Bool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "retuneSpeed", 1 }, "Retune Speed", Range (0.0f, 400.0f, 0.1f), 100.0f, FloatAttributes().withLabel ("ms")));
    // A new instance must land exactly on the selected scale note. Flex-Tune
    // is an explicit near-target relaxation, and its old non-zero default
    // made slower Retune settings sound as though they never reached target.
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "flexTune", 1 }, "Flex-Tune", Range (0.0f, 100.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "humanize", 1 }, "Humanize", Range (0.0f, 100.0f, 0.1f), 0.0f, FloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "naturalVibrato", 1 }, "Natural Vibrato", Range (-12.0f, 12.0f, 0.1f), 0.0f, FloatAttributes().withLabel (" ms")));
    // Hidden calibration-only controls. -1 restores the immutable production
    // knots; StudioForge uses these rather than environment propagation
    // through the AU host process.
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticRetune10T90Ms", 1 }, "Diagnostic Retune 10 T90", Range (-1.0f, 80.0f, 0.1f), -1.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticRetune25T90Ms", 1 }, "Diagnostic Retune 25 T90", Range (-1.0f, 140.0f, 0.1f), -1.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticFlexResidualScale", 1 }, "Diagnostic Flex Residual", Range (0.5f, 1.5f, 0.01f), 1.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticHumanizeScale", 1 }, "Diagnostic Humanize Scale", Range (0.5f, 1.5f, 0.01f), 1.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticNegativeNaturalPhaseScale", 1 }, "Diagnostic Natural Phase", Range (0.0f, 2.5f, 0.01f), 1.0f));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "diagnosticNaturalSurfaceHardTuneWeight", 1 }, "Diagnostic Natural Hard Tune Surface", Range (0.0f, 1.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "key", 1 }, "Key", juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 6));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "scale", 1 }, "Scale", juce::StringArray {
        "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic",
        "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian",
        "Mixolydian", "Locrian", "Minor Blues", "Whole Tone", "User Scale" }, 2));
    layout.add (std::make_unique<Choice> (juce::ParameterID { "inputType", 1 }, "Input Type", juce::StringArray { "Soprano", "Alto", "Tenor", "Low" }, 1));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "transpose", 1 }, "Transpose", Range (-12.0f, 12.0f, 1.0f), 0.0f, FloatAttributes().withLabel ("st")));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "referencePitch", 1 }, "Reference Pitch", Range (432.0f, 448.0f, 0.1f), 440.0f, FloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "tracking", 1 }, "Tracking", Range (0.0f, 100.0f, 1.0f), 50.0f, FloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<Parameter> (juce::ParameterID { "voicedColour", 1 }, "Voiced Colour", Range (0.0f, 100.0f, 0.1f), 0.0f, FloatAttributes().withLabel ("%")));
    return layout;
}

void VibeAutotuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    midiHeldNotes.fill (false);
    midiNoteOrder.fill (0);
    midiOrderCounter = 0;
    midiTargetNote.store (-1, std::memory_order_relaxed);
    analysisSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    // Live production is native 32 samples. The environment override is a
    // cached diagnostic rollback only; it is never consulted by processBlock.
    const auto* diagnosticQuantum = std::getenv ("VIBE_SOURCE_SCHEDULER_QUANTUM");
    activeSourceSchedulerQuantumSamples = diagnosticQuantum != nullptr && std::atoi (diagnosticQuantum) == 64 ? 64 : 32;
    const auto* retimingOverride = std::getenv ("VIBE_RETIMABLE_EPOCHS");
    const auto productionRetimingEnabled = retimingOverride == nullptr || std::atoi (retimingOverride) != 0;
    correctionTrajectory.setDiagnosticRetuneT90Overrides (-1.0, -1.0);
    const auto maximumLag = maximumPitchAnalysisFrameSize / 2 + 2;
    yinDifference.resize (static_cast<size_t> (maximumLag + 1), 0.0);
    yinCumulativeMean.resize (static_cast<size_t> (maximumLag + 1), 0.0);
    yinCrossCorrelation.resize (static_cast<size_t> (maximumLag + 1), 0.0);
    pitchFrameEnergyPrefix.resize (static_cast<size_t> (maximumPitchAnalysisFrameSize + 1), 0.0);
    pitchAnalysisFifo.resize (maximumPitchAnalysisFrameSize, 0.0f);
    pitchAnalysisFrame.resize (maximumPitchAnalysisFrameSize, 0.0f);
    rawPitchAnalysisFrame.resize (maximumPitchAnalysisFrameSize, 0.0f);
    psolaMaximumBlockSize = juce::jmax (samplesPerBlock, activeSourceSchedulerQuantumSamples);
    psolaMaximumChannels = juce::jlimit (1, 2, getTotalNumInputChannels());
    constexpr auto tdpsolaLatencySamples = VIBE_TDPSOLA_LATENCY_SAMPLES;
    for (auto& renderer : tdpsolaRenderers)
    {
        renderer.prepare (analysisSampleRate, psolaMaximumBlockSize, tdpsolaLatencySamples);
        // The release margin is one active source quantum plus at most one
        // partial output lead. A non-negative offline override measures only
        // the grain commitment horizon; it cannot alter target selection.
        const auto plannerHandoffMargin = VIBE_TDPSOLA_PLANNER_HANDOFF_MARGIN_SAMPLES >= 0
                                      ? VIBE_TDPSOLA_PLANNER_HANDOFF_MARGIN_SAMPLES
                                      : 2 * activeSourceSchedulerQuantumSamples;
        renderer.setPlannerHandoffMarginSamples (static_cast<uint64_t> (plannerHandoffMargin));
        CausalTDPSOLARenderer::DiagnosticAblation productionAblation;
        productionAblation.retimableEpochs = productionRetimingEnabled;
        renderer.setDiagnosticAblation (productionAblation);
    }
    // The TD-PSOLA renderer remains available for controlled diagnostics,
    // but it is not a safe production path yet: a live host can exercise
    // callback timing that the experimental grain scheduler does not own.
    // The spectral renderer has fixed storage and a fixed latency contract,
    // so it is the production live path until TD-PSOLA has equivalent
    // real-device stress coverage.
    usePhaseContinuousRenderer.store (! VIBE_USE_CAUSAL_TDPSOLA_RENDERER_CANDIDATE, std::memory_order_release);
    for (auto& renderer : phaseContinuousRenderers)
        renderer.prepare (analysisSampleRate, psolaMaximumBlockSize);
    // A TD-PSOLA quality candidate must expose the same latency to its
    // control stream and host PDC as it gives its output FIFO.
    rendererLatencySamples = VIBE_USE_CAUSAL_TDPSOLA_RENDERER_CANDIDATE
                           ? tdpsolaLatencySamples : phaseContinuousRenderers[0].latencySamples();
    rendererControlDelaySamples = rendererLatencySamples;
    rendererInput.resize (static_cast<size_t> (psolaMaximumBlockSize * psolaMaximumChannels), 0.0f);
    phaseContinuousRendererOutput.resize (static_cast<size_t> (psolaMaximumBlockSize * psolaMaximumChannels), 0.0f);
    resetPitchRenderer();
    resetPitchAnalysisState (false);
    smoothedPitchRatio = 1.0;
    smoothedCorrectionCents = 0.0;
    previousTrajectoryCorrectionCents = 0.0;
    previousRendererCorrectionCents = 0.0;
    hadValidTrajectoryCorrection = false;
    hadActiveRendererControl = false;
    committedTargetSwitchCount.store (0, std::memory_order_relaxed);
    dryBridgeActivationCount.store (0, std::memory_order_relaxed);
    maximumTrajectoryStepCents.store (0.0f, std::memory_order_relaxed);
    maximumRendererStepCents.store (0.0f, std::memory_order_relaxed);
    correctionTrajectory.reset();
    // Listening-build default: evaluate the bounded adjacent-note predictor
    // on the live hard-tune path. It does not alter TD-PSOLA timing, OLA, or
    // latency; this switch exists solely to audition target timing.
    correctionTrajectory.setRapidAdjacentTargetSelectionEnabled (true);
    noteTransition.reset();
    latestCorrectionTrajectory = {};
    resetPsolaRenderer();
    processedInputSamples = 0;
    currentAnalysisLatencySamples = 0;
    setLatencySamples (rendererLatencySamples);
    clearPitchMonitor (false);
}

void VibeAutotuneProcessor::releaseResources()
{
}

void VibeAutotuneProcessor::clearPitchMonitor (bool bypassed)
{
    latestDetectedMidiNote.store (-1, std::memory_order_relaxed);
    latestTargetMidiNote.store (-1, std::memory_order_relaxed);
    latestOutputMidiNote.store (-1, std::memory_order_relaxed);
    latestInputCents.store (0.0f, std::memory_order_relaxed);
    latestCentsDeviation.store (0.0f, std::memory_order_relaxed);
    latestVoiced.store (false, std::memory_order_relaxed);
    latestBypassed.store (bypassed, std::memory_order_relaxed);
    publishPitchTrackingData (std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::quiet_NaN(), false);
    smoothedPitchRatio = 1.0;
    smoothedCorrectionCents = 0.0;
    correctionTrajectory.reset();
    noteTransition.reset();
    latestCorrectionTrajectory = {};
}

VibeAutotuneProcessor::PitchAnalysisConfig VibeAutotuneProcessor::getPitchAnalysisConfig (int inputType) const noexcept
{
    // Diagnostic-only compile-time probe. Defaults preserve the released Alto contract.
#ifndef VIBE_ALTO_ANALYSIS_FRAME_SAMPLES
#define VIBE_ALTO_ANALYSIS_FRAME_SAMPLES 2048
#endif
#ifndef VIBE_ALTO_ANALYSIS_HOP_SAMPLES
#define VIBE_ALTO_ANALYSIS_HOP_SAMPLES 256
#endif
    switch (juce::jlimit (0, 3, inputType))
    {
        case 0: return { 1024, 512, 180.0, 1100.0, 0 };
        case 1: return { VIBE_ALTO_ANALYSIS_FRAME_SAMPLES, VIBE_ALTO_ANALYSIS_HOP_SAMPLES, 110.0, 700.0, 1 };
        case 2: return { 2048, 512, 80.0, 500.0, 2 };
        case 3: return { 2048, 1024, 55.0, 300.0, 3 };
        default: break;
    }

    return {};
}

bool VibeAutotuneProcessor::scaleContainsPitchClass (int root, int scale, int pitchClass) noexcept
{
    const auto safeRoot = juce::jlimit (0, 11, root);
    const auto safeScale = juce::jlimit (0, static_cast<int> (scaleIntervals.size()) - 1, scale);
    const auto safePitchClass = (pitchClass % 12 + 12) % 12;
    return scaleIntervals[static_cast<size_t> (safeScale)]
                         [static_cast<size_t> ((safePitchClass - safeRoot + 12) % 12)];
}

void VibeAutotuneProcessor::resetNotePolicyToScale (int key, int scale) noexcept
{
    const auto safeScale = juce::jlimit (0, userScaleIndex, scale);
    if (safeScale == userScaleIndex)
    {
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
            pitchClassDefaultPolicy[static_cast<size_t> (pitchClass)].store (
                noteRuleValue (NoteRule::IllegalNearest), std::memory_order_relaxed);
        for (auto& hasOverride : midiNoteHasOverride)
            hasOverride.store (false, std::memory_order_relaxed);
        enabledNoteMask.store (0, std::memory_order_relaxed);
        baseScaleMask.store (0, std::memory_order_relaxed);
        synchronisedPolicyKey = juce::jlimit (0, 11, key);
        synchronisedPolicyScale = userScaleIndex;
        notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    uint16_t mask = 0;
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
    {
        const auto rule = scaleContainsPitchClass (key, safeScale, pitchClass)
                        ? NoteRule::Legal : NoteRule::IllegalNearest;
        pitchClassDefaultPolicy[static_cast<size_t> (pitchClass)].store (noteRuleValue (rule), std::memory_order_relaxed);
        if (isLegalNoteRule (rule))
            mask |= static_cast<uint16_t> (1u << pitchClass);
    }
    for (auto& hasOverride : midiNoteHasOverride)
        hasOverride.store (false, std::memory_order_relaxed);

    enabledNoteMask.store (mask, std::memory_order_relaxed);
    baseScaleMask.store (mask, std::memory_order_relaxed);
    notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
    synchronisedPolicyKey = juce::jlimit (0, 11, key);
    synchronisedPolicyScale = safeScale;
}

void VibeAutotuneProcessor::synchroniseNotePolicyToScale() noexcept
{
    const auto key = juce::jlimit (0, 11, static_cast<int> (parameters.getRawParameterValue ("key")->load (std::memory_order_relaxed)));
    const auto scale = juce::jlimit (0, userScaleIndex,
                                     static_cast<int> (parameters.getRawParameterValue ("scale")->load (std::memory_order_relaxed)));
    if (key == synchronisedPolicyKey && scale == synchronisedPolicyScale)
        return;

    resetNotePolicyToScale (key, scale);
}

NotePolicySnapshot VibeAutotuneProcessor::makeNotePolicySnapshot() const noexcept
{
    NotePolicySnapshot snapshot;
    snapshot.groupOctaves = groupOctaves.load (std::memory_order_relaxed);
    // Voice is both an analyser profile and a musical target range. The
    // immutable snapshot keeps this UI-to-audio handoff realtime-safe.
    snapshot.targetRange = vocalTargetRangeForInputType (static_cast<int> (
        parameters.getRawParameterValue ("inputType")->load (std::memory_order_relaxed)));
    snapshot.midiTargetNote = midiTargetNote.load (std::memory_order_relaxed);
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        snapshot.pitchClassDefaults[static_cast<size_t> (pitchClass)] = noteRuleFromValue (
            pitchClassDefaultPolicy[static_cast<size_t> (pitchClass)].load (std::memory_order_relaxed));
    for (int midiNote = 0; midiNote <= 127; ++midiNote)
    {
        snapshot.midiNoteOverrides[static_cast<size_t> (midiNote)] = noteRuleFromValue (
            midiNoteOverridePolicy[static_cast<size_t> (midiNote)].load (std::memory_order_relaxed));
        snapshot.hasMidiNoteOverride[static_cast<size_t> (midiNote)] =
            midiNoteHasOverride[static_cast<size_t> (midiNote)].load (std::memory_order_relaxed);
    }
    return snapshot;
}

void VibeAutotuneProcessor::resetPitchAnalysisState (bool clearMonitor)
{
    std::fill (pitchHistory.begin(), pitchHistory.end(), 0.0);
    std::fill (pitchAnalysisFifo.begin(), pitchAnalysisFifo.end(), 0.0f);
    pitchHistoryIndex = 0;
    pitchHistoryCount = 0;
    pitchHistoryInitialised = false;
    pitchAnalysisWriteIndex = 0;
    pitchAnalysisSamplesAvailable = 0;
    pitchAnalysisSamplesSinceLastFrame = 0;
    voicingAttackFrames = 0;
    voicingReleaseFrames = 0;
    pendingOctaveFrames = 0;
    pendingOctaveFrequency = std::numeric_limits<double>::quiet_NaN();
    pendingPitchJumpFrames = 0;
    pendingPitchJumpFrequency = std::numeric_limits<double>::quiet_NaN();
    pendingSubharmonicFrames = 0;
    pendingSubharmonicFrequency = std::numeric_limits<double>::quiet_NaN();
    stableLog2F0 = std::numeric_limits<double>::quiet_NaN();
    noiseFloorRms = 1.0e-5;
    previousAnalysisFrameRms = 0.0;
    voicedRmsEnvelope = 0.0;
    audioTransientFramesRemaining = 0;
    smoothedPitchRatio = 1.0;
    smoothedCorrectionCents = 0.0;
    previousTrajectoryCorrectionCents = 0.0;
    hadValidTrajectoryCorrection = false;
    correctionTrajectory.reset();
    noteTransition.reset();
    latestCorrectionTrajectory = {};

    if (clearMonitor)
        clearPitchMonitor (false);
}

void VibeAutotuneProcessor::publishPitchTrackingData (double rawF0Hz, double stableF0Hz,
                                                       double confidence, bool voiced) noexcept
{
    latestRawF0Hz.store (rawF0Hz, std::memory_order_relaxed);
    latestStableF0Hz.store (stableF0Hz, std::memory_order_relaxed);
    latestPitchConfidence.store (confidence, std::memory_order_relaxed);
    latestPitchTimestampSamples.store (processedInputSamples, std::memory_order_relaxed);
    latestAnalysisLatencySamples.store (currentAnalysisLatencySamples, std::memory_order_relaxed);
}

void VibeAutotuneProcessor::resetPsolaRenderer() noexcept
{
    psolaControlWriteIndex = 0;
    psolaControlInsertionSequence = 0;
    controlMaterializationEventCount = 0;
    psolaTargetEventId = 0;
    psolaTargetEventTimestampSamples = 0;
    for (auto& frame : psolaControlFrames)
        frame = {};
}

void VibeAutotuneProcessor::resetPitchRenderer() noexcept
{
    for (auto& renderer : tdpsolaRenderers)
        renderer.reset();
    for (auto& renderer : phaseContinuousRenderers)
        renderer.reset();
    rendererCorrectionEngaged = false;
    rendererReleasingToUnity = false;
    rendererEngageControlFrames = 0;
    rendererReleaseControlFrames = 0;
    renderedPitchRatio = 1.0;
    lastValidRendererF0Hz = 0.0;
    lastValidRendererPitchRatio = 1.0;
    hasLastValidRendererControl = false;
    rendererInvalidControlSamples = 0;
    previousRendererCorrectionCents = 0.0;
    hadActiveRendererControl = false;
    tdpsolaReentryDrySamples = 0;
    rendererDiscontinuityData = {};
    previousRendererOutputSample = 0.0f;
    hasPreviousRendererOutputSample = false;
    std::fill (rendererInput.begin(), rendererInput.end(), 0.0f);
    sourceStageFill = 0;
    sourceStageStartSample = processedInputSamples;
    sourceSamplesReceived = 0;
    sourceSamplesScheduled = 0;
    sourceQuantumsScheduled = 0;
    sourceDropCount = 0;
    sourceDuplicateCount = 0;
    schedulerGapCount = 0;
    schedulerOverlapCount = 0;
    nextScheduledSourceSample = processedInputSamples;
    sourceStage.fill (0.0f);
}

void VibeAutotuneProcessor::processPitchRenderer (juce::AudioBuffer<float>& buffer,
                                                         int startSample, int sampleCount,
                                                         bool outputCorrectionActive, double outputPitchRatio,
                                                         double outputStableF0Hz,
                                                         bool inputPsolaVoiced, double inputPitchRatio,
                                                         double inputStableF0Hz,
                                                         double inputCommittedTargetHz,
                                                         uint64_t canonicalSourceControlTime,
                                                         uint64_t outputControlTime,
                                                         const float* const* stagedSource,
                                                         int stagedSourceSamples) noexcept
{
    const auto channels = buffer.getNumChannels();
    const auto samples = sampleCount;
    if (channels <= 0 || channels > psolaMaximumChannels || samples <= 0 || samples > psolaMaximumBlockSize
        || startSample < 0 || startSample + samples > buffer.getNumSamples())
        return;

    std::array<const float*, 2> inputPointers {};
    std::array<float*, 2> outputPointers {};

    // Pitch is tracked from channel 0.  A decorrelated stereo input cannot
    // safely be treated as two independent voices driven by that one F0:
    // re-synthesising both channels retains the side/double as an audible
    // harmony.  During correction, collapse only such a non-coherent source
    // to the tracked vocal before it reaches the pitch renderer. Coherent
    // dual-mono vocal tracks retain their normal stereo layout unchanged.
    auto forceMonoCorrection = false;
    if (outputCorrectionActive && channels == 2)
    {
        const auto* left = buffer.getReadPointer (0, startSample);
        const auto* right = buffer.getReadPointer (1, startSample);
        double crossEnergy = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
        for (int sample = 0; sample < samples; ++sample)
        {
            const auto l = static_cast<double> (left[sample]);
            const auto r = static_cast<double> (right[sample]);
            crossEnergy += l * r;
            leftEnergy += l * l;
            rightEnergy += r * r;
        }
        const auto correlation = crossEnergy / std::sqrt (leftEnergy * rightEnergy + 1.0e-12);
        forceMonoCorrection = correlation < 0.92;
    }
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* copiedInput = rendererInput.data() + static_cast<size_t> (channel * psolaMaximumBlockSize);
        const auto* source = buffer.getReadPointer (forceMonoCorrection ? 0 : channel, startSample);
        std::copy_n (source, samples, copiedInput);
        inputPointers[static_cast<size_t> (channel)] = copiedInput;
        outputPointers[static_cast<size_t> (channel)] = buffer.getWritePointer (channel, startSample);
    }

    // TD-PSOLA consumes source-time F0/ratio and owns the latency-aligned
    // dry fallback plus equal-power gate handoff.
    const auto sourceRatio = phaseForceUnityRatioForDiagnostic.load (std::memory_order_relaxed) ? 1.0
                           : phaseForceStaticUpRatioForDiagnostic.load (std::memory_order_relaxed) ? std::exp2 (1.0 / 12.0)
                           : inputPsolaVoiced ? inputPitchRatio : 1.0;
    const auto sourceF0 = inputPsolaVoiced ? inputStableF0Hz
                                           : std::numeric_limits<double>::quiet_NaN();
    if (usePhaseContinuousRenderer.load (std::memory_order_acquire))
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            auto* alternativeOutput = phaseContinuousRendererOutput.data()
                                    + static_cast<size_t> (channel * psolaMaximumBlockSize);
            phaseContinuousRenderers[static_cast<size_t> (channel)].process (
                inputPointers[static_cast<size_t> (channel)], alternativeOutput, samples,
                sourceRatio, outputCorrectionActive, sourceF0);
            std::copy_n (alternativeOutput, samples, outputPointers[static_cast<size_t> (channel)]);
        }
    }
    else
    {
        // TD-PSOLA writes a grain from the input arriving *now* into its
        // future OLA queue. Unlike the spectral renderer, it must therefore
        // receive the source-time control at this input timestamp, not the
        // latency-delayed control which describes audio becoming audible now.
        // Feeding the delayed stream here was the reason its note handoffs
        // could be clean yet land on an already superseded target.
        // The caller owns the single source/control coordinate. Do not
        // reconstruct it from `processedInputSamples`: analysis mutates that
        // member before this renderer pass, which previously skewed this
        // lookup by one host block.
        const auto sourceControlTime = canonicalSourceControlTime;
        const auto outputStateControlTime = outputControlTime != 0 ? outputControlTime : sourceControlTime;
        const auto sourceControl = getRendererControlFrame (sourceControlTime, 2);
        debugRendererControlLookupTime = sourceControlTime;
        debugRendererControl = sourceControl;
        const auto sourceControlValid = sourceControl.valid && ! sourceControl.holdDry
                                     && std::isfinite (sourceControl.stableF0Hz)
                                     && sourceControl.stableF0Hz > 0.0
                                     && std::isfinite (sourceControl.pitchRatio);
        const auto sourceTargetHz = sourceControlValid && sourceControl.targetMidi >= 0
                                  ? static_cast<double> (parameters.getRawParameterValue ("referencePitch")
                                                               ->load (std::memory_order_relaxed))
                                        * std::exp2 ((static_cast<double> (sourceControl.targetMidi) - 69.0) / 12.0)
                                  : std::numeric_limits<double>::quiet_NaN();
        for (int channel = 0; channel < channels; ++channel)
        {
            auto& renderer = tdpsolaRenderers[static_cast<size_t> (channel)];
            const auto schedulerF0 = sourceControlValid ? sourceControl.stableF0Hz : sourceF0;
            const auto schedulerRatio = sourceControlValid ? sourceControl.pitchRatio : sourceRatio;
            // Output state is materialized independently before ingestion.
            // Phase 2B can therefore read an output span without requiring a
            // source scheduler quantum in that same host callback.
            // Output span state is independent from ingestion, but it must
            // observe the control already causally materialised for this
            // source quantum.  `outputStateControlTime` is retained for
            // provenance; it must not expose a later source control.
            juce::ignoreUnused (outputStateControlTime);
            const auto outputControl = sourceControl;
            const auto outputControlValid = outputControl.valid && ! outputControl.holdDry
                                         && std::isfinite (outputControl.stableF0Hz)
                                         && outputControl.stableF0Hz > 0.0
                                         && std::isfinite (outputControl.pitchRatio);
            const auto outputTargetHz = outputControlValid && outputControl.targetMidi >= 0
                                      ? static_cast<double> (parameters.getRawParameterValue ("referencePitch")
                                                                   ->load (std::memory_order_relaxed))
                                            * std::exp2 ((static_cast<double> (outputControl.targetMidi) - 69.0) / 12.0)
                                      : std::numeric_limits<double>::quiet_NaN();
            const auto outputState = renderer.prepareOutputRenderState (
                inputPointers[static_cast<size_t> (channel)], samples,
                outputControlValid ? outputControl.stableF0Hz : schedulerF0,
                outputControlValid ? outputControl.pitchRatio : schedulerRatio,
                outputControlValid, false, outputTargetHz,
                outputControlValid ? outputControl.targetEventId : 0);
            if (stagedSourceSamples != 0)
            {
                const auto* schedulerSource = stagedSource != nullptr
                                            ? stagedSource[static_cast<size_t> (channel)]
                                            : inputPointers[static_cast<size_t> (channel)];
                const auto schedulerSamples = stagedSource != nullptr ? stagedSourceSamples : samples;
                renderer.ingestAndSchedule (
                    schedulerSource, schedulerSamples,
                    schedulerF0, schedulerRatio, sourceControlValid, false, sourceTargetHz,
                    sourceControlValid ? sourceControl.residualPitchCents : 0.0,
                    sourceControlValid && sourceControl.continuousReturnExcursionOwnsControl,
                    sourceControl.continuousReturnExcursionPhase,
                    sourceControlValid ? sourceControl.targetMidi : -1,
                    sourceControlValid ? sourceControl.timestampSamples : 0,
                    sourceControlValid ? sourceControl.targetEventId : 0,
                    sourceControlValid ? sourceControl.debugPublicationSample : 0);
            }
            renderer.readOutput (outputPointers[static_cast<size_t> (channel)], samples, outputState);
        }
    }

    for (int sample = 0; sample < samples; ++sample)
    {
        const auto wetMix = outputCorrectionActive ? 1.0f : 0.0f;

        const auto rendered = buffer.getReadPointer (0, startSample)[sample];
        if (hasPreviousRendererOutputSample)
        {
            const auto step = std::abs (rendered - previousRendererOutputSample);
            if (step > rendererDiscontinuityData.step)
            {
                rendererDiscontinuityData.sampleIndex = processedInputSamples
                                                      + static_cast<uint64_t> (startSample + sample);
                rendererDiscontinuityData.step = step;
                rendererDiscontinuityData.sourceF0Hz = outputStableF0Hz;
                rendererDiscontinuityData.pitchRatio = outputPitchRatio;
                rendererDiscontinuityData.synthesisPeriod = 0.0;
                rendererDiscontinuityData.sourcePeriod = 0.0;
                rendererDiscontinuityData.sourcePeriodJump = 0.0;
                rendererDiscontinuityData.tdpsolaOutputStep = 0.0;
                rendererDiscontinuityData.tdpsolaFallback = false;
                rendererDiscontinuityData.dryWetMix = wetMix;
                rendererDiscontinuityData.voiced = outputCorrectionActive;
            }
        }
        previousRendererOutputSample = rendered;
        hasPreviousRendererOutputSample = true;

    }
}

void VibeAutotuneProcessor::pushPsolaControlFrame (uint64_t timestampSamples, double stableF0Hz,
                                                    double pitchRatio, double residualPitchCents,
                                                    bool valid, bool holdDry,
                                                    bool targetBridge, bool transientBridge, int targetMidi,
                                                    double targetPitchCents, uint64_t targetEventId,
                                                    uint64_t targetEventTimestampSamples,
                                                    int rendererCrossingTargetMidi,
                                                    double rendererCrossingProgress,
                                                    bool continuousReturnExcursionActive,
                                                    uint8_t continuousReturnExcursionPhase,
                                                    bool continuousReturnExcursionConsumed,
                                                    bool continuousReturnExcursionOwnsControl,
                                                    const CorrectionTrajectoryData* materialization) noexcept
{
    auto& frame = psolaControlFrames[static_cast<size_t> (psolaControlWriteIndex)];
    frame = { timestampSamples, stableF0Hz, pitchRatio, residualPitchCents,
              valid, holdDry, targetBridge, transientBridge,
              targetMidi, targetPitchCents, targetEventId, targetEventTimestampSamples,
              rendererCrossingTargetMidi, rendererCrossingProgress,
              continuousReturnExcursionActive, continuousReturnExcursionPhase,
              continuousReturnExcursionConsumed, continuousReturnExcursionOwnsControl };
    frame.debugWritten = true;
    frame.debugInsertionSequence = psolaControlInsertionSequence++;
    frame.debugPublicationSample = debugControlPublicationInputSample;
    for (auto& renderer : tdpsolaRenderers)
        renderer.publishControlFrame ({ frame.timestampSamples, frame.debugPublicationSample,
                                        frame.targetEventId, frame.pitchRatio, frame.targetMidi });
    if (materialization != nullptr)
    {
        frame.debugControlAdvanceSamples = materialization->controlAdvanceSamples;
        frame.debugControlCorrectionBefore = materialization->controlCorrectionBefore;
        frame.debugControlCorrectionAfter = materialization->controlCorrectionAfter;

        recordControlMaterializationEvent (timestampSamples, targetEventId,
                                           targetEventTimestampSamples, pitchRatio,
                                           *materialization, true);
    }
    psolaControlWriteIndex = (psolaControlWriteIndex + 1) % static_cast<int> (psolaControlFrames.size());
}

void VibeAutotuneProcessor::recordControlMaterializationEvent (
    uint64_t timestampSamples, uint64_t targetEventId, uint64_t targetEventTimestampSamples,
    double pitchRatio, const CorrectionTrajectoryData& materialization,
    bool publishedToPsolaRing) noexcept
{
    if (! signalChainDiagnosticEnabled.load (std::memory_order_relaxed))
        return;

    auto& event = controlMaterializationEvents[static_cast<size_t> (
        controlMaterializationEventCount % static_cast<uint64_t> (controlMaterializationEventCapacity))];
    event.sequence = controlMaterializationEventCount;
    event.timestampSamples = timestampSamples;
    event.targetEventId = targetEventId;
    event.targetEventTimestampSamples = targetEventTimestampSamples;
    event.publishedAtInputSample = debugControlPublicationInputSample;
    event.hostBlockStartSample = debugControlPublicationBlockStartSample;
    event.hostBlockEndSample = debugControlPublicationBlockEndSample;
    event.publishedToPsolaRing = publishedToPsolaRing;
    event.controlAdvanceSamples = materialization.controlAdvanceSamples;
    event.transitionStartCents = materialization.transitionStartCents;
    event.transitionTargetCents = materialization.transitionTargetCents;
    event.transitionProgress = materialization.transitionProgress;
    event.transitionDurationMs = materialization.transitionDurationMs;
    event.correctionBeforeAdvance = materialization.controlCorrectionBefore;
    event.correctionAfterAdvance = materialization.controlCorrectionAfter;
    event.scheduledRatio = pitchRatio;
    event.scheduledCorrectionCents = 1200.0 * std::log2 (pitchRatio);
    ++controlMaterializationEventCount;
}

void VibeAutotuneProcessor::captureRendererControlLookupTrace (
    uint64_t queryTimestampSamples, uint8_t lookupRole,
    const PsolaControlFrame& rawBefore, bool foundBefore,
    const PsolaControlFrame& rawAfter, bool foundAfter, const PsolaControlFrame& returned,
    bool interpolationEligible, bool interpolationApplied, double interpolationT) const noexcept
{
    // Narrow fixture-specific forensic capture. It never participates in a
    // control decision and is inactive outside the offline diagnostic.
    constexpr std::array<uint64_t, 4> tracedQueries { 91008, 236288, 236416, 236544 };
    if (! signalChainDiagnosticEnabled.load (std::memory_order_relaxed)
        || std::find (tracedQueries.begin(), tracedQueries.end(), queryTimestampSamples) == tracedQueries.end()
        || rendererControlLookupTraceCount >= rendererControlLookupTraceCapacity)
        return;

    const auto describe = [] (const PsolaControlFrame& source, int index) noexcept
    {
        RendererControlRingEntry entry;
        entry.ringIndex = index;
        entry.written = source.debugWritten;
        entry.insertionSequence = source.debugInsertionSequence;
        entry.timestampSamples = source.timestampSamples;
        entry.pitchRatio = source.pitchRatio;
        entry.correctionCents = std::isfinite (source.pitchRatio) && source.pitchRatio > 0.0
                               ? 1200.0 * std::log2 (source.pitchRatio)
                               : std::numeric_limits<double>::quiet_NaN();
        entry.targetEventId = source.targetEventId;
        entry.valid = source.valid;
        entry.holdDry = source.holdDry;
        return entry;
    };

    auto& trace = rendererControlLookupTraces[static_cast<size_t> (rendererControlLookupTraceCount++)];
    trace = {};
    trace.queryTimestampSamples = queryTimestampSamples;
    trace.lookupRole = lookupRole;
    trace.psolaControlWriteIndex = psolaControlWriteIndex;
    trace.foundBefore = foundBefore;
    trace.foundAfter = foundAfter;
    trace.interpolationEligible = interpolationEligible;
    trace.interpolationApplied = interpolationApplied;
    trace.interpolationT = interpolationT;
    trace.before = describe (rawBefore, -1);
    trace.after = describe (rawAfter, -1);
    trace.returned = describe (returned, -1);
    for (size_t index = 0; index < psolaControlFrames.size(); ++index)
        trace.ring[index] = describe (psolaControlFrames[index], static_cast<int> (index));
}

VibeAutotuneProcessor::PsolaControlFrame
VibeAutotuneProcessor::getRendererControlFrame (uint64_t outputTimelineSample,
                                                 uint8_t lookupRole) const noexcept
{
    PsolaControlFrame before, after;
    bool foundBefore = false, foundAfter = false;
    for (const auto& candidate : psolaControlFrames)
    {
        if (candidate.timestampSamples <= outputTimelineSample)
        {
            if (! foundBefore || candidate.timestampSamples > before.timestampSamples)
            {
                before = candidate;
                foundBefore = true;
            }
        }
        else if (! foundAfter || candidate.timestampSamples < after.timestampSamples)
        {
            after = candidate;
            foundAfter = true;
        }
    }

    // Both F0 and ratio are continuous control signals. Interpolate only
    // inside a certified voiced run; an invalid frame is a hard unvoiced
    // boundary and is deliberately handed to the renderer's dry crossfade.
    const auto rawBefore = before;
    const auto rawAfter = after;
    const auto interpolationEligible = foundBefore && foundAfter && before.valid && after.valid
        && ! before.holdDry && ! after.holdDry
        && std::isfinite (before.stableF0Hz) && std::isfinite (after.stableF0Hz)
        && before.stableF0Hz > 0.0 && after.stableF0Hz > 0.0;
    auto interpolationApplied = false;
    auto interpolationT = 0.0;
    if (interpolationEligible)
    {
        const auto span = after.timestampSamples - before.timestampSamples;
        if (span > 0)
        {
            interpolationT = std::clamp (static_cast<double> (outputTimelineSample - before.timestampSamples)
                                         / static_cast<double> (span), 0.0, 1.0);
            before.stableF0Hz += (after.stableF0Hz - before.stableF0Hz) * interpolationT;
            before.pitchRatio += (after.pitchRatio - before.pitchRatio) * interpolationT;
            before.residualPitchCents += (after.residualPitchCents - before.residualPitchCents) * interpolationT;
            interpolationApplied = true;
        }
    }
    captureRendererControlLookupTrace (outputTimelineSample, lookupRole,
                                       rawBefore, foundBefore, rawAfter, foundAfter,
                                       before, interpolationEligible, interpolationApplied, interpolationT);
    return foundBefore ? before : PsolaControlFrame {};
}


void VibeAutotuneProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                   juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    for (int offset = 0; offset < buffer.getNumSamples(); offset += rendererControlIntervalSamples)
        processPitchRenderer (buffer, offset,
                                    juce::jmin (rendererControlIntervalSamples, buffer.getNumSamples() - offset),
                                    false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                    false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                    processedInputSamples + static_cast<uint64_t> (offset));
    processedInputSamples += static_cast<uint64_t> (juce::jmax (0, buffer.getNumSamples()));
    currentAnalysisLatencySamples = pitchAnalysisConfig.frameSize;
    clearPitchMonitor (true);
}

bool VibeAutotuneProcessor::findPitchCandidate (int expectedPeriod, int minimumLag, int maximumLag,
                                                PitchCandidate& candidate) const noexcept
{
    if (expectedPeriod < minimumLag || expectedPeriod > maximumLag)
        return false;

    const auto radius = juce::jmax (3, expectedPeriod / 10);
    const auto start = juce::jlimit (minimumLag, maximumLag, expectedPeriod - radius);
    const auto end = juce::jlimit (minimumLag, maximumLag, expectedPeriod + radius);
    auto bestPeriod = start;

    for (int lag = start + 1; lag <= end; ++lag)
        if (yinCumulativeMean[static_cast<size_t> (lag)] < yinCumulativeMean[static_cast<size_t> (bestPeriod)])
            bestPeriod = lag;

    const auto left = juce::jlimit (minimumLag, maximumLag, bestPeriod - radius);
    const auto right = juce::jlimit (minimumLag, maximumLag, bestPeriod + radius);
    candidate.period = bestPeriod;
    candidate.periodicity = juce::jlimit (0.0, 1.0, 1.0 - yinCumulativeMean[static_cast<size_t> (bestPeriod)]);
    candidate.valleyProminence = juce::jmax (0.0, 0.5 * (yinCumulativeMean[static_cast<size_t> (left)]
                                                          + yinCumulativeMean[static_cast<size_t> (right)])
                                                  - yinCumulativeMean[static_cast<size_t> (bestPeriod)]);
    return true;
}

void VibeAutotuneProcessor::publishUnvoicedFrame (uint64_t frameEndSample)
{
    const auto notePolicy = makeNotePolicySnapshot();
    latestCorrectionTrajectory = correctionTrajectory.update (std::numeric_limits<double>::quiet_NaN(), 440.0,
                                                               0, notePolicy,
                                                               0.0, 0.0, 0.0, 0.0, analysisSampleRate, pitchAnalysisConfig.hopSize, false);
    smoothedPitchRatio = 1.0;
    smoothedCorrectionCents = 0.0;
    pushPsolaControlFrame (frameEndSample, std::numeric_limits<double>::quiet_NaN(), 1.0, 0.0, false);
    latestDetectedMidiNote.store (-1, std::memory_order_relaxed);
    latestTargetMidiNote.store (-1, std::memory_order_relaxed);
    latestOutputMidiNote.store (-1, std::memory_order_relaxed);
    latestInputCents.store (0.0f, std::memory_order_relaxed);
    latestCentsDeviation.store (0.0f, std::memory_order_relaxed);
    latestVoiced.store (false, std::memory_order_relaxed);
    latestBypassed.store (false, std::memory_order_relaxed);
    processedInputSamples = frameEndSample;
    currentAnalysisLatencySamples = pitchAnalysisConfig.frameSize;
    publishPitchTrackingData (std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::quiet_NaN(), false);
}

void VibeAutotuneProcessor::updateCorrectionTrajectory (double stableFrequency, bool voiced,
                                                         uint64_t frameEndSample,
                                                         double rawFrequency,
                                                         double rawConfidence) noexcept
{
    const auto referencePitch = static_cast<double> (parameters.getRawParameterValue ("referencePitch")->load (std::memory_order_relaxed));
    const auto transpose = static_cast<int> (parameters.getRawParameterValue ("transpose")->load (std::memory_order_relaxed));
    const auto retuneSpeedMs = static_cast<double> (parameters.getRawParameterValue ("retuneSpeed")->load (std::memory_order_relaxed));
    const auto flexTunePercent = static_cast<double> (parameters.getRawParameterValue ("flexTune")->load (std::memory_order_relaxed));
    const auto humanizePercent = static_cast<double> (parameters.getRawParameterValue ("humanize")->load (std::memory_order_relaxed));
    const auto naturalVibratoPercent = static_cast<double> (parameters.getRawParameterValue ("naturalVibrato")->load (std::memory_order_relaxed));
    const auto policyRevision = notePolicyRevision.load (std::memory_order_relaxed);
    if (policyRevision != appliedNotePolicyRevision)
    {
        correctionTrajectory.resetTargetSelection();
        latestCorrectionTrajectory = {};
        appliedNotePolicyRevision = policyRevision;
    }
    const auto notePolicy = makeNotePolicySnapshot();

    // The previous analysis call has already scheduled controls through
    // frameEndSample - 128.  Advance the final 128-sample segment before
    // this YIN frame can commit a new target, so a re-anchor starts from the
    // actual in-flight target rather than from an old note or unity.
    const auto frameCentreSample = frameEndSample > static_cast<uint64_t> (pitchAnalysisConfig.frameSize / 2)
                                 ? frameEndSample - static_cast<uint64_t> (pitchAnalysisConfig.frameSize / 2) : 0;
    const auto finalScheduled = correctionTrajectory.advanceScheduledTransition (rendererControlIntervalSamples,
                                                                                   analysisSampleRate);
    if (finalScheduled.hasValidCorrection)
        recordControlMaterializationEvent (frameCentreSample, psolaTargetEventId,
                                           psolaTargetEventTimestampSamples, finalScheduled.pitchRatio,
                                           finalScheduled, false);
    latestCorrectionTrajectory = correctionTrajectory.update (stableFrequency, referencePitch, transpose,
                                                               notePolicy, retuneSpeedMs, flexTunePercent,
                                                               humanizePercent, naturalVibratoPercent,
                                                               analysisSampleRate, pitchAnalysisConfig.hopSize, voiced,
                                                               audioTransientFramesRemaining > 0,
                                                               TuningPolicy::Standard,
                                                               true,
                                                               rawFrequency, rawConfidence);
    // Ordinary target changes now receive a continuous target trajectory in
    // CorrectionTrajectory. NoteTransition is therefore an exceptional dry
    // safeguard for a large erroneous jump only.
    const auto targetBridge = noteTransition.update (voiced,
                                                      latestCorrectionTrajectory.extremeTargetChange,
                                                      retuneSpeedMs == 0.0);
    const auto transientBridge = voiced && audioTransientFramesRemaining > 0;
    // The trajectory's transient guard already slows correction around a
    // plosive/fricative. Routing that same one-frame event through dry caused
    // a wet->dry->wet renderer lifecycle change inside normal speech, which
    // is more audible than the guarded pitch control itself.
    const auto holdRendererDry = targetBridge;
    if (latestCorrectionTrajectory.targetChanged)
        committedTargetSwitchCount.fetch_add (1, std::memory_order_relaxed);
    if (targetBridge)
        dryBridgeActivationCount.fetch_add (1, std::memory_order_relaxed);
    if (voiced && audioTransientFramesRemaining > 0)
        --audioTransientFramesRemaining;
    smoothedPitchRatio = latestCorrectionTrajectory.hasValidCorrection ? latestCorrectionTrajectory.pitchRatio : 1.0;
    smoothedCorrectionCents = latestCorrectionTrajectory.hasValidCorrection ? latestCorrectionTrajectory.correctionCents : 0.0;
    if (latestCorrectionTrajectory.hasValidCorrection)
    {
        // The +9.6% delivery trim was measured for the retained spectral
        // diagnostic renderer. TD-PSOLA has a different delivered-pitch
        // gain, so applying that spectral trim to production makes every
        // held correction overshoot by roughly six cents.
        const auto rendererDeliveryCompensation = usePhaseContinuousRenderer.load (std::memory_order_relaxed)
                                              ? rendererDeliveryGain : VIBE_TDPSOLA_DELIVERY_GAIN;
        const auto deliveryRailCents = rendererDeliveryRailCents (retuneSpeedMs, flexTunePercent,
                                                                   humanizePercent, naturalVibratoPercent);
        const auto compensatedCents = juce::jlimit (-deliveryRailCents,
                                                     deliveryRailCents,
                                                     smoothedCorrectionCents * rendererDeliveryCompensation);
        smoothedPitchRatio = std::exp2 (compensatedCents / 1200.0);
        if (hadValidTrajectoryCorrection)
        {
            const auto trajectoryStep = static_cast<float> (std::abs (smoothedCorrectionCents - previousTrajectoryCorrectionCents));
            const auto previousMaximum = maximumTrajectoryStepCents.load (std::memory_order_relaxed);
            if (trajectoryStep > previousMaximum)
                maximumTrajectoryStepCents.store (trajectoryStep, std::memory_order_relaxed);
        }
        previousTrajectoryCorrectionCents = smoothedCorrectionCents;
        hadValidTrajectoryCorrection = true;
    }
    else
        hadValidTrajectoryCorrection = false;
    // A YIN estimate belongs to the frame that has just ended, not the host
    // block that happened to trigger analysis.  Using the old block cursor
    // here skewed renderer control by a variable host-block amount.
    if (latestCorrectionTrajectory.targetChanged)
    {
        ++psolaTargetEventId;
        psolaTargetEventTimestampSamples = frameCentreSample;
    }
    pushPsolaControlFrame (frameCentreSample, stableFrequency, smoothedPitchRatio,
                           latestCorrectionTrajectory.vibratoContributionCents,
                           latestCorrectionTrajectory.hasValidCorrection, holdRendererDry,
                           targetBridge, transientBridge, latestCorrectionTrajectory.targetNote,
                           latestCorrectionTrajectory.transitionTargetCents,
                           psolaTargetEventId, psolaTargetEventTimestampSamples,
                           latestCorrectionTrajectory.rendererCrossingActive
                               ? latestCorrectionTrajectory.rendererCrossingTargetNote : -1,
                           latestCorrectionTrajectory.rendererCrossingProgress,
                           latestCorrectionTrajectory.continuousReturnExcursionActive,
                           latestCorrectionTrajectory.continuousReturnExcursionPhase,
                           latestCorrectionTrajectory.continuousReturnExcursionConsumed,
                           latestCorrectionTrajectory.continuousReturnExcursionOwnsControl,
                           &latestCorrectionTrajectory);

    // A YIN hop is 512 samples, but renderer control is deliberately
    // delivered every 128 samples.  Schedule the three interior ticks now;
    // the fourth tick is advanced at the start of the next analysis update.
    // This keeps timestamps monotonic regardless of the host block size.
    for (int tick = 1; tick < pitchAnalysisConfig.hopSize / rendererControlIntervalSamples; ++tick)
    {
        const auto scheduled = correctionTrajectory.advanceScheduledTransition (rendererControlIntervalSamples,
                                                                                  analysisSampleRate);
        const auto rendererDeliveryCompensation = usePhaseContinuousRenderer.load (std::memory_order_relaxed)
                                              ? rendererDeliveryGain : VIBE_TDPSOLA_DELIVERY_GAIN;
        const auto deliveryRailCents = rendererDeliveryRailCents (retuneSpeedMs, flexTunePercent,
                                                                   humanizePercent, naturalVibratoPercent);
        const auto scheduledCorrectionCents = scheduled.hasValidCorrection
                                            ? juce::jlimit (-deliveryRailCents, deliveryRailCents,
                                                            1200.0 * std::log2 (scheduled.pitchRatio)
                                                                * rendererDeliveryCompensation)
                                            : 0.0;
        const auto scheduledRatio = scheduled.hasValidCorrection
                                  ? std::exp2 (scheduledCorrectionCents / 1200.0) : 1.0;
        pushPsolaControlFrame (frameCentreSample
                               + static_cast<uint64_t> (tick * rendererControlIntervalSamples),
                               stableFrequency, scheduledRatio, scheduled.vibratoContributionCents,
                               scheduled.hasValidCorrection, holdRendererDry,
                               targetBridge, transientBridge, scheduled.targetNote,
                               scheduled.transitionTargetCents,
                               psolaTargetEventId, psolaTargetEventTimestampSamples,
                               scheduled.rendererCrossingActive ? scheduled.rendererCrossingTargetNote : -1,
                               scheduled.rendererCrossingProgress,
                               scheduled.continuousReturnExcursionActive,
                               scheduled.continuousReturnExcursionPhase,
                               scheduled.continuousReturnExcursionConsumed,
                               scheduled.continuousReturnExcursionOwnsControl,
                               &scheduled);
    }
}


void VibeAutotuneProcessor::pushPitchAnalysisSamples (const float* samples, int numberOfSamples,
                                                       uint64_t blockStartSample)
{
    const auto frameSize = pitchAnalysisConfig.frameSize;

    for (int sample = 0; sample < numberOfSamples; ++sample)
    {
        pitchAnalysisFifo[static_cast<size_t> (pitchAnalysisWriteIndex)] = samples[sample];
        pitchAnalysisWriteIndex = (pitchAnalysisWriteIndex + 1) % maximumPitchAnalysisFrameSize;
        pitchAnalysisSamplesAvailable = juce::jmin (frameSize, pitchAnalysisSamplesAvailable + 1);
        ++pitchAnalysisSamplesSinceLastFrame;

        if (pitchAnalysisSamplesAvailable < frameSize || pitchAnalysisSamplesSinceLastFrame < pitchAnalysisConfig.hopSize)
            continue;

        const auto first = (pitchAnalysisWriteIndex - frameSize + maximumPitchAnalysisFrameSize)
                         % maximumPitchAnalysisFrameSize;
        const auto profile = pitchAnalysisProfilingEnabled.load (std::memory_order_relaxed);
        const auto fifoStart = profile ? juce::Time::getHighResolutionTicks() : 0;
        const auto firstSegment = juce::jmin (frameSize, maximumPitchAnalysisFrameSize - first);
        std::copy_n (pitchAnalysisFifo.data() + first, firstSegment, pitchAnalysisFrame.data());
        if (firstSegment < frameSize)
            std::copy_n (pitchAnalysisFifo.data(), frameSize - firstSegment, pitchAnalysisFrame.data() + firstSegment);
        if (profile)
            profiledFifoFrameTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - fifoStart);

        processedInputSamples = blockStartSample + static_cast<uint64_t> (sample + 1);
        currentAnalysisLatencySamples = frameSize;
        analysePitchFrame (processedInputSamples);
        pitchAnalysisSamplesSinceLastFrame = 0;
    }
}

void VibeAutotuneProcessor::analysePitchFrame (uint64_t frameEndSample)
{
    const auto profile = pitchAnalysisProfilingEnabled.load (std::memory_order_relaxed);
    const auto totalStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    const auto frameSize = pitchAnalysisConfig.frameSize;
    const auto minimumLag = juce::jmax (2, static_cast<int> (std::floor (analysisSampleRate / pitchAnalysisConfig.maximumFrequency)));
    const auto maximumLag = juce::jmin (frameSize / 2,
                                        static_cast<int> (yinDifference.size()) - 1,
                                        static_cast<int> (std::ceil (analysisSampleRate / pitchAnalysisConfig.minimumFrequency)));
    const auto clearPitchContinuity = [this]
    {
        // Do not leak a prior syllable's median/continuity state into the
        // next voiced onset.  The target selector must never receive an old
        // stable F0 after the detector has explicitly called a frame unvoiced.
        pitchHistoryCount = 0;
        pitchHistoryIndex = 0;
        stableLog2F0 = std::numeric_limits<double>::quiet_NaN();
        pendingOctaveFrames = 0;
        pendingOctaveFrequency = std::numeric_limits<double>::quiet_NaN();
        pendingPitchJumpFrames = 0;
        pendingPitchJumpFrequency = std::numeric_limits<double>::quiet_NaN();
        pendingSubharmonicFrames = 0;
        pendingSubharmonicFrequency = std::numeric_limits<double>::quiet_NaN();
    };

    // Analyse the raw envelope first.  It is deliberately kept separate
    // from the pitch-detector signal so consonant/onset detection still sees
    // the real vocal attack.
    double rawEnergy = 0.0;
    auto rawZeroCrossings = 0;
    for (int sample = 0; sample < frameSize; ++sample)
    {
        const auto value = static_cast<double> (pitchAnalysisFrame[static_cast<size_t> (sample)]);
        rawPitchAnalysisFrame[static_cast<size_t> (sample)] = static_cast<float> (value);
        rawEnergy += value * value;
        if (sample > 0 && ((pitchAnalysisFrame[static_cast<size_t> (sample - 1)] >= 0.0f)
                           != (pitchAnalysisFrame[static_cast<size_t> (sample)] >= 0.0f)))
            ++rawZeroCrossings;
    }

    const auto rawRms = std::sqrt (rawEnergy / static_cast<double> (frameSize));
    const auto rawZeroCrossingRate = static_cast<double> (rawZeroCrossings)
                                   / static_cast<double> (frameSize - 1);
    // Energy-only onset detection runs on the same completed analysis frame
    // as YIN. It has no extra buffer, FFT, allocation or host-block coupling.
    const auto transientThreshold = previousAnalysisFrameRms < 0.003
                                  ? 0.012
                                  : previousAnalysisFrameRms * 1.8;
    if (rawRms > 0.006 && rawRms > transientThreshold)
        audioTransientFramesRemaining = juce::jmax (audioTransientFramesRemaining, 5);
    previousAnalysisFrameRms = rawRms;
    if (maximumLag <= minimumLag + 2 || rawRms < silenceThreshold)
    {
        noiseFloorRms = juce::jmax (1.0e-6, noiseFloorRms * 0.995);
        clearPitchContinuity();
        ++voicingReleaseFrames;
        if (voicingReleaseFrames >= 3)
        {
            stableLog2F0 = std::numeric_limits<double>::quiet_NaN();
            pitchHistoryCount = 0;
            publishUnvoicedFrame (frameEndSample);
        }
        return;
    }

    // Precondition the detector only: a one-pole low-pass keeps the
    // fundamental while reducing fricative/high-harmonic energy, then an
    // adaptive centre clip removes low-level irregular waveform detail.
    // This buffer is analysis-only; the vocal audio remains untouched.
    const auto detectorCutoffHz = juce::jlimit (650.0, 900.0,
                                                 pitchAnalysisConfig.maximumFrequency * 1.10);
    const auto lowPassFeedback = std::exp (-2.0 * juce::MathConstants<double>::pi
                                           * detectorCutoffHz / analysisSampleRate);
    const auto centreClipLevel = juce::jmax (1.0e-5, rawRms * 0.12);
    // Each analysis frame overlaps the previous one. Starting the detector
    // filter at zero fabricated a frame-edge transient on every hop;
    // initialise it from the first real sample instead.
    auto filteredStage1 = static_cast<double> (pitchAnalysisFrame.front());
    auto filteredStage2 = filteredStage1;
    double energy = 0.0;
    auto zeroCrossings = 0;
    pitchFrameEnergyPrefix[0] = 0.0;
    for (int sample = 0; sample < frameSize; ++sample)
    {
        filteredStage1 += (static_cast<double> (pitchAnalysisFrame[static_cast<size_t> (sample)]) - filteredStage1)
                        * (1.0 - lowPassFeedback);
        filteredStage2 += (filteredStage1 - filteredStage2) * (1.0 - lowPassFeedback);
        const auto magnitude = std::abs (filteredStage2);
        const auto clipped = magnitude > centreClipLevel
                           ? std::copysign (magnitude - centreClipLevel, filteredStage2) : 0.0;
        pitchAnalysisFrame[static_cast<size_t> (sample)] = static_cast<float> (clipped);
        energy += clipped * clipped;
        pitchFrameEnergyPrefix[static_cast<size_t> (sample + 1)] = energy;
        if (sample > 0 && ((pitchAnalysisFrame[static_cast<size_t> (sample - 1)] >= 0.0f)
                           != (pitchAnalysisFrame[static_cast<size_t> (sample)] >= 0.0f)))
            ++zeroCrossings;
    }

    const auto rms = std::sqrt (energy / static_cast<double> (frameSize));
    const auto zeroCrossingRate = static_cast<double> (zeroCrossings) / static_cast<double> (frameSize - 1);

    const auto differenceStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    yinDifference[0] = 0.0;
    yinCrossCorrelation[0] = energy;
    const auto* frame = pitchAnalysisFrame.data();
    for (int lag = 1; lag <= maximumLag; ++lag)
    {
        // This is the detector's dominant live callback kernel.  Independent
        // accumulators remove the loop-carried floating-point dependency so
        // the compiler can issue multiple multiply-add chains without using
        // fast-math or changing the frame, lag range, or publication time.
        // The final reduction remains double precision; detector equivalence
        // is covered by the signal-chain/control chronology regression.
        const auto sampleCount = frameSize - lag;
        auto sample = 0;
        double sum0 = 0.0, sum1 = 0.0, sum2 = 0.0, sum3 = 0.0;
        for (; sample + 3 < sampleCount; sample += 4)
        {
            sum0 += static_cast<double> (frame[sample])     * static_cast<double> (frame[sample + lag]);
            sum1 += static_cast<double> (frame[sample + 1]) * static_cast<double> (frame[sample + lag + 1]);
            sum2 += static_cast<double> (frame[sample + 2]) * static_cast<double> (frame[sample + lag + 2]);
            sum3 += static_cast<double> (frame[sample + 3]) * static_cast<double> (frame[sample + lag + 3]);
        }
        auto correlationSum = (sum0 + sum1) + (sum2 + sum3);
        for (; sample < sampleCount; ++sample)
            correlationSum += static_cast<double> (frame[sample]) * static_cast<double> (frame[sample + lag]);

        yinCrossCorrelation[static_cast<size_t> (lag)] = correlationSum;
        yinDifference[static_cast<size_t> (lag)] = pitchFrameEnergyPrefix[static_cast<size_t> (frameSize - lag)]
                                                  + (energy - pitchFrameEnergyPrefix[static_cast<size_t> (lag)])
                                                  - 2.0 * correlationSum;
    }
    if (profile)
        profiledDifferenceTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - differenceStart);

    const auto cmndfStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    yinCumulativeMean[0] = 1.0;
    double runningDifferenceSum = 0.0;
    for (int lag = 1; lag <= maximumLag; ++lag)
    {
        runningDifferenceSum += yinDifference[static_cast<size_t> (lag)];
        yinCumulativeMean[static_cast<size_t> (lag)] = runningDifferenceSum > 0.0
            ? yinDifference[static_cast<size_t> (lag)] * static_cast<double> (lag) / runningDifferenceSum
            : 1.0;
    }
    if (profile)
        profiledCmndfTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - cmndfStart);

    const auto candidateSearchStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    auto primaryPeriod = minimumLag;
    auto foundThresholdCandidate = false;
    for (int lag = minimumLag; lag < maximumLag; ++lag)
    {
        if (yinCumulativeMean[static_cast<size_t> (lag)] < yinThreshold)
        {
            while (lag < maximumLag && yinCumulativeMean[static_cast<size_t> (lag + 1)] < yinCumulativeMean[static_cast<size_t> (lag)])
                ++lag;
            primaryPeriod = lag;
            foundThresholdCandidate = true;
            break;
        }
    }

    if (! foundThresholdCandidate)
        for (int lag = minimumLag + 1; lag <= maximumLag; ++lag)
            if (yinCumulativeMean[static_cast<size_t> (lag)] < yinCumulativeMean[static_cast<size_t> (primaryPeriod)])
                primaryPeriod = lag;

    std::array<PitchCandidate, 3> candidates {};
    const std::array<int, 3> candidateSeeds {{ primaryPeriod, primaryPeriod * 2, primaryPeriod / 2 }};
    auto candidateCount = 0;

    for (const auto seed : candidateSeeds)
    {
        PitchCandidate candidate;
        if (! findPitchCandidate (seed, minimumLag, maximumLag, candidate))
            continue;

        auto duplicate = false;
        for (int index = 0; index < candidateCount; ++index)
            duplicate = duplicate || std::abs (candidates[static_cast<size_t> (index)].period - candidate.period) <= 2;
        if (duplicate)
            continue;

        const auto autocorrelationStart = profile ? juce::Time::getHighResolutionTicks() : 0;
        const auto firstEnergy = pitchFrameEnergyPrefix[static_cast<size_t> (frameSize - candidate.period)];
        const auto secondEnergy = energy - pitchFrameEnergyPrefix[static_cast<size_t> (candidate.period)];
        candidate.correlation = yinCrossCorrelation[static_cast<size_t> (candidate.period)]
                              / std::sqrt (firstEnergy * secondEnergy + 1.0e-18);
        if (profile)
            profiledAutocorrelationTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - autocorrelationStart);

        const auto parabolicStart = profile ? juce::Time::getHighResolutionTicks() : 0;
        const auto previous = yinDifference[static_cast<size_t> (candidate.period - 1)];
        const auto current = yinDifference[static_cast<size_t> (candidate.period)];
        const auto next = yinDifference[static_cast<size_t> (candidate.period + 1)];
        const auto denominator = previous - 2.0 * current + next;
        auto offset = std::abs (denominator) > 1.0e-12 ? 0.5 * (previous - next) / denominator : 0.0;
        offset = juce::jlimit (-1.0, 1.0, offset);
        candidate.frequency = analysisSampleRate / (static_cast<double> (candidate.period) + offset);
        if (profile)
            profiledParabolicTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - parabolicStart);
        candidate.quality = 0.60 * candidate.periodicity
                          + 0.30 * juce::jmax (0.0, candidate.correlation)
                          + 0.10 * juce::jlimit (0.0, 1.0, candidate.valleyProminence * 12.0);
        candidates[static_cast<size_t> (candidateCount++)] = candidate;
    }
    if (profile)
        profiledCandidateSearchTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - candidateSearchStart);

    if (candidateCount == 0)
    {
        clearPitchContinuity();
        ++voicingReleaseFrames;
        if (voicingReleaseFrames >= 3)
            publishUnvoicedFrame (frameEndSample);
        return;
    }

    const auto octaveStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    auto rawCandidate = candidates[0];
    auto continuityCandidate = rawCandidate;
    auto continuityScore = -std::numeric_limits<double>::infinity();
    const auto previousStableFrequency = std::isfinite (stableLog2F0) ? std::exp2 (stableLog2F0) : 0.0;

    for (int index = 0; index < candidateCount; ++index)
    {
        const auto& candidate = candidates[static_cast<size_t> (index)];
        if (candidate.quality > rawCandidate.quality)
            rawCandidate = candidate;

        const auto continuityPenalty = previousStableFrequency > 0.0
            ? 0.065 * std::abs (std::log2 (candidate.frequency / previousStableFrequency)) : 0.0;
        const auto score = candidate.quality - continuityPenalty;
        if (score > continuityScore)
        {
            continuityScore = score;
            continuityCandidate = candidate;
        }
    }

    // YIN can make the first acceptable valley after a gap the period of a
    // subharmonic.  The companion at half that period is still in the
    // candidate set, but the raw quality score favours the long period by a
    // very small amount on some vowels.  Prefer that higher companion only
    // when it has nearly the same direct evidence.  This is deliberately not
    // an unconditional octave-up rule: a real low note keeps its octave when
    // the shorter-period candidate is materially weaker.
    constexpr double harmonicQualityTolerance = 0.025;
    constexpr double harmonicPeriodicityTolerance = 0.020;
    const PitchCandidate* harmonicCandidate = nullptr;
    for (int index = 0; index < candidateCount; ++index)
    {
        const auto& candidate = candidates[static_cast<size_t> (index)];
        const auto octaveDistance = 1200.0 * std::log2 (candidate.frequency / rawCandidate.frequency);
        if (octaveDistance > 850.0 && octaveDistance < 1550.0
            && (harmonicCandidate == nullptr || candidate.quality > harmonicCandidate->quality))
            harmonicCandidate = &candidate;
    }
    if (harmonicCandidate != nullptr
        && harmonicCandidate->quality >= rawCandidate.quality - harmonicQualityTolerance
        && harmonicCandidate->periodicity >= rawCandidate.periodicity - harmonicPeriodicityTolerance
        && harmonicCandidate->correlation >= rawCandidate.correlation - harmonicPeriodicityTolerance)
    {
        rawCandidate = *harmonicCandidate;
        continuityCandidate = rawCandidate;
    }

    const auto voicingStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    const auto tracking = juce::jlimit (0.0, 1.0,
        static_cast<double> (parameters.getRawParameterValue ("tracking")->load (std::memory_order_relaxed) * 0.01f));
    // Tracking=50 is the voiced/unvoiced sweet spot. It must follow the core
    // of an ordinary clean vocal, while a normal breath or consonant remains
    // dry. The permissive side deliberately opens much faster only above 50:
    // 80–100 is for difficult/weak recordings and, like established pitch
    // correction processors, accepts more false-positive risk there.
    const auto belowDefault = juce::jlimit (0.0, 1.0, (0.50 - tracking) * 2.0);
    const auto aboveDefault = juce::jlimit (0.0, 1.0, (tracking - 0.50) * 2.0);
    const auto conservative = belowDefault * belowDefault;
    const auto permissive = aboveDefault * aboveDefault;
    const auto requiredPeriodicity = 0.66 + 0.12 * conservative - 0.18 * permissive;
    const auto requiredCorrelation = 0.63 + 0.12 * conservative - 0.20 * permissive;
    const auto requiredProminence = 0.038 + 0.017 * conservative - 0.020 * permissive;
    const auto requiredRms = juce::jmax (static_cast<double> (silenceThreshold),
                                         noiseFloorRms * (1.60 + 0.55 * conservative - 0.45 * permissive));
    // Once a vowel is established, do not pitch-shift a much quieter fragment
    // inside it merely because YIN can find a plausible short period. This is
    // the usual breath/consonant tail that causes unnecessary note flutter in
    // a sustained vocal. The reference decays smoothly, so a genuinely quiet
    // sung phrase can still be tracked after a short adaptation period.
    const auto sustainedVoicedFloor = latestVoiced.load (std::memory_order_relaxed)
                                  ? voicedRmsEnvelope * (0.16 - 0.08 * permissive + 0.04 * conservative) : 0.0;
    const auto certifiedRms = juce::jmax (requiredRms, sustainedVoicedFloor);
    const auto maximumZeroCrossingRate = 0.25 + 0.05 * permissive - 0.07 * conservative;
    // The detector preprocessing deliberately removes high-frequency detail.
    // That is useful for F0 estimation, but on its own can make /s/, /t/ and
    // breath noise look deceptively periodic. Keep two raw-domain checks as
    // a voicing veto: genuine vowels have a low raw sign-change rate and
    // retain meaningful low-band energy after the analysis filter.
    const auto maximumRawZeroCrossingRate = 0.095 + 0.075 * permissive - 0.040 * conservative;
    const auto lowBandEnergyRatio = rawRms > 1.0e-8 ? rms / rawRms : 0.0;
    const auto minimumLowBandEnergyRatio = 0.27 + 0.07 * conservative - 0.09 * permissive;

    // Validate the chosen lag against the *unfiltered* waveform. A sharp
    // consonant or breath can acquire a convincing YIN valley after
    // low-pass/centre clipping, but it has little correlation at that same
    // period in the original signal. This adds no look-ahead or latency: one
    // bounded 1024-sample calculation runs only once per existing YIN hop.
    const auto rawPeriod = juce::jlimit (minimumLag, maximumLag, rawCandidate.period);
    double rawPeriodCorrelation = 0.0;
    double rawPeriodEnergyA = 0.0;
    double rawPeriodEnergyB = 0.0;
    for (int sample = 0; sample < frameSize - rawPeriod; ++sample)
    {
        const auto a = static_cast<double> (rawPitchAnalysisFrame[static_cast<size_t> (sample)]);
        const auto b = static_cast<double> (rawPitchAnalysisFrame[static_cast<size_t> (sample + rawPeriod)]);
        rawPeriodCorrelation += a * b;
        rawPeriodEnergyA += a * a;
        rawPeriodEnergyB += b * b;
    }
    const auto rawPeriodicity = rawPeriodEnergyA > 1.0e-12 && rawPeriodEnergyB > 1.0e-12
                              ? rawPeriodCorrelation / std::sqrt (rawPeriodEnergyA * rawPeriodEnergyB) : 0.0;
    const auto minimumRawPeriodicity = 0.41 + 0.11 * conservative - 0.13 * permissive;

    // A lower-octave candidate is only selected when its direct YIN evidence
    // is demonstrably stronger than the primary candidate, then persists for
    // two analysis frames. Equal-score periods are intentionally ambiguous:
    // automatically halving them would turn a genuine 440 Hz sine into 220 Hz.
    constexpr double subharmonicQualityMargin = 0.015;
    constexpr double subharmonicPeriodicityMargin = 0.010;
    const PitchCandidate* subharmonicCandidate = nullptr;
    for (int index = 0; index < candidateCount; ++index)
    {
        const auto& candidate = candidates[static_cast<size_t> (index)];
        const auto octaveDistance = 1200.0 * std::log2 (rawCandidate.frequency / candidate.frequency);
        if (octaveDistance > 850.0 && octaveDistance < 1550.0
            && (subharmonicCandidate == nullptr || candidate.quality > subharmonicCandidate->quality))
            subharmonicCandidate = &candidate;
    }

    if (subharmonicCandidate != nullptr
        && subharmonicCandidate->quality >= rawCandidate.quality + subharmonicQualityMargin
        && subharmonicCandidate->periodicity >= rawCandidate.periodicity - subharmonicPeriodicityMargin
        && subharmonicCandidate->correlation >= rawCandidate.correlation - subharmonicPeriodicityMargin)
    {
        const auto sameSubharmonic = std::isfinite (pendingSubharmonicFrequency)
                                  && std::abs (1200.0 * std::log2 (subharmonicCandidate->frequency / pendingSubharmonicFrequency)) < 80.0;
        pendingSubharmonicFrames = sameSubharmonic ? pendingSubharmonicFrames + 1 : 1;
        pendingSubharmonicFrequency = subharmonicCandidate->frequency;

        if (pendingSubharmonicFrames >= 2)
        {
            rawCandidate = *subharmonicCandidate;
            continuityCandidate = rawCandidate;
        }
    }
    else
    {
        pendingSubharmonicFrames = 0;
        pendingSubharmonicFrequency = std::numeric_limits<double>::quiet_NaN();
    }

    const auto voicedEvidence = rawCandidate.frequency >= pitchAnalysisConfig.minimumFrequency * 0.98
                             && rawCandidate.frequency <= pitchAnalysisConfig.maximumFrequency * 1.02
                             && rawCandidate.periodicity >= requiredPeriodicity
                             && rawCandidate.correlation >= requiredCorrelation
                             && rawCandidate.valleyProminence >= requiredProminence
                             && zeroCrossingRate <= maximumZeroCrossingRate
                             && rawZeroCrossingRate <= maximumRawZeroCrossingRate
                             && lowBandEnergyRatio >= minimumLowBandEnergyRatio
                             && rawPeriodicity >= minimumRawPeriodicity
                             && rms >= certifiedRms;
    if (profile)
        profiledVoicingTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - voicingStart);

    if (! voicedEvidence)
    {
        noiseFloorRms = juce::jmax (1.0e-6, noiseFloorRms * 0.98 + rms * 0.02);
        voicingAttackFrames = 0;
        clearPitchContinuity();
        ++voicingReleaseFrames;
        if (latestVoiced.load (std::memory_order_relaxed) && voicingReleaseFrames < 3 && previousStableFrequency > 0.0)
        {
            processedInputSamples = frameEndSample;
            currentAnalysisLatencySamples = pitchAnalysisConfig.frameSize;
            // Preserve the selector's brief target-state hold, but publish an
            // invalid renderer-control frame immediately.  Otherwise the
            // previous voiced ratio is replayed into a consonant/silence and
            // the phase vocoder can bubble at every voiced boundary.
            updateCorrectionTrajectory (std::numeric_limits<double>::quiet_NaN(), false, frameEndSample);
            publishPitchTrackingData (std::numeric_limits<double>::quiet_NaN(), previousStableFrequency,
                                      std::numeric_limits<double>::quiet_NaN(), true);
            return;
        }

        stableLog2F0 = std::numeric_limits<double>::quiet_NaN();
        pitchHistoryCount = 0;
        if (voicingReleaseFrames >= 3)
            voicedRmsEnvelope = 0.0;
        publishUnvoicedFrame (frameEndSample);
        return;
    }

    voicingReleaseFrames = 0;
    if (! latestVoiced.load (std::memory_order_relaxed) && ++voicingAttackFrames < 2)
    {
        publishUnvoicedFrame (frameEndSample);
        return;
    }
    voicingAttackFrames = 2;
    latestVoiced.store (true, std::memory_order_relaxed);
    const auto voicedEnvelopeDecay = std::exp (-static_cast<double> (pitchAnalysisConfig.hopSize)
                                               / (0.280 * analysisSampleRate));
    voicedRmsEnvelope = juce::jmax (rms, voicedRmsEnvelope * voicedEnvelopeDecay);

    // The YIN/preconditioner pair has a repeatable small negative lag bias on
    // the calibrated voiced-vowel probes: approximately -3.2 cents at both
    // F#4 level points and at both target offsets.  Correct it once here,
    // before the value becomes either note-policy input or TD-PSOLA control.
    // This is intentionally a fixed multiplicative calibration, not a
    // time-varying correction, so it cannot create a glide or react to noise.
    constexpr double detectorFrequencyCalibrationCents = 3.15;
    const auto detectorFrequencyCalibrationRatio = std::exp2 (detectorFrequencyCalibrationCents / 1200.0);
    auto validatedFrequency = continuityCandidate.frequency * detectorFrequencyCalibrationRatio;
    auto octaveConfirmed = false;
    if (previousStableFrequency > 0.0)
    {
        const auto octaveDistance = std::abs (1200.0 * std::log2 (rawCandidate.frequency / previousStableFrequency));
        if (octaveDistance > 850.0 && octaveDistance < 1550.0)
        {
            const auto samePendingOctave = std::isfinite (pendingOctaveFrequency)
                                         && std::abs (1200.0 * std::log2 (rawCandidate.frequency / pendingOctaveFrequency)) < 80.0;
            pendingOctaveFrames = samePendingOctave ? pendingOctaveFrames + 1 : 1;
            pendingOctaveFrequency = rawCandidate.frequency;
            // A target octave must be more stable than a single detector
            // decision.  Four to five frames was still short enough for a
            // periodic octave error to be committed on sustained vowels;
            // require 24 consecutive hops (~64 ms at 48 kHz) in either
            // direction. A genuine octave leap remains decisively slower
            // than this, while a harmonic vowel's alternating octave valley
            // no longer becomes an audible target change.
            constexpr int requiredOctaveConfirmationFrames = 24;
            octaveConfirmed = pendingOctaveFrames >= requiredOctaveConfirmationFrames;
            validatedFrequency = octaveConfirmed ? rawCandidate.frequency * detectorFrequencyCalibrationRatio : previousStableFrequency;
        }
        else
        {
            pendingOctaveFrames = 0;
            pendingOctaveFrequency = std::numeric_limits<double>::quiet_NaN();
        }

        // Fricatives, plosives and brief mouth noises can still yield a
        // convincing-looking YIN valley for one frame.  That false F0 is
        // neither a vocal note nor a usable renderer control point.  Hold
        // the last stable pitch until a non-octave jump is repeated on the
        // next hop; this rejects the single-frame "jump then snap back"
        // pattern that produces bubbles/clicks in pitch shifting.
        constexpr double transientPitchJumpCents = 180.0;
        constexpr double jumpConfirmationToleranceCents = 120.0;
        const auto jumpCents = std::abs (1200.0 * std::log2 (continuityCandidate.frequency
                                                              / previousStableFrequency));
        const auto octaveLikeJump = jumpCents > 850.0 && jumpCents < 1550.0;
        if (! octaveConfirmed && ! octaveLikeJump && jumpCents > transientPitchJumpCents)
        {
            const auto samePendingJump = std::isfinite (pendingPitchJumpFrequency)
                                      && std::abs (1200.0 * std::log2 (continuityCandidate.frequency
                                                                       / pendingPitchJumpFrequency))
                                           < jumpConfirmationToleranceCents;
            pendingPitchJumpFrames = samePendingJump ? pendingPitchJumpFrames + 1 : 1;
            pendingPitchJumpFrequency = continuityCandidate.frequency;
            if (pendingPitchJumpFrames < 2)
            {
                validatedFrequency = previousStableFrequency;
                audioTransientFramesRemaining = juce::jmax (audioTransientFramesRemaining, 5);
            }
        }
        else
        {
            pendingPitchJumpFrames = 0;
            pendingPitchJumpFrequency = std::numeric_limits<double>::quiet_NaN();
        }
    }
    if (profile)
        profiledOctaveTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - octaveStart);

    const auto smoothingStart = profile ? juce::Time::getHighResolutionTicks() : 0;
    if (octaveConfirmed)
    {
        // A confirmed octave event is a real note transition, not a spike.
        // Reset the short median history so it cannot add two stale hops.
        pitchHistory[0] = validatedFrequency;
        pitchHistory[1] = validatedFrequency;
        pitchHistory[2] = validatedFrequency;
        pitchHistoryIndex = 0;
        pitchHistoryCount = 3;
    }
    else
    {
        pitchHistory[static_cast<size_t> (pitchHistoryIndex)] = validatedFrequency;
        pitchHistoryIndex = (pitchHistoryIndex + 1) % 3;
        pitchHistoryCount = juce::jmin (3, pitchHistoryCount + 1);
    }
    std::array<double, 3> shortHistory {};
    for (int index = 0; index < pitchHistoryCount; ++index)
        shortHistory[static_cast<size_t> (index)] = pitchHistory[static_cast<size_t> (index)];
    std::sort (shortHistory.begin(), shortHistory.begin() + pitchHistoryCount);
    const auto medianFrequency = shortHistory[static_cast<size_t> (pitchHistoryCount / 2)];
    const auto medianLog2 = std::log2 (medianFrequency);

    if (! std::isfinite (stableLog2F0))
        stableLog2F0 = medianLog2;
    else
    {
        const auto deltaCents = std::abs (1200.0 * (medianLog2 - stableLog2F0));
        const auto transition = octaveConfirmed || deltaCents > 70.0;
        const auto smoothing = transition ? 0.82 : (0.12 + 0.25 * rawCandidate.periodicity);
        stableLog2F0 += (medianLog2 - stableLog2F0) * smoothing;
    }

    const auto stableFrequency = std::exp2 (stableLog2F0);
    const auto referencePitch = parameters.getRawParameterValue ("referencePitch")->load (std::memory_order_relaxed);
    const auto inputDisplay = calculateInputPitchDisplay (stableFrequency, referencePitch, true);
    const auto detectedMidiNote = inputDisplay.midiNote;
    const auto inputCents = static_cast<double> (inputDisplay.cents);
    // This is the exact causal input position reached by YIN when the
    // control becomes publishable. The host block may contain later samples,
    // but it does not change this analysis availability point.
    debugControlPublicationInputSample = frameEndSample;
    updateCorrectionTrajectory (stableFrequency, true, frameEndSample,
                                rawCandidate.frequency, rawCandidate.periodicity);
    const auto targetMidiNote = latestCorrectionTrajectory.targetNote;
    const auto outputMidiNote = latestCorrectionTrajectory.hasValidCorrection
                              ? juce::jlimit (0, 127, static_cast<int> (std::lround (69.0 + latestCorrectionTrajectory.correctedPitchCents / 100.0)))
                              : -1;
    latestDetectedMidiNote.store (detectedMidiNote, std::memory_order_relaxed);
    latestTargetMidiNote.store (targetMidiNote, std::memory_order_relaxed);
    latestOutputMidiNote.store (outputMidiNote, std::memory_order_relaxed);
    latestInputCents.store (static_cast<float> (inputCents), std::memory_order_relaxed);
    latestCentsDeviation.store (latestCorrectionTrajectory.hasValidCorrection
                                    ? static_cast<float> (latestCorrectionTrajectory.correctionCents)
                                    : 0.0f, std::memory_order_relaxed);
    latestBypassed.store (false, std::memory_order_relaxed);
    processedInputSamples = frameEndSample;
    currentAnalysisLatencySamples = pitchAnalysisConfig.frameSize;
    publishPitchTrackingData (rawCandidate.frequency, stableFrequency, rawCandidate.periodicity, true);
    if (profile)
    {
        profiledSmoothingTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - smoothingStart);
        ++profiledAnalysisCount;
        profiledTotalTicks += static_cast<uint64_t> (juce::Time::getHighResolutionTicks() - totalStart);
    }
}

void VibeAutotuneProcessor::setMidiTargetNoteFromUi (int midiNote) noexcept
{
    if (midiNote < 0 || midiNote > 127)
    {
        midiTargetNote.store (-1, std::memory_order_relaxed);
        return;
    }

    midiTargetNote.store (midiNote, std::memory_order_relaxed);
}

int VibeAutotuneProcessor::getMidiTargetNote() const noexcept
{
    return midiTargetNote.load (std::memory_order_relaxed);
}

void VibeAutotuneProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages)
{
    const auto clearMidiTargets = [this]
    {
        midiHeldNotes.fill (false);
        midiNoteOrder.fill (0);
        midiTargetNote.store (-1, std::memory_order_relaxed);
    };

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();
        // Controllers and hosts commonly release notes with CC 120/123.
        // Treat both as an authoritative flush so a lost NoteOff can never
        // leave the vocal locked to a stale MIDI target.
        if (message.isController() && (message.getControllerNumber() == 120
                                       || message.getControllerNumber() == 123))
        {
            clearMidiTargets();
        }
        else if (message.isNoteOn())
        {
            const auto note = juce::jlimit (0, 127, message.getNoteNumber());
            midiHeldNotes[static_cast<size_t> (note)] = true;
            midiNoteOrder[static_cast<size_t> (note)] = ++midiOrderCounter;
            midiTargetNote.store (note, std::memory_order_relaxed);
        }
        else if (message.isNoteOff())
        {
            const auto note = juce::jlimit (0, 127, message.getNoteNumber());
            midiHeldNotes[static_cast<size_t> (note)] = false;
            auto nextNote = -1;
            uint64_t newestOrder = 0;
            for (int candidate = 0; candidate <= 127; ++candidate)
                if (midiHeldNotes[static_cast<size_t> (candidate)]
                    && midiNoteOrder[static_cast<size_t> (candidate)] > newestOrder)
                {
                    newestOrder = midiNoteOrder[static_cast<size_t> (candidate)];
                    nextNote = candidate;
                }
            midiTargetNote.store (nextNote, std::memory_order_relaxed);
        }
    }

    {
        const auto numberOfSamples = buffer.getNumSamples();
        const auto blockStartSample = processedInputSamples;
        debugControlPublicationBlockStartSample = blockStartSample;
        debugControlPublicationBlockEndSample = blockStartSample + static_cast<uint64_t> (juce::jmax (0, numberOfSamples));
        if (signalChainDiagnosticEnabled.load (std::memory_order_relaxed))
            signalChainDiagnosticFrameCount = 0;

        if (parameters.getRawParameterValue ("bypass")->load (std::memory_order_relaxed) >= 0.5f)
        {
            // Bypass still uses the same source grid so its delayed dry path
            // cannot splice a partial quantum across callback boundaries.
            if (numberOfSamples > 0 && numberOfSamples < activeSourceSchedulerQuantumSamples)
            {
                if (sourceStageFill == 0)
                    sourceStageStartSample = blockStartSample;
                for (int channel = 0; channel < juce::jmin (buffer.getNumChannels(), psolaMaximumChannels); ++channel)
                    std::copy_n (buffer.getReadPointer (channel), numberOfSamples,
                                 sourceStage.data() + static_cast<size_t> (channel * sourceStageStorageSamples + sourceStageFill));
                sourceStageFill += numberOfSamples;
                const std::array<const float*, 2> stagedPointers {
                    sourceStage.data(), sourceStage.data() + sourceStageStorageSamples };
                const auto complete = sourceStageFill == activeSourceSchedulerQuantumSamples;
                processPitchRenderer (buffer, 0, numberOfSamples,
                                      false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                      false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::quiet_NaN(),
                                      complete ? sourceStageStartSample : blockStartSample,
                                      blockStartSample,
                                      complete ? stagedPointers.data() : nullptr,
                                      complete ? activeSourceSchedulerQuantumSamples : 0);
                if (complete)
                {
                    sourceStageFill = 0;
                    sourceStageStartSample += activeSourceSchedulerQuantumSamples;
                }
            }
            else
            {
                for (int offset = 0; offset < numberOfSamples; offset += rendererControlIntervalSamples)
                    processPitchRenderer (buffer, offset,
                                          juce::jmin (rendererControlIntervalSamples, numberOfSamples - offset),
                                          false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                          false, 1.0, std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::quiet_NaN(),
                                          blockStartSample + static_cast<uint64_t> (offset));
            }
            processedInputSamples = blockStartSample + static_cast<uint64_t> (juce::jmax (0, numberOfSamples));
            currentAnalysisLatencySamples = pitchAnalysisConfig.frameSize;
            clearPitchMonitor (true);
            return;
        }

        const auto inputType = static_cast<int> (parameters.getRawParameterValue ("inputType")->load (std::memory_order_relaxed));
        if (inputType != configuredInputType)
        {
            pitchAnalysisConfig = getPitchAnalysisConfig (inputType);
            configuredInputType = inputType;
            resetPitchAnalysisState (true);
            resetPsolaRenderer();
        }

        // Key/Scale define only the default policy. Changing either resets
        // manual rules and only the target-selector history; YIN and the
        // renderer keep running unchanged.
        synchroniseNotePolicyToScale();

        latestBypassed.store (false, std::memory_order_relaxed);
        if (buffer.getNumChannels() > 0)
            captureWaveformScope (buffer.getReadPointer (0), numberOfSamples, blockStartSample);
        // Renderer control is evaluated on a fixed internal interval, not at
        // the DAW block boundary. This preserves the calibrated 0–400 ms
        // semantics for every supported host block size.
        const auto currentRetuneMs = parameters.getRawParameterValue ("retuneSpeed")->load (std::memory_order_relaxed);
        const auto diagnosticRetune10T90Ms = parameters.getRawParameterValue ("diagnosticRetune10T90Ms")->load (std::memory_order_relaxed);
        const auto diagnosticRetune25T90Ms = parameters.getRawParameterValue ("diagnosticRetune25T90Ms")->load (std::memory_order_relaxed);
        correctionTrajectory.setDiagnosticRetuneT90Overrides (diagnosticRetune10T90Ms,
                                                               diagnosticRetune25T90Ms);
        correctionTrajectory.setDiagnosticJointControlScales (
            parameters.getRawParameterValue ("diagnosticFlexResidualScale")->load (std::memory_order_relaxed),
            parameters.getRawParameterValue ("diagnosticHumanizeScale")->load (std::memory_order_relaxed),
            parameters.getRawParameterValue ("diagnosticNegativeNaturalPhaseScale")->load (std::memory_order_relaxed));
        correctionTrajectory.setDiagnosticNaturalSurfaceAtHardTuneWeight (
            parameters.getRawParameterValue ("diagnosticNaturalSurfaceHardTuneWeight")->load (std::memory_order_relaxed));
        const auto voicedColourPercent = parameters.getRawParameterValue ("voicedColour")->load (std::memory_order_relaxed);
        for (auto& renderer : tdpsolaRenderers)
            renderer.setVoicedColourAmountPercent (voicedColourPercent);
        // Only the explicit 0 ms setting is hard tune.  Small non-zero
        // values remain a continuous glide, matching the control's label.
        const auto hardTune = currentRetuneMs == 0.0f;
        const auto effectiveRetuneMs = correctionTrajectory.getEffectiveRetuneTimeMs (currentRetuneMs);
        // The renderer receives a delayed control stream, so its dezipper
        // follows the calibrated response rather than the raw knob label.
        const auto retuneNormalised = effectiveRetuneMs / (effectiveRetuneMs + 100.0);
        const auto dezipperSeconds = hardTune ? rendererControlDezipperHardTuneSeconds
                                   : rendererControlDezipperFastSeconds
                                   + (rendererControlDezipperSlowSeconds - rendererControlDezipperFastSeconds)
                                         * retuneNormalised;

        // Source/control chronology is owned exclusively by completed 64
        // sample source quanta.  In particular, a trailing portion of a
        // large, non-aligned host callback is staged just like a short host
        // callback; it must never escape through a direct scheduler path.
        auto offset = 0;
        while (offset < numberOfSamples)
        {
        const auto copyCount = juce::jmin (activeSourceSchedulerQuantumSamples - sourceStageFill,
                                           numberOfSamples - offset);
        if (sourceStageFill == 0)
            sourceStageStartSample = blockStartSample + static_cast<uint64_t> (offset);
        for (int channel = 0; channel < juce::jmin (buffer.getNumChannels(), psolaMaximumChannels); ++channel)
            std::copy_n (buffer.getReadPointer (channel, offset), copyCount,
                         sourceStage.data() + static_cast<size_t> (channel * sourceStageStorageSamples + sourceStageFill));
        sourceStageFill += copyCount;
        sourceSamplesReceived += static_cast<uint64_t> (copyCount);

        const auto stagedQuantumReady = sourceStageFill == activeSourceSchedulerQuantumSamples;
        const auto outputTime = blockStartSample + static_cast<uint64_t> (offset);
        if (! stagedQuantumReady)
        {
            // Reading output is independent from source ingestion.  The
            // fixed renderer latency makes this safe while the partial source
            // stage waits for its remaining samples; no source/control state
            // is advanced on this path.
            processPitchRenderer (buffer, offset, copyCount,
                                  rendererCorrectionEngaged, renderedPitchRatio,
                                  lastValidRendererF0Hz, false, 1.0,
                                  std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::quiet_NaN(),
                                  sourceStageStartSample, outputTime, nullptr, 0);
            offset += copyCount;
            continue;
        }

        const auto intervalSamples = activeSourceSchedulerQuantumSamples;
        const auto sourceTime = sourceStageStartSample;
        const auto outputSamples = copyCount;
        // Advance analysis and control publication only through this source
        // segment before TD-PSOLA consumes it. This makes the visible control
        // set a function of absolute source time, not host callback size.
        if (buffer.getNumChannels() > 0 && yinDifference.size() >= static_cast<size_t> (pitchAnalysisConfig.frameSize / 2))
            pushPitchAnalysisSamples (sourceStage.data(), intervalSamples, sourceTime);
        else
            publishUnvoicedFrame (sourceTime + static_cast<uint64_t> (intervalSamples));
        const auto delayedControlTime = sourceTime > static_cast<uint64_t> (rendererControlDelaySamples)
                                      ? sourceTime - static_cast<uint64_t> (rendererControlDelaySamples)
                                      : 0;
        const auto rendererControl = getRendererControlFrame (delayedControlTime, 1);
        const auto rendererControlHasPitch = rendererControl.valid
                                          && std::isfinite (rendererControl.stableF0Hz) && rendererControl.stableF0Hz > 0.0
                                          && std::isfinite (rendererControl.pitchRatio);
        if (rendererControlHasPitch)
        {
            lastValidRendererF0Hz = rendererControl.stableF0Hz;
            lastValidRendererPitchRatio = rendererControl.pitchRatio;
            hasLastValidRendererControl = true;
        }
        // A transient flag is generated from raw vocal energy before F0
        // processing. It is an explicit veto for the renderer, not merely a
        // suggestion to slow Retune: a plosive/fricative must never reach a
        // pitch-shifting transform even when its filtered waveform happens
        // to resemble a periodic tone for one analysis frame.
        // A short voiced transient is still part of one processed vocal
        // stream. Its trajectory is already guarded upstream; routing it to
        // dry here creates the wet -> dry -> wet splice responsible for the
        // 2–3 s click. Only an explicit target bridge may request dry.
        const auto explicitDryBridge = rendererControl.holdDry;
        if (explicitDryBridge)
            tdpsolaReentryDrySamples = juce::jmax (tdpsolaReentryDrySamples,
                                                    static_cast<int> (std::lround (0.0213 * analysisSampleRate)));
        if (rendererControlHasPitch)
            rendererInvalidControlSamples = 0;
        else
            rendererInvalidControlSamples += intervalSamples;

        // A single missed YIN frame in an otherwise continuous vowel is not
        // a breath. Keep the prior certified control for at most 16 ms so a
        // detector dropout cannot reset TD-PSOLA's phase state mid-note.
        // A real transient/unvoiced veto still bypasses immediately.
        constexpr int detectorDropoutHoldSamples = 6 * rendererControlIntervalSamples;
        const auto usePreviousCertifiedControl = ! rendererControlHasPitch
                                               && ! explicitDryBridge
                                               && hasLastValidRendererControl
                                               && rendererInvalidControlSamples <= detectorDropoutHoldSamples;
        const auto rendererControlIsValid = (rendererControlHasPitch || usePreviousCertifiedControl)
                                         && ! explicitDryBridge;
        const auto rendererControlF0 = rendererControlHasPitch ? rendererControl.stableF0Hz
                                                               : lastValidRendererF0Hz;
        const auto rendererControlRatio = rendererControlHasPitch ? rendererControl.pitchRatio
                                                                  : lastValidRendererPitchRatio;
        const auto correctionMagnitudeCents = rendererControlIsValid
                                            ? std::abs (1200.0 * std::log2 (rendererControlRatio)) : 0.0;
        // Retune Speed must only control the time trajectory towards a valid
        // target. It must not decide whether an otherwise-valid 20/100/400
        // ms correction is rendered at all: that turns a musical glide into
        // a dry, off-target note. Keep only a tiny control-noise deadband;
        // the target selector, voiced certification and trajectory own all
        // musical decisions. Exact 0 ms remains the intentional snap mode.
        const auto flexAmount = juce::jlimit (0.0, 1.0, static_cast<double> (
            parameters.getRawParameterValue ("flexTune")->load (std::memory_order_relaxed) * 0.01f));
        // A Flex setting relaxes the correction near target, so it needs a
        // lower *entry* threshold to remain audible. This does not affect
        // hard-tune and is paired with a lower exit threshold plus a longer
        // hold below, preventing repeated wet/dry toggles.
        const auto naturalEngageThreshold = 4.0 - 2.0 * flexAmount;
        // A sub-2-cent hard-tune correction is below practical audibility but
        // can still create a phase mismatch when a pitch-synchronous path is
        // engaged. Keep that micro-error on latency-aligned dry.
        const auto engageThreshold = hardTune ? 2.5 : naturalEngageThreshold;
        if (! rendererControlIsValid)
        {
            // This is a hard voiced/unvoiced boundary, not a request to
            // glide a pitch-shifter over the following breath or consonant.
            // A previous revision tried to return the ratio to unity while
            // keeping this path wet; that transformed /s/, /t/, breaths and
            // end-of-phrase noise and was the direct source of the repeated
            // "bubbly"/clicky artefact. The latency-aligned crossfade in the
            // renderer takes care of the handoff without tuning invalid F0.
            rendererReleasingToUnity = false;
            rendererCorrectionEngaged = false;
            rendererEngageControlFrames = 0;
            rendererReleaseControlFrames = 0;
        }
        else if (! rendererCorrectionEngaged)
        {
            rendererEngageControlFrames = correctionMagnitudeCents >= engageThreshold
                                           ? rendererEngageControlFrames + 1 : 0;
            // Two ticks (5.3 ms at 48 kHz) are sufficient to reject a
            // one-frame false F0 while retaining a smooth 0 ms hard-tune.
            const auto requiredEngageFrames = hardTune ? VIBE_HARD_TUNE_ENGAGE_CONTROL_FRAMES : 3;
            rendererCorrectionEngaged = rendererEngageControlFrames >= requiredEngageFrames;
            rendererReleaseControlFrames = 0;
            if (rendererCorrectionEngaged)
            {
                rendererReleasingToUnity = false;
                rendererEngageControlFrames = 0;
            }
        }
        else
        {
            // Do not leave a phase-vocoder in the signal path for an entire
            // voiced phrase after it has reached unity. Even at a ratio of
            // 1.0, its analysis/resynthesis can add a faint doubled/chorused
            // vocal quality. A *sustained* near-unity region is returned to
            // the latency-aligned dry path. The 120 ms hysteresis prevents
            // vibrato and portamento from repeatedly crossfading two voices.
            const auto releaseThresholdCents = 1.0 - 0.55 * flexAmount;
            const auto releaseHoldSeconds = rendererReleaseHoldSeconds + 0.120 * flexAmount;
            const auto releaseFramesRequired = juce::jmax (1, static_cast<int> (std::ceil (
                releaseHoldSeconds * analysisSampleRate / rendererControlIntervalSamples)));
            rendererEngageControlFrames = 0;
            rendererReleasingToUnity = false;
            rendererReleaseControlFrames = correctionMagnitudeCents <= releaseThresholdCents
                                            ? rendererReleaseControlFrames + 1 : 0;
            if (rendererReleaseControlFrames >= releaseFramesRequired)
            {
                rendererCorrectionEngaged = false;
                rendererReleaseControlFrames = 0;
            }
        }

        // The renderer's dry path carries the same fixed latency as its wet
        // path.  Keeping a near-unity voiced phrase in spectral resynthesis
        // therefore has no timing benefit, but does create a faint second
        // voice from analysis/resynthesis.  `rendererCorrectionEngaged` is
        // the hysteretic decision made above specifically to avoid that.
        const auto correctionIsActive = rendererControlIsValid && rendererCorrectionEngaged;
        const auto rendererReleaseIsActive = rendererControlIsValid && rendererReleasingToUnity;
        const auto rendererWetIsActive = correctionIsActive && tdpsolaReentryDrySamples <= 0;
        // This is the correction actually sent to the audible renderer path,
        // not merely the trajectory's requested correction. The editor uses
        // it as an action meter.
        const auto requestedRendererRatio = rendererControlIsValid ? rendererControlRatio : 1.0;
        const auto dezipperAlpha = 1.0 - std::exp (-static_cast<double> (intervalSamples)
                                                   / (dezipperSeconds * analysisSampleRate));
        // A dry bridge is a renderer handoff, not a request to return the
        // pitch-control state to unity.  Holding this value prevents the
        // bridge itself from creating an extra down/up control excursion.
        if (! rendererControl.holdDry)
        {
            renderedPitchRatio += (requestedRendererRatio - renderedPitchRatio) * dezipperAlpha;
            renderedPitchRatio = juce::jlimit (0.84, 1.19, renderedPitchRatio);
            const auto previousCents = 1200.0 * std::log2 (renderedPitchRatio);
            const auto maximumStepCents = hardTune ? rendererMaximumHardTuneStepCents
                                                   : rendererMaximumContinuousStepCents;
            const auto limitedCents = juce::jlimit (previousRendererCorrectionCents - maximumStepCents,
                                                    previousRendererCorrectionCents + maximumStepCents,
                                                    previousCents);
            renderedPitchRatio = std::exp2 (limitedCents / 1200.0);
        }
        const auto deliveredCorrectionCents = (rendererWetIsActive || rendererControl.holdDry)
                                            ? 1200.0 * std::log2 (renderedPitchRatio) : 0.0;
        auto rendererStepCents = 0.0f;
        if (correctionIsActive)
        {
            if (hadActiveRendererControl)
            {
                rendererStepCents = static_cast<float> (std::abs (deliveredCorrectionCents
                                                                   - previousRendererCorrectionCents));
                const auto previousRendererMaximum = maximumRendererStepCents.load (std::memory_order_relaxed);
                if (rendererStepCents > previousRendererMaximum)
                    maximumRendererStepCents.store (rendererStepCents, std::memory_order_relaxed);
            }
            previousRendererCorrectionCents = deliveredCorrectionCents;
            hadActiveRendererControl = true;
        }
        else if (! rendererControl.holdDry)
            hadActiveRendererControl = false;
        if (rendererReleaseIsActive && std::abs (deliveredCorrectionCents) <= 0.75)
            rendererReleasingToUnity = false;
        latestCentsDeviation.store (correctionIsActive
                                         ? static_cast<float> (deliveredCorrectionCents)
                                         : 0.0f,
                                    std::memory_order_relaxed);
        // Feed the TD-PSOLA scheduler with source-time control. Its output
        // arrives one renderer latency later and is then mixed by the delayed
        // rendererWetIsActive stream above.
        const auto inputRendererControl = getRendererControlFrame (sourceTime, 3);
        debugCallerControlLookupTime = sourceTime;
        debugCallerControl = inputRendererControl;
        const auto inputPsolaVoiced = inputRendererControl.valid
                                   && std::isfinite (inputRendererControl.stableF0Hz)
                                   && inputRendererControl.stableF0Hz > 0.0
                                   && std::isfinite (inputRendererControl.pitchRatio)
                                   && ! inputRendererControl.holdDry;
        // The ratio is deliberately de-zippered by CorrectionTrajectory,
        // but the synthesis lattice must use the exact committed musical
        // destination. Feeding its cadence from transitionTargetCents made a
        // 12 ms A -> B -> A control ramp schedule intermediate grains and
        // therefore sound like a glide. Keep the two responsibilities apart.
        auto committedTargetHz = inputPsolaVoiced && inputRendererControl.targetMidi >= 0
                                     ? static_cast<double> (parameters.getRawParameterValue ("referencePitch")
                                                                 ->load (std::memory_order_relaxed))
                                           * std::exp2 ((static_cast<double> (inputRendererControl.targetMidi) - 69.0) / 12.0)
                                     : inputPsolaVoiced && std::isfinite (inputRendererControl.targetPitchCents)
                                           ? 440.0 * std::exp2 (inputRendererControl.targetPitchCents / 1200.0)
                                           : std::numeric_limits<double>::quiet_NaN();
        // A certified crossing can begin the synthesis cadence before its
        // selector commit.  Blend only over that short confirmed preview:
        // phase keeps integrating, existing OLA grains stay untouched, and
        // the first new grain follows the same continuous F0 contour as the
        // reference instead of holding the old note then overshooting.
        if (inputPsolaVoiced && std::isfinite (committedTargetHz)
            && inputRendererControl.rendererCrossingTargetMidi >= 0)
        {
            const auto previewTargetHz = static_cast<double> (parameters.getRawParameterValue ("referencePitch")
                                                                    ->load (std::memory_order_relaxed))
                                      * std::exp2 ((static_cast<double> (inputRendererControl.rendererCrossingTargetMidi) - 69.0) / 12.0);
            const auto previewProgress = juce::jlimit (0.0, 1.0,
                                                        inputRendererControl.rendererCrossingProgress);
            committedTargetHz += (previewTargetHz - committedTargetHz) * previewProgress;
        }
        const std::array<const float*, 2> stagedPointers {
            sourceStage.data(), sourceStage.data() + sourceStageStorageSamples };
        processPitchRenderer (buffer, offset, outputSamples, rendererWetIsActive,
                                    rendererWetIsActive ? renderedPitchRatio : 1.0,
                                    rendererWetIsActive ? rendererControlF0 : std::numeric_limits<double>::quiet_NaN(),
                                    inputPsolaVoiced,
                                    inputPsolaVoiced ? inputRendererControl.pitchRatio : 1.0,
                                    inputPsolaVoiced ? inputRendererControl.stableF0Hz
                                                     : std::numeric_limits<double>::quiet_NaN(),
                                    committedTargetHz, sourceTime, outputTime,
                                    stagedPointers.data(), activeSourceSchedulerQuantumSamples);
        tdpsolaReentryDrySamples = juce::jmax (0, tdpsolaReentryDrySamples - intervalSamples);
        const auto writeDiagnosticFrame = [this, outputTime, requestedRendererRatio,
                                           deliveredCorrectionCents, rendererStepCents,
                                           correctionIsActive, &rendererControl, inputRendererControl] (SignalChainDiagnosticFrame& frame)
        {
            frame = {};
            frame.sampleIndex = outputTime;
            frame.rawF0Hz = latestRawF0Hz.load (std::memory_order_relaxed);
            frame.stableF0Hz = latestStableF0Hz.load (std::memory_order_relaxed);
            frame.pitchConfidence = latestPitchConfidence.load (std::memory_order_relaxed);
            frame.inputPitchCents = latestCorrectionTrajectory.inputPitchCents;
            frame.targetPitchCents = latestCorrectionTrajectory.targetPitchCents;
            frame.candidateTargetCents = latestCorrectionTrajectory.candidateTargetNote >= 0
                                       ? (static_cast<double> (latestCorrectionTrajectory.candidateTargetNote) - 69.0) * 100.0
                                       : std::numeric_limits<double>::quiet_NaN();
            frame.requestedRendererRatio = requestedRendererRatio;
            frame.deliveredRendererCorrectionCents = static_cast<float> (deliveredCorrectionCents);
            const auto& rendererDiagnostic = tdpsolaRenderers[0].getDiagnosticState();
            frame.rendererPreGuardCorrectionCents = static_cast<float> (rendererDiagnostic.rendererPreGuardCorrectionCents);
            frame.rendererPostGuardCorrectionCents = static_cast<float> (rendererDiagnostic.rendererPostGuardCorrectionCents);
            frame.rendererInputCorrectionCents = static_cast<float> (rendererDiagnostic.rendererInputCorrectionCents);
            frame.rendererCorrectionSource = static_cast<uint8_t> (rendererDiagnostic.correctionSource);
            frame.rendererStepCents = rendererStepCents;
            frame.dryWetMix = static_cast<float> (rendererDiagnostic.wetMix);
            frame.retuneSpeedMs = parameters.getRawParameterValue ("retuneSpeed")->load (std::memory_order_relaxed);
            frame.flexTunePercent = parameters.getRawParameterValue ("flexTune")->load (std::memory_order_relaxed);
            frame.humanizePercent = parameters.getRawParameterValue ("humanize")->load (std::memory_order_relaxed);
            frame.naturalVibratoPercent = parameters.getRawParameterValue ("naturalVibrato")->load (std::memory_order_relaxed);
            frame.trackingValue = parameters.getRawParameterValue ("tracking")->load (std::memory_order_relaxed);
            frame.flexCorrectionGain = static_cast<float> (latestCorrectionTrajectory.flexCorrectionGain);
            frame.desiredCorrectionCents = static_cast<float> (latestCorrectionTrajectory.desiredCorrectionCents);
            frame.retuneCorrectionCents = static_cast<float> (latestCorrectionTrajectory.retuneCorrectionCents);
            frame.humanizeWeight = static_cast<float> (latestCorrectionTrajectory.humanizeWeight);
            frame.vibratoContributionCents = static_cast<float> (latestCorrectionTrajectory.vibratoContributionCents);
            frame.rawVibratoResidualCents = static_cast<float> (latestCorrectionTrajectory.rawVibratoResidualCents);
            frame.vibratoEnergyCents = static_cast<float> (latestCorrectionTrajectory.vibratoEnergyCents);
            frame.vibratoDeviationCents = static_cast<float> (latestCorrectionTrajectory.vibratoDeviationCents);
            frame.pitchNoteCenterCents = static_cast<float> (latestCorrectionTrajectory.noteCenterCents);
            frame.pitchDriftCents = static_cast<float> (latestCorrectionTrajectory.driftCents);
            frame.pitchVibratoRateHz = static_cast<float> (latestCorrectionTrajectory.vibratoRateHz);
            frame.pitchVibratoConfidence = static_cast<float> (latestCorrectionTrajectory.vibratoConfidence);
            frame.pitchNoteStability = static_cast<float> (latestCorrectionTrajectory.noteStability);
            frame.pitchDriftConfidence = static_cast<float> (latestCorrectionTrajectory.driftConfidence);
            frame.pitchJitterConfidence = static_cast<float> (latestCorrectionTrajectory.jitterConfidence);
            frame.referenceHz = parameters.getRawParameterValue ("referencePitch")->load (std::memory_order_relaxed);
            frame.voiceRangeMinHz = static_cast<int> (pitchAnalysisConfig.minimumFrequency);
            frame.voiceRangeMaxHz = static_cast<int> (pitchAnalysisConfig.maximumFrequency);
            frame.rawMidi = latestDetectedMidiNote.load (std::memory_order_relaxed);
            frame.candidateTargetMidi = latestCorrectionTrajectory.candidateTargetNote;
            frame.committedTargetMidi = latestCorrectionTrajectory.targetNote;
            frame.key = static_cast<int> (parameters.getRawParameterValue ("key")->load (std::memory_order_relaxed));
            frame.scale = static_cast<int> (parameters.getRawParameterValue ("scale")->load (std::memory_order_relaxed));
            frame.transpose = static_cast<int> (parameters.getRawParameterValue ("transpose")->load (std::memory_order_relaxed));
            frame.voiced = latestVoiced.load (std::memory_order_relaxed);
            frame.targetChanged = latestCorrectionTrajectory.targetChanged;
            frame.correctionIsActive = correctionIsActive;
            frame.targetBridge = rendererControl.targetBridge;
            frame.transientBridge = rendererControl.transientBridge;
            frame.trajectoryTransitionActive = latestCorrectionTrajectory.transitionActive;
            frame.trajectoryTransitionProgress = static_cast<float> (latestCorrectionTrajectory.transitionProgress);
            frame.trajectoryTransitionStartCents = latestCorrectionTrajectory.transitionStartCents;
            frame.trajectoryTransitionEndCents = latestCorrectionTrajectory.transitionEndCents;
            frame.trajectoryTransitionDurationMs = static_cast<float> (latestCorrectionTrajectory.transitionDurationMs);
            frame.trajectoryRapidAdjacentContour = latestCorrectionTrajectory.transitionUsesRapidAdjacentContour;
            frame.trajectoryRendererCrossingActive = latestCorrectionTrajectory.rendererCrossingActive;
            frame.trajectoryRendererCrossingTargetMidi = latestCorrectionTrajectory.rendererCrossingTargetNote;
            frame.trajectoryRendererCrossingProgress = static_cast<float> (latestCorrectionTrajectory.rendererCrossingProgress);
            frame.trajectoryExcursionActive = latestCorrectionTrajectory.continuousReturnExcursionActive;
            frame.trajectoryExcursionPhase = latestCorrectionTrajectory.continuousReturnExcursionPhase;
            frame.trajectoryExcursionConsumed = latestCorrectionTrajectory.continuousReturnExcursionConsumed;
            frame.trajectoryExcursionOwnsControl = latestCorrectionTrajectory.continuousReturnExcursionOwnsControl;
            frame.targetDecisionMidpointCents = latestCorrectionTrajectory.targetDecisionMidpointCents;
            frame.targetDecisionExcessCents = static_cast<float> (latestCorrectionTrajectory.targetDecisionExcessCents);
            frame.targetHysteresisCents = static_cast<float> (latestCorrectionTrajectory.targetHysteresisCents);
            frame.pendingTargetMidi = latestCorrectionTrajectory.pendingTargetNote;
            frame.pendingTargetFrames = latestCorrectionTrajectory.pendingTargetFrames;
            frame.targetDecisionBeyondHysteresis = latestCorrectionTrajectory.targetDecisionBeyondHysteresis;
            frame.rapidAdjacentRawCandidate = latestCorrectionTrajectory.rapidAdjacentRawCandidate;
            frame.rapidAdjacentPrearmed = latestCorrectionTrajectory.rapidAdjacentPrearmed;
            frame.rapidAdjacentCandidateMidi = latestCorrectionTrajectory.rapidAdjacentCandidateNote;
            frame.rapidAdjacentEvidenceFrames = latestCorrectionTrajectory.rapidAdjacentEvidenceFrames;
            frame.rapidAdjacentRawDistanceCents = latestCorrectionTrajectory.rapidAdjacentRawDistanceCents;
            frame.rapidLegatoAdjacentStep = latestCorrectionTrajectory.rapidLegatoAdjacentStep;
            frame.rapidLegatoHardLock = latestCorrectionTrajectory.rapidLegatoHardLock;
            frame.rapidLegatoTargetChanged = latestCorrectionTrajectory.rapidLegatoTargetChanged;
            frame.rapidLegatoContinuesDirection = latestCorrectionTrajectory.rapidLegatoContinuesDirection;
            frame.rapidLegatoChainStarts = latestCorrectionTrajectory.rapidLegatoChainStarts;
            frame.rapidLegatoChainActive = latestCorrectionTrajectory.rapidLegatoChainActive;
            frame.rapidLegatoCommittedSteps = latestCorrectionTrajectory.rapidLegatoCommittedSteps;
            frame.rapidLegatoSamplesSinceCommit = latestCorrectionTrajectory.rapidLegatoSamplesSinceCommit;
            frame.trajectoryPreviewNote = latestCorrectionTrajectory.trajectoryPreviewNote;
            frame.trajectoryPreviewDirection = latestCorrectionTrajectory.trajectoryPreviewDirection;
            frame.trajectoryPreviewUpdates = latestCorrectionTrajectory.trajectoryPreviewUpdates;
            frame.trajectoryPreviewTravelCents = latestCorrectionTrajectory.trajectoryPreviewTravelCents;
            frame.instantaneousInputMotionCentsPerSecond = latestCorrectionTrajectory.instantaneousInputMotionCentsPerSecond;
            frame.selectorCurrentTargetBefore = latestCorrectionTrajectory.selectorCurrentTargetBefore;
            frame.selectorPreviousTarget = latestCorrectionTrajectory.selectorPreviousTarget;
            frame.selectorCandidate = latestCorrectionTrajectory.selectorCandidate;
            frame.selectorFramesSinceCommit = latestCorrectionTrajectory.selectorFramesSinceCommit;
            frame.selectorDecision = latestCorrectionTrajectory.selectorDecision;
            frame.trajectoryPortamentoActive = latestCorrectionTrajectory.portamentoActive;
            frame.trajectoryUsesPortamentoTarget = latestCorrectionTrajectory.usesPortamentoTarget;
            frame.rapidLegatoDistanceCents = latestCorrectionTrajectory.rapidLegatoDistanceCents;
            frame.rapidLegatoInputMotionCentsPerSecond = latestCorrectionTrajectory.rapidLegatoInputMotionCentsPerSecond;
            frame.latticeReset = rendererDiagnostic.latticeReset;
            frame.dryGuardActive = rendererDiagnostic.dryGuardActive;
            frame.reentryHoldActive = rendererDiagnostic.reentryHoldActive;
            frame.sourceControlTimestamp = inputRendererControl.timestampSamples;
            frame.outputRendererControlTimestamp = rendererControl.timestampSamples;
            frame.outputRendererControlPitchRatio = rendererControl.pitchRatio;
            frame.outputRendererControlTargetMidi = rendererControl.targetMidi;
            frame.sourceControlF0Hz = inputRendererControl.stableF0Hz;
            frame.sourceControlTargetCents = inputRendererControl.targetPitchCents;
            frame.sourceControlTargetMidi = inputRendererControl.targetMidi;
            frame.sourceControlValid = inputRendererControl.valid;
            frame.sourceControlHoldDry = inputRendererControl.holdDry;
            frame.sourceControlPitchRatio = inputRendererControl.pitchRatio;
            frame.sourceControlMaterializationAdvanceSamples = inputRendererControl.debugControlAdvanceSamples;
            frame.sourceControlCorrectionBefore = inputRendererControl.debugControlCorrectionBefore;
            frame.sourceControlCorrectionAfter = inputRendererControl.debugControlCorrectionAfter;
            frame.sourceControlTrajectoryExcursionActive = inputRendererControl.continuousReturnExcursionActive;
            frame.sourceControlTrajectoryExcursionPhase = inputRendererControl.continuousReturnExcursionPhase;
            frame.sourceControlTrajectoryExcursionOwnsControl = inputRendererControl.continuousReturnExcursionOwnsControl;
            frame.callerControlLookupTime = debugCallerControlLookupTime;
            frame.rendererControlLookupTime = debugRendererControlLookupTime;
            frame.controlLookupDeltaSamples = static_cast<int64_t> (debugRendererControlLookupTime)
                                            - static_cast<int64_t> (debugCallerControlLookupTime);
            frame.callerControlTimestamp = debugCallerControl.timestampSamples;
            frame.rendererControlTimestamp = debugRendererControl.timestampSamples;
            frame.callerControlEventId = debugCallerControl.targetEventId;
            frame.rendererControlEventId = debugRendererControl.targetEventId;
            frame.callerControlPitchRatio = debugCallerControl.pitchRatio;
            frame.rendererControlPitchRatio = debugRendererControl.pitchRatio;
            frame.rendererStrongRatioStep = rendererDiagnostic.strongRatioStep;
            frame.rendererRapidCorrectionReversal = rendererDiagnostic.rapidCorrectionReversal;
            frame.rendererTargetCommitted = rendererDiagnostic.targetCommitted;
            frame.sourceCursorBeforeStrongStep = rendererDiagnostic.sourceCursorBeforeStrongStep;
            frame.sourceCursorAfterStrongStep = rendererDiagnostic.sourceCursorAfterStrongStep;
            frame.sourceCursorReanchorSuppressed = rendererDiagnostic.sourceCursorReanchorSuppressed;
            frame.fractionalPhaseOffsetAtSpawn = rendererDiagnostic.fractionalPhaseOffsetAtSpawn;
            frame.overlapPhaseOffsetAtSpawn = rendererDiagnostic.overlapPhaseOffsetAtSpawn;
            frame.olaOverlapWeight = static_cast<float> (rendererDiagnostic.currentOverlapWeight);
            frame.olaAudibleRatioAtStart = rendererDiagnostic.audibleRatioAtStart;
            frame.olaAudibleGrainAge = rendererDiagnostic.audibleGrainAge;
            frame.olaNewestTargetGrainWeight = rendererDiagnostic.newestTargetGrainWeight;
            frame.olaPreviousTargetGrainWeight = rendererDiagnostic.previousTargetGrainWeight;
            frame.olaNewestTargetFirstAudibleSample = rendererDiagnostic.newestTargetFirstAudibleSample;
            frame.olaPreviousTargetLastAudibleSample = rendererDiagnostic.previousTargetLastAudibleSample;
            frame.olaAudibleReadPosition = rendererDiagnostic.audibleReadPosition;
            frame.olaDominantGrainWeight = rendererDiagnostic.dominantGrainWeight;
            frame.olaDominantGrainEpoch = rendererDiagnostic.dominantGrainEpoch;
            frame.olaDominantGrainRatio = rendererDiagnostic.dominantGrainRatio;
            frame.olaDominantGrainExpectedF0 = rendererDiagnostic.dominantGrainExpectedF0;
            frame.olaDominantGrainTargetMidi = rendererDiagnostic.dominantGrainTargetMidi;
            frame.olaDominantGrainTargetEventId = rendererDiagnostic.dominantGrainTargetEventId;
            frame.olaDominantGrainControlEventId = rendererDiagnostic.dominantGrainControlEventId;
            frame.olaDominantGrainTargetControlTimestamp = rendererDiagnostic.dominantGrainTargetControlTimestamp;
            frame.olaDominantGrainTrajectoryExcursionId = rendererDiagnostic.dominantGrainTrajectoryExcursionId;
            frame.olaTargetCandidateWeight = rendererDiagnostic.targetCandidateWeight;
            frame.olaTargetCandidateEpoch = rendererDiagnostic.targetCandidateEpoch;
            frame.olaTargetCandidateExpectedF0 = rendererDiagnostic.targetCandidateExpectedF0;
            frame.actualSpawnEpoch = rendererDiagnostic.actualSpawnEpoch;
            frame.grainDecisionInputSample = rendererDiagnostic.grainDecisionInputSample;
            frame.grainFirstAudibleSample = rendererDiagnostic.grainFirstAudibleSample;
            frame.grainHalfWindowAtSpawn = rendererDiagnostic.grainHalfWindowAtSpawn;
            frame.grainSourceMarkAtSpawn = rendererDiagnostic.grainSourceMarkAtSpawn;
            frame.ratioAtSpawn = rendererDiagnostic.ratioAtSpawn;
            frame.spawnTargetMidi = rendererDiagnostic.targetMidiAtSpawn;
            frame.spawnTargetEventId = rendererDiagnostic.targetEventIdAtSpawn;
            frame.spawnTargetControlTimestamp = rendererDiagnostic.targetControlTimestampAtSpawn;
            frame.spawnTrajectoryExcursionId = rendererDiagnostic.trajectoryExcursionIdAtSpawn;
            frame.spawnSourceMarkF0 = rendererDiagnostic.sourcePeriodAtSpawn > 0.0
                                        ? analysisSampleRate / rendererDiagnostic.sourcePeriodAtSpawn : 0.0;
            frame.spawnExpectedF0 = frame.spawnSourceMarkF0 * frame.ratioAtSpawn;
            frame.olaSourceMarkTime = rendererDiagnostic.audibleSourceMarkTime;
            frame.olaSourceMarkF0 = rendererDiagnostic.audibleSourceMarkF0;
            frame.olaExpectedGrainF0 = rendererDiagnostic.audibleExpectedGrainF0;
            frame.synthExpectedPeriod = rendererDiagnostic.expectedOutputPeriod;
            frame.synthActualPeriod = rendererDiagnostic.actualSynthPeriod;
            frame.synthPeriodError = rendererDiagnostic.expectedSynthPeriodError;
            frame.synthSourceSpacing = rendererDiagnostic.actualSourceMarkSpacing;
            frame.sourceSelectionCursor = rendererDiagnostic.sourceSelectionCursor;
            frame.newestCompleteSourceMark = rendererDiagnostic.newestCompleteSourceMark;
            frame.sibilanceResidualMix = rendererDiagnostic.sibilanceResidualMix;
            frame.rendererHighFrequencyEvidence = rendererDiagnostic.highFrequencyEvidence;
            frame.rendererPeriodicCoherence = rendererDiagnostic.periodicCoherence;
            frame.pitchMarkCandidateOffset = rendererDiagnostic.pitchMarkCandidateOffset;
            frame.pitchMarkCandidateCorrelation = rendererDiagnostic.pitchMarkCandidateCorrelation;
            frame.confirmedPitchMarkOffset = rendererDiagnostic.confirmedPitchMarkOffset;
            frame.confirmedPitchMarkRun = rendererDiagnostic.confirmedPitchMarkRun;
            frame.rendererArtifactDryVeto = rendererDiagnostic.artifactDryVeto;
            frame.rendererGrainStarved = rendererDiagnostic.grainStarved;
            frame.rendererShouldRenderWet = rendererDiagnostic.shouldRenderWet;
            frame.rendererSafetyDrySamples = rendererDiagnostic.remainingSafetyDrySamples;
        };

        if (signalChainDiagnosticEnabled.load (std::memory_order_relaxed)
            && signalChainDiagnosticFrameCount < static_cast<int> (signalChainDiagnosticFrames.size()))
        {
            auto& frame = signalChainDiagnosticFrames[static_cast<size_t> (signalChainDiagnosticFrameCount++)];
            writeDiagnosticFrame (frame);
        }

        if (liveDiagnosticCaptureEnabled.load (std::memory_order_relaxed))
        {
            SignalChainDiagnosticFrame liveFrame;
            writeDiagnosticFrame (liveFrame);
            captureLiveDiagnosticFrame (liveFrame);
        }
        {
            if (sourceStageStartSample < nextScheduledSourceSample)
                ++schedulerOverlapCount;
            else if (sourceStageStartSample > nextScheduledSourceSample)
                ++schedulerGapCount;
            nextScheduledSourceSample = sourceStageStartSample + activeSourceSchedulerQuantumSamples;
            sourceSamplesScheduled += activeSourceSchedulerQuantumSamples;
            ++sourceQuantumsScheduled;
            sourceStageFill = 0;
            sourceStageStartSample = nextScheduledSourceSample;
        }
        offset += copyCount;
        }
        processedInputSamples = blockStartSample + static_cast<uint64_t> (juce::jmax (0, numberOfSamples));
        return;
    }

}


juce::String VibeAutotuneProcessor::noteNameForMidi (int midiNote)
{
    if (midiNote < 0 || midiNote > 127)
        return "--";

    static constexpr std::array<const char*, 12> names {{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }};
    return juce::String (names[static_cast<size_t> (midiNote % 12)]) + juce::String (midiNote / 12 - 1);
}

VibeAutotuneProcessor::InputPitchDisplayData VibeAutotuneProcessor::calculateInputPitchDisplay (double stableF0Hz,
                                                                                                   double referenceA4Hz,
                                                                                                   bool voiced) noexcept
{
    InputPitchDisplayData display;
    if (! voiced || ! std::isfinite (stableF0Hz) || stableF0Hz <= 0.0
        || ! std::isfinite (referenceA4Hz) || referenceA4Hz <= 0.0)
        return display;

    const auto midi = 69.0 + 12.0 * std::log2 (stableF0Hz / referenceA4Hz);
    const auto nearestMidi = juce::jlimit (0, 127, static_cast<int> (std::lround (midi)));
    display.midiNote = nearestMidi;
    display.cents = static_cast<float> (juce::jlimit (-100.0, 100.0,
                                                       (midi - static_cast<double> (nearestMidi)) * 100.0));
    return display;
}

VibeAutotuneProcessor::PitchMonitorData VibeAutotuneProcessor::getPitchMonitorData() const
{
    PitchMonitorData monitor;
    monitor.bypassed = latestBypassed.load (std::memory_order_relaxed);
    monitor.voiced = latestVoiced.load (std::memory_order_relaxed);
    monitor.detectedNote = noteNameForMidi (latestDetectedMidiNote.load (std::memory_order_relaxed));
    monitor.targetNote = noteNameForMidi (latestTargetMidiNote.load (std::memory_order_relaxed));
    monitor.outputNote = noteNameForMidi (latestOutputMidiNote.load (std::memory_order_relaxed));
    monitor.inputCents = latestInputCents.load (std::memory_order_relaxed);
    monitor.correctionCents = latestCentsDeviation.load (std::memory_order_relaxed);
    return monitor;
}

VibeAutotuneProcessor::PitchTrackingData VibeAutotuneProcessor::getPitchTrackingData() const noexcept
{
    PitchTrackingData data;
    data.rawF0Hz = latestRawF0Hz.load (std::memory_order_relaxed);
    data.stableF0Hz = latestStableF0Hz.load (std::memory_order_relaxed);
    data.confidence = latestPitchConfidence.load (std::memory_order_relaxed);
    data.voiced = latestVoiced.load (std::memory_order_relaxed);
    data.timestampSamples = latestPitchTimestampSamples.load (std::memory_order_relaxed);
    data.analysisLatencySamples = latestAnalysisLatencySamples.load (std::memory_order_relaxed);
    data.detectedMidiNote = latestDetectedMidiNote.load (std::memory_order_relaxed);
    data.targetMidiNote = latestTargetMidiNote.load (std::memory_order_relaxed);
    data.outputMidiNote = latestOutputMidiNote.load (std::memory_order_relaxed);
    data.inputCents = latestInputCents.load (std::memory_order_relaxed);
    data.correctionCents = latestCentsDeviation.load (std::memory_order_relaxed);
    return data;
}

VibeAutotuneProcessor::SourceSchedulerDiagnostics
VibeAutotuneProcessor::getSourceSchedulerDiagnostics() const noexcept
{
    return { sourceSamplesReceived, sourceSamplesScheduled, sourceQuantumsScheduled,
             sourceStageFill, sourceDropCount, sourceDuplicateCount,
             schedulerGapCount, schedulerOverlapCount,
             tdpsolaRenderers[0].getMaximumOutputAheadAtSourcePlanner(),
             tdpsolaRenderers[0].getMaximumOutputAdvanceBetweenSourcePlanners() };
}

uint64_t VibeAutotuneProcessor::getRendererOlaDeadlineViolationCount() const noexcept
{
    return tdpsolaRenderers[0].getOlaDeadlineViolationCount();
}

uint64_t VibeAutotuneProcessor::getRendererPendingDeadlineMissCount() const noexcept
{
    return tdpsolaRenderers[0].getPendingDeadlineMissCount();
}

uint64_t VibeAutotuneProcessor::getRendererLateCandidateCreationCount() const noexcept
{
    return tdpsolaRenderers[0].getLateCandidateCreationCount();
}

uint64_t VibeAutotuneProcessor::getRendererLateOlaWriteCount() const noexcept
{
    return tdpsolaRenderers[0].getLateOlaWriteCount();
}

int VibeAutotuneProcessor::getRendererPendingQueuePeakOccupancy() const noexcept
{
    return tdpsolaRenderers[0].getPendingQueuePeakOccupancy();
}

uint64_t VibeAutotuneProcessor::getRendererPendingQueueOverflowCount() const noexcept
{
    return tdpsolaRenderers[0].getPendingQueueOverflowCount();
}

int VibeAutotuneProcessor::getRendererRetimableEpochPeakOccupancy() const noexcept
{
    return tdpsolaRenderers[0].getRetimableEpochPeakOccupancy();
}

uint64_t VibeAutotuneProcessor::getRendererEpochRetimeCount() const noexcept
{
    return tdpsolaRenderers[0].getEpochRetimeCount();
}

uint64_t VibeAutotuneProcessor::getRendererRetimedSamplesTotal() const noexcept
{
    return tdpsolaRenderers[0].getRetimedSamplesTotal();
}

uint64_t VibeAutotuneProcessor::getRendererMaxSingleEpochRetime() const noexcept
{
    return tdpsolaRenderers[0].getMaxSingleEpochRetime();
}

uint64_t VibeAutotuneProcessor::getRendererRetimeDeadlineMissCount() const noexcept
{
    return tdpsolaRenderers[0].getRetimeDeadlineMissCount();
}

uint64_t VibeAutotuneProcessor::getRendererEpochOrderViolationCount() const noexcept
{
    return tdpsolaRenderers[0].getEpochOrderViolationCount();
}

uint64_t VibeAutotuneProcessor::getRendererTicketIssuedPastDeadlineCount() const noexcept
{
    return tdpsolaRenderers[0].getTicketIssuedPastDeadlineCount();
}

uint64_t VibeAutotuneProcessor::getRendererTicketMutationAfterIssueCount() const noexcept
{
    return tdpsolaRenderers[0].getTicketMutationAfterIssueCount();
}

void VibeAutotuneProcessor::setRendererEpochTraceEnabled (bool shouldEnable) noexcept
{
    for (auto& renderer : tdpsolaRenderers)
        renderer.setEpochTraceEnabled (shouldEnable);
}

int VibeAutotuneProcessor::copyRendererEpochTraces (CausalTDPSOLARenderer::EpochTrace* destination,
                                                     int maximumTraces) const noexcept
{
    return tdpsolaRenderers[0].copyEpochTraces (destination, maximumTraces);
}

void VibeAutotuneProcessor::setRendererOlaContributionTraceRange (uint64_t firstSample, uint64_t lastSample)
{
    for (auto& renderer : tdpsolaRenderers)
        renderer.setOlaContributionTraceRange (firstSample, lastSample);
}

int VibeAutotuneProcessor::copyRendererOlaContributionTraces (
    CausalTDPSOLARenderer::OlaContributionTrace* destination, int maximumTraces) const noexcept
{
    return tdpsolaRenderers[0].copyOlaContributionTraces (destination, maximumTraces);
}

VibeAutotuneProcessor::CorrectionDeliveryData VibeAutotuneProcessor::getCorrectionDeliveryData() const noexcept
{
    CorrectionDeliveryData data;
    data.targetSwitchCount = committedTargetSwitchCount.load (std::memory_order_relaxed);
    data.dryBridgeCount = dryBridgeActivationCount.load (std::memory_order_relaxed);
    data.maxTrajectoryStepCents = maximumTrajectoryStepCents.load (std::memory_order_relaxed);
    data.maxRendererStepCents = maximumRendererStepCents.load (std::memory_order_relaxed);
    return data;
}

VibeAutotuneProcessor::RendererDiscontinuityData VibeAutotuneProcessor::getRendererDiscontinuityData() const noexcept
{
    return rendererDiscontinuityData;
}

void VibeAutotuneProcessor::setSignalChainDiagnosticEnabled (bool shouldEnable) noexcept
{
    signalChainDiagnosticEnabled.store (shouldEnable, std::memory_order_relaxed);
    signalChainDiagnosticFrameCount = 0;
    rendererControlLookupTraceCount = 0;
    const auto track = shouldEnable || liveDiagnosticCaptureEnabled.load (std::memory_order_relaxed);
    for (auto& renderer : tdpsolaRenderers)
        renderer.setDiagnosticTrackingEnabled (track);
}

void VibeAutotuneProcessor::setRapidAdjacentTargetSelectionEnabled (bool shouldEnable) noexcept
{
    correctionTrajectory.setRapidAdjacentTargetSelectionEnabled (shouldEnable);
}

void VibeAutotuneProcessor::setRendererDiagnosticAblation (CausalTDPSOLARenderer::DiagnosticAblation value) noexcept
{
    // Renderer ownership experiments must not silently alter the musical
    // control path. A trajectory S-curve is enabled only by a dedicated
    // future diagnostic switch, never by target-aware grain ownership.
    correctionTrajectory.setDiagnosticRaisedCosineHardTuneLegato (false);
    correctionTrajectory.setDiagnosticRapidLegatoPreview (false);
    correctionTrajectory.setDiagnosticRapidLegatoReturnPreview (value.rapidLegatoReturnPreview);
    correctionTrajectory.setDiagnosticContinuousRapidReturnExcursion (value.continuousRapidReturnExcursion);
    correctionTrajectory.setDiagnosticContinuousReturnExcursionTuning (
        value.continuousReturnMinimumCoherentFrames,
        value.continuousReturnMinimumTravelCents,
        value.continuousReturnHoldMilliseconds);
    // Candidate rendering is strictly a renderer-side forensic experiment.
    // It must not change target timing or the correction trajectory: doing so
    // makes a listening comparison cease to be A/B against the shipped
    // production path. Keep every trajectory-only experiment disabled here.
    correctionTrajectory.setDiagnosticStrongHardTunePull (false);
    correctionTrajectory.setDiagnosticSlowRetuneInputReferenceAmountOverride (
        value.slowRetuneInputReferenceAmountOverride);
    usePhaseContinuousRenderer.store (value.phaseContinuousRenderer, std::memory_order_release);
    phaseForceUnityRatioForDiagnostic.store (value.phaseForceUnityRatio, std::memory_order_release);
    phaseForceStaticUpRatioForDiagnostic.store (value.phaseForceStaticUpRatio, std::memory_order_release);
    for (auto& renderer : tdpsolaRenderers)
        renderer.setDiagnosticAblation (value);
}

void VibeAutotuneProcessor::setControlCalibrationDiagnostic (double pitchCentreSeconds,
                                                              double wideVibratoNeutralRetention,
                                                              double wideVibratoHumanizedRetention) noexcept
{
    correctionTrajectory.setDiagnosticControlCalibration (pitchCentreSeconds,
                                                           wideVibratoNeutralRetention,
                                                           wideVibratoHumanizedRetention);
}

void VibeAutotuneProcessor::setNaturalVibratoSurfaceDiagnostic (
    const CorrectionTrajectory::NaturalVibratoSurface& surface) noexcept
{
    correctionTrajectory.setDiagnosticNaturalVibratoSurface (surface);
}

void VibeAutotuneProcessor::setNegativeNaturalPhaseScaleDiagnostic (double scale) noexcept
{
    correctionTrajectory.setDiagnosticNegativeNaturalPhaseScale (scale);
}

void VibeAutotuneProcessor::setHardTuneWideTransitionDiagnostic (double upwardDurationMs,
                                                                 double downwardDurationMs,
                                                                 int shape) noexcept
{
    correctionTrajectory.setDiagnosticHardTuneWideTransition (upwardDurationMs,
                                                               downwardDurationMs,
                                                               shape);
}

void VibeAutotuneProcessor::setLiveDiagnosticCaptureEnabled (bool shouldEnable) noexcept
{
    if (shouldEnable)
        liveDiagnosticClearRequested.store (true, std::memory_order_release);

    liveDiagnosticCaptureEnabled.store (shouldEnable, std::memory_order_release);
    const auto track = shouldEnable || signalChainDiagnosticEnabled.load (std::memory_order_relaxed);
    for (auto& renderer : tdpsolaRenderers)
        renderer.setDiagnosticTrackingEnabled (track);
}

bool VibeAutotuneProcessor::isLiveDiagnosticCaptureEnabled() const noexcept
{
    return liveDiagnosticCaptureEnabled.load (std::memory_order_acquire);
}

void VibeAutotuneProcessor::captureLiveDiagnosticFrame (const SignalChainDiagnosticFrame& frame) noexcept
{
    if (liveDiagnosticClearRequested.exchange (false, std::memory_order_acq_rel))
    {
        liveDiagnosticProducerCount = 0;
        liveDiagnosticPublishedCount.store (0, std::memory_order_release);
    }

    const auto slot = static_cast<size_t> (liveDiagnosticProducerCount
                                             % static_cast<uint64_t> (liveDiagnosticCapacity));
    (*liveDiagnosticFrames)[slot] = frame;
    ++liveDiagnosticProducerCount;
    liveDiagnosticPublishedCount.store (liveDiagnosticProducerCount, std::memory_order_release);
}

void VibeAutotuneProcessor::captureWaveformScope (const float* input, int sampleCount,
                                                   uint64_t startSample) noexcept
{
    if (input == nullptr || sampleCount <= 0)
        return;

    // 128 samples is only 2.7 ms at 48 kHz: fine enough to reveal consonants
    // and clicks while keeping 1024 buckets (~2.7 s) compact and UI-friendly.
    constexpr int bucketSamples = 128;
    for (int offset = 0; offset < sampleCount; offset += bucketSamples)
    {
        const auto count = juce::jmin (bucketSamples, sampleCount - offset);
        auto minimum = input[offset];
        auto maximum = input[offset];
        for (int index = 1; index < count; ++index)
        {
            const auto value = input[offset + index];
            minimum = juce::jmin (minimum, value);
            maximum = juce::jmax (maximum, value);
        }

        auto& frame = waveformScopeFrames[static_cast<size_t> (waveformScopeProducerCount
                                           % static_cast<uint64_t> (waveformScopeCapacity))];
        frame.sampleIndex.store (startSample + static_cast<uint64_t> (offset), std::memory_order_relaxed);
        frame.minimum.store (minimum, std::memory_order_relaxed);
        frame.maximum.store (maximum, std::memory_order_relaxed);
        frame.inputPitchCents.store (static_cast<float> (latestCorrectionTrajectory.inputPitchCents), std::memory_order_relaxed);
        frame.targetPitchCents.store (static_cast<float> (latestCorrectionTrajectory.targetPitchCents), std::memory_order_relaxed);
        frame.deliveredCorrectionCents.store (latestCentsDeviation.load (std::memory_order_relaxed), std::memory_order_relaxed);
        frame.voiced.store (latestVoiced.load (std::memory_order_relaxed), std::memory_order_relaxed);
        ++waveformScopeProducerCount;
        waveformScopePublishedCount.store (waveformScopeProducerCount, std::memory_order_release);
    }
}

int VibeAutotuneProcessor::copyWaveformScopeFrames (WaveformScopeFrame* destination,
                                                     int maximumFrames) const noexcept
{
    if (destination == nullptr || maximumFrames <= 0)
        return 0;
    const auto published = waveformScopePublishedCount.load (std::memory_order_acquire);
    const auto count = juce::jmin (juce::jmin (published, static_cast<uint64_t> (waveformScopeCapacity)),
                                   static_cast<uint64_t> (maximumFrames));
    const auto first = published - count;
    for (uint64_t index = 0; index < count; ++index)
    {
        const auto& source = waveformScopeFrames[static_cast<size_t> ((first + index)
                                                 % static_cast<uint64_t> (waveformScopeCapacity))];
        auto& frame = destination[static_cast<size_t> (index)];
        frame.sampleIndex = source.sampleIndex.load (std::memory_order_relaxed);
        frame.minimum = source.minimum.load (std::memory_order_relaxed);
        frame.maximum = source.maximum.load (std::memory_order_relaxed);
        frame.inputPitchCents = source.inputPitchCents.load (std::memory_order_relaxed);
        frame.targetPitchCents = source.targetPitchCents.load (std::memory_order_relaxed);
        frame.deliveredCorrectionCents = source.deliveredCorrectionCents.load (std::memory_order_relaxed);
        frame.voiced = source.voiced.load (std::memory_order_relaxed);
    }
    return static_cast<int> (count);
}

int VibeAutotuneProcessor::copyLiveDiagnosticFrames (SignalChainDiagnosticFrame* destination,
                                                      int maximumFrames) const noexcept
{
    if (destination == nullptr || maximumFrames <= 0 || liveDiagnosticFrames == nullptr)
        return 0;

    const auto published = liveDiagnosticPublishedCount.load (std::memory_order_acquire);
    const auto available = juce::jmin (published, static_cast<uint64_t> (liveDiagnosticCapacity));
    const auto count = juce::jmin (available, static_cast<uint64_t> (maximumFrames));
    const auto first = published - count;

    for (uint64_t i = 0; i < count; ++i)
        destination[static_cast<size_t> (i)] = (*liveDiagnosticFrames)[static_cast<size_t> ((first + i)
            % static_cast<uint64_t> (liveDiagnosticCapacity))];

    return static_cast<int> (count);
}

int VibeAutotuneProcessor::copySignalChainDiagnosticFrames (SignalChainDiagnosticFrame* destination,
                                                             int maximumFrames) const noexcept
{
    if (destination == nullptr || maximumFrames <= 0)
        return 0;

    const auto count = juce::jlimit (0, maximumFrames, signalChainDiagnosticFrameCount);
    std::copy_n (signalChainDiagnosticFrames.data(), count, destination);
    return count;
}

int VibeAutotuneProcessor::copyControlMaterializationEvents (ControlMaterializationEvent* destination,
                                                              int maximumEvents) const noexcept
{
    if (destination == nullptr || maximumEvents <= 0)
        return 0;

    const auto available = juce::jmin (controlMaterializationEventCount,
                                       static_cast<uint64_t> (controlMaterializationEventCapacity));
    const auto count = juce::jmin (available, static_cast<uint64_t> (maximumEvents));
    const auto first = controlMaterializationEventCount - count;
    for (uint64_t index = 0; index < count; ++index)
        destination[static_cast<size_t> (index)] = controlMaterializationEvents[static_cast<size_t> (
            (first + index) % static_cast<uint64_t> (controlMaterializationEventCapacity))];
    return static_cast<int> (count);
}

int VibeAutotuneProcessor::copyRendererControlLookupTraces (RendererControlLookupTrace* destination,
                                                             int maximumTraces) const noexcept
{
    if (destination == nullptr || maximumTraces <= 0)
        return 0;

    const auto count = juce::jlimit (0, maximumTraces, rendererControlLookupTraceCount);
    std::copy_n (rendererControlLookupTraces.data(), count, destination);
    return count;
}

void VibeAutotuneProcessor::setPitchAnalysisProfilingEnabled (bool shouldEnable) noexcept
{
    pitchAnalysisProfilingEnabled.store (shouldEnable, std::memory_order_relaxed);
    profiledAnalysisCount = 0;
    profiledFifoFrameTicks = 0;
    profiledDifferenceTicks = 0;
    profiledCmndfTicks = 0;
    profiledCandidateSearchTicks = 0;
    profiledParabolicTicks = 0;
    profiledAutocorrelationTicks = 0;
    profiledVoicingTicks = 0;
    profiledOctaveTicks = 0;
    profiledSmoothingTicks = 0;
    profiledTotalTicks = 0;
}

VibeAutotuneProcessor::PitchAnalysisPerformanceData VibeAutotuneProcessor::getPitchAnalysisPerformanceData() const noexcept
{
    const auto ticksPerSecond = static_cast<double> (juce::Time::getHighResolutionTicksPerSecond());
    const auto toMicroseconds = [ticksPerSecond] (uint64_t ticks, uint64_t count)
    {
        return count == 0 ? 0.0 : static_cast<double> (ticks) * 1.0e6 / ticksPerSecond / static_cast<double> (count);
    };

    PitchAnalysisPerformanceData data;
    data.analysisCount = profiledAnalysisCount;
    data.fifoFramePreparationMicroseconds = toMicroseconds (profiledFifoFrameTicks, profiledAnalysisCount);
    data.differenceFunctionMicroseconds = toMicroseconds (profiledDifferenceTicks, profiledAnalysisCount);
    data.cmndfMicroseconds = toMicroseconds (profiledCmndfTicks, profiledAnalysisCount);
    data.candidateSearchMicroseconds = toMicroseconds (profiledCandidateSearchTicks, profiledAnalysisCount);
    data.parabolicInterpolationMicroseconds = toMicroseconds (profiledParabolicTicks, profiledAnalysisCount);
    data.normalizedAutocorrelationMicroseconds = toMicroseconds (profiledAutocorrelationTicks, profiledAnalysisCount);
    data.voicingFeaturesMicroseconds = toMicroseconds (profiledVoicingTicks, profiledAnalysisCount);
    data.octaveScoringMicroseconds = toMicroseconds (profiledOctaveTicks, profiledAnalysisCount);
    data.smoothingMicroseconds = toMicroseconds (profiledSmoothingTicks, profiledAnalysisCount);
    data.totalMicroseconds = toMicroseconds (profiledTotalTicks, profiledAnalysisCount);
    return data;
}

juce::String VibeAutotuneProcessor::getLatestDetectedNoteName() const
{
    return getPitchMonitorData().detectedNote;
}

int VibeAutotuneProcessor::getLatestDetectedMidiNote() const noexcept
{
    return latestDetectedMidiNote.load (std::memory_order_relaxed);
}

int VibeAutotuneProcessor::getLatestTargetMidiNote() const noexcept
{
    return latestTargetMidiNote.load (std::memory_order_relaxed);
}

uint16_t VibeAutotuneProcessor::getEnabledNoteMask() const noexcept
{
    return enabledNoteMask.load (std::memory_order_relaxed);
}

void VibeAutotuneProcessor::setEnabledNoteMask (uint16_t newMask) noexcept
{
    const auto mask = newMask == 0 ? static_cast<uint16_t> (1u) : static_cast<uint16_t> (newMask & 0x0fffu);
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        setNoteRuleForPitchClass (pitchClass, (mask & static_cast<uint16_t> (1u << pitchClass)) != 0
                                                  ? NoteRule::Legal : NoteRule::IllegalNearest);
    baseScaleMask.store (mask, std::memory_order_relaxed);
}

uint16_t VibeAutotuneProcessor::getBaseScaleMask() const noexcept
{
    return baseScaleMask.load (std::memory_order_relaxed);
}

bool VibeAutotuneProcessor::hasCustomNotePolicy() const noexcept
{
    const auto currentScale = static_cast<int> (*parameters.getRawParameterValue ("scale"));
    if (currentScale == userScaleIndex)
        return true;

    const auto baseMask = getBaseScaleMask();
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
    {
        const auto defaultRule = (baseMask & static_cast<uint16_t> (1u << pitchClass)) != 0
                                   ? NoteRule::Legal : NoteRule::IllegalNearest;
        if (getNoteRuleForPitchClass (pitchClass) != defaultRule)
            return true;
    }

    if (! getGroupOctaves())
        for (int midiNote = 0; midiNote < 128; ++midiNote)
            if (getNoteRuleForMidiNote (midiNote) != getNoteRuleForPitchClass (midiNote % 12))
                return true;

    return false;
}

NoteRule VibeAutotuneProcessor::getNoteRuleForPitchClass (int pitchClass) const noexcept
{
    const auto safePitchClass = (pitchClass % 12 + 12) % 12;
    return noteRuleFromValue (pitchClassDefaultPolicy[static_cast<size_t> (safePitchClass)].load (std::memory_order_relaxed));
}

NoteRule VibeAutotuneProcessor::getNoteRuleForMidiNote (int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return NoteRule::IllegalNearest;
    const auto index = static_cast<size_t> (midiNote);
    if (midiNoteHasOverride[index].load (std::memory_order_relaxed))
        return noteRuleFromValue (midiNoteOverridePolicy[index].load (std::memory_order_relaxed));
    return getNoteRuleForPitchClass (midiNote % 12);
}

void VibeAutotuneProcessor::setNoteRuleForPitchClass (int pitchClass, NoteRule rule) noexcept
{
    const auto safePitchClass = (pitchClass % 12 + 12) % 12;
    pitchClassDefaultPolicy[static_cast<size_t> (safePitchClass)].store (noteRuleValue (rule), std::memory_order_relaxed);
    // A grouped edit changes every octave, so it also clears any local
    // exceptions for this pitch class.
    for (int midiNote = safePitchClass; midiNote <= 127; midiNote += 12)
        midiNoteHasOverride[static_cast<size_t> (midiNote)].store (false, std::memory_order_relaxed);

    auto mask = static_cast<uint16_t> (0);
    for (int note = 0; note < 12; ++note)
        if (isLegalNoteRule (getNoteRuleForPitchClass (note)))
            mask |= static_cast<uint16_t> (1u << note);
    enabledNoteMask.store (mask == 0 ? static_cast<uint16_t> (1u) : mask, std::memory_order_relaxed);
    notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
}

void VibeAutotuneProcessor::setNoteRuleForMidiNote (int midiNote, NoteRule rule) noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return;
    if (groupOctaves.load (std::memory_order_relaxed))
    {
        setNoteRuleForPitchClass (midiNote % 12, rule);
        return;
    }
    midiNoteOverridePolicy[static_cast<size_t> (midiNote)].store (noteRuleValue (rule), std::memory_order_relaxed);
    midiNoteHasOverride[static_cast<size_t> (midiNote)].store (true, std::memory_order_relaxed);
    notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
}

void VibeAutotuneProcessor::resetNoteRuleForPitchClass (int pitchClass) noexcept
{
    const auto safePitchClass = (pitchClass % 12 + 12) % 12;
    const auto defaultRule = (getBaseScaleMask() & static_cast<uint16_t> (1u << safePitchClass)) != 0
                           ? NoteRule::Legal : NoteRule::IllegalNearest;
    setNoteRuleForPitchClass (safePitchClass, defaultRule);
}

void VibeAutotuneProcessor::resetNoteRuleForMidiNote (int midiNote) noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return;

    if (groupOctaves.load (std::memory_order_relaxed))
    {
        resetNoteRuleForPitchClass (midiNote % 12);
        return;
    }

    midiNoteHasOverride[static_cast<size_t> (midiNote)].store (false, std::memory_order_relaxed);
    notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
}

void VibeAutotuneProcessor::resetNotePolicyToCurrentScale() noexcept
{
    const auto key = juce::jlimit (0, 11,
        static_cast<int> (parameters.getRawParameterValue ("key")->load (std::memory_order_relaxed)));
    const auto scale = juce::jlimit (0, userScaleIndex,
        static_cast<int> (parameters.getRawParameterValue ("scale")->load (std::memory_order_relaxed)));
    resetNotePolicyToScale (key, scale);
}

bool VibeAutotuneProcessor::getGroupOctaves() const noexcept
{
    return groupOctaves.load (std::memory_order_relaxed);
}

void VibeAutotuneProcessor::setGroupOctaves (bool shouldGroup) noexcept
{
    groupOctaves.store (shouldGroup, std::memory_order_relaxed);
    notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
}

juce::AudioProcessorParameter* VibeAutotuneProcessor::getBypassParameter() const
{
    return parameters.getParameter ("bypass");
}

void VibeAutotuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("enabledNoteMask", static_cast<int> (getEnabledNoteMask()), nullptr);
    state.setProperty ("baseScaleMask", static_cast<int> (getBaseScaleMask()), nullptr);
    state.setProperty ("groupOctaves", getGroupOctaves(), nullptr);
    juce::String defaults, overrides, overrideMask;
    for (int pitchClass = 0; pitchClass < 12; ++pitchClass)
        defaults += juce::String (static_cast<int> (getNoteRuleForPitchClass (pitchClass)));
    for (int midiNote = 0; midiNote <= 127; ++midiNote)
    {
        const auto index = static_cast<size_t> (midiNote);
        overrides += juce::String (static_cast<int> (noteRuleFromValue (
            midiNoteOverridePolicy[index].load (std::memory_order_relaxed))));
        overrideMask += midiNoteHasOverride[index].load (std::memory_order_relaxed) ? "1" : "0";
    }
    state.setProperty ("noteMapVersion", 2, nullptr);
    state.setProperty ("pitchClassDefaultPolicy", defaults, nullptr);
    state.setProperty ("midiNoteOverridePolicy", overrides, nullptr);
    state.setProperty ("midiNoteOverrideMask", overrideMask, nullptr);
    juce::MemoryOutputStream stream (destData, true);
    state.writeToStream (stream);
}

void VibeAutotuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (const auto state = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes)); state.isValid())
    {
        parameters.replaceState (state);
        if (state.hasProperty ("pitchClassDefaultPolicy"))
        {
            const auto defaults = state.getProperty ("pitchClassDefaultPolicy").toString();
            const auto overrides = state.getProperty ("midiNoteOverridePolicy").toString();
            const auto overrideMask = state.getProperty ("midiNoteOverrideMask").toString();
            setGroupOctaves (static_cast<bool> (state.getProperty ("groupOctaves", true)));
            for (int pitchClass = 0; pitchClass < 12 && pitchClass < defaults.length(); ++pitchClass)
                pitchClassDefaultPolicy[static_cast<size_t> (pitchClass)].store (
                    noteRuleValue (noteRuleFromValue (static_cast<uint8_t> (defaults[pitchClass] - '0'))), std::memory_order_relaxed);
            for (int midiNote = 0; midiNote <= 127; ++midiNote)
            {
                const auto index = static_cast<size_t> (midiNote);
                const auto hasOverride = midiNote < overrideMask.length() && overrideMask[midiNote] == '1';
                midiNoteHasOverride[index].store (hasOverride, std::memory_order_relaxed);
                if (midiNote < overrides.length())
                    midiNoteOverridePolicy[index].store (
                        noteRuleValue (noteRuleFromValue (static_cast<uint8_t> (overrides[midiNote] - '0'))), std::memory_order_relaxed);
            }
        }
        else if (state.hasProperty ("groupedNotePolicy"))
        {
            // Safe migration from the original flattened map.  Any MIDI rule
            // different from its pitch-class default becomes an override.
            const auto grouped = state.getProperty ("groupedNotePolicy").toString();
            const auto perMidi = state.getProperty ("perMidiNotePolicy").toString();
            setGroupOctaves (static_cast<bool> (state.getProperty ("groupOctaves", true)));
            for (int pitchClass = 0; pitchClass < 12 && pitchClass < grouped.length(); ++pitchClass)
                pitchClassDefaultPolicy[static_cast<size_t> (pitchClass)].store (
                    noteRuleValue (noteRuleFromValue (static_cast<uint8_t> (grouped[pitchClass] - '0'))), std::memory_order_relaxed);
            for (int midiNote = 0; midiNote <= 127; ++midiNote)
            {
                const auto index = static_cast<size_t> (midiNote);
                const auto defaultRule = getNoteRuleForPitchClass (midiNote % 12);
                const auto restoredRule = midiNote < perMidi.length()
                                        ? noteRuleFromValue (static_cast<uint8_t> (perMidi[midiNote] - '0')) : defaultRule;
                midiNoteOverridePolicy[index].store (noteRuleValue (restoredRule), std::memory_order_relaxed);
                midiNoteHasOverride[index].store (restoredRule != defaultRule, std::memory_order_relaxed);
            }
        }
        else if (state.hasProperty ("enabledNoteMask"))
            setEnabledNoteMask (static_cast<uint16_t> (static_cast<int> (state.getProperty ("enabledNoteMask"))));
        else
            resetNotePolicyToScale (static_cast<int> (parameters.getRawParameterValue ("key")->load()),
                                    static_cast<int> (parameters.getRawParameterValue ("scale")->load()));

        synchronisedPolicyKey = static_cast<int> (parameters.getRawParameterValue ("key")->load (std::memory_order_relaxed));
        synchronisedPolicyScale = static_cast<int> (parameters.getRawParameterValue ("scale")->load (std::memory_order_relaxed));
        notePolicyRevision.fetch_add (1, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* VibeAutotuneProcessor::createEditor()
{
    return new VibeAutotuneEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VibeAutotuneProcessor();
}
