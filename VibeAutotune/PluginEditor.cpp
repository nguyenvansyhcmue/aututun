#include "PluginEditor.h"

namespace
{
// Vibe's own visual language: near-black mineral surfaces, cool cyan for
// information and a restrained emerald/coral pair for state and correction.
const auto background = juce::Colour (0xff070b10);
const auto surface = juce::Colour (0xff111923);
const auto surfaceRaised = juce::Colour (0xff1a2631);
const auto cyan = juce::Colour (0xff21c7e8);
const auto emerald = juce::Colour (0xff35d0a5);
const auto amber = juce::Colour (0xfff0a14a);
const auto coral = juce::Colour (0xffe96b98);
const auto mutedText = juce::Colour (0xff9aabb8);

constexpr int keyboardFirstMidiNote = 24; // C1
constexpr int keyboardLastMidiNote = 83;  // B5

constexpr bool isBlackKey (int midiNote) noexcept
{
    const auto pitchClass = (midiNote % 12 + 12) % 12;
    return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
}

struct VocalKeyboardRange { int lowestMidi; int highestMidi; };

constexpr std::array<VocalKeyboardRange, 4> vocalKeyboardRanges {{
    { 54, 83 }, // Soprano: F#3–B5
    { 45, 77 }, // Alto: A2–F5
    { 40, 71 }, // Tenor: E2–B4
    { 33, 62 }  // Low: A1–D4
}};

class HardwareLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                              static_cast<float> (width), static_cast<float> (height))
                          .reduced (8.0f);
        bounds.removeFromBottom (22.0f);
        const auto diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
        bounds = bounds.withSizeKeepingCentre (diameter, diameter);
        const auto centre = bounds.getCentre();
        const auto radius = diameter * 0.5f;
        const auto active = slider.isMouseOverOrDragging() || slider.hasKeyboardFocus (true);
        const auto enabledAlpha = slider.isEnabled() ? 1.0f : 0.36f;

        g.setColour (juce::Colours::black.withAlpha (0.55f * enabledAlpha));
        g.fillEllipse (bounds.translated (0.0f, 3.0f));

        auto face = juce::ColourGradient (juce::Colour (0xff2a3c4c), centre.x - radius * 0.35f, centre.y - radius * 0.45f,
                                           juce::Colour (0xff09121b), centre.x + radius * 0.6f, centre.y + radius * 0.7f, false);
        face.multiplyOpacity (enabledAlpha);
        g.setGradientFill (face);
        g.fillEllipse (bounds);

        g.setColour (juce::Colour (0xff5f7485).withAlpha (0.75f * enabledAlpha));
        g.drawEllipse (bounds, 1.2f);
        g.setColour (juce::Colour (0xff05090d).withAlpha (enabledAlpha));
        g.drawEllipse (bounds.reduced (5.0f), 2.0f);

        const auto ringBounds = bounds.reduced (8.0f);
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, ringBounds.getWidth() * 0.5f, ringBounds.getHeight() * 0.5f,
                             0.0f, startAngle, endAngle, true);
        g.setColour (juce::Colour (0xff263d50).withAlpha (enabledAlpha));
        g.strokePath (track, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto valueAngle = startAngle + sliderPos * (endAngle - startAngle);
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, ringBounds.getWidth() * 0.5f, ringBounds.getHeight() * 0.5f,
                                0.0f, startAngle, valueAngle, true);
        g.setColour ((active ? cyan : emerald).brighter (0.08f).withAlpha (enabledAlpha));
        g.strokePath (valueArc, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto pointerStart = centre + juce::Point<float> (std::cos (valueAngle - juce::MathConstants<float>::halfPi),
                                                                std::sin (valueAngle - juce::MathConstants<float>::halfPi)) * radius * 0.16f;
        const auto pointerEnd = centre + juce::Point<float> (std::cos (valueAngle - juce::MathConstants<float>::halfPi),
                                                              std::sin (valueAngle - juce::MathConstants<float>::halfPi)) * radius * 0.56f;
        g.setColour (juce::Colours::white.withAlpha (0.92f * enabledAlpha));
        g.drawLine ({ pointerStart, pointerEnd }, 2.3f);
        g.setColour ((active ? cyan : juce::Colour (0xffb8e6ed)).withAlpha (enabledAlpha));
        g.fillEllipse (centre.x - 3.0f, centre.y - 3.0f, 6.0f, 6.0f);
    }
};

HardwareLookAndFeel hardwareLookAndFeel;
}

