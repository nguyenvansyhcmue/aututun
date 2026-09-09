#include "PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <limits>

int main (int argc, char* argv[])
{
    if (argc != 2)
        return 2;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (argv[1])));
    if (reader == nullptr)
        return 3;

    constexpr int block = 256;
    VibeAutotuneProcessor probe;
    probe.prepareToPlay (reader->sampleRate, block);
    // The pitch tracker runs before rendering. These values merely make the
    // probe's own output irrelevant and keep tracking policy deterministic.
    const auto set = [&probe] (const char* id, float value)
    {
        auto* parameter = probe.parameters.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    set ("bypass", 0.0f); set ("inputType", 1.0f); set ("tracking", 50.0f);
    set ("retuneSpeed", 400.0f); set ("flexTune", 0.0f);

    juce::AudioBuffer<float> audio (2, block);
    juce::MidiBuffer midi;
    uint64_t previousTimestamp = std::numeric_limits<uint64_t>::max();
    std::cout << "sample,stable_f0_hz,voiced\n";
    for (int64_t position = 0; position < reader->lengthInSamples; position += block)
    {
        audio.clear();
        const auto count = static_cast<int> (juce::jmin<int64_t> (block, reader->lengthInSamples - position));
        reader->read (&audio, 0, count, position, true, true);
        probe.processBlock (audio, midi);
        const auto tracking = probe.getPitchTrackingData();
        if (tracking.timestampSamples == previousTimestamp)
            continue;
        previousTimestamp = tracking.timestampSamples;
        std::cout << tracking.timestampSamples << ',';
        if (std::isfinite (tracking.stableF0Hz)) std::cout << tracking.stableF0Hz;
        else std::cout << "nan";
        std::cout << ',' << (tracking.voiced ? 1 : 0) << '\n';
    }
}
