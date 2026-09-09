#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdio>
#include <limits>

namespace
{
constexpr int blockSize = 256;

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

struct Result
{
    int voicedFrames = 0;
    int sustainedFrames = 0;
    int targetSwitches = 0;
    double meanAbsoluteCorrection = 0.0;
    double sustainedAbsoluteCorrection = 0.0;
    double maxCorrectionStep = 0.0;
    int outputSustainSegments = 0;
    int outputResidualFrames = 0;
    double outputResidualRms = 0.0;
    double outputVibratoDepth = 0.0;
    bool finite = true;
};

Result runCase (juce::AudioFormatReader& reader, float retuneMs, float humanizePercent,
                float flexTunePercent = 0.0f, float naturalVibratoPercent = 0.0f)
{
    VibeAutotuneProcessor processor;
    processor.prepareToPlay (reader.sampleRate, blockSize);
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "inputType", 1.0f);
    setParameter (processor, "key", 0.0f);
    setParameter (processor, "scale", 0.0f);
    setParameter (processor, "tracking", 50.0f);
    setParameter (processor, "retuneSpeed", retuneMs);
    setParameter (processor, "flexTune", flexTunePercent);
    setParameter (processor, "humanize", humanizePercent);
    setParameter (processor, "naturalVibrato", naturalVibratoPercent);
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);

    // A separate processor is used only as an output-pitch probe. Its YIN
    // tracker observes the renderer output; it never feeds audio or control
    // back into the processor under test.
    VibeAutotuneProcessor outputPitchProbe;
    outputPitchProbe.prepareToPlay (reader.sampleRate, blockSize);
    setParameter (outputPitchProbe, "bypass", 0.0f);
    setParameter (outputPitchProbe, "inputType", 1.0f);
    setParameter (outputPitchProbe, "key", 0.0f);
    setParameter (outputPitchProbe, "scale", 0.0f);
    setParameter (outputPitchProbe, "tracking", 50.0f);
    setParameter (outputPitchProbe, "retuneSpeed", 25.0f);
    setParameter (outputPitchProbe, "flexTune", 0.0f);
    setParameter (outputPitchProbe, "humanize", 0.0f);
    setParameter (outputPitchProbe, "naturalVibrato", 0.0f);
    setParameter (outputPitchProbe, "transpose", 0.0f);
    setParameter (outputPitchProbe, "referencePitch", 440.0f);

    juce::AudioBuffer<float> source (static_cast<int> (reader.numChannels), blockSize);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::AudioBuffer<float> outputProbeAudio (2, blockSize);
    juce::MidiBuffer midi;
    Result result;
    uint64_t lastTimestamp = std::numeric_limits<uint64_t>::max();
    juce::String previousTarget;
    auto targetRunFrames = 0;
    auto correctionSum = 0.0;
    auto sustainedCorrectionSum = 0.0;
    auto previousCorrection = std::numeric_limits<double>::quiet_NaN();
    uint64_t outputLastTimestamp = std::numeric_limits<uint64_t>::max();
    juce::String outputPreviousTarget;
    auto outputTargetRunFrames = 0;
    auto outputCenterCents = 0.0;
    auto outputHasCenter = false;
    auto outputSegmentResidualSquareSum = 0.0;
    auto outputSegmentResidualFrames = 0;
    constexpr auto outputCenterTimeSeconds = 0.20;

    const auto finishOutputSustainSegment = [&]
    {
        if (outputSegmentResidualFrames <= 0)
            return;

        const auto rms = std::sqrt (outputSegmentResidualSquareSum
                                    / static_cast<double> (outputSegmentResidualFrames));
        result.outputResidualRms += rms * static_cast<double> (outputSegmentResidualFrames);
        result.outputResidualFrames += outputSegmentResidualFrames;
        ++result.outputSustainSegments;
        outputSegmentResidualSquareSum = 0.0;
        outputSegmentResidualFrames = 0;
    };

    for (int64_t position = 0; position < reader.lengthInSamples; position += blockSize)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (blockSize, reader.lengthInSamples - position));
        source.clear();
        reader.read (&source, 0, count, position, true, true);
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = sample < count ? source.getSample (0, sample) : 0.0f;
            audio.setSample (0, sample, value);
            audio.setSample (1, sample, value);
        }

        processor.processBlock (audio, midi);
        for (int sample = 0; sample < count; ++sample)
            result.finite = result.finite && std::isfinite (audio.getSample (0, sample));

        outputProbeAudio.makeCopyOf (audio, true);
        outputPitchProbe.processBlock (outputProbeAudio, midi);
        const auto outputTracking = outputPitchProbe.getPitchTrackingData();
        if (outputTracking.timestampSamples != outputLastTimestamp)
        {
            outputLastTimestamp = outputTracking.timestampSamples;
            if (! outputTracking.voiced || ! std::isfinite (outputTracking.stableF0Hz)
                || outputTracking.stableF0Hz <= 0.0 || previousTarget.isEmpty())
            {
                finishOutputSustainSegment();
                outputTargetRunFrames = 0;
                outputPreviousTarget.clear();
                outputHasCenter = false;
            }
            else
            {
                // Segment by the source live-path target, not by a target
                // recomputed from the rendered signal. The latter can change
                // with Natural Vibrato and would bias the comparison.
                if (previousTarget != outputPreviousTarget)
                {
                    finishOutputSustainSegment();
                    outputTargetRunFrames = 1;
                    outputPreviousTarget = previousTarget;
                    outputHasCenter = false;
                }
                else
                    ++outputTargetRunFrames;

                const auto outputCents = 1200.0 * std::log2 (outputTracking.stableF0Hz / 440.0);
                const auto hopSeconds = static_cast<double> (outputTracking.analysisLatencySamples > 0
                                                                  ? 512 : 512) / reader.sampleRate;
                const auto centerAlpha = 1.0 - std::exp (-hopSeconds / outputCenterTimeSeconds);
                if (! outputHasCenter)
                {
                    outputCenterCents = outputCents;
                    outputHasCenter = true;
                }
                else
                    outputCenterCents += centerAlpha * (outputCents - outputCenterCents);

                if (outputTargetRunFrames >= 24)
                {
                    const auto residual = outputCents - outputCenterCents;
                    outputSegmentResidualSquareSum += residual * residual;
                    ++outputSegmentResidualFrames;
                }
            }
        }

        const auto tracking = processor.getPitchTrackingData();
        if (tracking.timestampSamples == lastTimestamp)
            continue;
        lastTimestamp = tracking.timestampSamples;
        const auto monitor = processor.getPitchMonitorData();
        if (! tracking.voiced || ! std::isfinite (tracking.stableF0Hz) || monitor.targetNote.isEmpty())
        {
            targetRunFrames = 0;
            previousTarget.clear();
            previousCorrection = std::numeric_limits<double>::quiet_NaN();
            continue;
        }

        if (! previousTarget.isEmpty() && monitor.targetNote != previousTarget)
            ++result.targetSwitches;
        targetRunFrames = monitor.targetNote == previousTarget ? targetRunFrames + 1 : 1;
        previousTarget = monitor.targetNote;

        const auto correction = std::abs (static_cast<double> (monitor.correctionCents));
        correctionSum += correction;
        ++result.voicedFrames;
        if (targetRunFrames >= 24) // >=256 ms at the 512-sample Alto hop
        {
            sustainedCorrectionSum += correction;
            ++result.sustainedFrames;
        }
        if (std::isfinite (previousCorrection))
            result.maxCorrectionStep = std::fmax (result.maxCorrectionStep, std::abs (correction - previousCorrection));
        previousCorrection = correction;
    }

    if (result.voicedFrames > 0)
        result.meanAbsoluteCorrection = correctionSum / static_cast<double> (result.voicedFrames);
    if (result.sustainedFrames > 0)
        result.sustainedAbsoluteCorrection = sustainedCorrectionSum / static_cast<double> (result.sustainedFrames);
    finishOutputSustainSegment();
    if (result.outputResidualFrames > 0)
    {
        result.outputResidualRms /= static_cast<double> (result.outputResidualFrames);
        result.outputVibratoDepth = std::sqrt (2.0) * result.outputResidualRms;
    }
    return result;
}