VibeAutotuneEditor::VibeAutotuneEditor (VibeAutotuneProcessor& p)
    : AudioProcessorEditor (&p), processor (p), monitor (processor.getPitchMonitorData())
{
    configureSelector (inputTypeSelector, inputTypeLabel, "VOICE", { "Soprano", "Alto", "Tenor", "Low" });
    configureSelector (keySelector, keyLabel, "KEY", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    configureSelector (scaleSelector, scaleLabel, "SCALE", { "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic",
                                                                "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian",
                                                                "Mixolydian", "Locrian", "Minor Blues", "Whole Tone", "User Scale" });

    configureKnob (retuneSpeedSlider, retuneSpeedLabel, "RETUNE", " ms", 100.0);
    configureKnob (pitchFlexibleSlider, pitchFlexibleLabel, "FLEX-TUNE", "", 30.0);
    configureKnob (humanizeSlider, humanizeLabel, "HUMANIZE", " %", 0.0);
    configureKnob (naturalVibratoSlider, naturalVibratoLabel, "NATURAL VIBRATO", "", 0.0);
    configureKnob (transposeSlider, transposeLabel, "TRANSPOSE", " st", 0.0);
    configureKnob (referencePitchSlider, referencePitchLabel, "REFERENCE", " Hz", 440.0);
    configureKnob (trackingSlider, trackingLabel, "TRACKING", " %", 50.0);
    configureKnob (voicedColourSlider, voicedColourLabel, "VOICED COLOUR", " %", 0.0);

    naturalVibratoSlider.textFromValueFunction = [] (double value)
    {
        const auto rounded = juce::roundToInt (value);
        return (rounded > 0 ? "+" : "") + juce::String (rounded) + " ms";
    };

    bypassButton.setClickingTogglesState (true);
    bypassButton.setColour (juce::TextButton::buttonColourId, surfaceRaised);
    bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff244d62));
    bypassButton.setColour (juce::TextButton::textColourOffId, cyan);
    bypassButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    addAndMakeVisible (bypassButton);

    const auto configureTab = [] (juce::TextButton& button)
    {
        button.setColour (juce::TextButton::buttonColourId, surfaceRaised);
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff174744));
        button.setColour (juce::TextButton::textColourOffId, mutedText);
        button.setColour (juce::TextButton::textColourOnId, emerald);
        button.setClickingTogglesState (true);
    };
    configureTab (dashboardTabButton);
    configureTab (waveformTabButton);
    dashboardTabButton.setToggleState (true, juce::dontSendNotification);
    dashboardTabButton.onClick = [this]
    {
        waveformTabSelected = false;
        dashboardTabButton.setToggleState (true, juce::dontSendNotification);
        waveformTabButton.setToggleState (false, juce::dontSendNotification);
        resized();
        repaint();
    };
    waveformTabButton.onClick = [this]
    {
        waveformTabSelected = true;
        dashboardTabButton.setToggleState (false, juce::dontSendNotification);
        waveformTabButton.setToggleState (true, juce::dontSendNotification);
        resized();
        repaint();
    };
    addAndMakeVisible (dashboardTabButton);
    addAndMakeVisible (waveformTabButton);


    diagnosticCaptureButton.setClickingTogglesState (true);
    diagnosticCaptureButton.setColour (juce::TextButton::buttonColourId, surfaceRaised);
    diagnosticCaptureButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff174744));
    diagnosticCaptureButton.setColour (juce::TextButton::textColourOffId, mutedText);
    diagnosticCaptureButton.setColour (juce::TextButton::textColourOnId, emerald);
    diagnosticCaptureButton.onClick = [this]
    {
        processor.setLiveDiagnosticCaptureEnabled (diagnosticCaptureButton.getToggleState());
        diagnosticCaptureButton.setButtonText (diagnosticCaptureButton.getToggleState() ? "CAPTURING" : "CAPTURE");
    };
    addAndMakeVisible (diagnosticCaptureButton);

    diagnosticExportButton.setColour (juce::TextButton::buttonColourId, surfaceRaised);
    diagnosticExportButton.setColour (juce::TextButton::textColourOffId, cyan);
    diagnosticExportButton.onClick = [this] { exportLiveDiagnosticCsv(); };
    addAndMakeVisible (diagnosticExportButton);

    groupOctavesButton.setToggleState (processor.getGroupOctaves(), juce::dontSendNotification);
    groupOctavesButton.setColour (juce::ToggleButton::textColourId, mutedText);
    groupOctavesButton.setColour (juce::ToggleButton::tickColourId, emerald);
    groupOctavesButton.onClick = [this] { processor.setGroupOctaves (groupOctavesButton.getToggleState()); repaint (pianoBounds); };
    addAndMakeVisible (groupOctavesButton);

    bypassAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "bypass", bypassButton);
    inputTypeAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "inputType", inputTypeSelector);
    keyAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "key", keySelector);
    scaleAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "scale", scaleSelector);
    retuneSpeedAttachment = std::make_unique<SliderAttachment> (processor.parameters, "retuneSpeed", retuneSpeedSlider);
    flexTuneAttachment = std::make_unique<SliderAttachment> (processor.parameters, "flexTune", pitchFlexibleSlider);
    humanizeAttachment = std::make_unique<SliderAttachment> (processor.parameters, "humanize", humanizeSlider);
    naturalVibratoAttachment = std::make_unique<SliderAttachment> (processor.parameters, "naturalVibrato", naturalVibratoSlider);
    transposeAttachment = std::make_unique<SliderAttachment> (processor.parameters, "transpose", transposeSlider);
    referencePitchAttachment = std::make_unique<SliderAttachment> (processor.parameters, "referencePitch", referencePitchSlider);
    trackingAttachment = std::make_unique<SliderAttachment> (processor.parameters, "tracking", trackingSlider);
    voicedColourAttachment = std::make_unique<SliderAttachment> (processor.parameters, "voicedColour", voicedColourSlider);

    setResizable (true, true);
    setResizeLimits (820, 680, 1440, 960);
    setSize (1080, 760);
    startTimerHz (30);
}

VibeAutotuneEditor::~VibeAutotuneEditor()
{
    stopTimer();
}

void VibeAutotuneEditor::configureSelector (juce::ComboBox& selector, juce::Label& label,
                                             const juce::String& title, const juce::StringArray& choices)
{
    selector.addItemList (choices, 1);
    selector.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff11171b));
    selector.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff46565e));
    selector.setColour (juce::ComboBox::textColourId, juce::Colours::white);
    selector.setColour (juce::ComboBox::arrowColourId, cyan);
    addAndMakeVisible (selector);

    label.setText (title, juce::dontSendNotification);
    label.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, mutedText);
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

