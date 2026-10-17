#include "PluginProcessor.h"

#include <algorithm>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

double midiToHz (int midi) { return 440.0 * std::exp2 ((static_cast<double> (midi) - 69.0) / 12.0); }

double estimateF0 (const std::vector<float>& samples, int start, int end)
{
    double first = std::numeric_limits<double>::quiet_NaN(), last = first;
    int count = 0;
    for (int sample = start + 1; sample < end; ++sample)
    {
        const auto previous = static_cast<double> (samples[static_cast<size_t> (sample - 1)]);
        const auto current = static_cast<double> (samples[static_cast<size_t> (sample)]);
        if (previous <= 0.0 && current > 0.0)
        {
            const auto crossing = static_cast<double> (sample - 1) - previous / (current - previous);
            if (count == 0) first = crossing;
            last = crossing;
            ++count;
        }
    }
    return count >= 2 ? sampleRate * static_cast<double> (count - 1) / (last - first)
                      : std::numeric_limits<double>::quiet_NaN();
}


double estimateAutocorrelationF0 (const std::vector<float>& samples, int start, int end)
{
    start = std::max (0, start);
    end = std::min (static_cast<int> (samples.size()), end);
    constexpr int minimumLag = 128, maximumLag = 300;
    if (end - start <= maximumLag + 1024)
        return std::numeric_limits<double>::quiet_NaN();
    auto bestLag = -1;
    auto bestScore = -std::numeric_limits<double>::infinity();
    for (int lag = minimumLag; lag <= maximumLag; ++lag)
    {
        double dot = 0.0, leftEnergy = 0.0, rightEnergy = 0.0;
        for (int sample = start + lag; sample < end; ++sample)
        {
            const auto left = static_cast<double> (samples[static_cast<size_t> (sample)]);
            const auto right = static_cast<double> (samples[static_cast<size_t> (sample - lag)]);
            dot += left * right;
            leftEnergy += left * left;
            rightEnergy += right * right;
        }
        const auto score = dot / std::sqrt (leftEnergy * rightEnergy + 1.0e-18);
        if (score > bestScore) { bestScore = score; bestLag = lag; }
    }
    return bestLag > 0 ? sampleRate / static_cast<double> (bestLag)
                       : std::numeric_limits<double>::quiet_NaN();
}

bool isCMajor (int midi) noexcept
{
    constexpr std::array<bool, 12> legal {{ true, false, true, false, true, true, false, true, false, true, false, true }};
    return legal[static_cast<size_t> (midi % 12)];
}

void require (bool value, const char* message)
{
    if (! value)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit (1);
    }
}

