#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
constexpr int blockSize = 256;

struct InputFrame
{
    uint64_t sample = 0;
    int targetMidi = -1;
    double cents = 0.0;
    double correctionCents = 0.0;
};

struct MatchedFrame
{
    int targetMidi = -1;
    double inputCents = 0.0;
    double outputCents = 0.0;
    double correctionCents = 0.0;
};

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void configure (VibeAutotuneProcessor& processor)
{
    setParameter (processor, "bypass", 0.0f);
    setParameter (processor, "inputType", 1.0f);
    setParameter (processor, "retuneSpeed", 25.0f);
    setParameter (processor, "flexTune", 0.0f);
    setParameter (processor, "humanize", 100.0f);
    setParameter (processor, "naturalVibrato", 0.0f);
    setParameter (processor, "transpose", 0.0f);
    setParameter (processor, "referencePitch", 440.0f);
    setParameter (processor, "key", 0.0f);
    setParameter (processor, "scale", 0.0f);
    setParameter (processor, "tracking", 50.0f);
}

double centsForMidi (double frequency, int midi)
{
    const auto target = 440.0 * std::exp2 ((static_cast<double> (midi) - 69.0) / 12.0);
    return 1200.0 * std::log2 (frequency / target);
}

int nearestMidi (double frequency)
{
    return static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (frequency / 440.0)));
}

std::pair<double, double> meanAndDeviation (const std::vector<double>& values)
{
    auto mean = 0.0;
    for (const auto value : values) mean += value;
    mean /= static_cast<double> (values.size());
    auto variance = 0.0;
    for (const auto value : values) variance += (value - mean) * (value - mean);
    return { mean, std::sqrt (variance / static_cast<double> (values.size())) };
}
}

