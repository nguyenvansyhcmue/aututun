#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
double estimateYinF0 (const std::vector<float>& frame, double sampleRate)
{
    constexpr int minimumLag = 80;   // 600 Hz at 48 kHz
    constexpr int maximumLag = 512;  // 94 Hz at 48 kHz
    constexpr double threshold = 0.15;
    const auto usable = static_cast<int> (frame.size()) - maximumLag;
    if (usable <= 0)
        return std::numeric_limits<double>::quiet_NaN();

    std::vector<double> difference (maximumLag + 1, 0.0);
    double frameEnergy = 0.0;
    for (int i = 0; i < usable; ++i)
        frameEnergy += static_cast<double> (frame[static_cast<size_t> (i)])
                     * static_cast<double> (frame[static_cast<size_t> (i)]);
    if (frameEnergy / usable < 1.0e-7)
        return std::numeric_limits<double>::quiet_NaN();

    for (int lag = 1; lag <= maximumLag; ++lag)
        for (int i = 0; i < usable; ++i)
        {
            const auto delta = static_cast<double> (frame[static_cast<size_t> (i)])
                             - static_cast<double> (frame[static_cast<size_t> (i + lag)]);
            difference[static_cast<size_t> (lag)] += delta * delta;
        }

    double cumulative = 0.0;
    auto chosenLag = -1;
    auto bestValue = std::numeric_limits<double>::infinity();
    auto bestLag = minimumLag;
    for (int lag = 1; lag <= maximumLag; ++lag)
    {
        cumulative += difference[static_cast<size_t> (lag)];
        if (lag < minimumLag || cumulative <= 1.0e-12)
            continue;
        const auto cmnd = difference[static_cast<size_t> (lag)] * lag / cumulative;
        if (cmnd < bestValue) { bestValue = cmnd; bestLag = lag; }
        if (cmnd < threshold
            && lag < maximumLag
            && difference[static_cast<size_t> (lag)] <= difference[static_cast<size_t> (lag + 1)])
        {
            chosenLag = lag;
            break;
        }
    }
    const auto lag = chosenLag >= 0 ? chosenLag : bestLag;
    if (lag <= minimumLag || lag >= maximumLag)
        return std::numeric_limits<double>::quiet_NaN();

    const auto left = difference[static_cast<size_t> (lag - 1)];
    const auto centre = difference[static_cast<size_t> (lag)];
    const auto right = difference[static_cast<size_t> (lag + 1)];
    const auto denominator = left - 2.0 * centre + right;
    const auto fractional = std::abs (denominator) > 1.0e-12
                          ? std::clamp (0.5 * (left - right) / denominator, -0.5, 0.5)
                          : 0.0;
    return sampleRate / (static_cast<double> (lag) + fractional);
}
}

int main (int argc, char* argv[])
{
    if (argc != 2)
        return 2;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (argv[1])));
    if (reader == nullptr)
        return 3;

    constexpr int frameSize = 2048;
    constexpr int hop = 256;
    std::vector<float> frame (frameSize, 0.0f);
    juce::AudioBuffer<float> block (1, hop);
    std::cout << "sample,independent_f0_hz\n";
    for (int64_t position = 0; position + hop <= reader->lengthInSamples; position += hop)
    {
        reader->read (&block, 0, hop, position, true, false);
        std::move (frame.begin() + hop, frame.end(), frame.begin());
        std::copy (block.getReadPointer (0), block.getReadPointer (0) + hop, frame.end() - hop);
        if (position + hop < frameSize)
            continue;
        const auto f0 = estimateYinF0 (frame, reader->sampleRate);
        std::cout << position + hop << ',';
        if (std::isfinite (f0)) std::cout << f0;
        else std::cout << "nan";
        std::cout << '\n';
    }
}
