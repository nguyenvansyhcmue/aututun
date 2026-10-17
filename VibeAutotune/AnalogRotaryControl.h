#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Parameter values remain owned by APVTS.  This model only owns the visual
// target/current angle and gesture state, so animation can never delay DSP.
struct KnobDefinition
{
    float minAngleDegrees = -135.0f;
    float maxAngleDegrees = 135.0f;
    float dragSensitivity = 1.0f;
    float fineMultiplier = 0.20f;
    float releaseSeconds = 0.11f;
    // A short, damped hardware flywheel tail. Values are normalised to the
    // parameter range, so a 0..1 and a 0..400 knob feel identical.
    float inertiaFriction = 25.0f;
    float inertiaStartThreshold = 0.055f;
    float inertiaStopThreshold = 0.006f;
    float maximumInertiaVelocity = 1.15f;
    float maximumInertiaSeconds = 0.12f;
};

class AnalogRotaryControl : public juce::Slider,
                                  private juce::Timer
{
public:
    explicit AnalogRotaryControl (KnobDefinition definition = {});
    ~AnalogRotaryControl() override;

    float getDisplayAngleRadians() const noexcept;
    float getPressDepth() const noexcept { return pressDepth; }
    bool isAnimating() const noexcept { return animating; }
    void setVerticalDragInverted (bool shouldInvert) noexcept { verticalDragInverted = shouldInvert; }

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void valueChanged() override;
    float targetAngleRadians() noexcept;

    KnobDefinition definition;
    float displayAngle = 0.0f;
    float pressDepth = 0.0f;
    bool animating = false;
    bool dragging = false;
    float lastDragY = 0.0f;
    float dragPixelsForFullRange = 200.0f;
    uint32_t lastTickMs = 0;
    double lastDragTimeSeconds = 0.0;
    float filteredVelocity = 0.0f;
    float inertiaVelocity = 0.0f;
    float inertiaAgeSeconds = 0.0f;
    int lastDragDirection = 0;
    bool applyingInertia = false;
    bool verticalDragInverted = false;
};