void runIllegalCMajorTone (int inputMidi)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    processor.setSignalChainDiagnosticEnabled (true);
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "inputType", 1.0f);
    setParameter (processor, "key", 0.0f);
    setParameter (processor, "scale", 1.0f); // Major
    setParameter (processor, "retuneSpeed", 0.0f);
    setParameter (processor, "flexTune", 0.0f);
    setParameter (processor, "humanize", 0.0f);
    setParameter (processor, "naturalVibrato", 0.0f);
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);

    // C#/D#/F#/G#/A# are exact half-way ties in C Major. Keep the
    // synthetic fixture outside the scale, but offset it slightly toward
    // the lower legal neighbour so a sub-cent detector fluctuation cannot
    // turn this renderer-delivery assertion into a selector tie test.
    const auto inputHz = midiToHz (inputMidi) * std::exp2 (-18.0 / 1200.0);
    constexpr int totalSamples = static_cast<int> (sampleRate * 3.0);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (totalSamples);
    auto phase = 0.0;
    auto sawLegalTarget = false;
    auto latestTarget = -1;

    for (int start = 0; start < totalSamples; start += blockSize)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = static_cast<float> (0.30 * std::sin (phase));
            phase += 2.0 * std::numbers::pi * inputHz / sampleRate;
            audio.setSample (0, sample, value);
            audio.setSample (1, sample, value);
        }
        processor.processBlock (audio, midi);
        output.insert (output.end(), audio.getReadPointer (0), audio.getReadPointer (0) + blockSize);
        const auto target = processor.getLatestTargetMidiNote();
        if (target >= 0)
        {
            latestTarget = target;
            require (isCMajor (target), "C Major live target contained an illegal pitch class");
            sawLegalTarget = true;
        }
    }

    require (sawLegalTarget, "C Major live path did not acquire a target");
    std::array<VibeAutotuneProcessor::SignalChainDiagnosticFrame, 4096> diagnosticFrames {};
    const auto diagnosticCount = processor.copySignalChainDiagnosticFrames (
        diagnosticFrames.data(), static_cast<int> (diagnosticFrames.size()));
    auto activeFrames = 0;
    auto maxMix = 0.0f;
    auto latestRequestedCents = 0.0;
    auto latestDeliveredCents = 0.0f;
    auto latestSourceTarget = -1;
    for (int index = 0; index < diagnosticCount; ++index)
    {
        const auto& frame = diagnosticFrames[static_cast<size_t> (index)];
        activeFrames += frame.correctionIsActive ? 1 : 0;
        maxMix = std::max (maxMix, frame.dryWetMix);
        latestRequestedCents = 1200.0 * std::log2 (frame.requestedRendererRatio);
        latestDeliveredCents = frame.deliveredRendererCorrectionCents;
        latestSourceTarget = frame.outputRendererControlTargetMidi;
    }
    std::array<VibeAutotuneProcessor::ControlMaterializationEvent, 4096> controlEvents {};
    const auto controlEventCount = processor.copyControlMaterializationEvents (
        controlEvents.data(), static_cast<int> (controlEvents.size()));
    for (int index = 0; index < controlEventCount; ++index)
    {
        const auto& event = controlEvents[static_cast<size_t> (index)];
        if (event.timestampSamples < 141000 || event.timestampSamples > 144000)
            continue;
        std::cout << "key_scale_control_history | sequence=" << event.sequence
                  << " timestamp=" << event.timestampSamples
                  << " target_event=" << event.targetEventId
                  << " scheduled_ct=" << event.scheduledCorrectionCents
                  << " after_ct=" << event.correctionAfterAdvance
                  << " transition_target=" << event.transitionTargetCents
                  << " progress=" << event.transitionProgress << '\n';
    }
    std::cout << "key_scale_renderer_probe | frames=" << diagnosticCount
              << " active=" << activeFrames
              << " max_wet=" << maxMix
              << " requested_ct=" << latestRequestedCents
              << " delivered_ct=" << latestDeliveredCents
              << " output_control_target=" << latestSourceTarget << '\n';
    if (diagnosticCount > 0)
    {
        const auto& frame = diagnosticFrames[static_cast<size_t> (diagnosticCount - 1)];
        std::cout << "key_scale_control_handoff_probe | caller_time=" << frame.callerControlLookupTime
                  << " renderer_time=" << frame.rendererControlLookupTime
                  << " caller_timestamp=" << frame.callerControlTimestamp
                  << " renderer_timestamp=" << frame.rendererControlTimestamp
                  << " caller_ct=" << 1200.0 * std::log2 (frame.callerControlPitchRatio)
                  << " renderer_ct=" << 1200.0 * std::log2 (frame.rendererControlPitchRatio)
                  << " caller_event=" << frame.callerControlEventId
                  << " renderer_event=" << frame.rendererControlEventId << '\n';
        std::cout << "key_scale_synthesis_probe | expected_period=" << frame.synthExpectedPeriod
                  << " actual_period=" << frame.synthActualPeriod
                  << " period_error=" << frame.synthPeriodError
                  << " source_spacing=" << frame.synthSourceSpacing
                  << " expected_hz=" << (frame.synthExpectedPeriod > 0.0 ? 48000.0 / frame.synthExpectedPeriod : 0.0)
                  << " actual_hz=" << (frame.synthActualPeriod > 0.0 ? 48000.0 / frame.synthActualPeriod : 0.0) << '\n';
        std::cout << "key_scale_audible_grain_probe | dominant_epoch=" << frame.olaDominantGrainEpoch
                  << " source_mark=" << frame.olaSourceMarkTime
                  << " source_hz=" << frame.olaSourceMarkF0
                  << " grain_age=" << frame.olaAudibleGrainAge
                  << " target_event=" << frame.olaDominantGrainTargetEventId
                  << " control_event=" << frame.olaDominantGrainControlEventId
                  << " target_timestamp=" << frame.olaDominantGrainTargetControlTimestamp
                  << " candidate_weight=" << frame.olaTargetCandidateWeight
                  << " candidate_epoch=" << frame.olaTargetCandidateEpoch
                  << " candidate_hz=" << frame.olaTargetCandidateExpectedF0 << '\n';
        std::cout << "key_scale_grain_probe | weight=" << frame.olaOverlapWeight
                  << " grain_starved=" << frame.rendererGrainStarved
                  << " should_wet=" << frame.rendererShouldRenderWet
                  << " audible_ratio=" << frame.olaAudibleRatioAtStart
                  << " dominant_ratio=" << frame.olaDominantGrainRatio
                  << " audible_expected_hz=" << frame.olaExpectedGrainF0
                  << " dominant_expected_hz=" << frame.olaDominantGrainExpectedF0
                  << " dominant_target=" << frame.olaDominantGrainTargetMidi
                  << " spawn_ratio=" << frame.ratioAtSpawn
                  << " spawn_expected_hz=" << frame.spawnExpectedF0
                  << " spawn_target=" << frame.spawnTargetMidi << '\n';
    }
    for (int index = 0; index < diagnosticCount; ++index)
    {
        const auto& frame = diagnosticFrames[static_cast<size_t> (index)];
        std::cout << "key_scale_renderer_tick | sample=" << frame.sampleIndex
                  << " raw_hz=" << frame.rawF0Hz
                  << " stable_hz=" << frame.stableF0Hz
                  << " input_ct=" << frame.inputPitchCents
                  << " target_ct=" << frame.targetPitchCents
                  << " desired_ct=" << frame.desiredCorrectionCents
                  << " requested_ct=" << 1200.0 * std::log2 (frame.requestedRendererRatio)
                  << " delivered_ct=" << frame.deliveredRendererCorrectionCents
                  << " renderer_input_ct=" << frame.rendererInputCorrectionCents
                  << " renderer_post_ct=" << frame.rendererPostGuardCorrectionCents
                  << " source_hz=" << frame.sourceControlF0Hz
                  << " source_target=" << frame.sourceControlTargetMidi
                  << " source_ct=" << 1200.0 * std::log2 (frame.sourceControlPitchRatio)
                  << " wet=" << frame.dryWetMix
                  << " active=" << frame.correctionIsActive << '\n';
    }
    const auto outputF0 = estimateF0 (output, processor.getLatencySamples() + static_cast<int> (sampleRate),
                                      processor.getLatencySamples() + static_cast<int> (sampleRate * 2.0));
    const auto outputAutocorrelationF0 = estimateAutocorrelationF0 (
        output, processor.getLatencySamples() + static_cast<int> (sampleRate),
        processor.getLatencySamples() + static_cast<int> (sampleRate * 2.0));
    std::cout << "key_scale_autocorrelation_probe | zero_cross_hz=" << outputF0
              << " autocorrelation_hz=" << outputAutocorrelationF0 << '\n';
    const auto outputMidi = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (outputF0 / 440.0)));
    std::cout << "key_scale_live_probe | input=" << inputMidi
              << " target=" << latestTarget
              << " output_hz=" << outputF0
              << " output=" << outputMidi
              << " latency_samples=" << processor.getLatencySamples() << '\n';
    require (std::isfinite (outputF0) && isCMajor (outputMidi), "C Major renderer output remained outside the selected scale");
    std::cout << "key_scale_live | input=" << inputMidi << " output=" << outputMidi << " target=legal\n";
}
}

int main()
{
    for (const auto illegalMidi : { 61, 63, 66, 68, 70 }) // C#, D#, F#, G#, A#
        runIllegalCMajorTone (illegalMidi);
}
