#include "PluginProcessor.h"

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

    const auto inputHz = midiToHz (inputMidi);
    constexpr int totalSamples = static_cast<int> (sampleRate * 3.0);
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;
    std::vector<float> output;
    output.reserve (totalSamples);
    auto phase = 0.0;
    auto sawLegalTarget = false;

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
            require (isCMajor (target), "C Major live target contained an illegal pitch class");
            sawLegalTarget = true;
        }
    }

    require (sawLegalTarget, "C Major live path did not acquire a target");
    const auto outputF0 = estimateF0 (output, processor.getLatencySamples() + static_cast<int> (sampleRate),
                                      processor.getLatencySamples() + static_cast<int> (sampleRate * 2.0));
    const auto outputMidi = static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (outputF0 / 440.0)));
    require (std::isfinite (outputF0) && isCMajor (outputMidi), "C Major renderer output remained outside the selected scale");
    std::cout << "key_scale_live | input=" << inputMidi << " output=" << outputMidi << " target=legal\n";
}
}

int main()
{
    for (const auto illegalMidi : { 61, 63, 66, 68, 70 }) // C#, D#, F#, G#, A#
        runIllegalCMajorTone (illegalMidi);
}
