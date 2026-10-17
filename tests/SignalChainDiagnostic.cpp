#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
int diagnosticBlockSize()
{
    const auto* requested = std::getenv ("VIBE_DIAGNOSTIC_BLOCK_SIZE");
    const auto value = requested != nullptr ? std::atoi (requested) : 256;
    return value > 0 ? value : 256;
}

std::vector<int> diagnosticBlockSequence()
{
    const auto* requested = std::getenv ("VIBE_DIAGNOSTIC_BLOCK_SEQUENCE");
    if (requested == nullptr || *requested == '\0')
        return {};

    std::vector<int> sequence;
    std::stringstream stream (requested);
    std::string token;
    while (std::getline (stream, token, ','))
    {
        const auto value = std::atoi (token.c_str());
        if (value <= 0)
            return {};
        sequence.push_back (value);
    }
    return sequence;
}

int diagnosticSourceSampleLimit (int64_t availableSamples)
{
    const auto* requested = std::getenv ("VIBE_DIAGNOSTIC_SOURCE_SAMPLE_LIMIT");
    if (requested == nullptr || *requested == '\0')
        return static_cast<int> (availableSamples);
    const auto value = std::strtoll (requested, nullptr, 10);
    return static_cast<int> (std::clamp<int64_t> (value, 0, availableSamples));
}
constexpr int rendererControlIntervalSamples = 128;

struct AudioMetrics
{
    double rms = 0.0;
    double peak = 0.0;
    double maximumStep = 0.0;
    int nonFiniteSamples = 0;
    int firstClippedSample = -1;
};

AudioMetrics measure (const juce::AudioBuffer<float>& audio)
{
    AudioMetrics metrics;
    auto sumSquares = 0.0;
    auto sampleCount = 0;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        const auto* samples = audio.getReadPointer (channel);
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto value = samples[sample];
            if (! std::isfinite (value))
            {
                ++metrics.nonFiniteSamples;
                continue;
            }
            sumSquares += value * value;
            metrics.peak = std::max (metrics.peak, std::abs (static_cast<double> (value)));
            if (metrics.firstClippedSample < 0 && std::abs (value) > 1.0f)
                metrics.firstClippedSample = sample;
            if (sample > 0)
                metrics.maximumStep = std::max (metrics.maximumStep,
                                                std::abs (static_cast<double> (value - samples[sample - 1])));
            ++sampleCount;
        }
    }
    metrics.rms = sampleCount > 0 ? std::sqrt (sumSquares / static_cast<double> (sampleCount)) : 0.0;
    return metrics;
}

// Offline-only output measurement. This is intentionally independent from
// the live YIN/control detector: it asks the rendered waveform itself which
// period is dominant around a control timestamp.
double estimateRenderedF0 (const juce::AudioBuffer<float>& audio, int centre, double sampleRate)
{
    constexpr int halfWindow = 1024;
    if (centre < halfWindow || centre + halfWindow >= audio.getNumSamples())
        return std::numeric_limits<double>::quiet_NaN();

    const auto* samples = audio.getReadPointer (0);
    std::array<double, 2 * halfWindow> frame {};
    auto mean = 0.0;
    for (int i = 0; i < 2 * halfWindow; ++i)
        mean += samples[centre - halfWindow + i];
    mean /= static_cast<double> (frame.size());

    auto energy = 0.0;
    for (int i = 0; i < 2 * halfWindow; ++i)
    {
        const auto phase = static_cast<double> (i) / static_cast<double> (frame.size() - 1);
        const auto value = (static_cast<double> (samples[centre - halfWindow + i]) - mean)
                         * (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * phase));
        frame[static_cast<size_t> (i)] = value;
        energy += value * value;
    }
    if (energy < 1.0e-8)
        return std::numeric_limits<double>::quiet_NaN();

    const auto minimumLag = static_cast<int> (std::floor (sampleRate / 700.0));
    const auto maximumLag = static_cast<int> (std::ceil (sampleRate / 110.0));
    auto bestLag = minimumLag;
    auto bestScore = -1.0;
    for (int lag = minimumLag; lag <= maximumLag; ++lag)
    {
        auto dot = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
        for (int i = lag; i < static_cast<int> (frame.size()); ++i)
        {
            const auto left = frame[static_cast<size_t> (i)];
            const auto right = frame[static_cast<size_t> (i - lag)];
            dot += left * right;
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        const auto score = dot / std::sqrt (leftEnergy * rightEnergy + 1.0e-12);
        if (score > bestScore) { bestScore = score; bestLag = lag; }
    }
    return bestScore >= 0.35 ? sampleRate / static_cast<double> (bestLag)
                             : std::numeric_limits<double>::quiet_NaN();
}

double estimateRenderedF0Yin (const juce::AudioBuffer<float>& audio, int centre, double sampleRate)
{
    constexpr int halfWindow = 1024;
    if (centre < halfWindow || centre + halfWindow >= audio.getNumSamples())
        return std::numeric_limits<double>::quiet_NaN();

    // YIN's cumulative mean normalized difference function is independent of
    // the normalized-autocorrelation measurement above. Unlike raw zero
    // crossings it remains valid on a formant-rich waveform with strong
    // harmonics, exactly the condition of this vocal benchmark.
    const auto* samples = audio.getReadPointer (0);
    constexpr int frameLength = 1024;
    const auto start = centre - frameLength / 2;
    std::array<double, frameLength> frame {};
    auto mean = 0.0;
    for (int i = 0; i < frameLength; ++i)
        mean += samples[start + i];
    mean /= static_cast<double> (frameLength);
    for (int i = 0; i < frameLength; ++i)
        frame[static_cast<size_t> (i)] = static_cast<double> (samples[start + i]) - mean;

    const auto minimumLag = static_cast<int> (std::floor (sampleRate / 700.0));
    const auto maximumLag = static_cast<int> (std::ceil (sampleRate / 110.0));
    std::array<double, 512> cmndf {};
    auto cumulativeDifference = 0.0;
    auto bestLag = minimumLag;
    auto bestValue = std::numeric_limits<double>::infinity();
    for (int lag = 1; lag <= maximumLag; ++lag)
    {
        auto difference = 0.0;
        for (int i = 0; i < frameLength - lag; ++i)
        {
            const auto delta = frame[static_cast<size_t> (i)] - frame[static_cast<size_t> (i + lag)];
            difference += delta * delta;
        }
        cumulativeDifference += difference;
        const auto value = lag == 0 ? 1.0 : difference * static_cast<double> (lag)
                                              / std::max (cumulativeDifference, 1.0e-12);
        cmndf[static_cast<size_t> (lag)] = value;
        if (lag >= minimumLag && value < bestValue)
        {
            bestValue = value;
            bestLag = lag;
        }
    }
    if (bestValue > 0.35)
        return std::numeric_limits<double>::quiet_NaN();
    auto refinedLag = static_cast<double> (bestLag);
    if (bestLag > minimumLag && bestLag < maximumLag)
    {
        const auto left = cmndf[static_cast<size_t> (bestLag - 1)];
        const auto centreValue = cmndf[static_cast<size_t> (bestLag)];
        const auto right = cmndf[static_cast<size_t> (bestLag + 1)];
        const auto denominator = left - 2.0 * centreValue + right;
        if (std::abs (denominator) > 1.0e-12)
            refinedLag += std::clamp (0.5 * (left - right) / denominator, -0.5, 0.5);
    }
    return sampleRate / refinedLag;
}

