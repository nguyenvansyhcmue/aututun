#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;
constexpr const char* artistIdentifier = "AudioUnit:Effects/aumf,ATxL,VST";

struct RenderCase
{
    juce::File input;
    juce::File output;
    float retuneMs = 10.0f;
    float flexTune = 0.0f;
    float humanize = 0.0f;
    float naturalVibrato = 0.0f;
};

juce::PluginDescription findArtist (juce::AudioPluginFormatManager& formats)
{
    juce::OwnedArray<juce::PluginDescription> descriptions;
    for (auto* format : formats.getFormats())
        if (format->getName().containsIgnoreCase ("AudioUnit"))
            format->findAllTypesForFile (descriptions, artistIdentifier);

    if (! descriptions.isEmpty())
        return *descriptions[0];
    return {};
}

juce::AudioProcessorParameter* findParameter (juce::AudioPluginInstance& plugin, const juce::String& name)
{
    for (auto* parameter : plugin.getParameters())
        if (parameter != nullptr && parameter->getName (128).equalsIgnoreCase (name))
            return parameter;
    return nullptr;
}

bool setParameterText (juce::AudioPluginInstance& plugin, const juce::String& name, const juce::String& value)
{
    auto* parameter = findParameter (plugin, name);
    if (parameter == nullptr)
    {
        std::cerr << "missing Artist parameter: " << name << '\n';
        return false;
    }
    parameter->setValueNotifyingHost (parameter->getValueForText (value));
    const auto actual = parameter->getCurrentValueAsText();
    std::cout << "parameter," << name << ",requested=" << value << ",actual=" << actual << '\n';
    if (actual.trim().isEmpty())
    {
        std::cerr << "Artist did not accept " << name << '=' << value << '\n';
        return false;
    }
    return true;
}

bool setParameterNormalised (juce::AudioPluginInstance& plugin, const juce::String& name, float value,
                             const juce::String& expectedText)
{
    auto* parameter = findParameter (plugin, name);
    if (parameter == nullptr)
    {
        std::cerr << "missing Artist parameter: " << name << '\n';
        return false;
    }
    parameter->setValueNotifyingHost (value);
    const auto actual = parameter->getCurrentValueAsText();
    std::cout << "parameter," << name << ",requested=" << expectedText << ",actual=" << actual << '\n';
    if (! actual.equalsIgnoreCase (expectedText))
    {
        std::cerr << "Artist did not accept " << name << '=' << expectedText << '\n';
        return false;
    }
    return true;
}

bool configureArtist (juce::AudioPluginInstance& plugin, const RenderCase& renderCase)
{
    // This is the fixed black-box reference preset. Each render changes only
    // one of Flex/Humanize/Natural Vibrato, supplied by RenderCase.
    // Artist exposes these as AU enums but does not parse their display text
    // through getValueForText(). Their stable normalized indices are 1/4,
    // 6/11, and 1/27 respectively, verified by the AU inspector.
    return setParameterNormalised (plugin, "Input Type", 1.0f / 4.0f, "Alto-Tenor")
        && setParameterNormalised (plugin, "Key", 6.0f / 11.0f, "F#")
        && setParameterNormalised (plugin, "Scale", 1.0f / 27.0f, "Minor")
        && setParameterText (plugin, "Detune", "0")
        && setParameterText (plugin, "Formant Correction", "Off")
        && setParameterText (plugin, "Use Classic Mode DSP", "0")
        && setParameterText (plugin, "Tracking", "50")
        && setParameterText (plugin, "Stereo Reference", "Left")
        && setParameterText (plugin, "Transpose", "0")
        && setParameterText (plugin, "Retune Speed", juce::String (renderCase.retuneMs))
        && setParameterText (plugin, "Flex-Tune", juce::String (renderCase.flexTune))
        && setParameterText (plugin, "Humanize", juce::String (renderCase.humanize))
        && setParameterText (plugin, "Natural Vibrato", juce::String (renderCase.naturalVibrato));
}

