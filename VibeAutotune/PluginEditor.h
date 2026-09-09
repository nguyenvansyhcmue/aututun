#pragma once

#include "PluginProcessor.h"

class VibeAutotuneEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit VibeAutotuneEditor (VibeAutotuneProcessor& p);
    ~VibeAutotuneEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;

private:
    // The hardware-style controls use a right-hand zero point.  This only
    // changes the visual/gesture direction of the JUCE Slider; the attached
    // APVTS parameter still retains its normal range and automation mapping.
    class HardwareRotarySlider : public juce::Slider
    {
    public:
        double proportionOfLengthToValue (double proportion) override
        {
            return juce::Slider::proportionOfLengthToValue (1.0 - proportion);
        }

        double valueToProportionOfLength (double value) override
        {
            return 1.0 - juce::Slider::valueToProportionOfLength (value);
        }
    };

    // Retune has a musical, non-linear faceplate: the useful fast range gets
    // more angular resolution while the parameter/automation remains exactly
    // 0–400 ms.  Landmarks: 50 ms at 12 o'clock and 125 ms near 10 o'clock.
    class RetuneRotarySlider final : public HardwareRotarySlider
    {
    public:
        double proportionOfLengthToValue (double proportion) override
        {
            const auto p = juce::jlimit (0.0, 1.0, proportion);
            if (p >= 0.5)
                return 100.0 * (1.0 - p);
            if (p >= 2.0 / 7.0)
                return 50.0 + (0.5 - p) * 350.0;
            return 125.0 + (2.0 / 7.0 - p) * 962.5;
        }

        double valueToProportionOfLength (double value) override
        {
            const auto ms = juce::jlimit (0.0, 400.0, value);
            if (ms <= 50.0)
                return 1.0 - ms * 0.01;
            if (ms <= 125.0)
                return 0.5 - (ms - 50.0) / 350.0;
            return (2.0 / 7.0) - (ms - 125.0) / 962.5;
        }
    };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    void configureKnob (juce::Slider& slider, juce::Label& label,
                        const juce::String& title, const juce::String& suffix,
                        double defaultValue);
    void configureSelector (juce::ComboBox& selector, juce::Label& label,
                            const juce::String& title, const juce::StringArray& choices);
    void drawPanel (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title) const;
    void drawKnobBackdrop (juce::Graphics& g, juce::Rectangle<float> area) const;
    void drawPianoKeyboard (juce::Graphics& g) const;
    void drawWaveformScope (juce::Graphics& g) const;
    void exportLiveDiagnosticCsv();

    VibeAutotuneProcessor& processor;
    VibeAutotuneProcessor::PitchMonitorData monitor;

    juce::TextButton bypassButton { "POWER" };
    juce::TextButton dashboardTabButton { "DASHBOARD" };
    juce::TextButton waveformTabButton { "WAVEFORM" };
    juce::TextButton diagnosticCaptureButton { "CAPTURE" };
    juce::TextButton diagnosticExportButton { "EXPORT CSV" };
    juce::ToggleButton groupOctavesButton { "GROUP OCTAVES" };
    juce::ComboBox inputTypeSelector, keySelector, scaleSelector;
    juce::Label inputTypeLabel, keyLabel, scaleLabel;

    RetuneRotarySlider retuneSpeedSlider;
    HardwareRotarySlider pitchFlexibleSlider;
    HardwareRotarySlider humanizeSlider;
    HardwareRotarySlider naturalVibratoSlider;
    HardwareRotarySlider transposeSlider, referencePitchSlider, trackingSlider, voicedColourSlider;
    juce::Label retuneSpeedLabel, pitchFlexibleLabel;
    juce::Label humanizeLabel;
    juce::Label naturalVibratoLabel;
    juce::Label transposeLabel, referencePitchLabel, trackingLabel, voicedColourLabel;

    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ComboBoxAttachment> inputTypeAttachment, keyAttachment, scaleAttachment;
    std::unique_ptr<SliderAttachment> retuneSpeedAttachment, flexTuneAttachment, humanizeAttachment,
                                      naturalVibratoAttachment;
    std::unique_ptr<SliderAttachment> transposeAttachment, referencePitchAttachment, trackingAttachment, voicedColourAttachment;

    juce::Rectangle<int> headerBounds, settingsBounds, waveformBounds, monitorBounds, mainControlsBounds, utilityControlsBounds, pianoBounds;
    std::array<juce::Rectangle<int>, 8> knobBounds;
    // C1 through B5 gives the range indicator enough context for every
    // supported input profile without turning the keyboard into a scroll view.
    std::array<juce::Rectangle<int>, 60> pianoKeyBounds;
    std::array<juce::Rectangle<int>, 60> noteStatusBounds;
    int activePianoNote = -1;
    int targetPianoNote = -1;
    float displayedAppliedCorrectionCents = 0.0f;
    bool monitorHasValidPitch = false;
    bool waveformTabSelected = false;
    std::array<VibeAutotuneProcessor::WaveformScopeFrame, VibeAutotuneProcessor::waveformScopeCapacity> waveformScopeFrames {};
    int waveformScopeFrameCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeAutotuneEditor)
};
