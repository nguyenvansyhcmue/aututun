#include "AnalogRotaryControl.h"

AnalogRotaryControl::AnalogRotaryControl (KnobDefinition d) : definition (d)
{
    setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (juce::Slider::TextBoxBelow, false, 82, 20);
    setRotaryParameters (juce::degreesToRadians (d.minAngleDegrees + 360.0f),
                         juce::degreesToRadians (d.maxAngleDegrees + 360.0f), true);
    dragPixelsForFullRange = (float) juce::jmax (80, juce::roundToInt (200.0f / juce::jmax (0.25f, d.dragSensitivity)));
    setMouseDragSensitivity (juce::roundToInt (dragPixelsForFullRange));
    // Do not construct a platform cursor from an AU/VST editor. On recent
    // macOS this can cross the host/plugin JUCE boundary during editor
    // creation; the default cursor is stable and keeps the host isolated.
    displayAngle = targetAngleRadians();
}

AnalogRotaryControl::~AnalogRotaryControl() { stopTimer(); }

float AnalogRotaryControl::targetAngleRadians() noexcept
{
    const auto span = definition.maxAngleDegrees - definition.minAngleDegrees;
    const auto p = (float) juce::jlimit (0.0, 1.0, valueToProportionOfLength (getValue()));
    return juce::degreesToRadians (definition.minAngleDegrees + p * span);
}

float AnalogRotaryControl::getDisplayAngleRadians() const noexcept { return displayAngle; }

void AnalogRotaryControl::valueChanged()
{
    // Attachments can set the real parameter value before the editor is on
    // screen. Snap that first assignment into place so a newly opened editor
    // never animates from the constructor's placeholder value.
    if (! isShowing() && lastTickMs == 0)
    {
        displayAngle = targetAngleRadians();
        animating = false;
        return;
    }
    if (! dragging)
    {
        if (! applyingInertia) inertiaVelocity = 0.0f; // automation/preset owns the new target
        animating = true;
        if (! isTimerRunning()) startTimerHz (60);
    }
}

void AnalogRotaryControl::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    lastDragY = e.position.y;
    filteredVelocity = 0.0f;
    inertiaVelocity = 0.0f;
    inertiaAgeSeconds = 0.0f;
    lastDragDirection = 0;
    lastDragTimeSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    pressDepth = 1.0f;
    animating = true;
    lastTickMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);
    juce::Slider::mouseDown (e);
}

void AnalogRotaryControl::mouseDrag (const juce::MouseEvent& e)
{
    // Incremental vertical drag is deliberately independent of timer rate.
    // Changing direction or pressing Shift mid-gesture cannot resurrect an
    // old velocity or jump back to a drag-start anchor.
    const auto physicalDeltaY = lastDragY - e.position.y;
    const auto deltaY = verticalDragInverted ? -physicalDeltaY : physicalDeltaY;
    lastDragY = e.position.y;
    const auto fine = e.mods.isShiftDown() ? definition.fineMultiplier : 1.0f;
    const auto range = getMaximum() - getMinimum();
    const auto deltaValue = (double) deltaY * range * (double) fine / (double) dragPixelsForFullRange;
    const auto wanted = getValue() + deltaValue;
    setValue (juce::jlimit (getMinimum(), getMaximum(), wanted), juce::sendNotificationSync);
    const auto nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto sampleSeconds = juce::jlimit (0.001, 0.050, nowSeconds - lastDragTimeSeconds);
    lastDragTimeSeconds = nowSeconds;
    const auto direction = deltaValue > 0.0 ? 1 : deltaValue < 0.0 ? -1 : 0;
    if (direction != 0)
    {
        // Reversing the hand cancels the previous momentum immediately.
        if (lastDragDirection != 0 && direction != lastDragDirection) filteredVelocity = 0.0f;
        const auto normalisedSample = (float) (deltaValue / juce::jmax (range, 1.0e-12) / sampleSeconds);
        filteredVelocity += (normalisedSample - filteredVelocity) * .58f;
        filteredVelocity = juce::jlimit (-definition.maximumInertiaVelocity,
                                          definition.maximumInertiaVelocity, filteredVelocity);
        lastDragDirection = direction;
    }
    displayAngle = targetAngleRadians();
    repaint();
}

