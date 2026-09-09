#include "CausalTDPSOLARenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>

namespace
{
using Contour = std::map<int64_t, double>;

Contour readContour (const char* path)
{
    Contour contour;
    std::ifstream stream (path);
    std::string line;
    std::getline (stream, line);
    while (std::getline (stream, line))
    {
        const auto firstComma = line.find (',');
        const auto secondComma = line.find (',', firstComma + 1);
        if (firstComma == std::string::npos || secondComma == std::string::npos)
            continue;
        const auto value = line.substr (firstComma + 1, secondComma - firstComma - 1);
        if (value == "nan")
            continue;
        contour.emplace (std::stoll (line.substr (0, firstComma)), std::stod (value));
    }
    return contour;
}

double valueAt (const Contour& contour, int64_t sample)
{
    const auto after = contour.lower_bound (sample);
    if (after == contour.end() || after == contour.begin())
        return after == contour.end() ? std::numeric_limits<double>::quiet_NaN() : after->second;
    const auto before = std::prev (after);
    const auto span = static_cast<double> (after->first - before->first);
    const auto t = std::clamp ((sample - before->first) / span, 0.0, 1.0);
    return before->second + (after->second - before->second) * t;
}
}

int main (int argc, char* argv[])
{
    if (argc != 5)
    {
        std::cerr << "usage: ArtistContourRendererDiagnostic <raw-wav> <raw-contour.csv> <artist-contour.csv> <output-wav>\n";
        return 2;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (argv[1])));
    const auto rawContour = readContour (argv[2]);
    const auto artistContour = readContour (argv[3]);
    if (reader == nullptr || rawContour.empty() || artistContour.empty())
        return 3;

    auto stream = juce::File (argv[4]).createOutputStream();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), reader->sampleRate, 1, 24, {}, 0));
    if (writer == nullptr)
        return 4;
    stream.release();

    constexpr int block = 128;
    CausalTDPSOLARenderer renderer;
    renderer.prepare (reader->sampleRate, block, 1152);
    juce::AudioBuffer<float> audio (1, block);
    for (int64_t position = 0; position < reader->lengthInSamples; position += block)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (block, reader->lengthInSamples - position));
        audio.clear();
        reader->read (&audio, 0, count, position, true, true);
        const auto time = position + count / 2;
        const auto rawHz = valueAt (rawContour, time);
        const auto artistHz = valueAt (artistContour, time);
        const auto voiced = std::isfinite (rawHz) && std::isfinite (artistHz) && rawHz > 0.0 && artistHz > 0.0;
        const auto ratio = voiced ? std::clamp (artistHz / rawHz, 0.90, 1.10) : 1.0;
        renderer.process (audio.getReadPointer (0), audio.getWritePointer (0), count,
                          rawHz, ratio, voiced, ! voiced, artistHz);
        writer->writeFromAudioSampleBuffer (audio, 0, count);
    }
    return 0;
}
