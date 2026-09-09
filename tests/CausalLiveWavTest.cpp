#include "CausalTDPSOLARenderer.h"
#include "LiveControlPipeline.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <fstream>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

struct ControlFrame { double f0 = 0.0, ratio = 1.0; bool voiced = false; };

int main (int argc, char* argv[])
{
    if (argc != 4 && argc != 5 && argc != 6)
    {
        std::cerr << "usage: CausalLiveWavTest dry.wav diagnostic.csv output.wav [live-preset[=retune-ms,flex-percent,humanize-percent,natural-vibrato-percent]] [baseline|no-residual|hann-only|psola-only|epoch-anchored]\n";
        return 2;
    }
    std::ifstream csv (argv[2]);
    std::string line;
    std::getline (csv, line);
    std::vector<std::string> header;
    {
        std::stringstream stream (line);
        std::string column;
        while (std::getline (stream, column, ',')) header.push_back (column);
    }
    const auto columnIndex = [&header] (std::initializer_list<const char*> names)
    {
        for (const auto* name : names)
            if (const auto found = std::find (header.begin(), header.end(), name); found != header.end())
                return static_cast<int> (std::distance (header.begin(), found));
        return -1;
    };
    const auto stableF0Column = columnIndex ({ "stableF0", "stable_f0" });
    const auto ratioColumn = columnIndex ({ "requestedRendererRatio", "requested_ratio" });
    const auto voicedColumn = columnIndex ({ "voiced" });
    if (stableF0Column < 0 || ratioColumn < 0 || voicedColumn < 0)
    {
        std::cerr << "diagnostic CSV is missing stable F0, renderer ratio, or voiced columns\n";
        return 3;
    }
    std::vector<ControlFrame> controls;
    while (std::getline (csv, line))
    {
        std::stringstream stream (line);
        std::vector<std::string> columns;
        std::string column;
        while (std::getline (stream, column, ',')) columns.push_back (column);
        if (columns.size() <= static_cast<size_t> (std::max ({ stableF0Column, ratioColumn, voicedColumn }))) continue;
        try
        {
            const auto f0 = columns[static_cast<size_t> (stableF0Column)] == "nan" ? 0.0
                     : std::stod (columns[static_cast<size_t> (stableF0Column)]);
            const auto ratio = std::stod (columns[static_cast<size_t> (ratioColumn)]);
            controls.push_back ({ f0, ratio, columns[static_cast<size_t> (voicedColumn)] == "1" });
        }
        catch (...) {}
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File inputFile (argv[1]);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (inputFile));
    if (reader == nullptr || controls.empty()) return 3;
    const auto samples = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> input (1, samples), rendered (2, samples);
    reader->read (&input, 0, samples, 0, true, false);
    CausalTDPSOLARenderer renderer;
    CausalTDPSOLARenderer::DiagnosticAblation ablation;
    const auto ablationName = argc == 6 ? std::string (argv[5]) : std::string ("baseline");
    if (ablationName == "no-residual")
        ablation.periodicResidual = false;
    else if (ablationName == "hann-only")
        ablation.correctionWindowBlend = false;
    else if (ablationName == "psola-only")
        ablation = { false, false, false };
    else if (ablationName == "epoch-anchored")
        ablation.waveformAnchoredSourceMarks = true;
    else if (ablationName != "baseline")
        return 2;
    renderer.setDiagnosticAblation (ablation);
    constexpr int block = 128;
    renderer.prepare (reader->sampleRate, block, 256);
    const auto presetArgument = argc == 5 ? std::string (argv[4]) : std::string {};
    const auto useLivePreset = presetArgument.rfind ("live-preset", 0) == 0;
    auto presetRetuneMs = 0.0;
    auto presetFlexPercent = 0.0;
    auto presetHumanizePercent = 0.0;
    auto presetNaturalVibratoPercent = 0.0;
    const auto equals = presetArgument.find ('=');
    if (useLivePreset && equals != std::string::npos)
    {
        try
        {
            const auto comma = presetArgument.find (',', equals + 1);
            presetRetuneMs = std::clamp (std::stod (presetArgument.substr (equals + 1, comma - equals - 1)), 0.0, 400.0);
            if (comma != std::string::npos)
            {
                const auto secondComma = presetArgument.find (',', comma + 1);
                presetFlexPercent = std::clamp (std::stod (presetArgument.substr (comma + 1, secondComma - comma - 1)), 0.0, 100.0);
                if (secondComma != std::string::npos)
                {
                    const auto thirdComma = presetArgument.find (',', secondComma + 1);
                    presetHumanizePercent = std::clamp (std::stod (presetArgument.substr (secondComma + 1, thirdComma - secondComma - 1)), 0.0, 100.0);
                    if (thirdComma != std::string::npos)
                        presetNaturalVibratoPercent = std::clamp (std::stod (presetArgument.substr (thirdComma + 1)), -100.0, 100.0);
                }
            }
        }
        catch (...) { return 2; }
    }
    LiveControlPipeline liveControl;
    LiveControlParameters liveParameters;
    NotePolicySnapshot chromaticPolicy;
    chromaticPolicy.pitchClassDefaults.fill (NoteRule::Legal);
    chromaticPolicy.midiNoteOverrides.fill (NoteRule::Legal);
    if (useLivePreset)
    {
        // Exact requested listening preset. The control is recomputed from
        // stable F0 instead of replaying the earlier CSV ratio (which was
        // generated with a different Flex setting).
        liveControl.prepare (reader->sampleRate, 64);
        liveParameters.referencePitchHz = 440.0;
        liveParameters.transpose = 0;
        liveParameters.retuneSpeedMs = presetRetuneMs;
        liveParameters.flexTunePercent = presetFlexPercent;
        liveParameters.humanizePercent = presetHumanizePercent;
        liveParameters.naturalVibratoPercent = presetNaturalVibratoPercent;
    }
    std::vector<float> output (block);
    auto maxStep = 0.0;
    auto previous = 0.0f;
    auto previousInput = 0.0f;
    auto inputMaxStep = 0.0;
    auto maximumNearUnityExcessStep = 0.0;
    auto maxStepSample = 0;
    ControlFrame maxStepControl;
    for (int start = 0; start < samples; start += block)
    {
        const auto count = std::min (block, samples - start);
        const auto& capturedControl = controls[std::min<size_t> (static_cast<size_t> (start / block), controls.size() - 1)];
        auto renderedControl = capturedControl;
        if (! useLivePreset)
            renderer.process (input.getReadPointer (0, start), output.data(), count,
                              capturedControl.f0, capturedControl.ratio, capturedControl.voiced);
        else
        {
            for (int offset = 0; offset < count;)
            {
                const auto liveFrame = offset == 0
                    ? liveControl.observePitch (capturedControl.f0, capturedControl.voiced, false,
                                                liveParameters, chromaticPolicy)
                    : liveControl.advance();
                const auto slice = std::min (liveControl.controlInterval(), count - offset);
                renderer.process (input.getReadPointer (0, start + offset), output.data() + offset, slice,
                                  liveFrame.stableF0Hz, liveFrame.pitchRatio, liveFrame.valid && liveFrame.voiced);
                renderedControl = { liveFrame.stableF0Hz, liveFrame.pitchRatio, liveFrame.valid && liveFrame.voiced };
                offset += slice;
            }
        }
        for (int i = 0; i < count; ++i)
        {
            const auto inputStep = std::abs (static_cast<double> (input.getSample (0, start + i) - previousInput));
            inputMaxStep = std::max (inputMaxStep, inputStep);
            previousInput = input.getSample (0, start + i);
            const auto step = std::abs (static_cast<double> (output[static_cast<size_t> (i)] - previous));
            const auto outputSample = start + i;
            const auto delayedInputSample = outputSample - renderer.latencySamples();
            const auto delayedDry = delayedInputSample >= 0 ? input.getSample (0, delayedInputSample) : 0.0f;
            const auto delayedDryPrevious = delayedInputSample > 0 ? input.getSample (0, delayedInputSample - 1) : 0.0f;
            const auto delayedDryStep = std::abs (static_cast<double> (delayedDry - delayedDryPrevious));
            const auto correctionCents = 1200.0 * std::log2 (std::max (renderedControl.ratio, 1.0e-12));
            if (outputSample >= renderer.latencySamples() && std::abs (correctionCents) < 3.0)
                maximumNearUnityExcessStep = std::max (maximumNearUnityExcessStep,
                                                       std::max (0.0, step - delayedDryStep));
            if (step > maxStep)
            {
                maxStep = step;
                maxStepSample = start + i;
                maxStepControl = renderedControl;
            }
            previous = output[static_cast<size_t> (i)];
            rendered.setSample (0, start + i, previous);
            rendered.setSample (1, start + i, previous);
        }
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (juce::File (argv[3]).createOutputStream());
    if (stream == nullptr) return 4;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.release(), reader->sampleRate,
                                                                            2, 16, {}, 0));
    if (writer == nullptr) return 5;
    writer->writeFromAudioSampleBuffer (rendered, 0, samples);
    std::cout << "causal_live_wav | frames=" << controls.size() << " latency=" << renderer.latencySamples()
              << " max_step=" << maxStep << " sample=" << maxStepSample
              << " f0=" << maxStepControl.f0 << " ratio=" << maxStepControl.ratio
              << " voiced=" << maxStepControl.voiced
              << " dry_step_at_max=" << std::abs (static_cast<double> (
                     (maxStepSample >= renderer.latencySamples() ? input.getSample (0, maxStepSample - renderer.latencySamples()) : 0.0f)
                     - (maxStepSample > renderer.latencySamples() ? input.getSample (0, maxStepSample - renderer.latencySamples() - 1) : 0.0f)))
              << " dry_max_step=" << inputMaxStep
              << " max_near_unity_excess_step=" << maximumNearUnityExcessStep
              << " preset=" << (useLivePreset ? "live" : "captured")
              << " retune_ms=" << (useLivePreset ? presetRetuneMs : -1.0)
              << " flex_percent=" << (useLivePreset ? presetFlexPercent : -1.0)
              << " humanize_percent=" << (useLivePreset ? presetHumanizePercent : -1.0)
              << " natural_vibrato_percent=" << (useLivePreset ? presetNaturalVibratoPercent : -999.0)
              << " ablation=" << ablationName
              << " output=" << argv[3] << '\n';
    return std::isfinite (maxStep) && maximumNearUnityExcessStep < 0.06 ? 0 : 1;
}