void VibeAutotuneEditor::exportLiveDiagnosticCsv()
{
    std::vector<VibeAutotuneProcessor::SignalChainDiagnosticFrame> frames (
        static_cast<size_t> (VibeAutotuneProcessor::liveDiagnosticCapacity));
    const auto count = processor.copyLiveDiagnosticFrames (frames.data(), static_cast<int> (frames.size()));
    if (count <= 0)
    {
        diagnosticExportButton.setButtonText ("NO DATA");
        return;
    }

    // JUCE follows strftime tokens here.  The former Unicode-style pattern
    // was emitted literally ("yyyy-MM-dd_HH-mm-ss"), so every export reused
    // one file and FileOutputStream appended separate captures together.
    // A diagnostic capture must be a single chronological take.
    const auto timestamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    const auto destination = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
        .getChildFile ("VibeAutotune-diagnostic-" + timestamp + ".csv");
    juce::FileOutputStream stream (destination);
    if (! stream.openedOk())
    {
        diagnosticExportButton.setButtonText ("SAVE FAILED");
        return;
    }

    stream.writeText ("sample_index,raw_f0_hz,stable_f0_hz,confidence,input_cents,target_cents,requested_ratio,delivered_cents,renderer_step_cents,dry_wet,retune_speed_ms,tracking,flex_gain,desired_cents,retune_cents,humanize,vibrato_cents,reference_hz,voice_min_hz,voice_max_hz,raw_midi,candidate_target_midi,target_midi,key,scale,transpose,voiced,target_changed,correction_active,target_bridge,transient_bridge\n", false, false, "UTF-8");
    for (int i = 0; i < count; ++i)
    {
        const auto& f = frames[static_cast<size_t> (i)];
        const auto line = juce::String (f.sampleIndex) + "," + juce::String (f.rawF0Hz, 5) + ","
            + juce::String (f.stableF0Hz, 5) + "," + juce::String (f.pitchConfidence, 5) + ","
            + juce::String (f.inputPitchCents, 3) + "," + juce::String (f.targetPitchCents, 3) + ","
            + juce::String (f.requestedRendererRatio, 7) + "," + juce::String (f.deliveredRendererCorrectionCents, 3) + ","
            + juce::String (f.rendererStepCents, 3) + "," + juce::String (f.dryWetMix, 4) + ","
            + juce::String (f.retuneSpeedMs, 3) + ","
            + juce::String (f.trackingValue, 3) + "," + juce::String (f.flexCorrectionGain, 4) + ","
            + juce::String (f.desiredCorrectionCents, 3) + "," + juce::String (f.retuneCorrectionCents, 3) + ","
            + juce::String (f.humanizeWeight, 4) + "," + juce::String (f.vibratoContributionCents, 3) + ","
            + juce::String (f.referenceHz, 3) + ","
            + juce::String (f.voiceRangeMinHz) + "," + juce::String (f.voiceRangeMaxHz) + ","
            + juce::String (f.rawMidi) + "," + juce::String (f.candidateTargetMidi) + ","
            + juce::String (f.committedTargetMidi) + ","
            + juce::String (f.key) + "," + juce::String (f.scale) + "," + juce::String (f.transpose) + ","
            + juce::String (f.voiced ? 1 : 0) + "," + juce::String (f.targetChanged ? 1 : 0) + ","
            + juce::String (f.correctionIsActive ? 1 : 0) + "," + juce::String (f.targetBridge ? 1 : 0) + ","
            + juce::String (f.transientBridge ? 1 : 0) + "\n";
        stream.writeText (line, false, false, "UTF-8");
    }
    stream.flush();
    diagnosticExportButton.setButtonText ("SAVED TO DESKTOP");
}

void VibeAutotuneEditor::configureKnob (juce::Slider& slider, juce::Label& label,
                                         const juce::String& title, const juce::String& suffix,
                                         double defaultValue)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setLookAndFeel (&hardwareLookAndFeel);
    slider.setRotaryParameters (juce::degreesToRadians (220.0f), juce::degreesToRadians (500.0f), true);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 82, 22);
    slider.setTextValueSuffix (suffix);
    slider.setDoubleClickReturnValue (true, defaultValue);
    slider.setColour (juce::Slider::rotarySliderFillColourId, emerald);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff3c4d55));
    slider.setColour (juce::Slider::thumbColourId, cyan);
    slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff0b1013));
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (slider);

    label.setText (title, juce::dontSendNotification);
    label.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    label.setColour (juce::Label::textColourId, mutedText);
    label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (label);
}

