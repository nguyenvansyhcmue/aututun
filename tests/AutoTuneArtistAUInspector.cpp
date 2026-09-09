#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager formats;
    juce::addHeadlessDefaultFormatsToManager (formats);

    juce::OwnedArray<juce::PluginDescription> descriptions;
    // AU registry identifier from `auval -a`: aumf / ATxL / VST.
    // Instantiating by registry identifier is more reliable than scanning a
    // component bundle, especially for copy-protected commercial AUs.
    const auto artistIdentifier = "AudioUnit:Effects/aumf,ATxL,VST";
    for (auto* format : formats.getFormats())
    {
        if (format->getName().containsIgnoreCase ("AudioUnit"))
            format->findAllTypesForFile (descriptions, artistIdentifier);
    }
    if (descriptions.isEmpty())
    {
        std::cerr << "Auto-Tune Artist AU was not discovered from " << artistIdentifier << '\n';
        return 1;
    }

    const auto& description = *descriptions[0];
    std::cout << "name=" << description.name << '\n'
              << "format=" << description.pluginFormatName << '\n'
              << "identifier=" << description.fileOrIdentifier << '\n';
    juce::String error;
    auto instance = formats.createPluginInstance (description, 48000.0, 256, error);
    if (instance == nullptr)
    {
        std::cerr << "unable to instantiate Auto-Tune Artist: " << error << '\n';
        return 2;
    }

    const auto parameters = instance->getParameters();
    std::cout << "parameter_count=" << parameters.size() << '\n';
    for (auto* parameter : parameters)
    {
        if (parameter == nullptr)
            continue;
        std::cout << parameter->getParameterIndex() << ','
                  << parameter->getName (128) << ','
                  << parameter->getCurrentValueAsText() << ','
                  << parameter->getNumSteps();
        const auto choices = parameter->getAllValueStrings();
        if (! choices.isEmpty())
            std::cout << ",choices=" << choices.joinIntoString ("|");
        std::cout << '\n';
    }
    for (const auto& name : { "Input Type", "Key", "Scale" })
    {
        juce::AudioProcessorParameter* parameter = nullptr;
        for (auto* candidate : parameters)
            if (candidate != nullptr && candidate->getName (128).equalsIgnoreCase (name))
                parameter = candidate;
        if (parameter == nullptr)
            continue;
        juce::String previous;
        std::cout << "mapping," << name << '=';
        for (int step = 0; step <= 1000; ++step)
        {
            const auto text = parameter->getText (static_cast<float> (step) / 1000.0f, 64);
            if (text != previous)
            {
                if (previous.isNotEmpty()) std::cout << '|';
                std::cout << step << ':' << text;
                previous = text;
            }
        }
        std::cout << '\n';
    }
    instance->releaseResources();
    return 0;
}
