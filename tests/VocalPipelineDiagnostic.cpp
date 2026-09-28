#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{
constexpr int blockSize = 256;

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        stream != nullptr ? wav.createWriterFor (stream.get(), sampleRate, 2, 24, {}, 0) : nullptr);
    if (writer == nullptr)
        return false;

    stream.release();
    return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

struct Stats
{
    int analysisFrames = 0;
    int voicedFrames = 0;
    int targetSwitches = 0;
    int rawOctaveJumps = 0;
    int stableOctaveJumps = 0;
    int correctionJumpsOver80Cents = 0;
    double maximumCorrectionCents = 0.0;
    double maximumCorrectionJumpCents = 0.0;
    double maximumOutputStep = 0.0;
    double maximumInputPeak = 0.0;
    double maximumOutputPeak = 0.0;
    bool clipped = false;
    bool finite = true;
};
}

int main (int argc, char* argv[])
{
    if (argc < 2 || argc > 12)
    {
        std::cerr << "usage: VibeAutotuneVocalDiagnostic <mono-or-stereo-wav> [retune-ms] [output-wav] [flex] [humanize] [natural-vibrato-ms] [pitch-centre-s] [wide-vibrato-neutral] [wide-vibrato-humanized] [phase-continuous-renderer] [stable-target-follows-trajectory]\n";
        return 2;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto input = juce::File (argv[1]);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (input));
    if (reader == nullptr)
    {
        std::cerr << "unable to read " << input.getFullPathName() << '\n';
        return 2;
    }

    VibeAutotuneProcessor processor;
    processor.prepareToPlay (reader->sampleRate, blockSize);
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "inputType", 1.0f); // Alto
    const auto retuneMs = argc >= 3 ? static_cast<float> (std::atof (argv[2])) : 25.0f;
    setParameter (processor, "retuneSpeed", retuneMs);
    const auto flexTune = argc >= 5 ? static_cast<float> (std::atof (argv[4])) : 0.0f;
    const auto humanize = argc >= 6 ? static_cast<float> (std::atof (argv[5])) : 0.0f;
    const auto naturalVibrato = argc >= 7 ? static_cast<float> (std::atof (argv[6])) : 0.0f;
    setParameter (processor, "flexTune", flexTune);
    setParameter (processor, "humanize", humanize);
    setParameter (processor, "naturalVibrato", naturalVibrato);
    if (argc >= 10)
        processor.setControlCalibrationDiagnostic (std::atof (argv[7]), std::atof (argv[8]), std::atof (argv[9]));
    if (argc >= 11)
    {
        CausalTDPSOLARenderer::DiagnosticAblation ablation;
        ablation.phaseContinuousRenderer = std::atoi (argv[10]) != 0;
        ablation.stableTargetFollowsTrajectory = argc >= 12 && std::atoi (argv[11]) != 0;
        processor.setRendererDiagnosticAblation (ablation);
    }
    setParameter (processor, "tracking", 50.0f);
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);
    setParameter (processor, "key", 6.0f);   // F#
    setParameter (processor, "scale", 2.0f); // Natural Minor

    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), blockSize);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::AudioBuffer<float> rendered (2, static_cast<int> (reader->lengthInSamples));
    juce::MidiBuffer midi;
    Stats stats;
    uint64_t previousTimestamp = std::numeric_limits<uint64_t>::max();
    double previousRawF0 = std::numeric_limits<double>::quiet_NaN();
    double previousStableF0 = std::numeric_limits<double>::quiet_NaN();
    double previousCorrection = std::numeric_limits<double>::quiet_NaN();
    juce::String previousTarget;
    float previousOutput = 0.0f;
    auto hasPreviousOutput = false;
    auto reportedExtremeFrames = 0;

    for (int64_t position = 0; position < reader->lengthInSamples; position += blockSize)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (blockSize, reader->lengthInSamples - position));
        source.clear();
        reader->read (&source, 0, count, position, true, true);
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = sample < count ? source.getSample (0, sample) : 0.0f;
            stats.maximumInputPeak = std::max (stats.maximumInputPeak, std::abs (static_cast<double> (value)));
            audio.setSample (0, sample, value);
            audio.setSample (1, sample, value);
        }

        processor.processBlock (audio, midi);
        rendered.copyFrom (0, position, audio, 0, 0, count);
        rendered.copyFrom (1, position, audio, 1, 0, count);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto value = audio.getSample (0, sample);
            stats.finite = stats.finite && std::isfinite (value);
            stats.clipped = stats.clipped || std::abs (value) >= 1.0f;
            stats.maximumOutputPeak = std::max (stats.maximumOutputPeak, std::abs (static_cast<double> (value)));
            if (hasPreviousOutput)
                stats.maximumOutputStep = std::max (stats.maximumOutputStep, std::abs (static_cast<double> (value - previousOutput)));
            previousOutput = value;
            hasPreviousOutput = true;
        }

        const auto tracking = processor.getPitchTrackingData();
        if (tracking.timestampSamples == previousTimestamp)
            continue;
        previousTimestamp = tracking.timestampSamples;
        ++stats.analysisFrames;
        if (! tracking.voiced || ! std::isfinite (tracking.stableF0Hz))
            continue;

        ++stats.voicedFrames;
        if (std::isfinite (previousRawF0)
            && std::abs (1200.0 * std::log2 (tracking.rawF0Hz / previousRawF0)) > 850.0)
            ++stats.rawOctaveJumps;
        if (std::isfinite (previousStableF0)
            && std::abs (1200.0 * std::log2 (tracking.stableF0Hz / previousStableF0)) > 850.0)
            ++stats.stableOctaveJumps;
        previousRawF0 = tracking.rawF0Hz;
        previousStableF0 = tracking.stableF0Hz;

        const auto monitor = processor.getPitchMonitorData();
        if (! previousTarget.isEmpty() && monitor.targetNote != previousTarget)
            ++stats.targetSwitches;
        previousTarget = monitor.targetNote;
        const auto correction = static_cast<double> (monitor.correctionCents);
        stats.maximumCorrectionCents = std::max (stats.maximumCorrectionCents, std::abs (correction));
        if (std::isfinite (previousCorrection))
        {
            const auto jump = std::abs (correction - previousCorrection);
            stats.maximumCorrectionJumpCents = std::max (stats.maximumCorrectionJumpCents, jump);
            if (jump > 80.0) ++stats.correctionJumpsOver80Cents;
            if ((std::abs (correction) > 150.0 || jump > 80.0) && reportedExtremeFrames++ < 24)
                std::cout << "  extreme | time_s=" << static_cast<double> (tracking.timestampSamples) / reader->sampleRate
                          << " raw_hz=" << tracking.rawF0Hz << " stable_hz=" << tracking.stableF0Hz
                          << " target=" << monitor.targetNote << " correction_ct=" << correction
                          << " jump_ct=" << jump << '\n';
        }
        previousCorrection = correction;
    }

    const auto delivery = processor.getCorrectionDeliveryData();
    std::cout << "vocal_diagnostic"
              << " retune_ms=" << retuneMs
              << " flex=" << flexTune
              << " humanize=" << humanize
              << " natural_vibrato_ms=" << naturalVibrato
              << " frames=" << stats.analysisFrames
              << " voiced=" << stats.voicedFrames
              << " target_switches=" << stats.targetSwitches
              << " raw_octave_jumps=" << stats.rawOctaveJumps
              << " stable_octave_jumps=" << stats.stableOctaveJumps
              << " correction_jumps_over_80ct=" << stats.correctionJumpsOver80Cents
              << " max_correction_ct=" << stats.maximumCorrectionCents
              << " max_correction_jump_ct=" << stats.maximumCorrectionJumpCents
              << " committed_target_switches=" << delivery.targetSwitchCount
              << " dry_bridges=" << delivery.dryBridgeCount
              << " max_trajectory_step_ct=" << delivery.maxTrajectoryStepCents
              << " max_renderer_step_ct=" << delivery.maxRendererStepCents
              << " input_peak=" << stats.maximumInputPeak
              << " output_peak=" << stats.maximumOutputPeak
              << " max_output_step=" << stats.maximumOutputStep
              << " finite=" << stats.finite
              << " clipped=" << stats.clipped << '\n';

    if (argc >= 4 && ! writeWav (juce::File (argv[3]), rendered, reader->sampleRate))
        return 3;
}