void VibeAutotuneEditor::paint (juce::Graphics& g)
{
    auto chassis = juce::ColourGradient (juce::Colour (0xff17232d), 0.0f, 0.0f,
                                         background, 0.0f, static_cast<float> (getHeight()), false);
    chassis.addColour (0.46, juce::Colour (0xff0c141c));
    g.setGradientFill (chassis);
    g.fillAll();

    drawPanel (g, headerBounds.toFloat(), {});
    drawPanel (g, settingsBounds.toFloat(), "PERFORMANCE");
    if (waveformTabSelected)
        drawPanel (g, waveformBounds.toFloat(), "LIVE WAVEFORM  ·  INPUT F0 / TARGET / RENDERED CORRECTION");
    else
        drawPanel (g, mainControlsBounds.toFloat(), "CORRECTION");
    drawPanel (g, pianoBounds.toFloat(), "NOTE MAP   ·   '-' EXCLUDED FROM SCALE");

    auto brandPlate = headerBounds.reduced (16, 12).withWidth (278);
    auto brandFinish = juce::ColourGradient (juce::Colour (0xff0b131a), brandPlate.getX(), brandPlate.getY(),
                                              juce::Colour (0xff101e27), brandPlate.getRight(), brandPlate.getBottom(), false);
    g.setGradientFill (brandFinish);
    g.fillRoundedRectangle (brandPlate.toFloat(), 9.0f);
    g.setColour (cyan.withAlpha (0.45f));
    g.drawRoundedRectangle (brandPlate.toFloat(), 9.0f, 1.0f);
    g.setColour (emerald);
    g.fillEllipse (brandPlate.getX() + 14.0f, brandPlate.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    g.drawText ("VibeAutotune", brandPlate.reduced (30, 2).withTrimmedBottom (14), juce::Justification::centredLeft);
    g.setColour (cyan);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText ("PRECISION PITCH INSTRUMENT", brandPlate.reduced (30, 2).withTrimmedTop (30), juce::Justification::centredLeft);

    if (waveformTabSelected)
        drawWaveformScope (g);

    if (! waveformTabSelected)
    {
    // One recessed instrument bay unifies the four performance controls and
    // the monitor without borrowing a proprietary panel geometry.
    const auto stage = mainControlsBounds.toFloat().reduced (12.0f, 25.0f);
    auto stageFinish = juce::ColourGradient (juce::Colour (0xff0d161e), stage.getX(), stage.getY(),
                                              juce::Colour (0xff081015), stage.getX(), stage.getBottom(), false);
    g.setGradientFill (stageFinish);
    g.fillRoundedRectangle (stage, 14.0f);
    g.setColour (juce::Colours::white.withAlpha (0.055f));
    g.drawRoundedRectangle (stage.reduced (0.5f), 14.0f, 1.0f);

    auto visual = monitorBounds.toFloat().reduced (6.0f);

    const auto centre = visual.getCentre();
    const auto diameter = juce::jmin (visual.getWidth() * 0.86f, visual.getHeight() * 0.88f);
    const auto dial = juce::Rectangle<float> (diameter, diameter).withCentre (centre);
    auto dialGlow = juce::ColourGradient (cyan.withAlpha (monitorHasValidPitch ? 0.18f : 0.045f), centre.x, centre.y,
                                          juce::Colour (0xff0a1115).withAlpha (0.0f), dial.getX(), dial.getY(), true);
    g.setGradientFill (dialGlow);
    g.fillEllipse (dial.expanded (13.0f));
    auto dialFinish = juce::ColourGradient (juce::Colour (0xff1c2a35), dial.getX(), dial.getY(),
                                             juce::Colour (0xff080e13), dial.getRight(), dial.getBottom(), false);
    g.setGradientFill (dialFinish);
    g.fillEllipse (dial);
    g.setColour (juce::Colour (0xff6b7c85).withAlpha (0.72f));
    g.drawEllipse (dial.expanded (3.0f), 1.5f);
    g.setColour (juce::Colour (0xff050a0f));
    g.fillEllipse (dial.reduced (14.0f));
    g.setColour (cyan.withAlpha (0.72f));
    g.drawEllipse (dial.reduced (14.0f), 1.25f);

    const auto dialCentre = dial.getCentre();
    for (int tick = 0; tick < 41; ++tick)
    {
        const auto cents = -100.0f + 200.0f * static_cast<float> (tick) / 40.0f;
        const auto angle = juce::degreesToRadians (-90.0f + cents * 0.9f);
        const auto direction = juce::Point<float> (std::cos (angle), std::sin (angle));
        const auto outer = dialCentre + direction * (diameter * 0.47f);
        const auto inner = dialCentre + direction * (diameter * ((tick % 10 == 0) ? 0.385f : 0.415f));
        const auto negative = cents < 0.0f;
        g.setColour ((tick == 20 ? amber : (negative ? coral : cyan)).withAlpha (tick % 5 == 0 ? 0.78f : 0.35f));
        g.drawLine ({ inner, outer }, tick == 20 ? 2.1f : 1.0f);
    }

    // Action meter: positive correction raises the vocal (right), negative
    // correction lowers it (left). Its source is independent from both the
    // raw input note and the selected target note.
    const auto cents = juce::jlimit (-100.0f, 100.0f, displayedAppliedCorrectionCents);
    const auto angle = juce::degreesToRadians (-90.0f + cents * 0.9f);
    const auto direction = juce::Point<float> (std::cos (angle), std::sin (angle));
    const auto needleStart = dialCentre + direction * (diameter * 0.12f);
    const auto needleEnd = dialCentre + direction * (diameter * 0.38f);
    const auto needleColour = cents >= 0.0f ? cyan : coral;
    g.setColour ((monitorHasValidPitch ? needleColour : mutedText).withAlpha (monitorHasValidPitch ? 1.0f : 0.45f));
    g.drawLine ({ needleStart, needleEnd }, 3.0f);
    g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (needleEnd));

    const auto status = monitor.bypassed ? "BYPASSED" : (monitorHasValidPitch ? "TRACKING" : "NO INPUT");
    const auto statusColour = monitor.bypassed ? amber : (monitorHasValidPitch ? emerald : mutedText);
    g.setColour (statusColour);
    g.fillEllipse (visual.getX() + 18.0f, visual.getY() + 17.0f, 7.0f, 7.0f);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (status, visual.toNearestInt().withTrimmedLeft (31).withHeight (28), juce::Justification::centredLeft);

    g.setColour (cyan);
    g.setFont (juce::FontOptions (48.0f, juce::Font::bold));
    const auto targetPitchClass = monitorHasValidPitch ? monitor.targetNote.retainCharacters ("CDEFGAB#") : juce::String ("--");
    g.drawText (targetPitchClass.isNotEmpty() ? targetPitchClass : "--",
                dial.toNearestInt().withTrimmedTop (diameter * 0.34f).withHeight (52), juce::Justification::centred);
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    const auto correctionText = monitorHasValidPitch
                              ? (displayedAppliedCorrectionCents > 0.05f ? "+" : "")
                                    + juce::String (displayedAppliedCorrectionCents, 1) + " ct"
                              : "--";
    g.drawText (correctionText,
                dial.toNearestInt().withTrimmedTop (diameter * 0.66f).withHeight (16), juce::Justification::centred);

    g.setColour (mutedText.withAlpha (0.65f));
    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    g.drawText ("LOWER", dial.toNearestInt().withTrimmedTop (diameter * 0.82f).withTrimmedLeft (10).withWidth (dial.getWidth() * 0.30f), juce::Justification::centred);
    g.drawText ("RAISE", dial.toNearestInt().withTrimmedTop (diameter * 0.82f).withTrimmedLeft (dial.getWidth() * 0.70f - 10).withWidth (dial.getWidth() * 0.30f), juce::Justification::centred);
    }

    if (! waveformTabSelected)
        for (size_t index = 0; index < 4; ++index)
            drawKnobBackdrop (g, knobBounds[index].toFloat().reduced (4.0f));

    drawPianoKeyboard (g);
}

