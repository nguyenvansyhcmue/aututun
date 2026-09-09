#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double targetHz = 220.0;
constexpr double inputCents = 30.0;

void setParameter (VibeAutotuneProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.parameters.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

double estimateF0 (const std::vector<float>& samples, int start, int end)
{
    auto firstCrossing = std::numeric_limits<double>::quiet_NaN();
    auto lastCrossing = std::numeric_limits<double>::quiet_NaN();
    auto crossings = 0;

    for (int sample = start + 1; sample < end; ++sample)
    {
        const auto previous = static_cast<double> (samples[static_cast<size_t> (sample - 1)]);
        const auto current = static_cast<double> (samples[static_cast<size_t> (sample)]);
        if (previous <= 0.0 && current > 0.0)
        {
            const auto crossing = static_cast<double> (sample - 1) - previous / (current - previous);
            if (crossings == 0)
                firstCrossing = crossing;
            lastCrossing = crossing;
            ++crossings;
        }
    }

    return crossings >= 2 ? sampleRate * static_cast<double> (crossings - 1) / (lastCrossing - firstCrossing)
                          : std::numeric_limits<double>::quiet_NaN();
}

bool require (bool condition, const char* message)
{
    if (! condition)
    {
        std::fprintf (stderr, "FAILED: %s\n", message);
        return false;
    }
    return true;
}
}

int main()
{
    constexpr auto blockSize = 256;
    constexpr auto durationSamples = static_cast<int> (sampleRate * 4.0);
    const auto inputHz = targetHz * std::exp2 (inputCents / 1200.0);

    auto passed = true;
    for (const auto formantEnabled : { false, true })
    {
        VibeAutotuneProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        setParameter (processor, "bypass", 0.0f);
        setParameter (processor, "inputType", 1.0f);
        setParameter (processor, "key", 0.0f);
        setParameter (processor, "scale", 0.0f);
        setParameter (processor, "tracking", 50.0f);
        setParameter (processor, "retuneSpeed", 0.0f);
        setParameter (processor, "flexTune", 0.0f);
        setParameter (processor, "humanize", 0.0f);
        setParameter (processor, "naturalVibrato", 0.0f);
        setParameter (processor, "transpose", 0.0f);
        setParameter (processor, "referencePitch", 440.0f);
        setParameter (processor, "formant", formantEnabled ? 1.0f : 0.0f);

        juce::AudioBuffer<float> block (2, blockSize);
        juce::MidiBuffer midi;
        std::vector<float> output;
        output.reserve (durationSamples);
        auto phase = 0.0;
        auto minCorrection = std::numeric_limits<double>::infinity();
        auto maxRatio = 0.0;
        auto sawVoiced = false;

        for (int start = 0; start < durationSamples; start += blockSize)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = static_cast<float> (0.30 * std::sin (phase));
                phase += 2.0 * std::numbers::pi * inputHz / sampleRate;
                block.setSample (0, sample, value);
                block.setSample (1, sample, value);
            }

            processor.processBlock (block, midi);
            output.insert (output.end(), block.getReadPointer (0), block.getReadPointer (0) + blockSize);

            const auto monitor = processor.getPitchMonitorData();
            if (monitor.voiced)
            {
                sawVoiced = true;
                minCorrection = std::fmin (minCorrection, static_cast<double> (monitor.correctionCents));
                maxRatio = std::fmax (maxRatio, std::exp2 (static_cast<double> (monitor.correctionCents) / 1200.0));
            }
        }

        const auto latency = processor.getLatencySamples();
        // This is a single steady-state region, explicitly after the reported
        // renderer latency and long after YIN/trajectory acquisition.
        const auto outputF0 = estimateF0 (output, latency + 2 * static_cast<int> (sampleRate),
                                          latency + 3 * static_cast<int> (sampleRate));
        const auto outputCents = 1200.0 * std::log2 (outputF0 / targetHz);
        const auto inputStableCents = 1200.0 * std::log2 (inputHz / targetHz);
        const auto correctionRatio = std::exp2 (minCorrection / 1200.0);

        std::fprintf (stderr,
                      "retune_polarity | formant=%d input_cents=%0.3f correction_cents=%0.3f ratio=%0.9f output_hz=%0.6f output_cents=%0.3f latency=%d\n",
                      formantEnabled ? 1 : 0, inputStableCents, minCorrection, correctionRatio, outputF0, outputCents, latency);

        passed &= require (sawVoiced, "YIN did not report the stable sine as voiced");
        passed &= require (std::isfinite (minCorrection) && minCorrection < -1.0,
                           "sharp input did not produce a negative correction");
        passed &= require (correctionRatio < 1.0 && maxRatio <= 1.000001,
                           "sharp input did not send a pitch-down ratio to the renderer");
        passed &= require (std::isfinite (outputF0), "output F0 could not be measured");
        passed &= require (std::abs (outputCents) < 2.0,
                           "production output did not reach the calibrated target tolerance");
    }
    return passed ? 0 : 1;
}