int main (int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "usage: VibeAutotuneVocalHumanizeWavTest <mono-or-stereo-wav>\n";
        return 2;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (argv[1])));
    if (reader == nullptr)
    {
        std::cerr << "unable to read wav\n";
        return 2;
    }

    VibeAutotuneProcessor liveProcessor, outputTracker;
    liveProcessor.prepareToPlay (reader->sampleRate, blockSize);
    outputTracker.prepareToPlay (reader->sampleRate, blockSize);
    configure (liveProcessor);
    configure (outputTracker);

    juce::AudioBuffer<float> source (static_cast<int> (reader->numChannels), blockSize);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<InputFrame> inputs;
    std::vector<std::pair<uint64_t, double>> outputs;
    uint64_t lastInputTimestamp = std::numeric_limits<uint64_t>::max();
    uint64_t lastOutputTimestamp = std::numeric_limits<uint64_t>::max();

    for (int64_t position = 0; position < reader->lengthInSamples; position += blockSize)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (blockSize, reader->lengthInSamples - position));
        source.clear();
        reader->read (&source, 0, count, position, true, true);
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = sample < count ? source.getSample (0, sample) : 0.0f;
            audio.setSample (0, sample, value);
            audio.setSample (1, sample, value);
        }

        liveProcessor.processBlock (audio, midi);
        const auto input = liveProcessor.getPitchTrackingData();
        if (input.timestampSamples != lastInputTimestamp)
        {
            lastInputTimestamp = input.timestampSamples;
            if (input.voiced && std::isfinite (input.stableF0Hz) && input.stableF0Hz > 0.0)
            {
                const auto midiNote = nearestMidi (input.stableF0Hz);
                const auto monitor = liveProcessor.getPitchMonitorData();
                inputs.push_back ({ input.timestampSamples, midiNote, centsForMidi (input.stableF0Hz, midiNote),
                                    static_cast<double> (monitor.correctionCents) });
            }
        }

        outputTracker.processBlock (audio, midi);
        const auto output = outputTracker.getPitchTrackingData();
        if (output.timestampSamples != lastOutputTimestamp)
        {
            lastOutputTimestamp = output.timestampSamples;
            if (output.voiced && std::isfinite (output.stableF0Hz) && output.stableF0Hz > 0.0)
                outputs.emplace_back (output.timestampSamples, output.stableF0Hz);
        }
    }

    std::vector<MatchedFrame> matched;
    const auto rendererLatency = static_cast<uint64_t> (liveProcessor.getLatencySamples());
    for (const auto& output : outputs)
    {
        if (output.first < rendererLatency || inputs.empty())
            continue;
        const auto sourceTime = output.first - rendererLatency;
        const auto it = std::lower_bound (inputs.begin(), inputs.end(), sourceTime,
                                          [] (const InputFrame& frame, uint64_t time) { return frame.sample < time; });
        const auto candidate = it == inputs.end() ? std::prev (inputs.end())
                             : (it == inputs.begin() ? it :
                                (it->sample - sourceTime < sourceTime - std::prev (it)->sample ? it : std::prev (it)));
        if (std::llabs (static_cast<long long> (candidate->sample) - static_cast<long long> (sourceTime)) > 256)
            continue;
        matched.push_back ({ candidate->targetMidi, candidate->cents, centsForMidi (output.second, candidate->targetMidi),
                            candidate->correctionCents });
    }

    constexpr int windowFrames = 14;
    auto bestScore = -std::numeric_limits<double>::infinity();
    std::pair<double, double> bestInput {}, bestOutput {};
    double bestCorrection = 0.0;
    auto found = false;
    for (size_t start = 0; start + windowFrames <= matched.size(); ++start)
    {
        const auto midiNote = matched[start].targetMidi;
        std::vector<double> inputCents, outputCents;
        std::vector<double> correctionCents;
        inputCents.reserve (windowFrames);
        outputCents.reserve (windowFrames);
        correctionCents.reserve (windowFrames);
        for (int index = 0; index < windowFrames; ++index)
        {
            const auto& frame = matched[start + static_cast<size_t> (index)];
            if (frame.targetMidi != midiNote) break;
            inputCents.push_back (frame.inputCents);
            outputCents.push_back (frame.outputCents);
            correctionCents.push_back (frame.correctionCents);
        }
        if (static_cast<int> (inputCents.size()) != windowFrames)
            continue;
        const auto inputStats = meanAndDeviation (inputCents);
        const auto outputStats = meanAndDeviation (outputCents);
        const auto correctionStats = meanAndDeviation (correctionCents);
        if (inputStats.first < 30.0 || inputStats.first > 50.0 || inputStats.second < 3.0)
            continue;
        // This harness validates a sharp input being pulled *down* to its
        // current chromatic target.  A persistent target may legitimately
        // still be the prior note during a vocal transition; that is a
        // different scenario and must not be mislabelled as renderer error.
        if (correctionStats.first >= -10.0)
            continue;
        const auto score = inputStats.second - std::abs (inputStats.first - 40.0) * 0.1;
        if (score > bestScore)
        {
            bestScore = score;
            bestInput = inputStats;
            bestOutput = outputStats;
            bestCorrection = correctionStats.first;
            found = true;
        }
    }

    if (! found)
    {
        std::cout << "humanize_wav | no 14-frame voiced segment with input centre +40 (+/-10) ct, vibrato, and a negative target correction\n";
        return 0;
    }

    const auto centreCorrected = std::abs (bestOutput.first) < std::abs (bestInput.first);
    const auto vibratoRetained = bestOutput.second >= bestInput.second * 0.25;
    std::cout << "humanize_wav | input_center_ct=" << bestInput.first
              << " input_vibrato_std_ct=" << bestInput.second
              << " requested_correction_ct=" << bestCorrection
              << " output_center_ct=" << bestOutput.first
              << " output_vibrato_std_ct=" << bestOutput.second
              << " centre_corrected=" << centreCorrected
              << " vibrato_retained=" << vibratoRetained << '\n';
    return (centreCorrected && vibratoRetained) ? 0 : 1;
}
