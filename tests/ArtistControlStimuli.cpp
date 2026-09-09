#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

struct Stimulus
{
    std::string fileName;
    double seconds = 3.0;
    double centreCents = 0.0;
    double vibratoHz = 0.0;
    double vibratoDepthCents = 0.0;
    double noteDurationSeconds = 3.0;
    double driftCentsPerSecond = 0.0;
};

float interpolateTemplate (const juce::AudioBuffer<float>& source, double position)
{
    const auto first = static_cast<int> (std::floor (position));
    if (first < 0 || first + 1 >= source.getNumSamples())
        return 0.0f;
    const auto fraction = static_cast<float> (position - first);
    const auto left = source.getSample (0, first);
    const auto right = source.getSample (0, first + 1);
    return left + (right - left) * fraction;
}

bool writeStimulus (const juce::File& outputDirectory, const juce::AudioBuffer<float>& sourceTemplate,
                    const Stimulus& stimulus)
{
    const auto samples = static_cast<int> (std::lround (stimulus.seconds * sampleRate));
    juce::AudioBuffer<float> audio (1, samples);
    // This source has already produced a correct Auto-Tune Artist F# render
    // in the colour benchmark. Its measured centre is F#4 -30 cents. Using
    // it avoids calibrating a commercial vocal tracker against an artificial
    // harmonic stack that it does not classify like a real vowel.
    constexpr double templateCentreCents = -30.0;
    auto sourcePosition = 0.0;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto time = static_cast<double> (sample) / sampleRate;
        const auto noteActive = time < stimulus.noteDurationSeconds;
        const auto cents = stimulus.centreCents
                         + stimulus.driftCentsPerSecond * time
                         + stimulus.vibratoDepthCents * std::sin (twoPi * stimulus.vibratoHz * time);
        const auto readRate = std::exp2 ((cents - templateCentreCents) / 1200.0);
        const auto value = noteActive ? interpolateTemplate (sourceTemplate, sourcePosition) : 0.0f;
        audio.setSample (0, sample, value);
        sourcePosition += readRate;
    }

    const auto file = outputDirectory.getChildFile (stimulus.fileName);
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        stream != nullptr ? wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0) : nullptr);
    if (writer == nullptr)
        return false;
    stream.release();
    return writer->writeFromAudioSampleBuffer (audio, 0, samples);
}
}

int main (int argc, char* argv[])
{
    const juce::File outputDirectory (argc == 2
        ? juce::String::fromUTF8 (argv[1])
        : juce::File::getCurrentWorkingDirectory().getChildFile ("benchmark/autotune-artist-controls/input"));
    if (! outputDirectory.createDirectory())
    {
        std::cerr << "unable to create " << outputDirectory.getFullPathName() << '\n';
        return 1;
    }

    const auto templateFile = juce::File::getCurrentWorkingDirectory().getChildFile (
        "benchmark/autotune-artist-colour-probe/fsharp-minor/vowel_fs4_flat30_minus12dbfs.wav");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (templateFile));
    if (reader == nullptr || reader->sampleRate != sampleRate)
    {
        std::cerr << "missing 48 kHz verified vowel template: " << templateFile.getFullPathName() << '\n';
        return 2;
    }
    juce::AudioBuffer<float> sourceTemplate (1, static_cast<int> (reader->lengthInSamples));
    reader->read (&sourceTemplate, 0, sourceTemplate.getNumSamples(), 0, true, false);

    std::vector<Stimulus> stimuli;
    for (const auto cents : std::array<int, 8> { -60, -30, -10, -5, 5, 10, 30, 60 })
    {
        const auto sign = cents < 0 ? "minus" : "plus";
        const auto magnitude = std::abs (cents);
        stimuli.push_back ({ "flex_fs4_" + std::string (sign)
                              + (magnitude < 10 ? "0" : "") + std::to_string (magnitude) + "ct_3s.wav",
                              3.0, static_cast<double> (cents) });
    }

    stimuli.insert (stimuli.end(), {
        { "humanize_fs4_plus30ct_vibrato5hz_30ct_3s.wav", 3.0, 30.0, 5.0, 30.0 },
        { "humanize_fs4_plus30ct_drift20ctps_3s.wav", 3.0, 30.0, 0.0, 0.0, 3.0, 20.0 },
        { "humanize_fs4_short200ms_plus30ct_vibrato5hz_30ct.wav", 1.0, 30.0, 5.0, 30.0, 0.200 },
        { "natural_vibrato_fs4_4hz_10ct_3s.wav", 3.0, 0.0, 4.0, 10.0 },
        { "natural_vibrato_fs4_6hz_30ct_3s.wav", 3.0, 0.0, 6.0, 30.0 },
        { "natural_vibrato_fs4_8hz_60ct_3s.wav", 3.0, 0.0, 8.0, 60.0 },
        { "control_fs4_flat_3s.wav", 3.0 }
    });

    auto succeeded = true;
    for (const auto& stimulus : stimuli)
    {
        succeeded = writeStimulus (outputDirectory, sourceTemplate, stimulus) && succeeded;
        std::cout << outputDirectory.getChildFile (stimulus.fileName).getFullPathName() << '\n';
    }
    return succeeded ? 0 : 1;
}