bool renderCase (juce::AudioPluginFormatManager& formats, const juce::PluginDescription& description,
                 juce::AudioFormatManager& audioFormats, const RenderCase& renderCase)
{
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (renderCase.input));
    if (reader == nullptr || reader->sampleRate != sampleRate)
    {
        std::cerr << "expected 48 kHz input: " << renderCase.input.getFullPathName() << '\n';
        return false;
    }

    juce::String error;
    auto plugin = formats.createPluginInstance (description, sampleRate, blockSize, error);
    if (plugin == nullptr)
    {
        std::cerr << "cannot instantiate Artist: " << error << '\n';
        return false;
    }
    if (! configureArtist (*plugin, renderCase))
        return false;

    // Start from the AU's declared buses. A default-constructed layout has
    // no bus at index zero and assigning into it crashes before processing.
    auto monoLayout = plugin->getBusesLayout();
    monoLayout.getMainInputChannelSet() = juce::AudioChannelSet::stereo();
    monoLayout.getMainOutputChannelSet() = juce::AudioChannelSet::stereo();
    if (! plugin->setBusesLayout (monoLayout))
    {
        std::cerr << "Artist rejected stereo bus layout\n";
        return false;
    }
    plugin->setPlayConfigDetails (2, 2, sampleRate, blockSize);
    plugin->prepareToPlay (sampleRate, blockSize);
    plugin->reset();

    renderCase.output.getParentDirectory().createDirectory();
    std::unique_ptr<juce::FileOutputStream> stream (renderCase.output.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        stream != nullptr ? wav.createWriterFor (stream.get(), sampleRate, 1, 24, {}, 0) : nullptr);
    if (writer == nullptr)
    {
        std::cerr << "cannot write " << renderCase.output.getFullPathName() << '\n';
        plugin->releaseResources();
        return false;
    }
    stream.release();

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int64_t position = 0; position < reader->lengthInSamples; position += blockSize)
    {
        const auto count = static_cast<int> (juce::jmin<int64_t> (blockSize, reader->lengthInSamples - position));
        buffer.clear();
        reader->read (&buffer, 0, count, position, true, false);
        buffer.copyFrom (1, 0, buffer, 0, 0, count);
        plugin->processBlock (buffer, midi);
        writer->writeFromAudioSampleBuffer (buffer, 0, count);
    }
    plugin->releaseResources();
    std::cout << "rendered," << renderCase.output.getFullPathName() << '\n';
    return true;
}

juce::String stemWithoutWav (const juce::File& file)
{
    return file.getFileNameWithoutExtension();
}

std::vector<RenderCase> makeCases (const juce::File& inputDirectory, const juce::File& outputDirectory, bool all)
{
    const auto makeOutput = [&] (const juce::File& input, const juce::String& suffix)
    {
        return outputDirectory.getChildFile (stemWithoutWav (input) + suffix + ".wav");
    };
    std::vector<RenderCase> result;
    const auto smokeInput = inputDirectory.getChildFile ("flex_fs4_plus30ct_3s.wav");
    result.push_back ({ smokeInput, makeOutput (smokeInput, "_artist_flex50_retune25"), 25.0f, 50.0f });
    if (! all)
        return result;

    result.clear();
    const auto appendMatrix = [&] (const juce::String& wildcard, const juce::String& label,
                                   const std::function<void (RenderCase&, float)>& setValue,
                                   const std::array<float, 2>* retunes)
    {
        juce::Array<juce::File> inputs;
        inputDirectory.findChildFiles (inputs, juce::File::findFiles, false, wildcard);
        for (const auto& input : inputs)
            for (const auto value : std::array<float, 5> { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
            {
                const auto emit = [&] (float retune)
                {
                    auto suffix = "_artist_" + label + juce::String (static_cast<int> (value));
                    if (retunes != nullptr)
                        suffix += "_retune" + juce::String (static_cast<int> (retune));
                    RenderCase c { input, makeOutput (input, suffix), retune };
                    setValue (c, value);
                    result.push_back (std::move (c));
                };
                if (retunes != nullptr) for (const auto retune : *retunes) emit (retune); else emit (10.0f);
            }
    };
    const std::array<float, 2> flexRetunes { 0.0f, 25.0f };
    appendMatrix ("flex_*.wav", "flex", [] (RenderCase& c, float value) { c.flexTune = value; }, &flexRetunes);
    appendMatrix ("humanize_*.wav", "humanize", [] (RenderCase& c, float value) { c.humanize = value; }, nullptr);
    appendMatrix ("natural_vibrato_*.wav", "naturalvibrato", [] (RenderCase& c, float value) { c.naturalVibrato = value; }, nullptr);
    return result;
}
}

int main (int argc, char* argv[])
{
    const auto all = argc >= 2 && juce::String::fromUTF8 (argv[1]).equalsIgnoreCase ("all");
    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile ("benchmark/autotune-artist-controls");
    const auto inputDirectory = root.getChildFile ("input");
    const auto outputDirectory = root.getChildFile ("artist-renders");

    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager pluginFormats;
    juce::addHeadlessDefaultFormatsToManager (pluginFormats);
    const auto artist = findArtist (pluginFormats);
    if (artist.name.isEmpty())
    {
        std::cerr << "Auto-Tune Artist is not registered as " << artistIdentifier << '\n';
        return 1;
    }
    juce::AudioFormatManager audioFormats;
    audioFormats.registerBasicFormats();
    const auto cases = makeCases (inputDirectory, outputDirectory, all);
    std::cout << "cases=" << cases.size() << '\n';
    for (const auto& c : cases)
        if (! renderCase (pluginFormats, artist, audioFormats, c))
            return 2;
    return 0;
}
