#include "AnalogHardwareButton.h"

namespace
{
float advanceSpring (float value, bool held, float dt)
{
    const auto target = held ? 1.0f : 0.0f;
    const auto rate = held ? 55.0f : 34.0f;
    return value + (target - value) * (1.0f - std::exp (-dt * rate));
}
}

void AnalogTextButton::mouseDown (const juce::MouseEvent& event)
{
    held = true;
    pressDepth = 1.0f;
    lastTickMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);
    repaint();
    juce::TextButton::mouseDown (event);
}

void AnalogTextButton::mouseUp (const juce::MouseEvent& event)
{
    juce::TextButton::mouseUp (event);
    held = false;
    lastTickMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);
}

void AnalogTextButton::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto dt = juce::jlimit (.001f, .05f, (now - lastTickMs) * .001f);
    lastTickMs = now;
    pressDepth = advanceSpring (pressDepth, held, dt);
    if (! held && pressDepth < .002f) { pressDepth = 0.0f; stopTimer(); }
    repaint();
}

void AnalogToggleButton::mouseDown (const juce::MouseEvent& event)
{
    held = true;
    pressDepth = 1.0f;
    lastTickMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);
    repaint();
    juce::ToggleButton::mouseDown (event);
}

void AnalogToggleButton::mouseUp (const juce::MouseEvent& event)
{
    juce::ToggleButton::mouseUp (event);
    held = false;
    lastTickMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);
}

void AnalogToggleButton::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto dt = juce::jlimit (.001f, .05f, (now - lastTickMs) * .001f);
    lastTickMs = now;
    pressDepth = advanceSpring (pressDepth, held, dt);
    if (! held && pressDepth < .002f) { pressDepth = 0.0f; stopTimer(); }
    repaint();
}

