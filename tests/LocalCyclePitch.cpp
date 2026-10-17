#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

// Deliberately local: one rising crossing per clean periodic cycle.  This is
// not a replacement F0 tracker for vocals; it is a transition microscope for
// the deterministic synthetic fixture.
int main (int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "usage: VibeAutotuneLocalCyclePitch <input-wav> <csv>\n";
        return 2;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File input (argv[1]);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (input));
    if (reader == nullptr || reader->numChannels == 0)
        return 2;

    juce::AudioBuffer<float> audio (1, static_cast<int> (reader->lengthInSamples));
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, false);
    std::vector<double> crossings;
    const auto* samples = audio.getReadPointer (0);
    for (int n = 1; n < audio.getNumSamples(); ++n)
    {
        const auto previous = static_cast<double> (samples[n - 1]);
        const auto current = static_cast<double> (samples[n]);
        if (previous <= 0.0 && current > 0.0 && current - previous > 1.0e-9)
            crossings.push_back (static_cast<double> (n - 1) - previous / (current - previous));
    }
    std::ofstream out (argv[2]);
    out << "cycleEndSample,periodSamples,f0Hz\n";
    out << std::fixed << std::setprecision (9);
    for (size_t i = 1; i < crossings.size(); ++i)
    {
        const auto period = crossings[i] - crossings[i - 1];
        if (period > 1.0)
            out << crossings[i] << ',' << period << ',' << reader->sampleRate / period << '\n';
    }
    return 0;
}
