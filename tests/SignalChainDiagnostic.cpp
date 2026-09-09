#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
constexpr int blockSize = 256;
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

void writeHeader (std::ofstream& csv)
{
    csv << "sampleIndex,timeMs,voiceRangeMin,voiceRangeMax,trackingValue,voiced,pitchConfidence,rawF0,stableF0,rawMidi,key,scale,committedTargetMidi,targetChanged,flexWeight,desiredCorrectionCents,retuneCorrectionCents,humanizeWeight,vibratoContributionCents,requestedRendererRatio,rendererPreGuardCorrectionCents,rendererPostGuardCorrectionCents,rendererCorrectionSource,deliveredRendererCorrectionCents,rendererInputCorrectionCents,rendererStepCents,correctionIsActive,targetBridge,transientBridge,dryWetMix,transpose,referenceHz,currentTargetCents,candidateTargetCents,trajectoryTransitionActive,trajectoryTransitionProgress,trajectoryTransitionStartCents,trajectoryTransitionEndCents,trajectoryTransitionDurationMs,trajectoryRapidAdjacentContour,trajectoryRendererCrossingActive,targetDecisionMidpointCents,targetDecisionExcessCents,targetHysteresisCents,pendingTargetMidi,pendingTargetFrames,targetDecisionBeyondHysteresis,rapidAdjacentRawCandidate,rapidLegatoAdjacentStep,rapidLegatoHardLock,rapidLegatoTargetChanged,rapidLegatoContinuesDirection,rapidLegatoChainStarts,rapidLegatoChainActive,rapidLegatoCommittedSteps,rapidLegatoSamplesSinceCommit,rapidLegatoDistanceCents,rapidLegatoInputMotionCentsPerSecond,latticeReset,dryGuardActive,reentryHoldActive,sourceControlTimestamp,sourceControlF0Hz,sourceControlTargetCents,sourceControlTargetMidi,sourceControlValid,sourceControlHoldDry,sourceControlPitchRatio,olaOverlapWeight,olaAudibleRatioAtStart,olaAudibleGrainAge,olaAudibleReadPosition,olaDominantGrainWeight,olaDominantGrainEpoch,olaDominantGrainRatio,olaDominantGrainExpectedF0,olaDominantGrainTargetMidi,olaDominantGrainTargetEventId,olaDominantGrainTargetControlTimestamp,olaTargetCandidateWeight,olaTargetCandidateEpoch,olaTargetCandidateExpectedF0,olaShadowFutureSegments,olaShadowAudibleSegments,olaShadowFutureSamples,olaShadowReplacementEligible,olaShadowReplacementFirstSample,olaShadowReplacementLastSample,olaShadowCandidateRenderApplied,nextEpochBeforeReversal,nextEpochAfterReversal,actualSpawnEpoch,ratioAtSpawn,spawnTargetMidi,spawnTargetEventId,spawnTargetControlTimestamp,spawnSourceMarkF0,spawnExpectedF0,olaSourceMarkTime,olaSourceMarkF0,olaExpectedGrainF0,synthExpectedPeriod,synthActualPeriod,synthPeriodError,synthSourceSpacing,sourceSelectionCursor,newestCompleteSourceMark,sibilanceResidualMix,rendererHighFrequencyEvidence,rendererPeriodicCoherence,pitchMarkCandidateOffset,pitchMarkCandidateCorrelation,confirmedPitchMarkOffset,confirmedPitchMarkRun,rendererArtifactDryVeto,rendererGrainStarved,rendererShouldRenderWet,rendererSafetyDrySamples\n";
}