// Match IndependentPitchContour exactly: its causal 2048-sample analysis
// frame is also the extractor used for the captured Artist control grid.
// Do not substitute the shorter centred forensic probe above for scoring.
double estimateControlGridF0 (const juce::AudioBuffer<float>& audio, int endSample, double sampleRate)
{
    constexpr int frameLength = 2048;
    constexpr double minimumFrequencyHz = 94.0;
    constexpr double maximumFrequencyHz = 600.0;
    constexpr double threshold = 0.15;
    if (endSample < frameLength || endSample > audio.getNumSamples())
        return std::numeric_limits<double>::quiet_NaN();

    const auto* samples = audio.getReadPointer (0) + endSample - frameLength;
    const auto minimumLag = static_cast<int> (std::floor (sampleRate / maximumFrequencyHz));
    const auto maximumLag = static_cast<int> (std::ceil (sampleRate / minimumFrequencyHz));
    const auto usable = frameLength - maximumLag;
    auto energy = 0.0;
    for (int index = 0; index < usable; ++index)
        energy += static_cast<double> (samples[index]) * samples[index];
    if (energy / usable < 1.0e-7)
        return std::numeric_limits<double>::quiet_NaN();

    std::array<double, 512> difference {};
    for (int lag = 1; lag <= maximumLag; ++lag)
        for (int index = 0; index < usable; ++index)
        {
            const auto delta = static_cast<double> (samples[index]) - samples[index + lag];
            difference[static_cast<size_t> (lag)] += delta * delta;
        }

    auto cumulative = 0.0;
    auto bestValue = std::numeric_limits<double>::infinity();
    auto bestLag = minimumLag;
    auto chosenLag = -1;
    for (int lag = 1; lag <= maximumLag; ++lag)
    {
        cumulative += difference[static_cast<size_t> (lag)];
        if (lag < minimumLag || cumulative <= 1.0e-12)
            continue;
        const auto cmnd = difference[static_cast<size_t> (lag)] * lag / cumulative;
        if (cmnd < bestValue) { bestValue = cmnd; bestLag = lag; }
        if (cmnd < threshold && lag < maximumLag
            && difference[static_cast<size_t> (lag)] <= difference[static_cast<size_t> (lag + 1)])
        { chosenLag = lag; break; }
    }
    const auto lag = chosenLag >= 0 ? chosenLag : bestLag;
    if (lag <= minimumLag || lag >= maximumLag)
        return std::numeric_limits<double>::quiet_NaN();
    const auto denominator = difference[static_cast<size_t> (lag - 1)]
                           - 2.0 * difference[static_cast<size_t> (lag)]
                           + difference[static_cast<size_t> (lag + 1)];
    const auto fractional = std::abs (denominator) > 1.0e-12
                          ? std::clamp (0.5 * (difference[static_cast<size_t> (lag - 1)]
                                             - difference[static_cast<size_t> (lag + 1)]) / denominator, -0.5, 0.5)
                          : 0.0;
    return sampleRate / (lag + fractional);
}

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        stream != nullptr ? wav.createWriterFor (stream.get(), sampleRate, audio.getNumChannels(), 24, {}, 0) : nullptr);
    if (writer == nullptr)
        return false;
    stream.release();
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return true;
}

void writeMetric (std::ofstream& metrics, const char* name, const AudioMetrics& value,
                  int latencySamples, const char* note)
{
    metrics << name << ',' << value.rms << ',' << value.peak << ',' << value.maximumStep << ','
            << value.nonFiniteSamples << ',' << value.firstClippedSample << ',' << latencySamples << ',' << note << '\n';
}

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    if (auto* parameter = processor.parameters.getParameter (id))
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

bool loadNaturalVibratoSurface (const juce::File& file,
                                CorrectionTrajectory::NaturalVibratoSurface& surface)
{
    std::ifstream stream (file.getFullPathName().toStdString());
    if (! stream)
        return false;

    std::vector<double> values;
    std::string token;
    while (std::getline (stream, token, ','))
    {
        std::stringstream parser (token);
        double value = 0.0;
        while (parser >> value)
            values.push_back (value);
    }
    if (values.size() != 21)
        return false;

    size_t index = 0;
    for (auto& row : surface)
        for (auto& value : row)
            value = values[index++];
    return true;
}

void writeHeader (std::ofstream& csv)
{
    csv << "sampleIndex,timeMs,voiceRangeMin,voiceRangeMax,trackingValue,retuneSpeedMs,flexTunePercent,humanizePercent,naturalVibratoPercent,voiced,pitchConfidence,rawF0,stableF0,rawMidi,key,scale,committedTargetMidi,targetChanged,flexWeight,desiredCorrectionCents,retuneCorrectionCents,humanizeWeight,vibratoContributionCents,rawVibratoResidualCents,vibratoEnergyCents,vibratoDeviationCents,pitchNoteCenterCents,pitchDriftCents,pitchVibratoRateHz,pitchVibratoConfidence,pitchNoteStability,pitchDriftConfidence,pitchJitterConfidence,requestedRendererRatio,rendererPreGuardCorrectionCents,rendererPostGuardCorrectionCents,rendererCorrectionSource,deliveredRendererCorrectionCents,rendererInputCorrectionCents,rendererStepCents,correctionIsActive,targetBridge,transientBridge,dryWetMix,transpose,referenceHz,currentTargetCents,candidateTargetCents,trajectoryTransitionActive,trajectoryTransitionProgress,trajectoryTransitionStartCents,trajectoryTransitionEndCents,trajectoryTransitionDurationMs,trajectoryRapidAdjacentContour,trajectoryRendererCrossingActive,trajectoryRendererCrossingTargetMidi,trajectoryRendererCrossingProgress,trajectoryExcursionActive,trajectoryExcursionPhase,trajectoryExcursionConsumed,trajectoryExcursionOwnsControl,targetDecisionMidpointCents,targetDecisionExcessCents,targetHysteresisCents,pendingTargetMidi,pendingTargetFrames,targetDecisionBeyondHysteresis,rapidAdjacentRawCandidate,rapidAdjacentPrearmed,rapidAdjacentCandidateMidi,rapidAdjacentEvidenceFrames,rapidAdjacentRawDistanceCents,rapidLegatoAdjacentStep,rapidLegatoHardLock,rapidLegatoTargetChanged,rapidLegatoContinuesDirection,rapidLegatoChainStarts,rapidLegatoChainActive,rapidLegatoCommittedSteps,rapidLegatoSamplesSinceCommit,rapidLegatoDistanceCents,rapidLegatoInputMotionCentsPerSecond,trajectoryPreviewNote,trajectoryPreviewDirection,trajectoryPreviewUpdates,trajectoryPreviewTravelCents,instantaneousInputMotionCentsPerSecond,selectorCurrentTargetBefore,selectorPreviousTarget,selectorCandidate,selectorFramesSinceCommit,selectorDecision,trajectoryPortamentoActive,trajectoryUsesPortamentoTarget,latticeReset,dryGuardActive,reentryHoldActive,sourceControlTimestamp,outputRendererControlTimestamp,outputRendererControlPitchRatio,outputRendererControlTargetMidi,sourceControlF0Hz,sourceControlTargetCents,sourceControlTargetMidi,sourceControlValid,sourceControlHoldDry,sourceControlPitchRatio,sourceControlTrajectoryExcursionActive,sourceControlTrajectoryExcursionPhase,sourceControlTrajectoryExcursionOwnsControl,rendererStrongRatioStep,rendererRapidCorrectionReversal,rendererTargetCommitted,sourceCursorBeforeStrongStep,sourceCursorAfterStrongStep,sourceCursorReanchorSuppressed,fractionalPhaseOffsetAtSpawn,overlapPhaseOffsetAtSpawn,olaOverlapWeight,olaAudibleRatioAtStart,olaAudibleGrainAge,olaAudibleReadPosition,olaDominantGrainWeight,olaDominantGrainEpoch,olaDominantGrainRatio,olaDominantGrainExpectedF0,olaDominantGrainTargetMidi,olaDominantGrainTargetEventId,olaDominantGrainTargetControlTimestamp,olaTargetCandidateWeight,olaTargetCandidateEpoch,olaTargetCandidateExpectedF0,olaShadowFutureSegments,olaShadowAudibleSegments,olaShadowFutureSamples,olaShadowReplacementEligible,olaShadowReplacementFirstSample,olaShadowReplacementLastSample,olaShadowCandidateRenderApplied,nextEpochBeforeReversal,nextEpochAfterReversal,actualSpawnEpoch,ratioAtSpawn,spawnTargetMidi,spawnTargetEventId,spawnTargetControlTimestamp,spawnSourceMarkF0,spawnExpectedF0,olaSourceMarkTime,olaSourceMarkF0,olaExpectedGrainF0,synthExpectedPeriod,synthActualPeriod,synthPeriodError,synthSourceSpacing,sourceSelectionCursor,newestCompleteSourceMark,sibilanceResidualMix,rendererHighFrequencyEvidence,rendererPeriodicCoherence,pitchMarkCandidateOffset,pitchMarkCandidateCorrelation,confirmedPitchMarkOffset,confirmedPitchMarkRun,rendererArtifactDryVeto,rendererGrainStarved,rendererShouldRenderWet,rendererSafetyDrySamples\n";
    // The main header above predates trajectory-only renderer provenance.
    // Replace its trailing newline so these two forensic columns remain part
    // of the same CSV record without perturbing existing column positions.
    csv.seekp (-1, std::ios_base::end);
    csv << ",olaDominantGrainTrajectoryExcursionId,spawnTrajectoryExcursionId"
        << ",callerControlLookupTime,rendererControlLookupTime,controlLookupDeltaSamples"
        << ",callerControlTimestamp,rendererControlTimestamp,callerControlEventId,rendererControlEventId"
        << ",callerControlPitchRatio,rendererControlPitchRatio"
        << ",olaDominantGrainControlEventId,olaNewestTargetGrainWeight,olaPreviousTargetGrainWeight"
        << ",olaNewestTargetFirstAudibleSample,olaPreviousTargetLastAudibleSample"
        << ",sourceControlMaterializationAdvanceSamples,sourceControlCorrectionBefore,sourceControlCorrectionAfter"
        << ",grainDecisionInputSample,grainFirstAudibleSample,grainHalfWindowAtSpawn,grainSourceMarkAtSpawn\n";
}