void VibeAutotuneEditor::drawPanel (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& title) const
{
    if (area.isEmpty())
        return;
    g.setColour (juce::Colours::black.withAlpha (0.42f));
    g.fillRoundedRectangle (area.translated (0.0f, 3.0f), 12.0f);
    auto finish = juce::ColourGradient (surfaceRaised, area.getX(), area.getY(),
                                        surface, area.getX(), area.getBottom(), false);
    g.setGradientFill (finish);
    g.fillRoundedRectangle (area, 12.0f);
    g.setColour (juce::Colour (0xff607888).withAlpha (0.75f));
    g.drawRoundedRectangle (area.reduced (0.5f), 12.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawLine (area.getX() + 10.0f, area.getY() + 1.0f, area.getRight() - 10.0f, area.getY() + 1.0f, 1.0f);
    if (title.isNotEmpty())
    {
        g.setColour (cyan.withAlpha (0.92f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (title, area.toNearestInt().reduced (18, 6).removeFromTop (16), juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xff60778a));
        g.fillEllipse (area.getRight() - 18.0f, area.getY() + 11.0f, 4.0f, 4.0f);
    }
}

void VibeAutotuneEditor::drawKnobBackdrop (juce::Graphics& g, juce::Rectangle<float> area) const
{
    // Match HardwareLookAndFeel exactly. A JUCE rotary reserves the lower
    // part of its bounds for the value box; using the whole bounds here made
    // the decorative outer ring appear below the actual knob.
    auto dialArea = area.reduced (8.0f);
    dialArea.removeFromBottom (22.0f);
    const auto diameter = juce::jmin (dialArea.getWidth(), dialArea.getHeight());
    dialArea = dialArea.withSizeKeepingCentre (diameter, diameter);
    const auto centre = dialArea.getCentre();
    g.setColour (juce::Colours::black.withAlpha (0.32f));
    g.fillEllipse (dialArea.translated (0.0f, 2.0f));
    auto gradient = juce::ColourGradient (cyan.withAlpha (0.09f), centre.x, centre.y,
                                          juce::Colour (0xff0b1117), dialArea.getX(), dialArea.getY(), true);
    g.setGradientFill (gradient);
    g.fillEllipse (dialArea);
    g.setColour (juce::Colour (0xff5d7180).withAlpha (0.8f));
    g.drawEllipse (dialArea, 1.15f);
}

void VibeAutotuneEditor::drawPianoKeyboard (juce::Graphics& g) const
{
    static constexpr std::array<const char*, 12> noteNames {{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }};
    const auto inputProfile = juce::jlimit (0, 3, inputTypeSelector.getSelectedItemIndex());
    const auto vocalRange = vocalKeyboardRanges[static_cast<size_t> (inputProfile)];

    const auto drawScaleNoteState = [&g, this] (int midiNote, juce::Rectangle<float> bounds, bool inVocalRange)
    {
        // Layer 1: the scale map is persistent. It answers only "is this a
        // permitted destination?" and is deliberately independent of the
        // live input-note and current correction target layers below.
        const auto rule = processor.getNoteRuleForMidiNote (midiNote);
        if (! isLegalNoteRule (rule))
            return;

        const auto opacity = inVocalRange ? 0.18f : 0.10f;
        g.setColour (emerald.withAlpha (opacity));
        g.fillRoundedRectangle (bounds.reduced (1.5f), 2.5f);
        g.setColour (emerald.withAlpha (inVocalRange ? 0.58f : 0.32f));
        g.drawRoundedRectangle (bounds.reduced (1.5f), 2.5f, 1.1f);
    };

    const auto drawKeyHighlights = [&g, this] (int midiNote, juce::Rectangle<float> bounds)
    {
        // Layers 2 and 3: raw input and target are separate display states.
        // Raw input can intentionally be on an illegal key (marked '-'); the
        // target remains the legal destination selected by correction logic.
        if (activePianoNote == midiNote)
        {
            g.setColour (cyan.withAlpha (0.32f));
            g.fillRoundedRectangle (bounds.reduced (2.0f), 2.5f);
        }
        if (targetPianoNote == midiNote)
        {
            g.setColour (emerald.withAlpha (0.40f));
            g.fillRoundedRectangle (bounds.reduced (2.0f), 2.5f);
            g.setColour (amber.withAlpha (0.95f));
            g.drawRoundedRectangle (bounds.reduced (2.0f), 2.5f, 2.0f);
        }
    };

    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        if (isBlackKey (midiNote))
            continue;

        const auto bounds = pianoKeyBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)].toFloat().reduced (1.0f);
        const auto inVocalRange = midiNote >= vocalRange.lowestMidi && midiNote <= vocalRange.highestMidi;
        auto keyFinish = juce::ColourGradient (inVocalRange ? juce::Colour (0xfff4fafb) : juce::Colour (0xff526068),
                                               bounds.getX(), bounds.getY(),
                                               inVocalRange ? juce::Colour (0xffadc2ca) : juce::Colour (0xff263137),
                                               bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (keyFinish);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (inVocalRange ? juce::Colour (0xff273d4e) : juce::Colour (0xff1a2429));
        g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
        drawScaleNoteState (midiNote, bounds, inVocalRange);
        drawKeyHighlights (midiNote, bounds);
        g.setColour (inVocalRange ? juce::Colour (0xff172936) : juce::Colour (0xff9daab1).withAlpha (0.45f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        if (midiNote % 12 == 0)
            g.drawText (juce::String (noteNames[0]) + juce::String (midiNote / 12 - 1),
                        bounds.toNearestInt().withTrimmedTop (bounds.getHeight() * 0.64f), juce::Justification::centred);
    }

    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        if (! isBlackKey (midiNote))
            continue;

        const auto bounds = pianoKeyBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)].toFloat();
        const auto inVocalRange = midiNote >= vocalRange.lowestMidi && midiNote <= vocalRange.highestMidi;
        auto keyFinish = juce::ColourGradient (inVocalRange ? juce::Colour (0xff2b3e4f) : juce::Colour (0xff172126),
                                               bounds.getX(), bounds.getY(),
                                               inVocalRange ? juce::Colour (0xff070c12) : juce::Colour (0xff05080a),
                                               bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill (keyFinish);
        g.fillRoundedRectangle (bounds, 3.0f);
        g.setColour (inVocalRange ? juce::Colour (0xff40566b) : juce::Colour (0xff19242a));
        g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
        drawScaleNoteState (midiNote, bounds, inVocalRange);
        drawKeyHighlights (midiNote, bounds);
    }

    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        const auto bounds = noteStatusBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)].toFloat();
        const auto rule = processor.getNoteRuleForMidiNote (midiNote);
        const auto illegal = ! isLegalNoteRule (rule);
        const auto bypass = rule == NoteRule::BypassLegal;
        g.setColour (illegal ? juce::Colour (0xff5c365f) : (bypass ? juce::Colour (0xff24574d) : juce::Colour (0xff303b42)));
        g.fillRoundedRectangle (bounds, 2.5f);
        g.setColour (illegal ? juce::Colour (0xffd68ad7) : (bypass ? emerald : juce::Colour (0xff667981)));
        g.drawRoundedRectangle (bounds, 2.5f, 1.0f);
        // Rule glyphs are a pure view of NoteRule; live input/target layers
        // above never overwrite this marker.
        const auto symbol = juce::String::fromUTF8 (noteRuleGlyph (rule));
        g.setColour (illegal ? juce::Colour (0xffffb5ff) : (bypass ? emerald.brighter (0.18f) : mutedText));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (symbol, bounds.toNearestInt(), juce::Justification::centred);
    }
}