void writeFrame (std::ofstream& csv, const VibeAutotuneProcessor::SignalChainDiagnosticFrame& f, double sampleRate)
{
    const auto correctionSource = f.rendererCorrectionSource == 0 ? "NORMAL_DESIRED"
                                : f.rendererCorrectionSource == 1 ? "INVALID_CONTROL"
                                                                  : "DRY_GUARD";
    csv << f.sampleIndex << ',' << (1000.0 * static_cast<double> (f.sampleIndex) / sampleRate) << ','
        << f.voiceRangeMinHz << ',' << f.voiceRangeMaxHz << ',' << f.trackingValue << ',' << f.voiced << ','
        << f.pitchConfidence << ',' << f.rawF0Hz << ',' << f.stableF0Hz << ',' << f.rawMidi << ',' << f.key << ',' << f.scale << ','
        << f.committedTargetMidi << ',' << f.targetChanged << ',' << f.flexCorrectionGain << ','
        << f.desiredCorrectionCents << ',' << f.retuneCorrectionCents << ',' << f.humanizeWeight << ','
        << f.vibratoContributionCents << ',' << f.requestedRendererRatio << ','
        << f.rendererPreGuardCorrectionCents << ',' << f.rendererPostGuardCorrectionCents << ',' << correctionSource << ','
        << f.deliveredRendererCorrectionCents << ',' << f.rendererInputCorrectionCents << ',' << f.rendererStepCents << ',' << f.correctionIsActive << ','
        << f.targetBridge << ',' << f.transientBridge << ',' << f.dryWetMix << ','
        << f.transpose << ',' << f.referenceHz << ',' << f.targetPitchCents << ',' << f.candidateTargetCents << ','
        << f.trajectoryTransitionActive << ',' << f.trajectoryTransitionProgress << ','
        << f.trajectoryTransitionStartCents << ',' << f.trajectoryTransitionEndCents << ','
        << f.trajectoryTransitionDurationMs << ',' << f.trajectoryRapidAdjacentContour << ','
        << f.trajectoryRendererCrossingActive << ','
        << f.targetDecisionMidpointCents << ',' << f.targetDecisionExcessCents << ','
        << f.targetHysteresisCents << ',' << f.pendingTargetMidi << ',' << f.pendingTargetFrames << ','
        << f.targetDecisionBeyondHysteresis << ',' << f.rapidAdjacentRawCandidate << ','
        << f.rapidLegatoAdjacentStep << ',' << f.rapidLegatoHardLock << ',' << f.rapidLegatoTargetChanged << ','
        << f.rapidLegatoContinuesDirection << ','
        << f.rapidLegatoChainStarts << ',' << f.rapidLegatoChainActive << ','
        << f.rapidLegatoCommittedSteps << ',' << f.rapidLegatoSamplesSinceCommit << ','
        << f.rapidLegatoDistanceCents << ',' << f.rapidLegatoInputMotionCentsPerSecond << ','
        << f.latticeReset << ',' << f.dryGuardActive << ',' << f.reentryHoldActive << ','
        << f.sourceControlTimestamp << ',' << f.sourceControlF0Hz << ',' << f.sourceControlTargetCents << ','
        << f.sourceControlTargetMidi << ',' << f.sourceControlValid << ',' << f.sourceControlHoldDry << ',' << f.sourceControlPitchRatio << ','
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
        << f.rendererArtifactDryVeto << ',' << f.rendererGrainStarved << ',' << f.rendererShouldRenderWet << ',' << f.rendererSafetyDrySamples << '\n';
}
}

