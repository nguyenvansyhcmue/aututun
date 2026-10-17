#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    VibeAutotuneProcessor processor;
    processor.setPlayConfigDetails (1, 1, 48000.0, 512);
    processor.prepareToPlay (48000.0, 512);

    // Reference-only visual state. This never changes production defaults;
    // it lets the pixel snapshot exercise arcs, pointers and selectors at the
    // same landmarks as the supplied target artwork.
    const auto setParameter = [&] (const char* id, float value)
    {
        if (auto* parameter = processor.parameters.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    };
    setParameter ("retuneSpeed", 100.0f);
    setParameter ("flexTune", 50.0f);
    setParameter ("humanize", 30.0f);
    setParameter ("naturalVibrato", 2.4f);
    setParameter ("tracking", 50.0f);
    setParameter ("voicedColour", 25.0f);
    setParameter ("inputType", 2.0f); // Tenor
    setParameter ("key", 7.0f);       // G
    setParameter ("scale", 1.0f);     // Major

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    if (editor == nullptr)
        return 1;

    editor->setSize (1448, 1086);
   #if JUCE_DEBUG
    editor->keyPressed (juce::KeyPress ('r', juce::ModifierKeys(), 'r'));
   #endif

    juce::Image snapshot (juce::Image::ARGB, 1448, 1086, true);
    juce::Graphics graphics (snapshot);
    editor->paintEntireComponent (graphics, true);

    const auto output = juce::File (argc > 1 ? argv[1] : "/tmp/VibeAutotuneEditorSnapshot.png");
    if (auto stream = output.createOutputStream())
    {
        juce::PNGImageFormat png;
        return png.writeImageToStream (snapshot, *stream) ? 0 : 2;
    }
    return 3;
}