void printResult (const char* label, float value, const Result& result)
{
    std::fprintf (stderr,
                  "%s | value=%g voiced_frames=%d sustained_frames=%d mean_abs_correction_ct=%.3f sustained_abs_correction_ct=%.3f target_switches=%d output_sustain_segments=%d output_residual_frames=%d output_residual_rms_ct=%.3f output_vibrato_depth_ct=%.3f max_correction_step_ct=%.3f finite=%d\n",
                  label, value, result.voicedFrames, result.sustainedFrames,
                  result.meanAbsoluteCorrection, result.sustainedAbsoluteCorrection,
                  result.targetSwitches, result.outputSustainSegments, result.outputResidualFrames,
                  result.outputResidualRms, result.outputVibratoDepth,
                  result.maxCorrectionStep, result.finite ? 1 : 0);
}
}

int main (int argc, char* argv[])
{
    if (argc != 2 && argc != 4)
    {
        std::fprintf (stderr, "usage: VibeAutotuneVocalControlWavTest <mono-or-stereo-wav> [retune|humanize|flex|natural-vibrato value]\n");
        return 2;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (argv[1])));
    if (reader == nullptr)
    {
        std::fprintf (stderr, "unable to read wav\n");
        return 2;
    }

    if (argc == 4)
    {
        const auto value = std::strtof (argv[3], nullptr);
        const auto mode = juce::String (argv[2]);
        if (mode == "retune")
            printResult ("wav_retune", value, runCase (*reader, value, 0.0f));
        else if (mode == "humanize")
            printResult ("wav_humanize", value, runCase (*reader, 10.0f, value));
        else if (mode == "flex")
            printResult ("wav_flex", value, runCase (*reader, 25.0f, 0.0f, value));
        else if (mode == "natural-vibrato")
            printResult ("wav_natural_vibrato", value, runCase (*reader, 25.0f, 0.0f, 0.0f, value));
        else
        {
            std::fprintf (stderr, "mode must be retune, humanize, flex, or natural-vibrato\n");
            return 2;
        }
        return 0;
    }

    for (const auto retune : { 0.0f, 25.0f, 100.0f })
        printResult ("wav_retune", retune, runCase (*reader, retune, 0.0f));
}
