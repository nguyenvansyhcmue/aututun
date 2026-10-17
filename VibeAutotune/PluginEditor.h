#pragma once

#include "PluginProcessor.h"
#include <functional>
#include <utility>
#include "AnalogRotaryControl.h"
#include "AnalogHardwareButton.h"

class VibeAutotuneEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit VibeAutotuneEditor (VibeAutotuneProcessor& p);
    ~VibeAutotuneEditor() override;

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& event) override;
    bool keyPressed (const juce::KeyPress& key) override;

private:
    class NoteRuleHitTarget final : public juce::Component
    {
    public:
        std::function<void (const juce::ModifierKeys&)> onGesture;
        void mouseDown (const juce::MouseEvent& event) override
        {
            if (onGesture != nullptr)
                onGesture (event.mods);
        }
    };

    struct NoteMapVisualSlot
    {
        int midiNote = -1;
        bool isBlack = false;
        juce::Rectangle<float> keyBounds;
        juce::Rectangle<float> indicatorBounds;
    };

    // Retune has a musical, non-linear faceplate: the useful fast range gets
    // more angular resolution while the parameter/automation remains exactly
    // 0–400 ms.  Landmarks: 50 ms at 12 o'clock and 125 ms near 10 o'clock.
    class RetuneRotarySlider final : public AnalogRotaryControl
    {
    public:
        RetuneRotarySlider() { setVerticalDragInverted (true); }

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
    void drawPianoKeyboard (juce::Graphics& g) const;
    void drawPitchMonitor (juce::Graphics& g) const;
    void drawRuleMarker (juce::Graphics& g, juce::Rectangle<float> bounds,
                         NoteRule rule, float opacity) const;
    int pianoNoteAt (juce::Point<int> position) const noexcept;
    bool isNoteInSelectedVoiceRange (int midiNote) const noexcept;
    void applyNoteRule (int midiNote, NoteRule rule);
    void showNoteRuleMenu (int midiNote, juce::Rectangle<int> targetBounds);
    void handleNoteRuleGesture (int midiNote, const juce::ModifierKeys& modifiers);

    VibeAutotuneProcessor& processor;
    VibeAutotuneProcessor::PitchMonitorData monitor;

    AnalogTextButton bypassButton { "POWER" };
    AnalogTextButton settingsButton { "SETTINGS" };
    AnalogTextButton editNotesButton { "EDIT NOTES" };
    AnalogToggleButton groupOctavesButton { "GROUP OCTAVES" };
    // Primary selectors remain in the utility section.  The Note Map owns
    // compact mirrored controls so musical context is visible where notes are
    // edited without relocating the main controls.
    juce::ComboBox inputTypeSelector, keySelector, scaleSelector;
    juce::ComboBox noteMapVoiceSelector, noteMapKeySelector, noteMapScaleSelector;
    juce::Label inputTypeLabel, keyLabel, scaleLabel;

    RetuneRotarySlider retuneSpeedSlider;
    AnalogRotaryControl pitchFlexibleSlider;
    AnalogRotaryControl humanizeSlider;
    AnalogRotaryControl naturalVibratoSlider;
    AnalogRotaryControl transposeSlider, referencePitchSlider, trackingSlider, voicedColourSlider;
    juce::Label retuneSpeedLabel, pitchFlexibleLabel;
    juce::Label humanizeLabel;
    juce::Label naturalVibratoLabel;
    juce::Label transposeLabel, referencePitchLabel, trackingLabel, voicedColourLabel;

    std::unique_ptr<ButtonAttachment> bypassAttachment;
    std::unique_ptr<ComboBoxAttachment> inputTypeAttachment, keyAttachment, scaleAttachment;
    std::unique_ptr<ComboBoxAttachment> noteMapScaleAttachment;
    std::unique_ptr<SliderAttachment> retuneSpeedAttachment, flexTuneAttachment, humanizeAttachment,
                                      naturalVibratoAttachment;
    std::unique_ptr<SliderAttachment> transposeAttachment, referencePitchAttachment, trackingAttachment, voicedColourAttachment;

    juce::Rectangle<int> headerBounds, correctionBounds, monitorBounds, setupBounds, pianoBounds;
    std::array<juce::Rectangle<int>, 8> knobBounds;
    // C1 through B5 gives the range indicator enough context for every
    // supported input profile without turning the keyboard into a scroll view.
    // A single geometry record per MIDI note is the source of truth for
    // drawing, hit-testing and the interactive editor overlay.
    std::array<NoteMapVisualSlot, 60> noteMapSlots;
    std::array<NoteRuleHitTarget, 60> noteRuleHitTargets;
    int activePianoNote = -1;
    int targetPianoNote = -1;
    int uiPressedPianoNote = -1;
    int uiPressFramesRemaining = 0;
    float displayedAppliedCorrectionCents = 0.0f;
    float displayedPitchConfidence = 0.0f;
    // Presentation-only motion state. It is fed from published monitor
    // snapshots on the message thread and is never read by the audio thread.
    float ribbonMotionPhase = 0.0f;
    float displayedRibbonMotion = 0.0f;
    bool monitorHasValidPitch = false;
    bool editNotesEnabled = false;
    std::array<VibeAutotuneProcessor::WaveformScopeFrame, VibeAutotuneProcessor::waveformScopeCapacity> monitorScopeFrames {};
    int monitorScopeFrameCount = 0;

   #if JUCE_DEBUG
    juce::Image targetReferenceImage;
    bool showTargetOverlay = false;
    bool useReferenceVisualState = false;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VibeAutotuneEditor)
};