int main (int argc, char* argv[])
{
    if (argc != 3 && argc != 5 && argc != 7 && argc != 8 && argc != 9 && argc != 10 && argc != 11 && argc != 12)
    {
        std::cerr << "usage: VibeAutotuneSignalChainDiagnostic <input-wav> <output-directory> [classic formant] [retune-ms flex-percent [waveform-anchored-source-marks [voiced-colour-percent [phase-continuous-renderer [phase-force-unity-ratio [phase-force-static-up-ratio]]]]]]\n";
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
    std::ofstream metrics (outputDirectory.getChildFile ("case_metrics.csv").getFullPathName().toStdString());
    metrics << "case,rms,peak,maxSampleStep,nonFiniteSamples,firstClippedSample,latencySamples,note\n";
    std::ofstream csv (outputDirectory.getChildFile ("full_default_intervals.csv").getFullPathName().toStdString());
    std::ofstream events (outputDirectory.getChildFile ("full_default_events.csv").getFullPathName().toStdString());
    writeHeader (csv);
    events << "sampleIndex,timeMs,event,detail\n";

    const auto renderedFile = outputDirectory.getChildFile ("full_default_render.wav");
    std::unique_ptr<juce::FileOutputStream> renderedStream (renderedFile.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> renderedWriter (
        renderedStream != nullptr ? wav.createWriterFor (renderedStream.get(), reader->sampleRate, 1, 24, {}, 0) : nullptr);
    if (renderedWriter == nullptr)
    {
        std::cerr << "unable to create render wav\n";
        return 2;
    }
    renderedStream.release();

    VibeAutotuneProcessor processor;
    processor.prepareToPlay (reader->sampleRate, blockSize);
    processor.setSignalChainDiagnosticEnabled (true);
    CausalTDPSOLARenderer::DiagnosticAblation ablation;
    ablation.waveformAnchoredSourceMarks = waveformAnchoredSourceMarks;
    ablation.phaseContinuousRenderer = phaseContinuousRenderer;
    ablation.phaseForceUnityRatio = phaseForceUnityRatio;
    ablation.phaseForceStaticUpRatio = phaseForceStaticUpRatio;
    processor.setRendererDiagnosticAblation (ablation);
    // Explicitly state the shipped default preset for binding audit.
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "retuneSpeed", retuneMs);
    setParameter (processor, "flexTune", flexTunePercent);
    setParameter (processor, "humanize", 0.0f);
    setParameter (processor, "naturalVibrato", 0.0f);
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
              << "Flex-Tune," << flexTunePercent << '/' << flexTunePercent << '/' << flexTunePercent << ",trajectory\nHumanize,0%/0/0,trajectory\n"
              << "Natural Vibrato,0%/0/0,trajectory\nGroup Octaves,state/state,state,note policy\nNote Map Rules,state/state,state,target selector\n";

    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), blockSize);
    // Production AU now exposes a mono vocal path. Feeding a two-channel
    // buffer here bypasses the host's mono layout negotiation and can leave
    // the renderer grain-starved, producing a misleading dry benchmark.
    juce::AudioBuffer<float> output (1, blockSize);
    // The production WAV is written as a stream. Retaining a second full
    // stereo copy is useful only for the short sample-level forensic trace;
    // on a long raw vocal it needlessly exhausts memory before the actual
    // listenable export is complete.
    const auto retainFullOutputForForensics = reader->lengthInSamples <= static_cast<int64_t> (30 * reader->sampleRate);
    juce::AudioBuffer<float> fullDefaultOutput (1, retainFullOutputForForensics
                                                    ? static_cast<int> (reader->lengthInSamples) : 0);
    juce::MidiBuffer midi;
    std::vector<VibeAutotuneProcessor::SignalChainDiagnosticFrame> frames (16);
    std::vector<VibeAutotuneProcessor::SignalChainDiagnosticFrame> allDiagnosticFrames;
    bool previousVoiced = false, previousActive = false, previousTargetBridge = false, previousTransientBridge = false;
    auto previousCommittedTarget = -2;
    double previousSpawnEpoch = -1.0;
    float previousWet = 0.0f;

    for (int64_t position = 0; position < reader->lengthInSamples; position += blockSize)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (blockSize, reader->lengthInSamples - position));
        source.clear(); output.clear();
        reader->read (&source, 0, count, position, true, true);
        for (int sample = 0; sample < count; ++sample)
            output.setSample (0, sample, source.getSample (0, sample));
        processor.processBlock (output, midi);
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
              << ",max_renderer_step_ct=" << data.maxRendererStepCents << '\n';
    if (retainFullOutputForForensics)
        writeMetric (metrics, "J_full_default", measure (fullDefaultOutput), rendererLatencySamples,
                     "production path; target commits and bridge counts in diagnostic summary");
    else
        metrics << "J_full_default,streamed,streamed,streamed,0,-1," << rendererLatencySamples
                << ",production path streamed to WAV\n";
    std::cout << "csv=" << outputDirectory.getChildFile ("full_default_intervals.csv").getFullPathName() << '\n';
    std::cout << "events=" << outputDirectory.getChildFile ("full_default_events.csv").getFullPathName() << '\n';
    std::cout << "render=" << renderedFile.getFullPathName() << '\n';
}
