#include "AnalogRotaryControl.h"
#include "AnalogHardwareButton.h"

#include <cmath>
#include <iostream>

namespace
{
struct DragCounter final : juce::Slider::Listener
{
    void sliderValueChanged (juce::Slider*) override {}
    void sliderDragStarted (juce::Slider*) override { ++starts; }
    void sliderDragEnded (juce::Slider*) override { ++ends; }
    int starts = 0, ends = 0;
};

bool near (double a, double b, double tolerance = .001) { return std::abs (a - b) <= tolerance; }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    AnalogRotaryControl knob;
    knob.setBounds (0, 0, 120, 140);
    knob.setRange (0.0, 100.0, .1);
    knob.setDoubleClickReturnValue (true, 42.0);
    knob.setValue (50.0, juce::dontSendNotification);

    DragCounter counter;
    knob.addListener (&counter);
    const auto source = juce::Desktop::getInstance().getMainMouseSource();
    const auto downTime = juce::Time::getCurrentTime();
    const auto event = [&] (float y, juce::ModifierKeys modifiers, int clicks = 1, bool dragged = false)
    {
        return juce::MouseEvent (source, { 60.0f, y }, modifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &knob, &knob, juce::Time::getCurrentTime(), { 60.0f, 100.0f }, downTime,
                                 clicks, dragged);
    };

    const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    knob.mouseDown (event (100.0f, left));
    knob.mouseDrag (event (-100.0f, left, 1, true));
    if (! near (knob.getValue(), 100.0)) return 1; // min -> max in one gesture

    knob.mouseDrag (event (-80.0f, left, 1, true));
    if (! (knob.getValue() < 100.0)) return 2; // reverse cancels old direction immediately

    const auto beforeFine = knob.getValue();
    const auto shifted = left.withFlags (juce::ModifierKeys::shiftModifier);
    knob.mouseDrag (event (-100.0f, shifted, 1, true));
    if (! near (knob.getValue() - beforeFine, 2.0, .11)) return 3;
    const auto releaseValue = knob.getValue();
    knob.mouseUp (event (-100.0f, left, 1, true));
    if (counter.starts != 1 || counter.ends != 1) return 4;
    for (int tick = 0; tick < 9; ++tick)
    {
        juce::Thread::sleep (18);
        juce::Timer::callPendingTimersSynchronously();
    }
    if (knob.getValue() < releaseValue || knob.getValue() - releaseValue > 8.0) return 13;

    knob.mouseDoubleClick (event (70.0f, left, 2));
    if (! near (knob.getValue(), 42.0)) return 5;

    knob.keyPressed (juce::KeyPress (juce::KeyPress::homeKey));
    if (! near (knob.getValue(), 0.0)) return 6;
    knob.keyPressed (juce::KeyPress (juce::KeyPress::endKey));
    if (! near (knob.getValue(), 100.0)) return 7;

    knob.setValue (50.0, juce::dontSendNotification);
    juce::MouseWheelDetails wheel;
    wheel.deltaY = 1.0f;
    knob.mouseWheelMove (event (70.0f, {}), wheel);
    if (! (knob.getValue() > 50.0) || counter.starts != 3 || counter.ends != 3) return 8;

    knob.setValue (0.0, juce::dontSendNotification);
    if (knob.getDisplayAngleRadians() < juce::degreesToRadians (-135.01f)) return 9;
    knob.setValue (100.0, juce::dontSendNotification);
    if (knob.getDisplayAngleRadians() > juce::degreesToRadians (135.01f)) return 10;

    AnalogTextButton hardwareButton ("POWER");
    hardwareButton.setBounds (0, 0, 64, 64);
    const auto buttonEvent = [&] (juce::ModifierKeys modifiers)
    {
        return juce::MouseEvent (source, { 32.0f, 32.0f }, modifiers, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 &hardwareButton, &hardwareButton, juce::Time::getCurrentTime(), { 32.0f, 32.0f },
                                 juce::Time::getCurrentTime(), 1, false);
    };
    hardwareButton.mouseDown (buttonEvent (left));
    if (! near (hardwareButton.getPressDepth(), 1.0)) return 11;
    hardwareButton.mouseUp (buttonEvent (left));
    for (int tick = 0; tick < 10; ++tick)
    {
        juce::Thread::sleep (20);
        juce::Timer::callPendingTimersSynchronously();
    }
    if (hardwareButton.getPressDepth() > .02f) return 12;

    // No timer/repaint loop is allowed to remain alive after physics settles.
    for (int tick = 0; tick < 12; ++tick)
    {
        juce::Thread::sleep (18);
        juce::Timer::callPendingTimersSynchronously();
    }
    if (knob.isAnimating()) return 14;

    std::cout << "ANALOG_HARDWARE_CONTROLS: PASS\n";
    return 0;
}