void AnalogRotaryControl::mouseUp (const juce::MouseEvent& e)
{
    juce::Slider::mouseUp (e);
    dragging = false;
    const auto age = juce::Time::getMillisecondCounterHiRes() * 0.001 - lastDragTimeSeconds;
    inertiaVelocity = age <= .080 && std::abs (filteredVelocity) >= definition.inertiaStartThreshold
                        ? filteredVelocity : 0.0f;
    inertiaAgeSeconds = 0.0f;
    animating = true;
    startTimerHz (60);
}

void AnalogRotaryControl::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
        return;
    inertiaVelocity = filteredVelocity = 0.0f;

    const auto range = getMaximum() - getMinimum();
    const auto normalStep = getInterval() > 0.0 ? getInterval() : range * .01;
    const auto fine = e.mods.isShiftDown() ? definition.fineMultiplier : 1.0f;
    const auto direction = wheel.deltaY > 0.0f ? 1.0 : -1.0;
    juce::Slider::ScopedDragNotification drag (*this);
    setValue (juce::jlimit (getMinimum(), getMaximum(), getValue() + direction * normalStep * fine),
              juce::sendNotificationSync);
}

bool AnalogRotaryControl::keyPressed (const juce::KeyPress& key)
{
    inertiaVelocity = filteredVelocity = 0.0f;
    const auto range = getMaximum() - getMinimum();
    const auto normalStep = getInterval() > 0.0 ? getInterval() : range * 0.01;
    const auto step = normalStep * (key.getModifiers().isShiftDown() ? definition.fineMultiplier : 1.0f);
    if (key == juce::KeyPress::homeKey) { setValue (getMinimum(), juce::sendNotificationSync); return true; }
    if (key == juce::KeyPress::endKey) { setValue (getMaximum(), juce::sendNotificationSync); return true; }
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::downKey) { setValue (juce::jmax (getMinimum(), getValue() - step), juce::sendNotificationSync); return true; }
    if (key == juce::KeyPress::rightKey || key == juce::KeyPress::upKey) { setValue (juce::jmin (getMaximum(), getValue() + step), juce::sendNotificationSync); return true; }
    return juce::Slider::keyPressed (key);
}

void AnalogRotaryControl::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto dt = juce::jlimit (0.001f, 0.05f, (now - lastTickMs) * 0.001f);
    lastTickMs = now;
    if (! dragging && std::abs (inertiaVelocity) > 0.0f)
    {
        const auto range = getMaximum() - getMinimum();
        const auto previous = getValue();
        const auto wanted = previous + (double) inertiaVelocity * range * dt;
        applyingInertia = true;
        setValue (juce::jlimit (getMinimum(), getMaximum(), wanted), juce::sendNotificationSync);
        applyingInertia = false;
        inertiaAgeSeconds += dt;
        inertiaVelocity *= std::exp (-definition.inertiaFriction * dt);
        const auto hitLimit = (getValue() <= getMinimum() && inertiaVelocity < 0.0f)
                           || (getValue() >= getMaximum() && inertiaVelocity > 0.0f);
        if (hitLimit || inertiaAgeSeconds >= definition.maximumInertiaSeconds
                     || std::abs (inertiaVelocity) < definition.inertiaStopThreshold)
            inertiaVelocity = 0.0f;
    }
    const auto currentTarget = targetAngleRadians();
    if (! dragging) displayAngle += (currentTarget - displayAngle) * (1.0f - std::exp (-dt * 34.0f));
    pressDepth += ((dragging ? 1.0f : 0.0f) - pressDepth) * (1.0f - std::exp (-dt / definition.releaseSeconds * 6.0f));
    if (! dragging && inertiaVelocity == 0.0f && std::abs (currentTarget - displayAngle) < 0.0005f && pressDepth < 0.002f) { displayAngle = currentTarget; pressDepth = 0.0f; animating = false; stopTimer(); }
    repaint();
}