void VibeAutotuneEditor::drawWaveformScope (juce::Graphics& g) const
{
    auto area = waveformBounds.toFloat().reduced (14.0f, 25.0f);
    if (waveformScopeFrameCount < 2 || area.getWidth() <= 2.0f)
    {
        g.setColour (mutedText.withAlpha (0.65f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("WAITING FOR INPUT", area.toNearestInt(), juce::Justification::centred);
        return;
    }

    const auto centreY = area.getCentreY();
    g.setColour (juce::Colour (0xff4d6070).withAlpha (0.35f));
    g.drawHorizontalLine (juce::roundToInt (centreY), area.getX(), area.getRight());
    for (int division = 1; division < 8; ++division)
    {
        const auto x = area.getX() + area.getWidth() * static_cast<float> (division) / 8.0f;
        g.setColour (juce::Colour (0xff4d6070).withAlpha (0.18f));
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
    }

    const auto count = waveformScopeFrameCount;
    const auto xFor = [&area, count] (int index)
    {
        return area.getX() + area.getWidth() * static_cast<float> (index) / static_cast<float> (juce::jmax (1, count - 1));
    };
    const auto waveScale = area.getHeight() * 0.42f;
    for (int index = 0; index < count; ++index)
    {
        const auto& frame = waveformScopeFrames[static_cast<size_t> (index)];
        const auto x = xFor (index);
        g.setColour ((frame.voiced ? cyan : mutedText).withAlpha (frame.voiced ? 0.52f : 0.22f));
        g.drawLine ({ x, centreY - frame.maximum * waveScale, x, centreY - frame.minimum * waveScale }, 1.0f);
    }

    // Pitch error relative to the selected target: cyan is input F0 and
    // coral is the correction delivered to the renderer. The amber zero line
    // is the target itself, so a smooth portamento appears as a slope instead
    // of a semitone staircase.
    constexpr float centsRange = 240.0f;
    const auto pitchY = [&area] (float cents)
    {
        return area.getCentreY() - juce::jlimit (-1.0f, 1.0f, cents / centsRange) * area.getHeight() * 0.43f;
    };
    juce::Path inputPath, renderedPath;
    auto inputStarted = false;
    for (int index = 0; index < count; ++index)
    {
        const auto& frame = waveformScopeFrames[static_cast<size_t> (index)];
        if (! frame.voiced || ! std::isfinite (frame.inputPitchCents) || ! std::isfinite (frame.targetPitchCents))
        {
            inputStarted = false;
            continue;
        }
        const auto x = xFor (index);
        const auto inputY = pitchY (frame.inputPitchCents - frame.targetPitchCents);
        const auto renderedY = pitchY ((frame.inputPitchCents - frame.targetPitchCents) + frame.deliveredCorrectionCents);
        if (! inputStarted)
        {
            inputPath.startNewSubPath (x, inputY);
            renderedPath.startNewSubPath (x, renderedY);
            inputStarted = true;
        }
        else
        {
            inputPath.lineTo (x, inputY);
            renderedPath.lineTo (x, renderedY);
        }
    }
    g.setColour (cyan.brighter (0.12f));
    g.strokePath (inputPath, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
    g.setColour (coral.withAlpha (0.94f));
    g.strokePath (renderedPath, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved));
    g.setColour (amber.withAlpha (0.8f));
    g.drawHorizontalLine (juce::roundToInt (centreY), area.getX(), area.getRight());
    g.setColour (mutedText);
    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    g.drawText ("CYAN INPUT F0", area.toNearestInt().withTrimmedTop (2).withWidth (110), juce::Justification::centredLeft);
    g.drawText ("CORAL RENDERED", area.toNearestInt().withTrimmedTop (2).withTrimmedLeft (118).withWidth (120), juce::Justification::centredLeft);
    g.drawText ("AMBER TARGET", area.toNearestInt().withTrimmedTop (2).withTrimmedLeft (245).withWidth (110), juce::Justification::centredLeft);
}

void VibeAutotuneEditor::resized()
{
    auto area = getLocalBounds().reduced (14);
    headerBounds = area.removeFromTop (64);
    area.removeFromTop (8);
    settingsBounds = area.removeFromTop (136);
    area.removeFromTop (10);
    pianoBounds = area.removeFromBottom (120);
    area.removeFromBottom (10);
    if (waveformTabSelected)
    {
        waveformBounds = area;
        mainControlsBounds = {};
    }
    else
    {
        mainControlsBounds = area;
        waveformBounds = {};
    }

    bypassButton.setBounds (headerBounds.getRight() - 112, headerBounds.getY() + 13, 92, 32);
    dashboardTabButton.setBounds (headerBounds.getX() + 302, headerBounds.getY() + 13, 104, 32);
    waveformTabButton.setBounds (headerBounds.getX() + 412, headerBounds.getY() + 13, 100, 32);
    diagnosticExportButton.setBounds (headerBounds.getRight() - 244, headerBounds.getY() + 13, 122, 32);
    diagnosticCaptureButton.setBounds (headerBounds.getRight() - 358, headerBounds.getY() + 13, 104, 32);
    groupOctavesButton.setBounds (pianoBounds.getRight() - 156, pianoBounds.getY() + 6, 138, 18);
    auto strip = settingsBounds.reduced (18, 24);
    auto selectorRow = strip.removeFromTop (42);
    const auto layoutSelector = [] (juce::Rectangle<int> column, juce::Label& label, juce::ComboBox& selector)
    {
        label.setBounds (column.removeFromTop (15));
        selector.setBounds (column.withTrimmedTop (2));
    };
    const auto voiceWidth = juce::jlimit (130, 190, selectorRow.getWidth() / 3);
    const auto keyWidth = juce::jlimit (62, 92, selectorRow.getWidth() / 6);
    layoutSelector (selectorRow.removeFromLeft (voiceWidth).withTrimmedRight (8), inputTypeLabel, inputTypeSelector);
    layoutSelector (selectorRow.removeFromLeft (keyWidth).reduced (4, 0), keyLabel, keySelector);
    layoutSelector (selectorRow.reduced (8, 0), scaleLabel, scaleSelector);

    // Existing engine parameters form the compact upper strip.
    utilityControlsBounds = strip.withTrimmedTop (6);
    auto utilityRow = utilityControlsBounds;
    const auto layoutUtilityKnob = [this, &utilityRow] (int index, juce::Label& label, juce::Slider& slider)
    {
        auto cell = utilityRow.removeFromLeft (utilityRow.getWidth() / (4 - index)).reduced (5, 0);
        label.setBounds (cell.removeFromTop (15));
        knobBounds[static_cast<size_t> (4 + index)] = cell;
        slider.setBounds (cell);
    };
    layoutUtilityKnob (0, transposeLabel, transposeSlider);
    layoutUtilityKnob (1, referencePitchLabel, referencePitchSlider);
    layoutUtilityKnob (2, trackingLabel, trackingSlider);
    layoutUtilityKnob (3, voicedColourLabel, voicedColourSlider);

    auto correctionArea = mainControlsBounds.reduced (20, 30);
    const auto sideWidth = juce::jlimit (145, 230, static_cast<int> (correctionArea.getWidth() * 0.24f));
    auto leftColumn = correctionArea.removeFromLeft (sideWidth).withTrimmedRight (8);
    auto rightColumn = correctionArea.removeFromRight (sideWidth).withTrimmedLeft (8);
    monitorBounds = correctionArea.reduced (6, 0);

    const auto layoutMainKnob = [this] (int index, juce::Rectangle<int> cell,
                                        juce::Label& label, juce::Slider& slider)
    {
        cell.reduce (2, 0);
        label.setBounds (cell.removeFromTop (18));
        const auto diameter = juce::jmin (cell.getWidth(), cell.getHeight());
        knobBounds[static_cast<size_t> (index)] = cell.withSizeKeepingCentre (diameter, diameter);
        slider.setBounds (knobBounds[static_cast<size_t> (index)]);
    };

    const auto leftTop = leftColumn.removeFromTop (leftColumn.getHeight() / 2);
    const auto rightTop = rightColumn.removeFromTop (rightColumn.getHeight() / 2);
    layoutMainKnob (0, leftTop, retuneSpeedLabel, retuneSpeedSlider);
    layoutMainKnob (1, leftColumn, pitchFlexibleLabel, pitchFlexibleSlider);
    layoutMainKnob (2, rightTop, humanizeLabel, humanizeSlider);
    layoutMainKnob (3, rightColumn, naturalVibratoLabel, naturalVibratoSlider);

    auto keyboard = pianoBounds.reduced (18, 26);
    auto statusRow = keyboard.removeFromTop (18);
    keyboard.removeFromTop (5);
    const auto whiteKeyCount = 35; // C1–B5 contains five complete octaves.
    const auto whiteKeyWidth = juce::jmax (1, keyboard.getWidth() / whiteKeyCount);
    const auto blackKeyWidth = juce::jmax (8, static_cast<int> (whiteKeyWidth * 0.58f));
    auto whiteKeyArea = keyboard;
    int whiteKeyIndex = 0;
    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        if (isBlackKey (midiNote))
            continue;
        const auto isLastWhite = ++whiteKeyIndex == whiteKeyCount;
        pianoKeyBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)]
            = whiteKeyArea.removeFromLeft (isLastWhite ? whiteKeyArea.getWidth() : whiteKeyWidth);
    }

    const auto blackKeyHeight = static_cast<int> (keyboard.getHeight() * 0.58f);
    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        if (! isBlackKey (midiNote))
            continue;
        const auto& leftWhite = pianoKeyBounds[static_cast<size_t> (midiNote - 1 - keyboardFirstMidiNote)];
        pianoKeyBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)]
            = { leftWhite.getRight() - blackKeyWidth / 2, leftWhite.getY(), blackKeyWidth, blackKeyHeight };
    }

    // Each rule marker tracks its physical piano key, rather than forming a
    // separate 12-cell strip. This makes an out-of-scale marker unambiguous
    // across all octaves and keeps the edit target smaller than its key.
    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        const auto key = pianoKeyBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)];
        const auto markerWidth = juce::jmax (8, juce::jmin (16, key.getWidth() - 3));
        noteStatusBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)]
            = { key.getCentreX() - markerWidth / 2, statusRow.getY() + 2, markerWidth, juce::jmax (10, statusRow.getHeight() - 4) };
    }
}

