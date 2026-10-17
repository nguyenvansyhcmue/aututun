#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class AnalogTextButton final : public juce::TextButton,
                               private juce::Timer
{
public:
    explicit AnalogTextButton (const juce::String& text) : juce::TextButton (text) {}
    ~AnalogTextButton() override { stopTimer(); }

    float getPressDepth() const noexcept { return pressDepth; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    float pressDepth = 0.0f;
    bool held = false;
    uint32_t lastTickMs = 0;
};

class AnalogToggleButton final : public juce::ToggleButton,
                                 private juce::Timer
{
public:
    explicit AnalogToggleButton (const juce::String& text) : juce::ToggleButton (text) {}
    ~AnalogToggleButton() override { stopTimer(); }

    float getPressDepth() const noexcept { return pressDepth; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    float pressDepth = 0.0f;
    bool held = false;
    uint32_t lastTickMs = 0;
};