void writeFrame (std::ofstream& csv, const VibeAutotuneProcessor::SignalChainDiagnosticFrame& f, double sampleRate)
{
    const auto correctionSource = f.rendererCorrectionSource == 0 ? "NORMAL_DESIRED"
                                : f.rendererCorrectionSource == 1 ? "INVALID_CONTROL"
                                                                  : "DRY_GUARD";
    csv << f.sampleIndex << ',' << (1000.0 * static_cast<double> (f.sampleIndex) / sampleRate) << ','
        << f.voiceRangeMinHz << ',' << f.voiceRangeMaxHz << ',' << f.trackingValue << ','
        << f.retuneSpeedMs << ',' << f.flexTunePercent << ',' << f.humanizePercent << ','
        << f.naturalVibratoPercent << ',' << f.voiced << ','
        << f.pitchConfidence << ',' << f.rawF0Hz << ',' << f.stableF0Hz << ',' << f.rawMidi << ',' << f.key << ',' << f.scale << ','
        << f.committedTargetMidi << ',' << f.targetChanged << ',' << f.flexCorrectionGain << ','
        << f.desiredCorrectionCents << ',' << f.retuneCorrectionCents << ',' << f.humanizeWeight << ','
        << f.vibratoContributionCents << ',' << f.rawVibratoResidualCents << ','
        << f.vibratoEnergyCents << ',' << f.vibratoDeviationCents << ','
        << f.pitchNoteCenterCents << ',' << f.pitchDriftCents << ','
        << f.pitchVibratoRateHz << ',' << f.pitchVibratoConfidence << ',' << f.pitchNoteStability << ','
        << f.pitchDriftConfidence << ',' << f.pitchJitterConfidence << ','
        << f.requestedRendererRatio << ','
        << f.rendererPreGuardCorrectionCents << ',' << f.rendererPostGuardCorrectionCents << ',' << correctionSource << ','
        << f.deliveredRendererCorrectionCents << ',' << f.rendererInputCorrectionCents << ',' << f.rendererStepCents << ',' << f.correctionIsActive << ','
        << f.targetBridge << ',' << f.transientBridge << ',' << f.dryWetMix << ','
        << f.transpose << ',' << f.referenceHz << ',' << f.targetPitchCents << ',' << f.candidateTargetCents << ','
        << f.trajectoryTransitionActive << ',' << f.trajectoryTransitionProgress << ','
        << f.trajectoryTransitionStartCents << ',' << f.trajectoryTransitionEndCents << ','
        << f.trajectoryTransitionDurationMs << ',' << f.trajectoryRapidAdjacentContour << ','
        << f.trajectoryRendererCrossingActive << ','
        << f.trajectoryRendererCrossingTargetMidi << ',' << f.trajectoryRendererCrossingProgress << ','
        << f.trajectoryExcursionActive << ',' << static_cast<int> (f.trajectoryExcursionPhase) << ','
        << f.trajectoryExcursionConsumed << ',' << f.trajectoryExcursionOwnsControl << ','
        << f.targetDecisionMidpointCents << ',' << f.targetDecisionExcessCents << ','
        << f.targetHysteresisCents << ',' << f.pendingTargetMidi << ',' << f.pendingTargetFrames << ','
        << f.targetDecisionBeyondHysteresis << ',' << f.rapidAdjacentRawCandidate << ','
        << f.rapidAdjacentPrearmed << ',' << f.rapidAdjacentCandidateMidi << ','
        << f.rapidAdjacentEvidenceFrames << ',' << f.rapidAdjacentRawDistanceCents << ','
        << f.rapidLegatoAdjacentStep << ',' << f.rapidLegatoHardLock << ',' << f.rapidLegatoTargetChanged << ','
        << f.rapidLegatoContinuesDirection << ','
        << f.rapidLegatoChainStarts << ',' << f.rapidLegatoChainActive << ','
        << f.rapidLegatoCommittedSteps << ',' << f.rapidLegatoSamplesSinceCommit << ','
        << f.rapidLegatoDistanceCents << ',' << f.rapidLegatoInputMotionCentsPerSecond << ','
        << f.trajectoryPreviewNote << ',' << f.trajectoryPreviewDirection << ','
        << f.trajectoryPreviewUpdates << ',' << f.trajectoryPreviewTravelCents << ','
        << f.instantaneousInputMotionCentsPerSecond << ','
        << f.selectorCurrentTargetBefore << ',' << f.selectorPreviousTarget << ','
        << f.selectorCandidate << ',' << f.selectorFramesSinceCommit << ',' << f.selectorDecision << ','
        << f.trajectoryPortamentoActive << ','
        << f.trajectoryUsesPortamentoTarget << ','
        << f.latticeReset << ',' << f.dryGuardActive << ',' << f.reentryHoldActive << ','
        << f.sourceControlTimestamp << ',' << f.outputRendererControlTimestamp << ','
        << f.outputRendererControlPitchRatio << ',' << f.outputRendererControlTargetMidi << ','
        << f.sourceControlF0Hz << ',' << f.sourceControlTargetCents << ','
        << f.sourceControlTargetMidi << ',' << f.sourceControlValid << ',' << f.sourceControlHoldDry << ',' << f.sourceControlPitchRatio << ','
        << f.sourceControlTrajectoryExcursionActive << ',' << static_cast<int> (f.sourceControlTrajectoryExcursionPhase) << ','
        << f.sourceControlTrajectoryExcursionOwnsControl << ','
        << f.rendererStrongRatioStep << ',' << f.rendererRapidCorrectionReversal << ',' << f.rendererTargetCommitted << ','
        << f.sourceCursorBeforeStrongStep << ',' << f.sourceCursorAfterStrongStep << ','
        << f.sourceCursorReanchorSuppressed << ',' << f.fractionalPhaseOffsetAtSpawn << ',' << f.overlapPhaseOffsetAtSpawn << ','
        << f.olaOverlapWeight << ',' << f.olaAudibleRatioAtStart << ',' << f.olaAudibleGrainAge << ','
        << f.olaAudibleReadPosition << ',' << f.olaDominantGrainWeight << ','
        << f.olaDominantGrainEpoch << ',' << f.olaDominantGrainRatio << ','
        << f.olaDominantGrainExpectedF0 << ',' << f.olaDominantGrainTargetMidi << ','
        << f.olaDominantGrainTargetEventId << ',' << f.olaDominantGrainTargetControlTimestamp << ','
        << f.olaTargetCandidateWeight << ','
        << f.olaTargetCandidateEpoch << ',' << f.olaTargetCandidateExpectedF0 << ','
        << f.olaShadowFutureSegments << ',' << f.olaShadowAudibleSegments << ',' << f.olaShadowFutureSamples << ','
        << f.olaShadowReplacementEligible << ',' << f.olaShadowReplacementFirstSample << ','
        << f.olaShadowReplacementLastSample << ',' << f.olaShadowCandidateRenderApplied << ','
        << f.nextEpochBeforeReversal << ','
        << f.nextEpochAfterReversal << ',' << f.actualSpawnEpoch << ',' << f.ratioAtSpawn << ','
        << f.spawnTargetMidi << ',' << f.spawnTargetEventId << ',' << f.spawnTargetControlTimestamp << ','
        << f.spawnSourceMarkF0 << ',' << f.spawnExpectedF0 << ','
        << f.olaSourceMarkTime << ',' << f.olaSourceMarkF0 << ',' << f.olaExpectedGrainF0 << ','
        << f.synthExpectedPeriod << ',' << f.synthActualPeriod << ',' << f.synthPeriodError << ',' << f.synthSourceSpacing << ','
        << f.sourceSelectionCursor << ',' << f.newestCompleteSourceMark << ',' << f.sibilanceResidualMix << ','
        << f.rendererHighFrequencyEvidence << ',' << f.rendererPeriodicCoherence << ','
        << f.pitchMarkCandidateOffset << ',' << f.pitchMarkCandidateCorrelation << ','
        << f.confirmedPitchMarkOffset << ',' << f.confirmedPitchMarkRun << ','
        << f.rendererArtifactDryVeto << ',' << f.rendererGrainStarved << ',' << f.rendererShouldRenderWet << ',' << f.rendererSafetyDrySamples << ','
        << f.olaDominantGrainTrajectoryExcursionId << ',' << f.spawnTrajectoryExcursionId << ','
        << f.callerControlLookupTime << ',' << f.rendererControlLookupTime << ',' << f.controlLookupDeltaSamples << ','
        << f.callerControlTimestamp << ',' << f.rendererControlTimestamp << ','
        << f.callerControlEventId << ',' << f.rendererControlEventId << ','
        << f.callerControlPitchRatio << ',' << f.rendererControlPitchRatio << ','
        << f.olaDominantGrainControlEventId << ','
        << f.olaNewestTargetGrainWeight << ',' << f.olaPreviousTargetGrainWeight << ','
        << f.olaNewestTargetFirstAudibleSample << ',' << f.olaPreviousTargetLastAudibleSample << ','
        << f.sourceControlMaterializationAdvanceSamples << ','
        << f.sourceControlCorrectionBefore << ',' << f.sourceControlCorrectionAfter << ','
        << f.grainDecisionInputSample << ',' << f.grainFirstAudibleSample << ','
        << f.grainHalfWindowAtSpawn << ',' << f.grainSourceMarkAtSpawn << '\n';
}
}