void VibeAutotuneEditor::mouseDown (const juce::MouseEvent& event)
{
    for (int midiNote = keyboardFirstMidiNote; midiNote <= keyboardLastMidiNote; ++midiNote)
    {
        if (! noteStatusBounds[static_cast<size_t> (midiNote - keyboardFirstMidiNote)].contains (event.getPosition()))
            continue;

        const auto currentRule = processor.getGroupOctaves()
                               ? processor.getNoteRuleForPitchClass (midiNote % 12)
                               : processor.getNoteRuleForMidiNote (midiNote);
        const auto nextRule = static_cast<NoteRule> ((static_cast<int> (currentRule) + 1) % 5);
        if (processor.getGroupOctaves())
            processor.setNoteRuleForPitchClass (midiNote % 12, nextRule);
        else
            processor.setNoteRuleForMidiNote (midiNote, nextRule);
        repaint (pianoBounds);
        return;
    }
}

void VibeAutotuneEditor::timerCallback()
{
    monitor = processor.getPitchMonitorData();
    waveformScopeFrameCount = processor.copyWaveformScopeFrames (waveformScopeFrames.data(),
                                                                   static_cast<int> (waveformScopeFrames.size()));
    const auto tracking = processor.getPitchTrackingData();
    monitorHasValidPitch = monitor.voiced && tracking.voiced && std::isfinite (tracking.stableF0Hz)
                        && tracking.stableF0Hz > 0.0 && std::isfinite (tracking.confidence)
                        && tracking.confidence >= 0.40;
    const auto requestedMeterCents = monitorHasValidPitch
                                   ? juce::jlimit (-100.0f, 100.0f, monitor.correctionCents) : 0.0f;
    // 30 Hz UI-only one-pole: meter returns gracefully to neutral on an
    // unvoiced/low-confidence frame without affecting DSP or latency.
    displayedAppliedCorrectionCents += (requestedMeterCents - displayedAppliedCorrectionCents)
                                     * (monitorHasValidPitch ? 0.40f : 0.28f);
    const auto detectedMidiNote = processor.getLatestDetectedMidiNote();
    const auto targetMidiNote = processor.getLatestTargetMidiNote();
    activePianoNote = monitorHasValidPitch && detectedMidiNote >= 0 ? detectedMidiNote : -1;
    targetPianoNote = monitorHasValidPitch && targetMidiNote >= 0 ? targetMidiNote : -1;
    groupOctavesButton.setToggleState (processor.getGroupOctaves(), juce::dontSendNotification);
    const auto scaleCaption = processor.hasCustomNotePolicy() ? "SCALE *" : "SCALE";
    if (scaleLabel.getText() != scaleCaption)
        scaleLabel.setText (scaleCaption, juce::dontSendNotification);
    repaint();
}