int main (int argc, char* argv[])
{
    if (argc != 3 && argc != 5 && argc != 7 && argc != 8 && argc != 9 && argc != 10 && argc != 11 && argc != 12 && argc != 13 && argc != 14 && argc != 15 && argc != 16 && argc != 19 && argc != 20 && argc != 21 && argc != 22 && argc != 25 && argc != 26 && argc != 31 && argc != 32 && argc != 33 && argc != 34 && argc != 35 && argc != 36)
    {
        std::cerr << "usage: VibeAutotuneSignalChainDiagnostic <input-wav> <output-directory> [... existing args ... tail-replacement onset-H fade-H old-tail-gain minimum-step-cents]\n";
        return 2;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File inputFile (argv[1]);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inputFile));
    if (reader == nullptr)
    {
        std::cerr << "unable to read input wav\n";
        return 2;
    }

    const juce::File outputDirectory (argv[2]);
    outputDirectory.createDirectory();
    const auto classicEnabled = argc >= 5 ? std::atoi (argv[3]) != 0 : false;
    const auto formantEnabled = argc >= 5 ? std::atoi (argv[4]) != 0 : true;
    const auto retuneMs = argc >= 7 ? std::strtof (argv[5], nullptr) : 25.0f;
    const auto flexTunePercent = argc >= 7 ? std::strtof (argv[6], nullptr) : 30.0f;
    const auto waveformAnchoredSourceMarks = argc >= 8 && std::atoi (argv[7]) != 0;
    const auto voicedColourPercent = argc >= 9 ? std::strtof (argv[8], nullptr) : 0.0f;
    // Omit this optional argument to exercise the production default. Passing
    // 0 explicitly selects the retained TD-PSOLA regression path.
    const auto phaseContinuousRenderer = argc >= 10 ? std::atoi (argv[9]) != 0 : true;
    const auto phaseForceUnityRatio = argc >= 11 && std::atoi (argv[10]) != 0;
    const auto phaseForceStaticUpRatio = argc >= 12 && std::atoi (argv[11]) != 0;
    const auto humanizePercent = argc >= 13 ? std::strtof (argv[12], nullptr) : 0.0f;
    const auto naturalVibratoPercent = argc >= 14 ? std::strtof (argv[13], nullptr) : 0.0f;
    const juce::File naturalSurfaceFile (argc >= 15 ? argv[14] : "");
    const auto negativeNaturalPhaseScale = argc >= 16 ? std::strtod (argv[15], nullptr) : 1.0;
    const auto pitchCentreSeconds = argc >= 19 ? std::strtod (argv[16], nullptr) : 0.250;
    const auto wideVibratoNeutralRetention = argc >= 19 ? std::strtod (argv[17], nullptr)
                                                         : VIBE_WIDE_VIBRATO_NEUTRAL_RETENTION;
    const auto wideVibratoHumanizedRetention = argc >= 19 ? std::strtod (argv[18], nullptr)
                                                           : VIBE_WIDE_VIBRATO_HUMANIZED_RETENTION;
    const auto rapidLegatoReturnPreview = argc >= 20 && std::atoi (argv[19]) != 0;
    const auto continuousRapidReturnExcursion = argc >= 21 && std::atoi (argv[20]) != 0;
    const auto preserveSourceCursorDuringTrajectoryExcursion = argc >= 22 && std::atoi (argv[21]) != 0;
    const auto excursionMinimumCoherentFrames = argc >= 25 ? std::atoi (argv[22]) : 4;
    const auto excursionMinimumTravelCents = argc >= 25 ? std::strtod (argv[23], nullptr) : 11.0;
    const auto excursionHoldMilliseconds = argc >= 25 ? std::strtod (argv[24], nullptr) : 50.0;
    const auto slowRetuneInputReferenceAmountOverride = argc >= 26 ? std::strtod (argv[25], nullptr) : -1.0;
    const auto targetAwareTailReplacement = argc >= 31 && std::atoi (argv[26]) != 0;
    const auto targetAwareTailOnsetHalfWindows = argc >= 31 ? std::strtod (argv[27], nullptr) : 0.0;
    const auto targetAwareTailFadeHalfWindows = argc >= 31 ? std::strtod (argv[28], nullptr) : 0.25;
    const auto targetAwareOldTailGain = argc >= 31 ? std::strtod (argv[29], nullptr) : 0.5;
    const auto targetAwareMinimumStepCents = argc >= 31 ? std::strtod (argv[30], nullptr) : 50.0;
    const auto targetAwareNewGrainHammingBlend = argc >= 32 ? std::strtod (argv[31], nullptr) : 0.25;
    const auto targetAwareWindowHalfFactorOverride = argc >= 33 ? std::strtod (argv[32], nullptr) : -1.0;
    const auto trajectoryExcursionTailReplacement = argc >= 34 && std::atoi (argv[33]) != 0;
    const auto cadenceOwnedExcursionRatio = argc >= 35 && std::atoi (argv[34]) != 0;
    const auto excursionDrivesSynthesisCadence = argc >= 36 && std::atoi (argv[35]) != 0;
    const auto pendingGrainQueue = []
    {
        const auto* value = std::getenv ("VIBE_PENDING_GRAIN_QUEUE");
        return value == nullptr || std::atoi (value) != 0;
    }();
    const auto pendingQueueAllFutureAudible = []
    {
        const auto* value = std::getenv ("VIBE_PENDING_QUEUE_ALL_FUTURE_AUDIBLE");
        return value != nullptr && std::atoi (value) != 0;
    }();
    const auto retimableEpochs = []
    {
        const auto* value = std::getenv ("VIBE_RETIMABLE_EPOCHS");
        // Match the production default; `0` remains an explicit A/B rollback.
        return value == nullptr || std::atoi (value) != 0;
    }();
    const auto rapidAdjacentTargetSelection = []
    {
        const auto* value = std::getenv ("VIBE_RAPID_ADJACENT_SELECTOR");
        return value != nullptr && std::atoi (value) != 0;
    }();
    const auto diagnosticWideTransitionMs = []
    {
        const auto* value = std::getenv ("VIBE_DIAGNOSTIC_WIDE_TRANSITION_MS");
        return value != nullptr ? std::strtod (value, nullptr) : -1.0;
    }();
    const auto diagnosticWideTransitionShape = []
    {
        const auto* value = std::getenv ("VIBE_DIAGNOSTIC_WIDE_TRANSITION_SHAPE");
        return value != nullptr ? std::atoi (value) : 0;
    }();
    const auto diagnosticWideTransitionDownMs = [&]
    {
        const auto* value = std::getenv ("VIBE_DIAGNOSTIC_WIDE_TRANSITION_DOWN_MS");
        return value != nullptr ? std::strtod (value, nullptr) : diagnosticWideTransitionMs;
    }();
    const auto epochTraceEnabled = []
    {
        const auto* value = std::getenv ("VIBE_EPOCH_TRACE");
        return value != nullptr && std::atoi (value) != 0;
    }();
    const auto olaTraceStart = []
    {
        const auto* value = std::getenv ("VIBE_OLA_TRACE_START");
        return value != nullptr ? static_cast<uint64_t> (std::strtoull (value, nullptr, 10)) : uint64_t { 1 };
    }();
    const auto olaTraceEnd = []
    {
        const auto* value = std::getenv ("VIBE_OLA_TRACE_END");
        return value != nullptr ? static_cast<uint64_t> (std::strtoull (value, nullptr, 10)) : uint64_t { 0 };
    }();
    std::ofstream metrics (outputDirectory.getChildFile ("case_metrics.csv").getFullPathName().toStdString());
    metrics << "case,rms,peak,maxSampleStep,nonFiniteSamples,firstClippedSample,latencySamples,note\n";
    std::ofstream csv (outputDirectory.getChildFile ("full_default_intervals.csv").getFullPathName().toStdString());
    std::ofstream events (outputDirectory.getChildFile ("full_default_events.csv").getFullPathName().toStdString());
    writeHeader (csv);
    events << "sampleIndex,timeMs,event,detail\n";

    const auto renderedFile = outputDirectory.getChildFile ("full_default_render.wav");
    std::unique_ptr<juce::OutputStream> renderedStream (renderedFile.createOutputStream());
    juce::WavAudioFormat wav;
    // Use PCM16 for the streamed diagnostic export. The legacy 24-bit packing
    // path can introduce a false spike in long exports; the in-memory metric
    // remains the authoritative safety measurement, while PCM16 is stable for
    // the independent contour/envelope comparisons performed by this tool.
    // Keep this diagnostic compatible with JUCE 7 hosts as well as JUCE 8.
    // The legacy overload transfers ownership of renderedStream to the writer.
    const juce::StringPairArray renderedMetadata;
    std::unique_ptr<juce::AudioFormatWriter> renderedWriter (
        renderedStream != nullptr ? wav.createWriterFor (renderedStream.release(), reader->sampleRate,
                                                          juce::AudioChannelSet::mono(), 16, renderedMetadata, 0) : nullptr);
    if (renderedWriter == nullptr)
    {
        std::cerr << "unable to create render wav\n";
        return 2;
    }

    const auto blockSequence = diagnosticBlockSequence();
    const auto blockSize = blockSequence.empty()
                         ? diagnosticBlockSize()
                         : *std::max_element (blockSequence.begin(), blockSequence.end());
    const auto sourceSampleLimit = static_cast<int64_t> (diagnosticSourceSampleLimit (reader->lengthInSamples));
    VibeAutotuneProcessor processor;
    if (argc >= 15 && naturalSurfaceFile.getFileName() != "-")
    {
        CorrectionTrajectory::NaturalVibratoSurface surface;
        if (! loadNaturalVibratoSurface (naturalSurfaceFile, surface))
        {
            std::cerr << "natural surface must contain exactly 21 comma-separated gains\n";
            return 2;
        }
        processor.setNaturalVibratoSurfaceDiagnostic (surface);
    }
    processor.setNegativeNaturalPhaseScaleDiagnostic (negativeNaturalPhaseScale);
    processor.setControlCalibrationDiagnostic (pitchCentreSeconds,
                                               wideVibratoNeutralRetention,
                                               wideVibratoHumanizedRetention);
    processor.prepareToPlay (reader->sampleRate, blockSize);
    processor.setHardTuneWideTransitionDiagnostic (diagnosticWideTransitionMs,
                                                   diagnosticWideTransitionDownMs,
                                                   diagnosticWideTransitionShape);
    processor.setRapidAdjacentTargetSelectionEnabled (rapidAdjacentTargetSelection);
    processor.setRendererEpochTraceEnabled (epochTraceEnabled);
    if (olaTraceStart <= olaTraceEnd)
        processor.setRendererOlaContributionTraceRange (olaTraceStart, olaTraceEnd);
    processor.setSignalChainDiagnosticEnabled (true);
    CausalTDPSOLARenderer::DiagnosticAblation ablation;
    ablation.waveformAnchoredSourceMarks = waveformAnchoredSourceMarks;
    ablation.phaseContinuousRenderer = phaseContinuousRenderer;
    ablation.phaseForceUnityRatio = phaseForceUnityRatio;
    ablation.phaseForceStaticUpRatio = phaseForceStaticUpRatio;
    ablation.rapidLegatoReturnPreview = rapidLegatoReturnPreview;
    ablation.continuousRapidReturnExcursion = continuousRapidReturnExcursion;
    ablation.preserveSourceCursorDuringTrajectoryExcursion = preserveSourceCursorDuringTrajectoryExcursion;
    ablation.continuousReturnMinimumCoherentFrames = excursionMinimumCoherentFrames;
    ablation.continuousReturnMinimumTravelCents = excursionMinimumTravelCents;
    ablation.continuousReturnHoldMilliseconds = excursionHoldMilliseconds;
    ablation.slowRetuneInputReferenceAmountOverride = slowRetuneInputReferenceAmountOverride;
    ablation.targetAwareTailReplacement = targetAwareTailReplacement;
    ablation.targetAwareTailOnsetHalfWindows = targetAwareTailOnsetHalfWindows;
    ablation.targetAwareTailFadeHalfWindows = targetAwareTailFadeHalfWindows;
    ablation.targetAwareOldTailGain = targetAwareOldTailGain;
    ablation.targetAwareMinimumStepCents = targetAwareMinimumStepCents;
    ablation.targetAwareNewGrainHammingBlend = targetAwareNewGrainHammingBlend;
    ablation.targetAwareWindowHalfFactorOverride = targetAwareWindowHalfFactorOverride;
    ablation.trajectoryExcursionTailReplacement = trajectoryExcursionTailReplacement;
    ablation.cadenceOwnedExcursionRatio = cadenceOwnedExcursionRatio;
    ablation.excursionDrivesSynthesisCadence = excursionDrivesSynthesisCadence;
    ablation.pendingGrainQueue = pendingGrainQueue;
    ablation.pendingQueueAllFutureAudible = pendingQueueAllFutureAudible;
    ablation.retimableEpochs = retimableEpochs;
    processor.setRendererDiagnosticAblation (ablation);
    // Explicitly state the shipped default preset for binding audit.
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "retuneSpeed", retuneMs);
    setParameter (processor, "flexTune", flexTunePercent);
    setParameter (processor, "humanize", humanizePercent);
    setParameter (processor, "naturalVibrato", naturalVibratoPercent);
    setParameter (processor, "voicedColour", voicedColourPercent);
    // Match the reference-performance page explicitly. Do not rely on the
    // processor's shipped defaults: a diagnostic must remain reproducible if
    // a future UI/default preset changes.
    setParameter (processor, "inputType", 1.0f); // Alto
    setParameter (processor, "key", 6.0f);       // F#
    setParameter (processor, "scale", 2.0f);     // Natural Minor
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);
    setParameter (processor, "tracking", 50.0f);
    juce::ignoreUnused (classicEnabled, formantEnabled);

    std::cout << "parameter_binding,ui/apvts/dsp,module\n"
              << "Voice,Alto/1/Alto,YIN profile\nKey,F#/6/6,target selector\nScale,Natural Minor/2/2,note policy\n"
              << "Transpose,0 st/0/0,trajectory\nReference,440 Hz/440/440,Hz-note mapping\nTracking,50%/50/50,YIN gate\n"
              << "Retune," << retuneMs << " ms/" << retuneMs << "/" << retuneMs << ",trajectory\n"
              << "Flex-Tune," << flexTunePercent << '/' << flexTunePercent << '/' << flexTunePercent << ",trajectory\n"
              << "Humanize," << humanizePercent << "%/" << humanizePercent << '/' << humanizePercent << ",trajectory\n"
              << "Natural Vibrato," << naturalVibratoPercent << " ms/" << naturalVibratoPercent << '/' << naturalVibratoPercent
              << ",trajectory\nGroup Octaves,state/state,state,note policy\nNote Map Rules,state/state,state,target selector\n";

    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), blockSize);
    // The direct diagnostic normally uses the mono vocal path. A StudioForge
    // VST3 insert, however, is hosted in a canonical stereo track buffer even
    // when its source WAV is mono. Make that topology selectable so that a
    // selector/renderer trace can be compared to the actual host without
    // changing the production processor or its signal path.
    const auto* stereoHostValue = std::getenv ("VIBE_DIAGNOSTIC_STEREO_HOST");
    const auto emulateStereoHost = stereoHostValue != nullptr
                                && std::atoi (stereoHostValue) != 0;
    const auto renderChannels = emulateStereoHost ? 2 : 1;
    juce::AudioBuffer<float> output (renderChannels, blockSize);
    // The production WAV is written as a stream. Retaining a second full
    // stereo copy is useful only for the short sample-level forensic trace;
    // on a long raw vocal it needlessly exhausts memory before the actual
    // listenable export is complete.
    const auto retainFullOutputForForensics = sourceSampleLimit <= static_cast<int64_t> (30 * reader->sampleRate);
    juce::AudioBuffer<float> fullDefaultOutput (1, retainFullOutputForForensics
                                                    ? static_cast<int> (sourceSampleLimit) : 0);
    juce::MidiBuffer midi;
    std::vector<VibeAutotuneProcessor::SignalChainDiagnosticFrame> frames (16);
    std::vector<VibeAutotuneProcessor::SignalChainDiagnosticFrame> allDiagnosticFrames;
    bool previousVoiced = false, previousActive = false, previousTargetBridge = false, previousTransientBridge = false;
    auto previousCommittedTarget = -2;
    double previousSpawnEpoch = -1.0;
    float previousWet = 0.0f;
    float previousCallbackOutput = 0.0f;
    bool hasPreviousCallbackOutput = false;
    double maximumCallbackBoundaryStep = 0.0;
    int64_t maximumCallbackBoundarySample = -1;

    auto sequenceIndex = size_t { 0 };
    for (int64_t position = 0; position < sourceSampleLimit;)
    {
        const auto requestedBlockSize = blockSequence.empty()
                                      ? blockSize
                                      : blockSequence[sequenceIndex++ % blockSequence.size()];
        const auto count = static_cast<int> (juce::jmin<int64_t> (requestedBlockSize, sourceSampleLimit - position));
        // The processor must see the actual final callback length.  Passing
        // the preallocated capacity here zero-pads EOS and can conceal a
        // retained 1..63 sample source-stage tail.
        source.setSize (static_cast<int> (reader->numChannels), count, false, false, true);
        output.setSize (renderChannels, count, false, false, true);
        source.clear(); output.clear();
        reader->read (&source, 0, count, position, true, true);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto input = source.getSample (0, sample);
            output.setSample (0, sample, input);
            if (renderChannels == 2)
                output.setSample (1, sample, input);
        }
        processor.processBlock (output, midi);
        if (count > 0 && hasPreviousCallbackOutput)
        {
            const auto boundaryStep = std::abs (static_cast<double> (output.getSample (0, 0)
                                                                      - previousCallbackOutput));
            if (boundaryStep > maximumCallbackBoundaryStep)
            {
                maximumCallbackBoundaryStep = boundaryStep;
                maximumCallbackBoundarySample = position;
            }
        }
        if (count > 0)
        {
            previousCallbackOutput = output.getSample (0, count - 1);
            hasPreviousCallbackOutput = true;
        }
        renderedWriter->writeFromAudioSampleBuffer (output, 0, count);
        if (retainFullOutputForForensics)
            fullDefaultOutput.copyFrom (0, static_cast<int> (position), output, 0, 0, count);
        const auto copied = processor.copySignalChainDiagnosticFrames (frames.data(), static_cast<int> (frames.size()));
        for (int i = 0; i < copied; ++i)
        {
            const auto& f = frames[static_cast<size_t> (i)];
            allDiagnosticFrames.push_back (f);
            writeFrame (csv, f, reader->sampleRate);
            const auto ms = 1000.0 * static_cast<double> (f.sampleIndex) / reader->sampleRate;
            if (f.voiced != previousVoiced) events << f.sampleIndex << ',' << ms << ",voiced," << f.voiced << '\n';
            // targetChanged is an analysis-frame field, so it remains visible
            // for several 128-sample renderer intervals. Record one commit
            // event only when the committed target itself actually changes.
            if (f.committedTargetMidi != previousCommittedTarget)
            {
                if (f.committedTargetMidi >= 0)
                    events << f.sampleIndex << ',' << ms << ",target_commit," << f.committedTargetMidi << '\n';
                previousCommittedTarget = f.committedTargetMidi;
            }
            // Grain-level handoff trace.  This is diagnostic-only: it makes
            // the exact grain that first carries a changed ratio visible,
            // together with the audible OLA state at that timestamp.
            if (f.actualSpawnEpoch >= 0.0 && f.actualSpawnEpoch != previousSpawnEpoch)
            {
                events << f.sampleIndex << ',' << ms << ",grain_spawn,"
                       << "epoch=" << f.actualSpawnEpoch
                       << ";ratio=" << f.ratioAtSpawn
                       << ";sourceF0=" << f.spawnSourceMarkF0
                       << ";expectedF0=" << f.spawnExpectedF0
                       << ";overlap=" << f.olaOverlapWeight
                       << ";audibleRatio=" << f.olaAudibleRatioAtStart
                       << ";age=" << f.olaAudibleGrainAge << '\n';
                previousSpawnEpoch = f.actualSpawnEpoch;
            }
            if (f.correctionIsActive != previousActive) events << f.sampleIndex << ',' << ms << ",correction_active," << f.correctionIsActive << '\n';
            if (f.targetBridge != previousTargetBridge) events << f.sampleIndex << ',' << ms << ",target_bridge," << f.targetBridge << '\n';
            if (f.transientBridge != previousTransientBridge) events << f.sampleIndex << ',' << ms << ",transient_bridge," << f.transientBridge << '\n';
            if (f.rendererStepCents > 10.0f) events << f.sampleIndex << ',' << ms << ",renderer_step_gt_10ct," << f.rendererStepCents << '\n';
            if ((previousWet == 0.0f) != (f.dryWetMix == 0.0f)) events << f.sampleIndex << ',' << ms << ",dry_wet_ramp," << f.dryWetMix << '\n';
            previousVoiced = f.voiced; previousActive = f.correctionIsActive;
            previousTargetBridge = f.targetBridge; previousTransientBridge = f.transientBridge; previousWet = f.dryWetMix;
        }
        position += count;
    }

    if (epochTraceEnabled)
    {
        std::array<CausalTDPSOLARenderer::EpochTrace, CausalTDPSOLARenderer::epochTraceCapacity> epochTraces {};
        const auto traceCount = processor.copyRendererEpochTraces (epochTraces.data(), static_cast<int> (epochTraces.size()));
        std::ofstream epochCsv (outputDirectory.getChildFile ("renderer_epoch_trace.csv").getFullPathName().toStdString());
        epochCsv << "sequence,decisionSourceSample,sourceScheduleSample,pendingSynthesisMarkBefore,pendingSynthesisMarkAfter,sourceMarkCursorBefore,sourceMarkCursorAfter,selectedMark,synthesisMark,halfWindow,firstAudible,controlTimestamp,controlPublicationSample,targetEventId,fallbackControlTimestamp,fallbackControlPublicationSample,fallbackTargetEventId,pendingCandidateId,sealSample,ticketIssueSample,pendingFinalizationSample,synthPhaseBefore,synthPhaseAfter,sourcePeriod,effectiveSynthesisPeriod,ratio,fallbackRatio,retimeSamples,deferred,retimed,finalized\n";
        for (int i = 0; i < traceCount; ++i)
        {
            const auto& t = epochTraces[static_cast<size_t> (i)];
            epochCsv << t.sequence << ',' << t.decisionSourceSample << ',' << t.sourceScheduleSample << ','
                     << t.pendingSynthesisMarkBefore << ',' << t.pendingSynthesisMarkAfter << ','
                     << t.sourceMarkCursorBefore << ',' << t.sourceMarkCursorAfter << ','
                     << t.selectedMark << ',' << t.synthesisMark << ',' << t.halfWindow << ',' << t.firstAudible << ','
                     << t.controlTimestamp << ',' << t.controlPublicationSample << ',' << t.targetEventId << ','
                     << t.fallbackControlTimestamp << ',' << t.fallbackControlPublicationSample << ',' << t.fallbackTargetEventId << ','
                     << t.pendingCandidateId << ',' << t.sealSample << ',' << t.ticketIssueSample << ',' << t.pendingFinalizationSample << ','
                     << t.synthPhaseBefore << ',' << t.synthPhaseAfter << ',' << t.sourcePeriod << ','
                     << t.effectiveSynthesisPeriod << ',' << t.ratio << ',' << t.fallbackRatio << ',' << t.retimeSamples << ','
                     << t.deferred << ',' << t.retimed << ',' << t.finalized << '\n';
        }
    }
    if (olaTraceStart <= olaTraceEnd)
    {
        std::vector<CausalTDPSOLARenderer::OlaContributionTrace> olaTraces (65536);
        const auto traceCount = processor.copyRendererOlaContributionTraces (olaTraces.data(), static_cast<int> (olaTraces.size()));
        std::ofstream olaCsv (outputDirectory.getChildFile ("renderer_ola_contribution_trace.csv").getFullPathName().toStdString());
        olaCsv << "grainId,targetEventId,controlTimestamp,correctionGeneration,synthesisMark,halfWindow,destinationSample,sourceMark,ratio,numeratorContribution,weightContribution\n";
        for (int i = 0; i < traceCount; ++i)
        {
            const auto& t = olaTraces[static_cast<size_t> (i)];
            olaCsv << t.grainId << ',' << t.targetEventId << ',' << t.controlTimestamp << ','
                   << t.correctionGeneration << ',' << t.synthesisMark << ',' << t.halfWindow << ','
                   << t.destinationSample << ',' << t.sourceMark << ',' << t.ratio << ','
                   << t.numeratorContribution << ',' << t.weightContribution << '\n';
        }
    }

    // This sidecar is a write-time trace, not an output-time sampling. It is
    // deliberately separate from full_default_intervals.csv so a reviewer
    // can see whether update(), advance(128), and push() were ordered as
    // expected at a reversal.
    std::vector<VibeAutotuneProcessor::ControlMaterializationEvent> materializations (
        static_cast<size_t> (VibeAutotuneProcessor::controlMaterializationEventCapacity));
    const auto materializationCount = processor.copyControlMaterializationEvents (
        materializations.data(), static_cast<int> (materializations.size()));
    std::ofstream materializationCsv (
        outputDirectory.getChildFile ("control_materialization_events.csv").getFullPathName().toStdString());
    materializationCsv << "sequence,timestampSamples,timeMs,targetEventId,targetEventTimestampSamples,publishedAtInputSample,publicationDelaySamples,hostBlockStart,hostBlockEnd,publishedToPsolaRing,advanceSamples,transitionStartCents,transitionTargetCents,transitionProgress,transitionDurationMs,correctionBeforeAdvance,correctionAfterAdvance,scheduledRatio,scheduledCorrectionCents\n";
    for (int index = 0; index < materializationCount; ++index)
    {
        const auto& event = materializations[static_cast<size_t> (index)];
        materializationCsv << event.sequence << ',' << event.timestampSamples << ','
                           << (1000.0 * static_cast<double> (event.timestampSamples) / reader->sampleRate) << ','
                           << event.targetEventId << ',' << event.targetEventTimestampSamples << ','
                           << event.publishedAtInputSample << ','
                           << (static_cast<int64_t> (event.publishedAtInputSample) - static_cast<int64_t> (event.timestampSamples)) << ','
                           << event.hostBlockStartSample << ',' << event.hostBlockEndSample << ','
                           << event.publishedToPsolaRing << ',' << event.controlAdvanceSamples << ',' << event.transitionStartCents << ','
                           << event.transitionTargetCents << ',' << event.transitionProgress << ','
                           << event.transitionDurationMs << ',' << event.correctionBeforeAdvance << ','
                           << event.correctionAfterAdvance << ',' << event.scheduledRatio << ','
                           << event.scheduledCorrectionCents << '\n';
    }

    std::array<VibeAutotuneProcessor::RendererControlLookupTrace,
               VibeAutotuneProcessor::rendererControlLookupTraceCapacity> lookupTraces {};
    const auto lookupTraceCount = processor.copyRendererControlLookupTraces (
        lookupTraces.data(), static_cast<int> (lookupTraces.size()));
    std::ofstream lookupTraceCsv (
        outputDirectory.getChildFile ("renderer_control_lookup_ring_trace.csv").getFullPathName().toStdString());
    lookupTraceCsv << "queryTimestamp,lookupRole,writeIndex,foundBefore,foundAfter,interpolationEligible,interpolationApplied,interpolationT,beforeTimestamp,beforeSequence,beforeRatio,beforeCorrection,beforeEvent,afterTimestamp,afterSequence,afterRatio,afterCorrection,afterEvent,returnedTimestamp,returnedSequence,returnedRatio,returnedCorrection,returnedEvent,ringIndex,written,insertionSequence,timestamp,ratio,correction,targetEventId,valid,holdDry\n";
    for (int traceIndex = 0; traceIndex < lookupTraceCount; ++traceIndex)
    {
        const auto& trace = lookupTraces[static_cast<size_t> (traceIndex)];
        for (const auto& entry : trace.ring)
            lookupTraceCsv << trace.queryTimestampSamples << ',' << static_cast<int> (trace.lookupRole) << ',' << trace.psolaControlWriteIndex << ','
                           << trace.foundBefore << ',' << trace.foundAfter << ','
                           << trace.interpolationEligible << ',' << trace.interpolationApplied << ','
                           << trace.interpolationT << ','
                           << trace.before.timestampSamples << ',' << trace.before.insertionSequence << ','
                           << trace.before.pitchRatio << ',' << trace.before.correctionCents << ',' << trace.before.targetEventId << ','
                           << trace.after.timestampSamples << ',' << trace.after.insertionSequence << ','
                           << trace.after.pitchRatio << ',' << trace.after.correctionCents << ',' << trace.after.targetEventId << ','
                           << trace.returned.timestampSamples << ',' << trace.returned.insertionSequence << ','
                           << trace.returned.pitchRatio << ',' << trace.returned.correctionCents << ',' << trace.returned.targetEventId << ','
                           << entry.ringIndex << ',' << entry.written << ',' << entry.insertionSequence << ','
                           << entry.timestampSamples << ',' << entry.pitchRatio << ',' << entry.correctionCents << ','
                           << entry.targetEventId << ',' << entry.valid << ',' << entry.holdDry << '\n';
    }

    // Sample-resolution forensic trace for the legato onset under audit.
    // Control CSV is intentionally decimated; a click is not. Keep this
    // narrow window separate so A/B can identify the exact waveform sample
    // without flooding the normal event log.
    if (retainFullOutputForForensics)
    {
        const auto forensicStart = static_cast<int> (std::lround (0.880 * reader->sampleRate));
        const auto forensicEnd = static_cast<int> (std::lround (0.960 * reader->sampleRate));
        for (int sample = juce::jmax (1, forensicStart);
             sample < juce::jmin (fullDefaultOutput.getNumSamples(), forensicEnd); ++sample)
        {
            const auto delta = std::abs (fullDefaultOutput.getSample (0, sample)
                                       - fullDefaultOutput.getSample (0, sample - 1));
            events << sample << ',' << (1000.0 * static_cast<double> (sample) / reader->sampleRate)
                   << ",wave_delta," << delta << '\n';
        }

        std::ofstream plateauAudit (outputDirectory.getChildFile ("plateau_audit.csv").getFullPathName().toStdString());
        // This is deliberately the same 256-sample lattice as the Artist
        // control-grid CSVs.  It turns a full render into a machine-readable
        // candidate for the surface optimiser; it is not a live DSP path.
        std::ofstream contour (outputDirectory.getChildFile ("contour.csv").getFullPathName().toStdString());
        contour << "sample,independent_f0_hz\n";
        for (int sample = 2048; sample <= fullDefaultOutput.getNumSamples(); sample += 256)
        {
            const auto f0 = estimateControlGridF0 (fullDefaultOutput, sample, reader->sampleRate);
            contour << sample << ',';
            if (std::isfinite (f0)) contour << f0;
            else contour << "nan";
            contour << '\n';
        }
        // TD-PSOLA is causal: output at t + processor latency belongs to
        // source/control at t.  Never compare a control frame with audio at
        // the same timestamp or an intact hard lock will look falsely flat.
        const auto rendererLatencySamples = processor.getLatencySamples();
        plateauAudit << "controlTimeMs,renderedTimeMs,targetMidi,targetHz,sourceF0,requestedRatio,trajectoryActive,renderedF0Autocorrelation,renderedErrorAutocorrelationCents,renderedF0Yin,renderedErrorYinCents,plateauCandidate\n";
        for (const auto& f : allDiagnosticFrames)
        {
            const auto renderedSample = static_cast<int> (f.sampleIndex) + rendererLatencySamples;
            const auto renderedF0 = estimateRenderedF0 (fullDefaultOutput, renderedSample, reader->sampleRate);
            const auto yinF0 = estimateRenderedF0Yin (fullDefaultOutput, renderedSample, reader->sampleRate);
            const auto targetHz = std::isfinite (f.sourceControlTargetCents)
                                ? static_cast<double> (f.referenceHz) * std::exp2 (f.sourceControlTargetCents / 1200.0)
                                : std::numeric_limits<double>::quiet_NaN();
            const auto errorCents = std::isfinite (renderedF0) && std::isfinite (targetHz) && targetHz > 0.0
                                  ? 1200.0 * std::log2 (renderedF0 / targetHz)
                                  : std::numeric_limits<double>::quiet_NaN();
            const auto yinErrorCents = std::isfinite (yinF0) && std::isfinite (targetHz) && targetHz > 0.0
                                     ? 1200.0 * std::log2 (yinF0 / targetHz)
                                     : std::numeric_limits<double>::quiet_NaN();
            const auto plateauCandidate = f.voiced && f.sourceControlValid && ! f.trajectoryTransitionActive
                                       && ! f.dryGuardActive && f.sourceControlTargetMidi >= 0;
            plateauAudit << (1000.0 * static_cast<double> (f.sampleIndex) / reader->sampleRate) << ','
                         << (1000.0 * static_cast<double> (renderedSample) / reader->sampleRate) << ','
                         << f.sourceControlTargetMidi << ',' << targetHz << ',' << f.sourceControlF0Hz << ','
                         << f.sourceControlPitchRatio << ',' << f.trajectoryTransitionActive << ',' << renderedF0 << ','
                         << errorCents << ',' << yinF0 << ',' << yinErrorCents << ','
                         << plateauCandidate << '\n';
        }
    }

    const auto data = processor.getCorrectionDeliveryData();
    const auto rendererLatencySamples = processor.getLatencySamples();
    std::cout << "full_default,target_commits=" << data.targetSwitchCount
              << ",dry_bridges=" << data.dryBridgeCount
              << ",max_trajectory_step_ct=" << data.maxTrajectoryStepCents
              << ",max_renderer_step_ct=" << data.maxRendererStepCents
              << ",pending_deadline_misses=" << processor.getRendererPendingDeadlineMissCount()
              << ",late_candidate_creation=" << processor.getRendererLateCandidateCreationCount()
              << ",late_ola_writes=" << processor.getRendererLateOlaWriteCount()
              << ",pending_queue_peak=" << processor.getRendererPendingQueuePeakOccupancy()
              << ",pending_queue_overflow=" << processor.getRendererPendingQueueOverflowCount()
              << ",retimable_epoch_peak=" << processor.getRendererRetimableEpochPeakOccupancy()
              << ",epoch_retimes=" << processor.getRendererEpochRetimeCount()
              << ",retimed_samples_total=" << processor.getRendererRetimedSamplesTotal()
              << ",max_single_epoch_retime=" << processor.getRendererMaxSingleEpochRetime()
              << ",retime_deadline_misses=" << processor.getRendererRetimeDeadlineMissCount()
              << ",epoch_order_violations=" << processor.getRendererEpochOrderViolationCount()
              << ",ticket_issued_past_deadline=" << processor.getRendererTicketIssuedPastDeadlineCount()
              << ",ticket_mutation_after_issue=" << processor.getRendererTicketMutationAfterIssueCount() << '\n';
    const auto scheduler = processor.getSourceSchedulerDiagnostics();
    std::cout << "source_scheduler,received=" << scheduler.sourceSamplesReceived
              << ",scheduled=" << scheduler.sourceSamplesScheduled
              << ",quantums=" << scheduler.quantumsScheduled
              << ",stage_fill=" << scheduler.partialStageFill
              << ",drops=" << scheduler.sourceDropCount
              << ",duplicates=" << scheduler.sourceDuplicateCount
              << ",gaps=" << scheduler.schedulerGapCount
              << ",overlaps=" << scheduler.schedulerOverlapCount
              << ",max_output_ahead_at_planner=" << scheduler.maximumOutputAheadAtSourcePlanner
              << ",max_output_advance_between_planners=" << scheduler.maximumOutputAdvanceBetweenSourcePlanners
              << '\n';
    std::cout << "callback_boundary,max_step=" << maximumCallbackBoundaryStep
              << ",sample=" << maximumCallbackBoundarySample << '\n';
    if (retainFullOutputForForensics)
        writeMetric (metrics, "J_full_default", measure (fullDefaultOutput), rendererLatencySamples,
                     "production path; target commits and bridge counts in diagnostic summary");
    else
        metrics << "J_full_default,streamed,streamed,streamed,0,-1," << rendererLatencySamples
                << ",production path streamed to WAV\n";
    std::cout << "csv=" << outputDirectory.getChildFile ("full_default_intervals.csv").getFullPathName() << '\n';
    std::cout << "events=" << outputDirectory.getChildFile ("full_default_events.csv").getFullPathName() << '\n';
    std::cout << "control_materialization="
              << outputDirectory.getChildFile ("control_materialization_events.csv").getFullPathName() << '\n';
    std::cout << "renderer_control_lookup_ring_trace="
              << outputDirectory.getChildFile ("renderer_control_lookup_ring_trace.csv").getFullPathName() << '\n';
    if (epochTraceEnabled)
        std::cout << "renderer_epoch_trace="
                  << outputDirectory.getChildFile ("renderer_epoch_trace.csv").getFullPathName() << '\n';
    if (olaTraceStart <= olaTraceEnd)
        std::cout << "renderer_ola_contribution_trace="
                  << outputDirectory.getChildFile ("renderer_ola_contribution_trace.csv").getFullPathName() << '\n';
    std::cout << "render=" << renderedFile.getFullPathName() << '\n';
}
