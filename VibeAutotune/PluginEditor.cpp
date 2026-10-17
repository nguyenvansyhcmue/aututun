#include "PluginEditor.h"
#include "VibeAutotuneBinaryData.h"
#include <cmath>

namespace
{
const auto champagne = juce::Colour (0xffd5c5a7), brass = juce::Colour (0xffd5a34b);
const auto graphite = juce::Colour (0xff171816), raised = juce::Colour (0xff2a2a26);
const auto glass = juce::Colour (0xff070908), ivory = juce::Colour (0xffe3ddcf);
const auto ice = juce::Colour (0xffa9c0ca), smokeInk = juce::Colour (0xffaaa69d);
const auto mint = juce::Colour (0xff91bba0), muted = juce::Colour (0xff716f67);
constexpr int firstKey = 24, lastKey = 83;
constexpr bool blackKey (int n) { const int p = (n % 12 + 12) % 12; return p == 1 || p == 3 || p == 6 || p == 8 || p == 10; }
struct Range { int low, high; };
constexpr std::array<Range, 4> voiceRanges {{{54,83}, {45,77}, {40,71}, {33,62}}};

struct LiveTuneLayoutMetrics
{
    static constexpr float designWidth = 1448.0f;
    // The visual chassis deliberately finishes directly beneath the Note Map.
    // Keeping a taller artboard here produced an empty black "footer" inside
    // the AU editor, before Logic's own unavoidable host footer.
    static constexpr float designHeight = 1008.0f;

    explicit LiveTuneLayoutMetrics (juce::Rectangle<int> client)
        : scale (juce::jmin ((float) client.getWidth() / designWidth, (float) client.getHeight() / designHeight)),
          originX ((float) client.getX() + ((float) client.getWidth() - designWidth * scale) * .5f),
          // The plugin surface is top-anchored.  Vertical centring created a
          // black band above the chassis whenever a host supplied a few extra
          // content pixels for its native title bar.
          originY ((float) client.getY()) {}

    juce::Rectangle<int> rect (float x, float y, float width, float height) const noexcept
    {
        return { juce::roundToInt (originX + x * scale), juce::roundToInt (originY + y * scale),
                 juce::roundToInt (width * scale), juce::roundToInt (height * scale) };
    }

    float scale = 1.0f, originX = 0.0f, originY = 0.0f;
};

juce::Image loadUiAsset (const char* bytes, int size)
{
    return juce::ImageFileFormat::loadFrom (bytes, (size_t) size);
}

const juce::Image& blackGlassFrame()
{
    static const auto image = loadUiAsset (VibeAutotuneBinaryData::ui_02_black_glass_frame_exact_png,
                                            VibeAutotuneBinaryData::ui_02_black_glass_frame_exact_pngSize);
    jassert (image.isValid());
    return image;
}

const juce::Image& blackGlassInterior()
{
    // The kit image is a crop that includes a partial chrome frame. Keep only
    // its neutral glass/reflection field; the complete portrait frame is
    // drawn at runtime so no baked partial border crosses dynamic labels.
    static const auto image = blackGlassFrame().getClippedImage ({ 20, 25, 230, 170 });
    jassert (image.isValid());
    return image;
}

const juce::Image& analogKnobFace()
{
    static const auto image = loadUiAsset (VibeAutotuneBinaryData::ui_knob_assembly_neutral_v2_png,
                                            VibeAutotuneBinaryData::ui_knob_assembly_neutral_v2_pngSize);
    jassert (image.isValid());
    return image;
}

const juce::Image& champagnePanelTexture()
{
    static const auto image = loadUiAsset (VibeAutotuneBinaryData::ui_champagne_brushed_neutral_v1_png,
                                            VibeAutotuneBinaryData::ui_champagne_brushed_neutral_v1_pngSize);
    jassert (image.isValid());
    return image;
}

class HardwareLook final : public juce::LookAndFeel_V4
{
public:
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
    {
        const auto bounds = slider.getLocalBounds();
        const auto large = slider.getName() == "RETUNE";
        const auto utility = slider.getName() == "TRANSPOSE" || slider.getName() == "REFERENCE"
                          || slider.getName() == "TRACKING" || slider.getName() == "VOICED COLOUR";
        const auto textWidth = juce::roundToInt ((float) bounds.getWidth() * (large ? .55f : utility ? .68f : .62f));
        const auto textHeight = juce::roundToInt ((float) bounds.getHeight() * (large ? .13f : utility ? .21f : .25f));
        juce::Slider::SliderLayout layout;
        layout.textBoxBounds = { bounds.getCentreX() - textWidth / 2, bounds.getBottom() - textHeight,
                                 textWidth, textHeight };
        layout.sliderBounds = bounds.withTrimmedTop (large ? juce::jmax (18, bounds.getHeight() / 13) : 0)
                                    .withTrimmedBottom (textHeight + juce::jmax (7, bounds.getHeight() / 30));
        const auto offset = large ? 12.0f : utility ? 0.0f : 6.0f;
        layout.sliderBounds.translate (0, juce::roundToInt (offset * (float) bounds.getHeight() / (large ? 389.0f : 165.0f)));
        return layout;
    }

    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        // JUCE's rotary value box is a child Label of the Slider. Styling it
        // here gives every live value a consistent recessed hardware window
        // without affecting the separately-added parameter captions.
        if (dynamic_cast<juce::Slider*> (label.getParentComponent()) == nullptr)
        {
            LookAndFeel_V4::drawLabel (g, label);
            return;
        }
        auto r = label.getLocalBounds().toFloat().reduced (.5f);
        g.setColour (juce::Colours::black.withAlpha (.58f)); g.fillRoundedRectangle (r.translated (0, 2.0f), 7.0f);
        auto inset = juce::ColourGradient (juce::Colour (0xff1c1d19), r.getX(), r.getY(), juce::Colour (0xff050605), r.getX(), r.getBottom(), false);
        g.setGradientFill (inset); g.fillRoundedRectangle (r, 7.0f);
        g.setColour (champagne.withAlpha (.70f)); g.drawRoundedRectangle (r, 7.0f, .85f);
        g.setColour (ivory); g.setFont (juce::FontOptions (juce::jmax (10.0f, r.getHeight() * .61f), juce::Font::bold));
        g.drawText (label.getText(), r.toNearestInt().reduced (4, 0), juce::Justification::centred, true);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool down,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        g.setColour (juce::Colours::black.withAlpha (0.30f)); g.fillRoundedRectangle (r.translated (0, 1.5f), 5.0f);
        auto inset = juce::ColourGradient (juce::Colour (0xff1b1c19), 0, 0, juce::Colour (0xff080908), 0, (float) height, false);
        g.setGradientFill (inset); g.fillRoundedRectangle (r, 5.0f);
        g.setColour ((box.hasKeyboardFocus (true) || down ? brass : champagne.withAlpha (0.56f))); g.drawRoundedRectangle (r, 5.0f, 1.0f);
        const auto cx = (float) buttonX + (float) buttonW * .5f;
        const auto cy = (float) buttonY + (float) buttonH * .52f;
        juce::Path arrow; arrow.startNewSubPath (cx - 5.0f, cy - 2.0f); arrow.lineTo (cx, cy + 3.0f); arrow.lineTo (cx + 5.0f, cy - 2.0f);
        g.setColour (brass); g.strokePath (arrow, juce::PathStrokeType (1.8f));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override { return juce::FontOptions (juce::jmax (13.0f, box.getHeight() * .36f), juce::Font::bold); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool over, bool down) override
    {
        const auto press = [&]
        {
            if (const auto* analog = dynamic_cast<const AnalogTextButton*> (&button)) return analog->getPressDepth();
            return down ? 1.0f : 0.0f;
        }();
        if (button.getButtonText() != "POWER" && button.getButtonText() != "SETTINGS" && button.getButtonText() != "EDIT NOTES")
        {
            LookAndFeel_V4::drawButtonBackground (g, button, button.findColour (juce::TextButton::buttonColourId), over, down);
            return;
        }
        if (button.getButtonText() == "EDIT NOTES")
        {
            auto r = button.getLocalBounds().toFloat().reduced (.5f).translated (0, press * 1.3f);
            g.setColour (juce::Colours::black.withAlpha (.36f + press * .16f)); g.fillRoundedRectangle (r.translated (0, 1.5f - press * .9f), 4.0f);
            auto fill = juce::ColourGradient (juce::Colour (0xff292a26), r.getX(), r.getY(), juce::Colour (0xff10110f), r.getX(), r.getBottom(), false);
            g.setGradientFill (fill); g.fillRoundedRectangle (r, 4.0f);
            g.setColour ((over || button.getToggleState() ? brass : champagne).withAlpha (.70f)); g.drawRoundedRectangle (r, 4.0f, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (.11f - press * .075f)); g.drawHorizontalLine (juce::roundToInt (r.getY() + 1), r.getX() + 5, r.getRight() - 5);
            return;
        }
        auto r = button.getLocalBounds().toFloat().reduced (2.0f);
        const auto d = juce::jmin (r.getWidth(), r.getHeight()); r = r.withSizeKeepingCentre (d, d).translated (0, press * 1.2f);
        const auto active = button.getButtonText() == "POWER" ? ! button.getToggleState() : button.getToggleState();
        // Keep the active halo as a circular rim only.  A filled expansion
        // reads as a square host-button backing at small editor scales.
        if (active && button.getButtonText() == "POWER")
        {
            g.setColour (brass.withAlpha (.24f));
            g.drawEllipse (r.expanded (4.0f), 2.0f);
        }
        g.setColour (juce::Colours::black.withAlpha (.55f + press * .17f)); g.fillEllipse (r.translated (0, 2.0f - press * 1.3f));
        auto buttonMetal = juce::ColourGradient (juce::Colour (0xff34332d), r.getX(), r.getY(), juce::Colour (0xff050605), r.getX(), r.getBottom(), false);
        g.setGradientFill (buttonMetal); g.fillEllipse (r);
        g.setColour (button.getButtonText() == "POWER" ? brass : champagne.withAlpha (.62f)); g.drawEllipse (r.reduced (1), active ? 2.0f : 1.4f);
        g.setColour (juce::Colour (0xff080907)); g.drawEllipse (r.reduced (5), 1.2f);
        g.setColour (juce::Colours::white.withAlpha ((over ? .18f : .10f) * (1.0f - press * .62f))); g.drawEllipse (r.reduced (4), .85f);
        if (active || down) { auto lamp = juce::ColourGradient (juce::Colour (0xff6b4318), r.getCentreX(), r.getCentreY()-r.getHeight()*.18f, juce::Colour (0xff17120b), r.getCentreX(), r.getBottom(), true); g.setGradientFill (lamp); g.fillEllipse (r.reduced (7)); }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool down) override
    {
        const auto press = [&]
        {
            if (const auto* analog = dynamic_cast<const AnalogTextButton*> (&button)) return analog->getPressDepth();
            return down ? 1.0f : 0.0f;
        }();
        if (button.getButtonText() == "EDIT NOTES")
        {
            auto r = button.getLocalBounds().toFloat().translated (0, press * 1.3f);
            const auto compact = r.getWidth() < 72.0f;
            const auto pencilX = compact ? r.getCentreX() - 4.0f : r.getX() + 16.0f;
            g.setColour (brass.withAlpha (.90f));
            g.drawLine (pencilX, r.getCentreY() + 3, pencilX + 8, r.getCentreY() - 5, 1.7f);
            g.drawLine (pencilX + 2, r.getCentreY() + 5, pencilX - 1, r.getCentreY() + 6, 1.2f);
            if (! compact)
            {
                g.setColour (ivory); g.setFont (juce::FontOptions (juce::jmax (10.0f, r.getHeight() * .30f), juce::Font::bold));
                g.drawText (button.getButtonText(), r.toNearestInt().withTrimmedLeft (30), juce::Justification::centred);
            }
            return;
        }
        if (button.getButtonText() != "POWER" && button.getButtonText() != "SETTINGS")
        { LookAndFeel_V4::drawButtonText (g, button, false, false); return; }
        auto r = button.getLocalBounds().toFloat().translated (0, press * 1.2f); const auto c = r.getCentre();
        if (button.getButtonText() == "POWER")
        {
            g.setColour (ivory); g.drawEllipse (c.x - r.getWidth()*.18f, c.y - r.getHeight()*.12f, r.getWidth()*.36f, r.getHeight()*.36f, 2.0f);
            g.drawLine (c.x, c.y - r.getHeight()*.26f, c.x, c.y - r.getHeight()*.03f, 2.0f);
        }
        else
        {
            // A real vector gear rather than a text glyph, so it retains the
            // same hardware language at every editor scale factor.
            g.setColour (ivory.withAlpha (.82f));
            juce::Path gear;
            constexpr int teeth = 8;
            for (int i = 0; i < teeth * 2; ++i)
            {
                const auto a = -juce::MathConstants<float>::halfPi + (float) i * juce::MathConstants<float>::twoPi / (float) (teeth * 2);
                const auto radius = r.getWidth() * (i % 2 == 0 ? .245f : .185f);
                const auto p = c + juce::Point<float> (std::cos (a), std::sin (a)) * radius;
                if (i == 0) gear.startNewSubPath (p); else gear.lineTo (p);
            }
            gear.closeSubPath(); g.fillPath (gear); g.setColour (juce::Colour (0xff10110f)); g.fillEllipse (c.x - r.getWidth()*.075f, c.y - r.getWidth()*.075f, r.getWidth()*.15f, r.getWidth()*.15f);
        }
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool over, bool down) override
    {
        if (button.getButtonText() != "GROUP OCTAVES") { LookAndFeel_V4::drawToggleButton (g, button, over, down); return; }
        const auto press = [&]
        {
            if (const auto* analog = dynamic_cast<const AnalogToggleButton*> (&button)) return analog->getPressDepth();
            return down ? 1.0f : 0.0f;
        }();
        auto r = button.getLocalBounds().toFloat().translated (0, press);
        auto pill = juce::Rectangle<float> (r.getRight() - 56.0f, r.getCentreY() - 13.0f, 52.0f, 26.0f);
        g.setColour (juce::Colours::black.withAlpha (.62f)); g.fillRoundedRectangle (pill.translated (0, 2.0f), 13.0f);
        g.setColour (juce::Colour (0xff171815)); g.fillRoundedRectangle (pill, 13.0f);
        g.setColour (button.getToggleState() ? brass : champagne.withAlpha (.46f)); g.drawRoundedRectangle (pill, 13.0f, 1.2f);
        const auto bulb = button.getToggleState() ? pill.getRight() - 23.0f : pill.getX() + 4.0f;
        if (button.getToggleState()) { g.setColour (brass.withAlpha (.20f)); g.fillEllipse (bulb - 4, pill.getY(), 26, 26); }
        g.setColour (button.getToggleState() ? juce::Colour (0xffffc962) : juce::Colour (0xff6f6d65)); g.fillEllipse (bulb, pill.getY() + 4.0f, 18.0f, 18.0f);
        g.setColour (champagne); g.setFont (juce::FontOptions (juce::jmax (10.0f, r.getHeight() * .28f), juce::Font::plain)); g.drawText (button.getButtonText(), r.toNearestInt().withTrimmedRight (62), juce::Justification::centredRight);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float position, float start, float end, juce::Slider& slider) override
    {
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        const auto provisionalDiameter = juce::jmin (r.getWidth(), r.getHeight());
        const auto provisionalLarge = provisionalDiameter > 180.0f;
        r = r.reduced (provisionalLarge ? 0.0f : 5.0f);
        const auto d = juce::jmin (r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre (d, d);
        const auto c = r.getCentre();
        const auto large = d > 180.0f;
        const auto angle = [&] { if (const auto* analog = dynamic_cast<const AnalogRotaryControl*> (&slider)) return analog->getDisplayAngleRadians(); return start + position * (end - start); }();
        const auto press = [&] { if (const auto* analog = dynamic_cast<const AnalogRotaryControl*> (&slider)) return analog->getPressDepth(); return 0.0f; }();
        // The source crops include champagne panel pixels around the knob.
        // Recreate that geometry instead of painting a second full grey/dark
        // disc: ticks sit directly on the panel and the socket hugs the body.
        const auto utility = slider.getName() == "TRANSPOSE" || slider.getName() == "REFERENCE"
                          || slider.getName() == "TRACKING" || slider.getName() == "VOICED COLOUR";
        const auto body = r.reduced (d * (large ? .015f : utility ? .025f : .020f));
        const auto socket = body.reduced (d * (large ? .047f : .068f));
        g.setColour (juce::Colours::black.withAlpha (0.31f + press * .15f)); g.fillEllipse (socket.expanded (d * .022f).translated (0, d * (.020f - press * .010f)));
        g.setColour (juce::Colour (0xff080907)); g.fillEllipse (socket.expanded (d * .010f));
        g.setColour (champagne.withAlpha (.48f)); g.drawEllipse (socket.expanded (d * .006f), large ? 1.15f : .85f);
        for (int notch = 0; notch < 31; ++notch)
        {
            const auto a = start + (end - start) * (float) notch / 30.0f;
            const auto v = juce::Point<float> (std::cos (a), std::sin (a));
            const auto major = notch == 0 || notch == 15 || notch == 30 || notch % 5 == 0;
            g.setColour ((major ? graphite : juce::Colour (0xff5f5a50)).withAlpha (major ? .88f : .68f));
            g.drawLine ({ c + v * d * (major ? .455f : .468f), c + v * d * .498f }, major ? 1.05f : .65f);
        }

        // Endpoint legends live outside the rotating assembly, matching the
        // engraved scale on the reference and remaining readable at any value.
        // A medium-weight humanist sans survives Logic's scaled AU window far
        // better than the former thin default face. The warm lower highlight
        // is intentionally restrained: it reads as shallow faceplate engraving
        // rather than a drop shadow or a second line of text.
        const auto drawScaleLegend = [&] (const juce::String& text, juce::Rectangle<float> bounds, float height)
        {
            auto font = juce::Font (juce::FontOptions ("Avenir Next", "Medium", height))
                            .withHorizontalScale (.94f).withExtraKerningFactor (.015f);
            g.setFont (font);
            g.setColour (champagne.withAlpha (.24f));
            g.drawText (text, bounds.translated (0.0f, 1.0f).toNearestInt(), juce::Justification::centred, false);
            g.setColour (juce::Colour (0xff35322d).withAlpha (.96f));
            g.drawText (text, bounds.toNearestInt(), juce::Justification::centred, false);
        };
        if (slider.getName() == "RETUNE")
        {
            const auto legendHeight = juce::jmax (10.5f, d * .046f);
            drawScaleLegend ("0", { r.getX() - 4, r.getBottom() + 3, 22, 15 }, legendHeight);
            drawScaleLegend ("400", { r.getRight() - 22, r.getBottom() + 3, 32, 15 }, legendHeight);
        }
        else if (slider.getName() == "TRANSPOSE")
        {
            const auto legendHeight = juce::jmax (9.25f, d * .078f);
            drawScaleLegend ("-12", { r.getX() - 10, r.getBottom() + 2, 25, 13 }, legendHeight);
            drawScaleLegend ("+12", { r.getRight() - 15, r.getBottom() + 2, 28, 13 }, legendHeight);
        }
        else if (slider.getName() == "REFERENCE" || slider.getName() == "TRACKING" || slider.getName() == "VOICED COLOUR")
        {
            const auto reference = slider.getName() == "REFERENCE";
            const auto legendHeight = juce::jmax (9.25f, d * .078f);
            drawScaleLegend (reference ? "400" : "0", { r.getX() - 10, r.getBottom() + 2, 28, 13 }, legendHeight);
            drawScaleLegend (reference ? "480" : "100", { r.getRight() - 18, r.getBottom() + 2, 32, 13 }, legendHeight);
        }

        // The socket and scale remain fixed, while the complete mechanical
        // cap assembly rotates. The user therefore perceives the hardware
        // itself turning instead of seeing only a detached pointer move.
        const auto pressedScale = 1.0f - press * .008f;
        const auto cap = body.reduced (d * (large ? .083f : utility ? .095f : .100f));
        // Complete photoreal neutral assembly reconstructed from the supplied
        // target: toothed outer grip, polished rims and spun-metal face.
        // Keep a real stationary scale annulus around the rotating hardware.
        // The previous near-full-size texture covered the engraved ticks and
        // made the whole control read as one flat circular bitmap.
        const auto texturedBody = body.reduced (d * (large ? .052f : .074f)).translated (0.0f, press * 1.5f);
        g.saveState();
        g.addTransform (juce::AffineTransform::rotation (angle, c.x, c.y)
                            .scaled (pressedScale, pressedScale, c.x, c.y));
        g.drawImageWithin (analogKnobFace(), juce::roundToInt (texturedBody.getX()), juce::roundToInt (texturedBody.getY()),
                           juce::roundToInt (texturedBody.getWidth()), juce::roundToInt (texturedBody.getHeight()),
                           juce::RectanglePlacement::centred);
        g.restoreState();
        // Keep the progress ring primitive-only. Some macOS hosts repaint a
        // plug-in editor while its peer is being attached; avoiding a mutable
        // Path here prevents the allocator corruption observed in that path.
        const auto amberArc = body.reduced (d * .024f);
        const auto progressAngle = std::isfinite (angle) ? juce::jlimit (start, end, angle) : start;
        const auto progress = (progressAngle - start) / (end - start);
        const auto segments = 36;
        const auto drawProgressRing = [&] (juce::Colour colour, float thickness)
        {
            g.setColour (colour);
            for (int i = 0; i < segments; ++i)
            {
                const auto from = (float) i / (float) segments;
                const auto to = (float) (i + 1) / (float) segments;
                if (from >= progress)
                    break;

                const auto clampedTo = juce::jmin (to, progress);
                const auto pointAt = [&] (float fraction)
                {
                    const auto a = start + (end - start) * fraction;
                    return c + juce::Point<float> (std::cos (a) * amberArc.getWidth() * .5f,
                                                    std::sin (a) * amberArc.getHeight() * .5f);
                };
                g.drawLine ({ pointAt (from), pointAt (clampedTo) }, thickness);
            }
        };
        drawProgressRing (brass.withAlpha (.20f), large ? 7.0f : 4.0f);
        drawProgressRing (brass.withAlpha (.98f), large ? 4.5f : 2.6f);
        g.saveState();
        g.addTransform (juce::AffineTransform::rotation (angle, c.x, c.y)
                            .scaled (pressedScale, pressedScale, c.x, c.y)
                            .translated (0.0f, press * 1.5f));
        // A short inlaid paint mark at the rim reads as a real hardware cap;
        // it deliberately does not run from the centre like a clock hand.
        const auto pointerBaseY = c.y - cap.getHeight() * .245f;
        const auto pointerEndY = c.y - cap.getHeight() * .435f;
        g.setColour (juce::Colours::black.withAlpha (.52f)); g.drawLine ({ { c.x, pointerBaseY + 1.2f }, { c.x, pointerEndY + 1.2f } }, large ? 5.2f : 3.5f);
        g.setColour (brass.withAlpha (.92f)); g.drawLine ({ { c.x, pointerBaseY }, { c.x, pointerEndY } }, large ? 2.45f : 1.55f);
        g.restoreState();
        if (slider.isMouseOverOrDragging())
        {
            g.setColour (brass.withAlpha (slider.isMouseButtonDown() ? .24f : .13f));
            g.drawEllipse (body.expanded (d * .018f), large ? 1.15f : .85f);
        }
    }
};
HardwareLook hardware;
}

VibeAutotuneEditor::VibeAutotuneEditor (VibeAutotuneProcessor& p) : AudioProcessorEditor (&p), processor (p), monitor (processor.getPitchMonitorData())
{
    configureSelector (inputTypeSelector, inputTypeLabel, "VOICE", { "Soprano", "Alto", "Tenor", "Low" });
    configureSelector (keySelector, keyLabel, "KEY", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    configureSelector (scaleSelector, scaleLabel, "SCALE", { "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Minor Blues", "Whole Tone", "User Scale" });
    // The lower strip mirrors the real utility controls.  It is not a second
    // parameter set: every gesture forwards to the authoritative selector.
    const auto configureContextSelector = [this] (juce::ComboBox& selector, const juce::StringArray& choices)
    {
        selector.addItemList (choices, 1);
        selector.setLookAndFeel (&hardware);
        selector.setColour (juce::ComboBox::backgroundColourId, graphite);
        selector.setColour (juce::ComboBox::outlineColourId, champagne.withAlpha (.5f));
        selector.setColour (juce::ComboBox::textColourId, ivory);
        selector.setColour (juce::ComboBox::arrowColourId, brass);
        addAndMakeVisible (selector);
    };
    configureContextSelector (noteMapVoiceSelector, { "Soprano", "Alto", "Tenor", "Low" });
    configureContextSelector (noteMapKeySelector, { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" });
    configureContextSelector (noteMapScaleSelector, { "Chromatic", "Major", "Natural Minor", "Major Pentatonic", "Minor Pentatonic", "Harmonic Minor", "Melodic Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian", "Minor Blues", "Whole Tone", "User Scale" });
    configureKnob (retuneSpeedSlider, retuneSpeedLabel, "RETUNE", " ms", 100); configureKnob (pitchFlexibleSlider, pitchFlexibleLabel, "FLEX TUNE", " %", 0);
    configureKnob (humanizeSlider, humanizeLabel, "HUMANIZE", " %", 0); configureKnob (naturalVibratoSlider, naturalVibratoLabel, "NATURAL VIBRATO", "", 0);
    configureKnob (transposeSlider, transposeLabel, "TRANSPOSE", " st", 0); configureKnob (referencePitchSlider, referencePitchLabel, "REFERENCE", " Hz", 440);
    configureKnob (trackingSlider, trackingLabel, "TRACKING", " %", 50); configureKnob (voicedColourSlider, voicedColourLabel, "VOICED COLOUR", " %", 0);
    retuneSpeedSlider.setTooltip ("Sets pitch correction response time."); pitchFlexibleSlider.setTooltip ("Reduces correction near the target pitch."); humanizeSlider.setTooltip ("Preserves natural movement on sustained notes."); naturalVibratoSlider.setTooltip ("Adjusts retained natural vibrato."); trackingSlider.setTooltip ("Adjusts pitch tracking sensitivity.");
    bypassButton.setClickingTogglesState (true); bypassButton.setLookAndFeel (&hardware); bypassButton.setColour (juce::TextButton::buttonColourId, raised); bypassButton.setColour (juce::TextButton::buttonOnColourId, brass.darker (.45f)); bypassButton.setColour (juce::TextButton::textColourOffId, champagne); bypassButton.setColour (juce::TextButton::textColourOnId, ivory); addAndMakeVisible (bypassButton);
    bypassButton.setTooltip ("Turns pitch correction on or off."); inputTypeSelector.setTooltip ("Selects the vocal range."); keySelector.setTooltip ("Sets the musical key."); scaleSelector.setTooltip ("Sets the correction scale."); transposeSlider.setTooltip ("Transposes the correction target."); referencePitchSlider.setTooltip ("Sets the tuning reference pitch."); voicedColourSlider.setTooltip ("Adjusts voiced tone colour.");
    settingsButton.setEnabled (false); settingsButton.setLookAndFeel (&hardware); settingsButton.setColour (juce::TextButton::buttonColourId, raised); settingsButton.setColour (juce::TextButton::textColourOffId, champagne.withAlpha (.55f)); settingsButton.setTooltip ("Settings reserved for a future host-safe preferences panel."); addAndMakeVisible (settingsButton);
   #if JUCE_DEBUG
    targetReferenceImage = juce::ImageFileFormat::loadFrom (juce::File (VIBE_UI_REFERENCE_IMAGE_PATH));
    jassert (targetReferenceImage.isValid());
    settingsButton.setEnabled (true);
    settingsButton.setTooltip ("Debug UI comparison: click to toggle target overlay; press R for reference telemetry.");
    settingsButton.onClick = [this] { showTargetOverlay = ! showTargetOverlay; repaint(); };
    setWantsKeyboardFocus (true);
   #endif
    editNotesButton.setClickingTogglesState (true); editNotesButton.setLookAndFeel (&hardware); editNotesButton.setColour (juce::TextButton::buttonColourId, graphite); editNotesButton.setColour (juce::TextButton::buttonOnColourId, raised); editNotesButton.setColour (juce::TextButton::textColourOffId, champagne); editNotesButton.setColour (juce::TextButton::textColourOnId, brass);
    editNotesButton.onClick = [this]
    {
        editNotesEnabled = editNotesButton.getToggleState();
        for (auto& target : noteRuleHitTargets)
            target.setVisible (editNotesEnabled);
        repaint (pianoBounds);
    };
    editNotesButton.setTooltip ("Show or hide per-note correction rules"); addAndMakeVisible (editNotesButton);
    for (int index = 0; index < (int) noteRuleHitTargets.size(); ++index)
    {
        auto& target = noteRuleHitTargets[(size_t) index];
        const auto midiNote = firstKey + index;
        target.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        target.setTitle ("Edit MIDI note " + juce::String (midiNote));
        target.onGesture = [this, midiNote] (const juce::ModifierKeys& modifiers)
        {
            handleNoteRuleGesture (midiNote, modifiers);
        };
        addAndMakeVisible (target);
        target.setVisible (false);
    }
    groupOctavesButton.setLookAndFeel (&hardware); groupOctavesButton.setToggleState (processor.getGroupOctaves(), juce::dontSendNotification); groupOctavesButton.setColour (juce::ToggleButton::textColourId, champagne); groupOctavesButton.setColour (juce::ToggleButton::tickColourId, brass); groupOctavesButton.onClick = [this] { processor.setGroupOctaves (groupOctavesButton.getToggleState()); repaint (pianoBounds); }; addAndMakeVisible (groupOctavesButton);
    bypassAttachment = std::make_unique<ButtonAttachment> (processor.parameters, "bypass", bypassButton);
    inputTypeAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "inputType", inputTypeSelector); keyAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "key", keySelector); scaleAttachment = std::make_unique<ComboBoxAttachment> (processor.parameters, "scale", scaleSelector);
    noteMapVoiceSelector.onChange = [this] { inputTypeSelector.setSelectedItemIndex (noteMapVoiceSelector.getSelectedItemIndex(), juce::sendNotification); };
    noteMapKeySelector.onChange = [this] { keySelector.setSelectedItemIndex (noteMapKeySelector.getSelectedItemIndex(), juce::sendNotification); };
    noteMapScaleSelector.onChange = [this] { scaleSelector.setSelectedItemIndex (noteMapScaleSelector.getSelectedItemIndex(), juce::sendNotification); };
    inputTypeSelector.onChange = [this]
    {
        noteMapVoiceSelector.setSelectedItemIndex (inputTypeSelector.getSelectedItemIndex(), juce::dontSendNotification);
        repaint (pianoBounds);
    };
    keySelector.onChange = [this]
    {
        noteMapKeySelector.setSelectedItemIndex (keySelector.getSelectedItemIndex(), juce::dontSendNotification);
        processor.resetNotePolicyToCurrentScale();
        repaint (pianoBounds);
    };
    scaleSelector.onChange = [this]
    {
        noteMapScaleSelector.setSelectedItemIndex (scaleSelector.getSelectedItemIndex(), juce::dontSendNotification);
        processor.resetNotePolicyToCurrentScale();
        repaint (pianoBounds);
    };
    noteMapVoiceSelector.setSelectedItemIndex (inputTypeSelector.getSelectedItemIndex(), juce::dontSendNotification);
    noteMapKeySelector.setSelectedItemIndex (keySelector.getSelectedItemIndex(), juce::dontSendNotification);
    noteMapScaleSelector.setSelectedItemIndex (scaleSelector.getSelectedItemIndex(), juce::dontSendNotification);
    retuneSpeedAttachment = std::make_unique<SliderAttachment> (processor.parameters, "retuneSpeed", retuneSpeedSlider); flexTuneAttachment = std::make_unique<SliderAttachment> (processor.parameters, "flexTune", pitchFlexibleSlider); humanizeAttachment = std::make_unique<SliderAttachment> (processor.parameters, "humanize", humanizeSlider); naturalVibratoAttachment = std::make_unique<SliderAttachment> (processor.parameters, "naturalVibrato", naturalVibratoSlider);
    transposeAttachment = std::make_unique<SliderAttachment> (processor.parameters, "transpose", transposeSlider); referencePitchAttachment = std::make_unique<SliderAttachment> (processor.parameters, "referencePitch", referencePitchSlider); trackingAttachment = std::make_unique<SliderAttachment> (processor.parameters, "tracking", trackingSlider); voicedColourAttachment = std::make_unique<SliderAttachment> (processor.parameters, "voicedColour", voicedColourSlider);
    // APVTS installs its own display converters when attachments are created.
    // Replace those afterwards so the hardware windows match the faceplate
    // units without altering the actual parameter ranges or DSP values.
    retuneSpeedSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)); };
    for (auto* percentage : { &pitchFlexibleSlider, &humanizeSlider, &trackingSlider, &voicedColourSlider })
        percentage->textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)); };
    naturalVibratoSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * (100.0 / 12.0))) + " %"; };
    referencePitchSlider.textFromValueFunction = [] (double v) { return juce::String (v, 1); };
    transposeSlider.textFromValueFunction = [] (double v) { const auto n = juce::roundToInt (v); return (n > 0 ? "+" : "") + juce::String (n); };
    for (auto* slider : { static_cast<juce::Slider*> (&retuneSpeedSlider), static_cast<juce::Slider*> (&pitchFlexibleSlider), static_cast<juce::Slider*> (&humanizeSlider), static_cast<juce::Slider*> (&naturalVibratoSlider), static_cast<juce::Slider*> (&transposeSlider), static_cast<juce::Slider*> (&referencePitchSlider), static_cast<juce::Slider*> (&trackingSlider), static_cast<juce::Slider*> (&voicedColourSlider) })
        slider->updateText();
    setResizable (true, true); setResizeLimits (840, 585, 1448, 1008);
    if (auto* constrainer = getConstrainer()) constrainer->setFixedAspectRatio (LiveTuneLayoutMetrics::designWidth / LiveTuneLayoutMetrics::designHeight);
    setSize (1024, 713); startTimerHz (30);
}
VibeAutotuneEditor::~VibeAutotuneEditor() { stopTimer(); bypassButton.setLookAndFeel (nullptr); settingsButton.setLookAndFeel (nullptr); editNotesButton.setLookAndFeel (nullptr); groupOctavesButton.setLookAndFeel (nullptr); inputTypeSelector.setLookAndFeel (nullptr); keySelector.setLookAndFeel (nullptr); scaleSelector.setLookAndFeel (nullptr); noteMapVoiceSelector.setLookAndFeel (nullptr); noteMapKeySelector.setLookAndFeel (nullptr); noteMapScaleSelector.setLookAndFeel (nullptr); for (auto* s : { static_cast<juce::Slider*> (&retuneSpeedSlider), static_cast<juce::Slider*> (&pitchFlexibleSlider), static_cast<juce::Slider*> (&humanizeSlider), static_cast<juce::Slider*> (&naturalVibratoSlider), static_cast<juce::Slider*> (&transposeSlider), static_cast<juce::Slider*> (&referencePitchSlider), static_cast<juce::Slider*> (&trackingSlider), static_cast<juce::Slider*> (&voicedColourSlider) }) s->setLookAndFeel (nullptr); }
void VibeAutotuneEditor::configureSelector (juce::ComboBox& s, juce::Label& l, const juce::String& t, const juce::StringArray& c) { s.addItemList (c, 1); s.setLookAndFeel (&hardware); s.setColour (juce::ComboBox::backgroundColourId, graphite); s.setColour (juce::ComboBox::outlineColourId, champagne.withAlpha (.5f)); s.setColour (juce::ComboBox::textColourId, ivory); s.setColour (juce::ComboBox::arrowColourId, brass); addAndMakeVisible (s); l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (10, juce::Font::bold)); l.setColour (juce::Label::textColourId, graphite); addAndMakeVisible (l); }
void VibeAutotuneEditor::configureKnob (juce::Slider& s, juce::Label& l, const juce::String& t, const juce::String& suffix, double d) { s.setName (t); s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); s.setLookAndFeel (&hardware); s.setRotaryParameters (juce::degreesToRadians (225.f), juce::degreesToRadians (495.f), true); s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 82, 20); s.setTextValueSuffix (suffix); s.setDoubleClickReturnValue (true, d); s.setColour (juce::Slider::textBoxTextColourId, ivory); s.setColour (juce::Slider::textBoxBackgroundColourId, glass); s.setColour (juce::Slider::textBoxOutlineColourId, champagne.withAlpha (.3f)); addAndMakeVisible (s); l.setText (t, juce::dontSendNotification); l.setFont (juce::FontOptions (10, juce::Font::bold)); l.setColour (juce::Label::textColourId, graphite); l.setJustificationType (juce::Justification::centred); addAndMakeVisible (l); }

void VibeAutotuneEditor::drawPanel (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& title) const
{
    constexpr auto radius = 9.0f;
    g.setColour (juce::Colours::black.withAlpha (.50f)); g.fillRoundedRectangle (r.translated (0, 3), radius);
    auto edge = juce::ColourGradient (juce::Colour (0xfff1e8d6), r.getX(), r.getY(), juce::Colour (0xff716957), r.getX(), r.getBottom(), false);
    g.setGradientFill (edge); g.fillRoundedRectangle (r, radius);
    auto inner = r.reduced (2.0f);
    auto fill = juce::ColourGradient (juce::Colour (0xffeadccd), inner.getX(), inner.getY(), juce::Colour (0xffbba793), inner.getX(), inner.getBottom(), false);
    fill.addColour (.53, juce::Colour (0xffd8c7b4));
    g.setGradientFill (fill); g.fillRoundedRectangle (inner, radius - 2.0f);
    // Restrained horizontal machining; clipped so the panel never reads as a
    // flat beige card or leaks texture through its rounded corners.
    g.saveState();
    juce::Path clip; clip.addRoundedRectangle (inner, radius - 2.0f); g.reduceClipRegion (clip);
    g.setOpacity (.32f);
    g.drawImage (champagnePanelTexture(), inner,
                 juce::RectanglePlacement::stretchToFit);
    g.setOpacity (1.0f);
    for (float y = inner.getY() + 4; y < inner.getBottom(); y += 4.0f)
    {
        g.setColour ((juce::roundToInt (y) % 17 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (.009f));
        g.drawHorizontalLine (juce::roundToInt (y), inner.getX(), inner.getRight());
    }
    g.restoreState();
    g.setColour (juce::Colours::white.withAlpha (.38f)); g.drawRoundedRectangle (r.reduced (.5f), radius, 1.0f);
    juce::ignoreUnused (title);
}
void VibeAutotuneEditor::paint (juce::Graphics& g)
{
    // Main chassis: dark hardware perimeter, inset champagne work surface and
    // multi-line metal edges. This replaces the former full-window beige fill.
    g.fillAll (juce::Colour (0xff070a0a));
    const LiveTuneLayoutMetrics layout (getLocalBounds());
    const auto D = [&] (float x, float y, float w, float h) { return layout.rect (x, y, w, h).toFloat(); };
    // The last physical surface is the Note Map / keyboard tray.  Do not add
    // a second decorative lower deck: it makes the UI look like it has an
    // unfinished black footer.
    // One continuous outer shell.  Every later layer is inset from this
    // rectangle, so no child surface can visually cut through its edge.
    // Start the shell close to the host content edge.  The previous 44px
    // reserve was rendered as an empty black header gap in every plugin host.
    auto chassis = D (7, 8, 1434, 986);
    auto chassisFill = juce::ColourGradient (juce::Colour (0xff242622), chassis.getX(), chassis.getY(), juce::Colour (0xff080908), chassis.getX(), chassis.getBottom(), false);
    g.setGradientFill (chassisFill); g.fillRoundedRectangle (chassis, 13.0f);
    g.setColour (juce::Colour (0xffe3d7c0).withAlpha (.62f)); g.drawRoundedRectangle (chassis.reduced (.7f), 12.5f, 1.1f);
    g.setColour (juce::Colour (0xff504b40).withAlpha (.74f)); g.drawRoundedRectangle (chassis.reduced (3.2f), 10.0f, .8f);
    // All visible surfaces use this one inset bay. The former rail geometry
    // began outside it, so the metal slabs could visually sit on top of the
    // header and Note Map at the chassis edges.
    auto workSurface = chassis.reduced (10.0f);
    auto bg=juce::ColourGradient(juce::Colour(0xffd5c9b2),workSurface.getX(),workSurface.getY(),juce::Colour(0xffa89c87),workSurface.getX(),workSurface.getBottom(),false);g.setGradientFill(bg);g.fillRoundedRectangle(workSurface,8.0f);
    g.setColour (juce::Colour (0xff4d473d).withAlpha (.66f)); g.drawRoundedRectangle (workSurface.reduced (.5f), 8.0f, .8f);
    g.saveState(); juce::Path workClip; workClip.addRoundedRectangle (workSurface, 8.0f); g.reduceClipRegion (workClip);
    for (int y = juce::roundToInt (workSurface.getY()); y < juce::roundToInt (workSurface.getBottom()); y += 4) { g.setColour ((y % 19 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (0.008f)); g.drawHorizontalLine (y, workSurface.getX(), workSurface.getRight()); }
    g.restoreState();
    // A single recessed content well owns all three control panels and the
    // Note Map. The visible gutter makes their parent/child relationship
    // unambiguous and keeps every inner face away from the metal shell.
    const auto contentWell = D (35, 96, 1378, 880);
    g.setColour (juce::Colours::black.withAlpha (.38f));
    g.fillRoundedRectangle (contentWell.translated (0.0f, 2.0f), 10.0f);
    auto wellFill = juce::ColourGradient (juce::Colour (0xff6d6659), contentWell.getX(), contentWell.getY(),
                                          juce::Colour (0xff2c2d29), contentWell.getX(), contentWell.getBottom(), false);
    g.setGradientFill (wellFill); g.fillRoundedRectangle (contentWell, 9.0f);
    g.setColour (juce::Colour (0xffe1d5bf).withAlpha (.34f));
    g.drawRoundedRectangle (contentWell.reduced (.5f), 9.0f, .8f);
    // Machined fastener: cast shadow, recessed collar, radial metal face and
    // a milled slot with its own highlight.  It remains procedural so it is
    // crisp at every host scale instead of looking like a stretched icon.
    auto drawFastener = [&] (juce::Point<float> centre, float slotAngle)
    {
        constexpr float diameter = 15.0f;
        const auto face = juce::Rectangle<float> (diameter, diameter).withCentre (centre);
        g.setColour (juce::Colours::black.withAlpha (.50f));
        g.fillEllipse (face.translated (0.8f, 1.5f).expanded (1.0f));
        auto collar = juce::ColourGradient (juce::Colour (0xfff0dfbd), face.getX(), face.getY(),
                                            juce::Colour (0xff574b3b), face.getRight(), face.getBottom(), false);
        collar.addColour (.48, juce::Colour (0xffbda77e));
        g.setGradientFill (collar); g.fillEllipse (face);
        g.setColour (juce::Colour (0xff191915)); g.fillEllipse (face.reduced (2.0f));
        const auto disc = face.reduced (2.8f);
        auto steel = juce::ColourGradient (juce::Colour (0xfff4ead6), disc.getX() + disc.getWidth() * .24f, disc.getY() + disc.getHeight() * .20f,
                                           juce::Colour (0xff292823), disc.getRight(), disc.getBottom(), true);
        steel.addColour (.34, juce::Colour (0xffb3a993));
        steel.addColour (.72, juce::Colour (0xff5d5a50));
        g.setGradientFill (steel); g.fillEllipse (disc);
        juce::Path highlightArc;
        highlightArc.addCentredArc (disc.getCentreX(), disc.getCentreY(), disc.getWidth() * .46f,
                                    disc.getHeight() * .46f, 0.0f,
                                    juce::MathConstants<float>::pi * 1.05f,
                                    juce::MathConstants<float>::pi * 1.75f, true);
        g.setColour (juce::Colours::white.withAlpha (.28f));
        g.strokePath (highlightArc, juce::PathStrokeType (1.0f));
        g.setColour (juce::Colours::black.withAlpha (.52f));
        g.drawEllipse (disc.reduced (.25f), .65f);

        juce::Path slot;
        slot.addRoundedRectangle (-3.1f, -.72f, 6.2f, 1.44f, .55f);
        const auto transform = juce::AffineTransform::rotation (slotAngle).translated (centre.x, centre.y);
        g.setColour (juce::Colour (0xff11110f).withAlpha (.88f));
        g.fillPath (slot, transform);
        g.setColour (juce::Colour (0xfff1dfb7).withAlpha (.32f));
        g.strokePath (slot, juce::PathStrokeType (.42f), transform);
    };
    // Narrow side cheeks sit inside the same work-surface clip as every
    // other layer. They frame the contents instead of obscuring their edges.
    // Rails are short inner cheeks, not full-height slabs.  Their ends stay
    // clear of the corner fasteners and the outer chassis border.
    auto leftRail = D (18, 37, 16, 928), rightRail = D (1414, 37, 16, 928);
    g.saveState();
    juce::Path railClip; railClip.addRoundedRectangle (workSurface, 8.0f); g.reduceClipRegion (railClip);
    auto drawRail = [&] (juce::Rectangle<float> rail, bool isLeft)
    {
        const auto inward = isLeft ? 1.0f : -1.0f;
        g.setColour (juce::Colours::black.withAlpha (.42f));
        g.fillRoundedRectangle (rail.translated (inward * 2.0f, 1.2f).expanded (.8f, 0.0f), 2.6f);

        auto metal = juce::ColourGradient (isLeft ? juce::Colour (0xff5c5043) : juce::Colour (0xffe8d8bd), rail.getX(), rail.getY(),
                                           isLeft ? juce::Colour (0xffe8d8bd) : juce::Colour (0xff5c5043), rail.getRight(), rail.getY(), false);
        metal.addColour (.16, isLeft ? juce::Colour (0xff9f8d77) : juce::Colour (0xffd8c6aa));
        metal.addColour (.46, juce::Colour (0xfff0e1ca));
        metal.addColour (.72, juce::Colour (0xffa18c74));
        metal.addColour (.91, juce::Colour (0xff6f5f50));
        g.setGradientFill (metal); g.fillRoundedRectangle (rail, 2.4f);

        g.saveState();
        g.reduceClipRegion (rail.toNearestInt());
        for (float y = rail.getY() + 4.0f; y < rail.getBottom() - 3.0f; y += 5.0f)
        {
            const auto alpha = ((juce::roundToInt (y) / 5) & 1) != 0 ? .030f : .018f;
            g.setColour (juce::Colours::white.withAlpha (alpha));
            g.drawHorizontalLine (juce::roundToInt (y), rail.getX() + 1.0f, rail.getRight() - 1.0f);
        }
        g.restoreState();

        const auto outerX = isLeft ? rail.getX() + .8f : rail.getRight() - .8f;
        const auto innerX = isLeft ? rail.getRight() - 1.3f : rail.getX() + 1.3f;
        g.setColour (juce::Colours::white.withAlpha (.34f));
        g.drawVerticalLine (juce::roundToInt (outerX), rail.getY() + 3.0f, rail.getBottom() - 3.0f);
        g.setColour (juce::Colour (0xff211d18).withAlpha (.72f));
        g.drawVerticalLine (juce::roundToInt (innerX), rail.getY() + 2.0f, rail.getBottom() - 2.0f);
        g.setColour (juce::Colour (0xfff2e3ca).withAlpha (.25f));
        g.drawHorizontalLine (juce::roundToInt (rail.getY() + 1.0f), rail.getX() + 2.0f, rail.getRight() - 2.0f);
        g.setColour (juce::Colours::black.withAlpha (.46f));
        g.drawHorizontalLine (juce::roundToInt (rail.getBottom() - 1.0f), rail.getX() + 2.0f, rail.getRight() - 2.0f);
        g.drawRoundedRectangle (rail.reduced (.35f), 2.1f, .7f);
    };
    drawRail (leftRail, true);
    drawRail (rightRail, false);
    g.restoreState();
    // Hardware fasteners are the topmost chassis detail, never hidden by a rail.
    drawFastener (D (25, 26, 1, 1).getPosition(), -.64f);
    drawFastener (D (1423, 26, 1, 1).getPosition(), .58f);
    drawFastener (D (25, 982, 1, 1).getPosition(), .72f);
    drawFastener (D (1423, 982, 1, 1).getPosition(), -.52f);
    // Header is a separate recessed faceplate, not a black shadow bleeding
    // into the champagne surface below it.
    auto header = headerBounds.toFloat();
    auto headerFill = juce::ColourGradient (juce::Colour (0xff1b1c1a), header.getX(), header.getY(), juce::Colour (0xff080a09), header.getX(), header.getBottom(), false);
    g.setColour (juce::Colours::black.withAlpha (.40f)); g.fillRoundedRectangle (header.translated (0.0f, 2.0f), 5.0f);
    g.setGradientFill (headerFill); g.fillRoundedRectangle (header, 4.0f);
    g.setColour (juce::Colours::black.withAlpha (.46f)); g.drawRoundedRectangle (header.reduced (.2f), 4.0f, 1.0f);
    g.setColour (champagne.withAlpha (.56f)); g.drawRoundedRectangle (header.reduced (.5f), 3.5f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (.10f)); g.drawHorizontalLine (juce::roundToInt (header.getY() + 2), header.getX() + 10, header.getRight() - 10);
    auto logoArea = layout.rect (62, 18, 365, 58);
    auto logoFont = juce::Font (juce::FontOptions (46.0f * layout.scale, juce::Font::bold)).withHorizontalScale (1.42f);
    g.setFont (logoFont);
    g.setColour (juce::Colours::black.withAlpha (.72f)); g.drawText ("LIVE TUNE", logoArea.translated (0, 2), juce::Justification::centredLeft);
    auto logoMetal = juce::ColourGradient (juce::Colour (0xfffffff7), (float) logoArea.getX(), (float) logoArea.getY(), juce::Colour (0xffb7aa95), (float) logoArea.getX(), (float) logoArea.getBottom(), false);
    logoMetal.addColour (.48, juce::Colour (0xffe3d9ca)); g.setGradientFill (logoMetal); g.drawText ("LIVE TUNE", logoArea, juce::Justification::centredLeft);
    // Tight, semi-bold engraving: readable at the compact default size and
    // refined rather than the former oversized, highly-spaced treatment.
    // Align the engraving to the LCD monitor's optical centre rather than
    // leaving it floating between the logo and the monitor.
    const auto subtitleArea = layout.rect (543, 19, 433, 54);
    auto subtitleFont = juce::Font (juce::FontOptions ("Avenir Next", juce::Font::bold, 12.0f * layout.scale))
                            .withHorizontalScale (.98f).withExtraKerningFactor (.14f);
    g.setFont (subtitleFont);
    g.setColour (juce::Colours::black.withAlpha (.58f));
    g.drawText ("REALTIME VOCAL TUNING", subtitleArea.translated (0, 1), juce::Justification::centred);
    g.setColour (champagne.withAlpha (.82f));
    g.drawText ("REALTIME VOCAL TUNING", subtitleArea, juce::Justification::centred);
    drawPanel(g,correctionBounds.toFloat(),"CORRECTION"); drawPanel(g,monitorBounds.toFloat(),"LIVE PITCH"); drawPanel(g,setupBounds.toFloat(),"UTILITY");
    g.setColour (graphite.withAlpha (.58f));
    g.drawHorizontalLine (juce::roundToInt (layout.originY + 220.0f * layout.scale), layout.originX + 1008.0f * layout.scale, layout.originX + 1382.0f * layout.scale);
    g.setColour (juce::Colours::black.withAlpha (.42f)); g.fillRoundedRectangle (pianoBounds.toFloat().translated (0.0f, 2.0f), 10.0f);
    g.setColour(graphite);g.fillRoundedRectangle(pianoBounds.toFloat(),10);g.setColour(champagne.withAlpha(.45f));g.drawRoundedRectangle(pianoBounds.toFloat().reduced(.5f),10,1);
    const auto N = [&] (float x, float y, float w, float h) { return layout.rect (x, y, w, h).toFloat(); };
    // Share the selector row's optical centre. The old y=723 caption floated
    // above the controls and made the Note Map header look disconnected.
    g.setColour(champagne);g.setFont(juce::FontOptions(16.0f*layout.scale,juce::Font::bold));
    g.drawText("NOTE MAP",N(65,728,105,30).toNearestInt(),juce::Justification::centredLeft);
    drawPitchMonitor(g); drawPianoKeyboard(g);
}

void VibeAutotuneEditor::drawPitchMonitor (juce::Graphics& g) const
{
    auto d=monitorBounds.toFloat().reduced(14,14);
    const LiveTuneLayoutMetrics layout (getLocalBounds());
    auto visualHasPitch = monitorHasValidPitch;
    auto visualInputNote = monitor.detectedNote;
    auto visualTargetNote = monitor.targetNote;
    auto visualCorrection = displayedAppliedCorrectionCents;
    auto visualConfidence = displayedPitchConfidence;
    auto visualStatus = monitor.bypassed ? juce::String ("BYPASSED") : (monitorHasValidPitch ? juce::String ("TRACKING") : juce::String ("NO INPUT"));
   #if JUCE_DEBUG
    if (useReferenceVisualState)
    {
        visualHasPitch = true;
        visualInputNote = "F#3";
        visualTargetNote = "G3";
        visualCorrection = 23.4f;
        visualConfidence = .87f;
        visualStatus = "TRACKING";
    }
   #endif
    // A neutral, non-state-bearing chrome asset from the kit owns the glass
    // material. The supplied crop is the upper glass/chrome section rather
    // than a full portrait panel, so preserve its native aspect ratio and
    // extend the lower glass procedurally instead of distorting the bitmap.
    g.setColour (juce::Colours::black.withAlpha (.46f));
    g.fillRoundedRectangle (d.translated (0.0f, 2.2f).expanded (1.1f), 8.2f);
    auto bezel = juce::ColourGradient (juce::Colour (0xff8b877d), d.getX(), d.getY(),
                                       juce::Colour (0xff252a28), d.getX(), d.getBottom(), false);
    bezel.addColour (.17, juce::Colour (0xffd1c6b2));
    bezel.addColour (.43, juce::Colour (0xff474a45));
    bezel.addColour (.78, juce::Colour (0xff171a18));
    g.setGradientFill (bezel); g.fillRoundedRectangle (d, 7.8f);
    const auto gasket = d.reduced (2.0f);
    g.setColour (juce::Colour (0xff080a09)); g.fillRoundedRectangle (gasket, 6.6f);
    const auto glassFace = d.reduced (4.0f);
    auto glassBase = juce::ColourGradient (juce::Colour (0xff0a0e0d), glassFace.getX(), glassFace.getY(),
                                           juce::Colour (0xff020504), glassFace.getX(), glassFace.getBottom(), false);
    glassBase.addColour (.55, juce::Colour (0xff030706));
    g.setGradientFill (glassBase); g.fillRoundedRectangle (glassFace, 5.5f);
    const auto& glass = blackGlassInterior();
    const auto glassHeight = juce::jmin (145.0f * layout.scale, d.getHeight() * .27f);
    g.setOpacity (.24f);
    g.drawImageWithin (glass, juce::roundToInt (glassFace.getX()), juce::roundToInt (glassFace.getY()),
                       juce::roundToInt (glassFace.getWidth()), juce::roundToInt (glassHeight), juce::RectanglePlacement::centred);
    g.setOpacity (1.0f);
    g.setColour (juce::Colours::white.withAlpha (.075f)); g.drawRoundedRectangle (glassFace.reduced (.4f), 5.2f, .8f);
    g.setColour (champagne.withAlpha (.48f)); g.drawRoundedRectangle (d.reduced (.55f), 7.3f, .8f);
    g.setColour (juce::Colours::black.withAlpha (.72f)); g.drawRoundedRectangle (gasket, 6.3f, 1.15f);

    const auto status = visualStatus;
    auto top = d.toNearestInt().reduced (24, 19).removeFromTop (58);
    auto inputColumn = top.removeFromLeft (110);
    auto targetColumn = top.removeFromRight (110);
    const auto meterFont = [&] (float height, const juce::String& style, float tracking = 0.0f)
    {
        return juce::Font (juce::FontOptions ("Avenir Next", style, height * layout.scale))
                   .withHorizontalScale (.95f).withExtraKerningFactor (tracking);
    };
    g.setFont (meterFont (15.0f, "Demi Bold", .035f));
    g.setColour (smokeInk.withAlpha (.88f)); g.drawText ("INPUT", inputColumn.removeFromTop (17), juce::Justification::centredLeft);
    g.setColour (champagne.withAlpha (.92f)); g.drawText ("TARGET", targetColumn.removeFromTop (17), juce::Justification::centredRight);
    g.setFont (meterFont (27.0f, "Demi Bold", .018f));
    g.setColour (smokeInk); g.drawText (visualHasPitch ? visualInputNote : "--", inputColumn, juce::Justification::centredLeft);
    g.setColour (champagne); g.drawText (visualHasPitch ? visualTargetNote : "--", targetColumn, juce::Justification::centredRight);
    auto c = juce::Point<float> (d.getCentreX(), d.getY() + d.getHeight() * .438f);
    auto radius = juce::jmin (d.getWidth() * .45f, d.getHeight() * .34f);
    auto circle = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
    g.setColour (juce::Colours::black.withAlpha (.62f)); g.drawEllipse (circle.expanded (1.4f), 2.2f);
    g.setColour (champagne.withAlpha (.18f)); g.drawEllipse (circle, .85f);
    for (int i = 0; i < 49; ++i)
    {
        const auto a = juce::MathConstants<float>::pi * 1.12f + (float) i * (juce::MathConstants<float>::pi * 1.76f / 48.0f);
        const auto v = juce::Point<float> (std::cos (a), std::sin (a));
        const auto major = i == 0 || i == 24 || i == 48 || i % 6 == 0;
        g.setColour (i == 24 ? brass.withAlpha (.94f) : smokeInk.withAlpha (major ? .72f : .48f));
        g.drawLine ({ circle.getCentre() + v * radius * (major ? .835f : .875f),
                      circle.getCentre() + v * radius * .975f }, i == 24 ? 1.9f : major ? 1.25f : .78f);
    }
    // The atomic image defines the desired silhouette, but these paths are
    // deliberately live: they are deformed by the fixed-size snapshot history
    // copied on the message thread at 30 Hz. No drawing data is read from the
    // audio callback and no value ever feeds back into DSP.
    juce::Path circleClip; circleClip.addEllipse (circle);
    const auto count = juce::jlimit (16, 96, monitorScopeFrameCount);
    const auto first = juce::jmax (0, monitorScopeFrameCount - count);
    const auto ribbonPoint = [&] (float p, int strand)
    {
        const auto frameIndex = juce::jlimit (0, count - 1, juce::roundToInt (p * (float) (count - 1)));
        const auto& frame = monitorScopeFrames[(size_t) (first + frameIndex)];
        const auto input = std::isfinite (frame.inputPitchCents) ? frame.inputPitchCents : 0.0f;
        const auto delivered = std::isfinite (frame.deliveredCorrectionCents) ? frame.deliveredCorrectionCents : 0.0f;
        const auto livePitch = juce::jlimit (-1.0f, 1.0f, (input + delivered * p) / 150.0f);
        const auto envelope = juce::jlimit (0.0f, 1.0f, (frame.maximum - frame.minimum) * 2.8f);
        const auto arch = std::sin (p * juce::MathConstants<float>::pi);
        // Two broad acoustic lobes with a shallow centre crossover, matching
        // the supplied monitor rather than a generic single sine/arch.
        const auto silhouette = .10f - .18f * arch
                              - .14f * std::sin (p * juce::MathConstants<float>::pi * 3.0f)
                              + .025f * std::sin ((p - .08f) * juce::MathConstants<float>::twoPi);
        const auto spread = (strand - 5.0f) * (.015f + arch * .028f);
        // The family still follows the same two-lobe silhouette, but each
        // strand now drifts at a related rate. This makes a sustained vocal
        // feel alive without turning the monitor into a generic sine wave.
        const auto phase = ribbonMotionPhase * (1.0f + (strand - 5.0f) * .026f);
        const auto filament = std::sin (p * juce::MathConstants<float>::twoPi * (1.18f + strand * .014f) + strand * .36f + phase)
                            * (.009f + envelope * .012f);
        const auto travellingDetail = std::sin (p * juce::MathConstants<float>::twoPi * (2.2f + strand * .07f) - phase * 1.37f)
                                   * displayedRibbonMotion * (.010f + arch * .018f);
        return juce::Point<float> (circle.getX() + p * circle.getWidth(),
                                   circle.getCentreY() + radius * (silhouette + spread + filament + travellingDetail + livePitch * .055f));
    };
    g.saveState(); g.reduceClipRegion (circleClip);
    if (visualHasPitch)
    {
        constexpr int ribbonPoints = 80;
        constexpr int splitPoint = 40;
        // A softly filled core gives the filament family the volume visible
        // in the reference while every contour remains driven by live pitch
        // history. It is not a static waveform bitmap.
        for (int half = 0; half < 2; ++half)
        {
            const auto begin = half == 0 ? 0 : splitPoint;
            const auto finish = half == 0 ? splitPoint : ribbonPoints - 1;
            juce::Path band;
            for (int i = begin; i <= finish; ++i)
            {
                const auto point = ribbonPoint ((float) i / (ribbonPoints - 1), 1);
                if (i == begin) band.startNewSubPath (point); else band.lineTo (point);
            }
            for (int i = finish; i >= begin; --i)
                band.lineTo (ribbonPoint ((float) i / (ribbonPoints - 1), 9));
            band.closeSubPath();
            g.setColour ((half == 0 ? brass : ice).withAlpha (.16f));
            g.fillPath (band);
        }
        for (int strand = 0; strand < 11; ++strand)
        {
            juce::Path warmPath, coolPath;
            for (int i = 0; i < ribbonPoints; ++i)
            {
                const auto point = ribbonPoint ((float) i / (ribbonPoints - 1), strand);
                if (i == 0) warmPath.startNewSubPath (point); else if (i <= splitPoint) warmPath.lineTo (point);
                if (i == splitPoint) coolPath.startNewSubPath (point); else if (i > splitPoint) coolPath.lineTo (point);
            }
            const auto primary = strand == 5;
            const auto width = primary ? 1.45f : .68f;
            const auto alpha = primary ? .96f : .24f + strand * .025f;
            if (primary) { g.setColour (brass.withAlpha (.18f)); g.strokePath (warmPath, juce::PathStrokeType (5.8f)); g.setColour (ice.withAlpha (.15f)); g.strokePath (coolPath, juce::PathStrokeType (5.8f)); }
            g.setColour (brass.withAlpha (alpha)); g.strokePath (warmPath, juce::PathStrokeType (width));
            g.setColour (ice.withAlpha (alpha)); g.strokePath (coolPath, juce::PathStrokeType (width));
        }
    }
    // The pale centre trace is always present. It makes NO INPUT an honest
    // idle state while the coloured multi-strand families remain realtime.
    juce::Path neutralTrace;
    for (int i = 0; i < 72; ++i)
    {
        const auto p = (float) i / 71.0f;
        auto point = ribbonPoint (p, 5); point.y += radius * .050f;
        if (i == 0) neutralTrace.startNewSubPath (point); else neutralTrace.lineTo (point);
    }
    g.setColour (juce::Colour (0xffe6ecea).withAlpha (visualHasPitch ? .78f : .22f));
    g.strokePath (neutralTrace, juce::PathStrokeType (1.8f));
    g.restoreState();
    // Reference gauge signature: vertical pitch axis, hot lamp, lower arc,
    // cents legends and amber correction fan.
    g.setColour (ice.withAlpha (.72f)); g.drawLine (c.x, circle.getY() - 4.0f, c.x, c.y - radius * .28f, 1.1f);
    g.setColour (brass.withAlpha (.22f)); g.fillEllipse (c.x - 9, c.y - radius * .53f - 9, 18, 18);
    g.setColour (juce::Colour (0xffffd37b)); g.fillEllipse (c.x - 3.3f, c.y - radius * .53f - 3.3f, 6.6f, 6.6f);
    juce::Path lowerArc; lowerArc.addCentredArc (c.x, c.y + radius * .06f, radius * .70f, radius * .70f, 0, juce::degreesToRadians (132.0f), juce::degreesToRadians (228.0f), true);
    g.setColour (ice.withAlpha (.62f)); g.strokePath (lowerArc, juce::PathStrokeType (1.2f));
    for (int i = 0; i < 17; ++i) { const auto a=juce::degreesToRadians(132.0f+i*6.0f); const auto v=juce::Point<float>(std::cos(a),std::sin(a)); g.setColour(brass.withAlpha(.84f)); g.drawLine({c+v*radius*.72f,c+v*radius*.79f},1.0f); }
    g.setColour (brass.withAlpha (.74f)); g.drawLine (c.x, c.y + radius * .70f, c.x, c.y + radius * .94f, 2.0f);
    // Keep cents values outside the radial tick field: they must be legible
    // labels, never read as extra ticks or collide with the lower arc.
    const auto drawCentsLegend = [&] (const juce::String& text, float x)
    {
        auto legend = juce::Rectangle<float> (40.0f * layout.scale, 18.0f * layout.scale)
                          .withCentre ({ x, c.y + radius * .60f });
        g.setColour (juce::Colours::black.withAlpha (.55f));
        g.fillRoundedRectangle (legend.translated (0.0f, 1.0f), 3.0f * layout.scale);
        g.setColour (juce::Colour (0xff070a09).withAlpha (.88f));
        g.fillRoundedRectangle (legend, 3.0f * layout.scale);
        g.setColour (smokeInk.withAlpha (.90f));
        g.setFont (meterFont (11.0f, "Medium", .015f));
        g.drawText (text, legend.toNearestInt(), juce::Justification::centred);
    };
    drawCentsLegend ("-50", c.x - radius * .72f);
    drawCentsLegend ("+50", c.x + radius * .72f);
    // The target note belongs directly to the glass display.  Avoid a second
    // rounded plate here: it reads as a floating sticker over the waveform.
    auto tgt = visualHasPitch ? visualTargetNote.retainCharacters ("ABCDEFG#b0123456789") : juce::String ("--");
    // Keep the note label below the moving trace so it never collides with
    // the waveform while remaining centred in the monitor glass.
    auto targetReadout = juce::Rectangle<float> (radius * 1.18f, radius * .30f).withCentre ({ c.x, c.y + radius * .34f });
    g.setColour (ivory); g.setFont (meterFont (radius * .30f / layout.scale, "Demi Bold", .012f));
    g.drawText (tgt.isEmpty() ? "--" : tgt, targetReadout.translated (0.0f, 1.6f).toNearestInt(), juce::Justification::centred);
    g.setColour (ivory);
    g.drawText (tgt.isEmpty() ? "--" : tgt, targetReadout.toNearestInt(), juce::Justification::centred);
    const auto correction = visualHasPitch ? ((visualCorrection > .05f ? "+" : "") + juce::String (visualCorrection, 1) + " ct") : "--";
    auto bottom = d.toNearestInt().withTrimmedTop ((int) d.getHeight() - 82).reduced (16, 0);
    const auto sep1 = bottom.getX() + juce::roundToInt (bottom.getWidth() * .376f);
    const auto sep2 = bottom.getX() + juce::roundToInt (bottom.getWidth() * .722f);
    std::array<juce::Rectangle<int>, 3> cols {{
        { bottom.getX(), bottom.getY(), sep1 - bottom.getX(), bottom.getHeight() },
        { sep1, bottom.getY(), sep2 - sep1, bottom.getHeight() },
        { sep2, bottom.getY(), bottom.getRight() - sep2, bottom.getHeight() }
    }};
    g.setColour (juce::Colours::black.withAlpha (.34f));
    g.drawHorizontalLine (bottom.getY(), (float) bottom.getX() + 4.0f, (float) bottom.getRight() - 4.0f);
    g.setColour (smokeInk.withAlpha (.46f));
    g.drawVerticalLine (sep1, (float) bottom.getY() + 5.0f, (float) bottom.getBottom() - 5.0f);
    g.drawVerticalLine (sep2, (float) bottom.getY() + 5.0f, (float) bottom.getBottom() - 5.0f);
    g.setColour (champagne.withAlpha (.90f));
    g.setFont (meterFont (11.5f, "Demi Bold", .025f));
    g.drawText ("CORRECTION", cols[0].withHeight (20), juce::Justification::centred);
    g.drawText ("CONFIDENCE", cols[1].withHeight (20), juce::Justification::centred);
    g.drawText ("STATUS", cols[2].withHeight (20), juce::Justification::centred);
    g.setFont (meterFont (19.0f, "Demi Bold", .008f));
    g.setColour (ivory.withAlpha (.94f));
    g.drawText (correction, cols[0].withTrimmedTop (27), juce::Justification::centred);
    g.setColour (smokeInk.withAlpha (.92f));
    g.drawText (visualHasPitch ? juce::String (visualConfidence, 2) : "--", cols[1].withTrimmedTop (27), juce::Justification::centred);
    g.setColour (champagne.withAlpha (.96f));
    g.drawText (status, cols[2].withTrimmedTop (27).reduced (3, 0), juce::Justification::centred, true);
}

void VibeAutotuneEditor::drawRuleMarker (juce::Graphics& g, juce::Rectangle<float> b,
                                         NoteRule rule, float opacity) const
{
    // A restrained graphite/smoke palette belongs to the champagne hardware
    // surface.  State remains explicit through the vector mark and a subtle
    // tonal shift, without the old purple/neon ruler dominating the keyboard.
    g.saveState();
    g.setOpacity (juce::jlimit (0.0f, 1.0f, opacity));
    b = b.reduced (0.75f);
    const auto fill = rule == NoteRule::Legal          ? juce::Colour (0xff292b2a)
                    : rule == NoteRule::IllegalNearest ? juce::Colour (0xff65434d)
                    : rule == NoteRule::IllegalUp      ? juce::Colour (0xff515b5e)
                    : rule == NoteRule::IllegalDown    ? juce::Colour (0xff5c545c)
                                                        : juce::Colour (0xff464947);
    g.setColour (juce::Colours::black.withAlpha (.62f));
    g.fillRoundedRectangle (b.translated (0.0f, 1.0f), 2.2f);
    g.setColour (fill); g.fillRoundedRectangle (b, 2.2f);
    g.setColour (champagne.withAlpha (.28f));
    g.drawRoundedRectangle (b.reduced (.35f), 1.9f, .65f);

    const auto midX = b.getCentreX(), midY = b.getCentreY();
    const auto white = ivory.withAlpha (.92f);
    if (rule == NoteRule::Legal || rule == NoteRule::IllegalNearest)
    {
        // Every ordinary slot has the same centred machined dash.  Colour and
        // intensity carry Legal/Excluded semantics without leaving apparently
        // random blank blocks in the ruler.
        g.setColour (rule == NoteRule::Legal ? white.withAlpha (.38f) : white);
        g.drawLine (b.getX() + b.getWidth() * .25f, midY, b.getRight() - b.getWidth() * .25f, midY, 1.5f);
    }
    else if (rule == NoteRule::IllegalUp || rule == NoteRule::IllegalDown)
    {
        juce::Path triangle; const auto top = rule == NoteRule::IllegalUp;
        triangle.addTriangle (midX, top ? b.getY() + 3.0f : b.getBottom() - 3.0f,
                              b.getX() + 3.0f, top ? b.getBottom() - 3.0f : b.getY() + 3.0f,
                              b.getRight() - 3.0f, top ? b.getBottom() - 3.0f : b.getY() + 3.0f);
        g.setColour (white); g.fillPath (triangle);
    }
    else if (rule == NoteRule::BypassLegal)
    {
        g.setColour (white.withAlpha (.82f));
        g.drawLine (b.getX()+3.0f,b.getY()+3.0f,b.getRight()-3.0f,b.getBottom()-3.0f,1.2f);
        g.drawLine (b.getRight()-3.0f,b.getY()+3.0f,b.getX()+3.0f,b.getBottom()-3.0f,1.2f);
    }
    g.restoreState();
}
void VibeAutotuneEditor::drawPianoKeyboard (juce::Graphics& g) const
{
    // The keyboard reads like hardware: visible range is selected by VOICE,
    // and the live detected note visibly depresses instead of drawing a dot.
    const auto isActive = [&] (int note) { return note == activePianoNote; };
    const auto isTargetOnly = [&] (int note) { return note == targetPianoNote && note != activePianoNote; };
    for (int note = firstKey; note <= lastKey; ++note)
    {
        if (blackKey (note)) continue;
        const auto keyFrame = noteMapSlots[(size_t) (note-firstKey)].keyBounds.reduced (.55f);
        auto b = keyFrame;
        const auto inRange = isNoteInSelectedVoiceRange (note);
        const auto pressed = isActive (note) || (note == uiPressedPianoNote && uiPressFramesRemaining > 0);
        // A real key press exposes a small top gap and a deeper lower shadow;
        // six design pixels remains readable at host-scaled editor sizes.
        // Keep the pressed face inside the physical key frame.  Translating
        // the whole rectangle made the glow spill below the keyboard rail.
        if (pressed) b = keyFrame.withTrimmedTop (2.0f).withTrimmedBottom (2.0f).translated (0.0f, 1.0f);
        // Soft ivory keytops keep the piano readable without the harsh white
        // block competing with the champagne faceplate.
        auto key = juce::ColourGradient (juce::Colour (0xfffaf6ed), b.getX(), b.getY(), juce::Colour (0xffd8cfbf), b.getX(), b.getBottom(), false);
        if (! inRange) key = juce::ColourGradient (juce::Colour (0xff706b60), b.getX(), b.getY(), juce::Colour (0xff393a35), b.getX(), b.getBottom(), false);
        if (pressed)
        {
            key = juce::ColourGradient (juce::Colour (0xffb18a50), b.getX(), b.getY(),
                                        juce::Colour (0xff1f130a), b.getX(), b.getBottom(), false);
            key.addColour (.24, juce::Colour (0xff8d693a));
            key.addColour (.58, juce::Colour (0xff543619));
        }
        g.setGradientFill (key); g.fillRoundedRectangle (b, 1.6f);
        g.setColour (juce::Colours::white.withAlpha (pressed ? .18f : inRange ? .46f : .12f)); g.drawLine (b.getX()+1,b.getY()+1,b.getRight()-1,b.getY()+1,.8f);
        g.setColour (pressed ? juce::Colour (0xffc59a58) : juce::Colour (0xff373832)); g.drawRoundedRectangle (b, 1.6f, pressed ? 1.0f : .85f);
        if (pressed)
        {
            g.setColour (juce::Colour (0xff2a1d12).withAlpha (.82f));
            g.drawLine (b.getX() + 2.0f, b.getBottom() - 2.0f, b.getRight() - 2.0f, b.getBottom() - 2.0f, 1.5f);
        }
        if (isTargetOnly (note)) { g.setColour (brass.withAlpha (.82f)); g.drawRoundedRectangle (b.reduced (2.5f), 1.0f, 1.4f); }
        if (note % 12 == 0)
        {
            g.setColour (inRange ? champagne : champagne.withAlpha (.28f)); g.setFont (juce::FontOptions (9, juce::Font::bold));
            auto band = juce::Rectangle<float> (b.getX()+3.0f, b.getBottom()-20.0f, b.getWidth()-4.0f, 18.0f);
            g.drawText ("C"+juce::String(note/12-1), band.toNearestInt(), juce::Justification::bottomLeft);
        }
    }
    for (int note = firstKey; note <= lastKey; ++note)
    {
        if (! blackKey (note)) continue;
        const auto keyFrame = noteMapSlots[(size_t) (note-firstKey)].keyBounds;
        auto b = keyFrame;
        const auto inRange = isNoteInSelectedVoiceRange (note);
        const auto pressed = isActive (note) || (note == uiPressedPianoNote && uiPressFramesRemaining > 0);
        // The black-key face is inset and pressed within its own frame; never
        // let the active treatment float outside or cover neighbouring keys.
        if (pressed) b = keyFrame.withTrimmedTop (2.0f).withTrimmedBottom (2.0f).translated (0.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (inRange ? .62f : .28f)); g.fillRoundedRectangle (b.translated (0, 3), 2.0f);
        auto key = juce::ColourGradient (juce::Colour (0xff292a28), b.getX(), b.getY(), juce::Colour (0xff050606), b.getX(), b.getBottom(), false);
        if (! inRange) key = juce::ColourGradient (juce::Colour (0xff242522), b.getX(), b.getY(), juce::Colour (0xff121311), b.getX(), b.getBottom(), false);
        if (pressed)
        {
            key = juce::ColourGradient (juce::Colour (0xff9f783f), b.getX(), b.getY(),
                                        juce::Colour (0xff160d07), b.getX(), b.getBottom(), false);
            key.addColour (.24, juce::Colour (0xff70491f));
            key.addColour (.60, juce::Colour (0xff38200f));
        }
        g.setGradientFill (key); g.fillRoundedRectangle (b, 2.0f);
        g.setColour (juce::Colours::white.withAlpha (pressed ? .14f : inRange ? .14f : .04f)); g.drawLine (b.getX()+2,b.getY()+1,b.getRight()-2,b.getY()+1,.8f);
        g.setColour (pressed ? juce::Colour (0xffbd9150) : champagne.withAlpha (inRange ? .22f : .07f)); g.drawRoundedRectangle (b, 2.0f, pressed ? 1.0f : .7f);
        if (pressed)
        {
            g.setColour (juce::Colour (0xff24180e).withAlpha (.88f));
            g.drawLine (b.getX() + 2.0f, b.getBottom() - 2.0f, b.getRight() - 2.0f, b.getBottom() - 2.0f, 1.4f);
        }
        if (isTargetOnly (note)) { g.setColour (brass.withAlpha (.82f)); g.drawRoundedRectangle (b.reduced (1.6f), 1.0f, 1.2f); }
    }
    // The ruler is an editing surface, not permanent decoration.  Keeping it
    // hidden until the pencil is engaged makes the interaction contract clear.
    if (editNotesEnabled)
        for (int note = firstKey; note <= lastKey; ++note)
        {
            const auto& slot = noteMapSlots[(size_t) (note - firstKey)];
            drawRuleMarker (g, slot.indicatorBounds, processor.getNoteRuleForMidiNote (note),
                            isNoteInSelectedVoiceRange (note) ? 1.0f : .26f);
        }
}

void VibeAutotuneEditor::resized()
{
    // One locked master artboard. At non-matching host aspect ratios the
    // surrounding chassis absorbs the excess instead of distorting the UI.
    const LiveTuneLayoutMetrics layout (getLocalBounds());
    const auto D = [&] (float x, float y, float w, float h) { return layout.rect (x, y, w, h); };

    const auto controlHeading = juce::FontOptions (18.0f * layout.scale, juce::Font::bold);
    for (auto* label : { &retuneSpeedLabel, &pitchFlexibleLabel, &humanizeLabel, &naturalVibratoLabel,
                         &transposeLabel, &referencePitchLabel, &trackingLabel, &voicedColourLabel,
                         &inputTypeLabel, &keyLabel, &scaleLabel })
        label->setFont (controlHeading);
    retuneSpeedLabel.setFont (juce::FontOptions (25.0f * layout.scale, juce::Font::bold));
    for (auto* label : { &inputTypeLabel, &keyLabel, &scaleLabel })
        label->setFont (juce::FontOptions (12.0f * layout.scale, juce::Font::bold));

    // The hardware rails occupy x=7..38 and x=1410..1441. Everything the
    // user reads as the control surface must sit inside this same inner bay.
    // Previously the panel grid began at x=20 and visibly overran the rails.
    // Header sits fully inside the champagne bay instead of sharing the
    // chassis' top edge.  This removes the black seam and corner collisions.
    const auto safeFaceplate = D (20, 56, 1408, 926);
    headerBounds = D (43, 20, 1362, 66);
    correctionBounds = D (43, 140, 492, 560);
    monitorBounds = D (543, 140, 433, 560);
    setupBounds = D (984, 140, 421, 560);
    pianoBounds = D (43, 716, 1362, 252);

    // Small, deliberate hardware controls; the previous oversized power
    // glyph competed with the header instead of reading as a switch.
    // Keep the power switch optically centred in the header bay.  The larger
    // square hit area gives the concentric rings enough breathing room while
    // preserving the compact hardware silhouette.
    bypassButton.setBounds (D (1268, 64, 54, 54));
    settingsButton.setBounds (D (1351, 72, 34, 34));

    // Correction column: dominant, but no longer so oversized that it
    // overwhelms the monitor and makes the three-panel composition lopsided.
    retuneSpeedLabel.setBounds (D (83, 184, 258, 22));
    knobBounds[0] = D (82, 239, 258, 322); retuneSpeedSlider.setBounds (knobBounds[0]);

    const auto character = [&] (int index, float y, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (D (370, y, 130, 18));
        knobBounds[(size_t) index] = D (370, y + 27, 130, 128); slider.setBounds (knobBounds[(size_t) index]);
    };
    character (1, 151, pitchFlexibleLabel, pitchFlexibleSlider);
    character (2, 338, humanizeLabel, humanizeSlider);
    character (3, 524, naturalVibratoLabel, naturalVibratoSlider);

    // One contiguous musical-context strip; these are the real APVTS controls.
    // Primary musical setup stays above the utility knobs, as on hardware
    // units. The mirrored controls below are only a local Note Map context.
    inputTypeLabel.setBounds (D (1015, 151, 145, 16));
    inputTypeSelector.setBounds (D (1015, 171, 145, 40));
    keyLabel.setBounds (D (1172, 151, 78, 16));
    keySelector.setBounds (D (1172, 171, 78, 40));
    scaleLabel.setBounds (D (1262, 151, 122, 16));
    scaleSelector.setBounds (D (1262, 171, 122, 40));

    const auto utility = [&] (int index, float x, float y, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (D (x, y, 150, 20));
        knobBounds[(size_t) index] = D (x, y + 26, 150, 154); slider.setBounds (knobBounds[(size_t) index]);
    };
    // Utility controls start high enough to balance the monitor instead of
    // leaving an accidental, empty upper third of the right-hand panel.
    utility (4, 1032, 251, transposeLabel, transposeSlider);
    utility (5, 1228, 251, referencePitchLabel, referencePitchSlider);
    utility (6, 1032, 456, trackingLabel, trackingSlider);
    utility (7, 1228, 456, voicedColourLabel, voicedColourSlider);

    // The context controls occupy their own row.  The old y=739..773 row
    // overlapped the note ruler at y=762 and visually merged both systems.
    noteMapVoiceSelector.setBounds (D (178, 728, 116, 30));
    noteMapKeySelector.setBounds (D (306, 728, 74, 30));
    noteMapScaleSelector.setBounds (D (392, 728, 154, 30));
    editNotesButton.setBounds (D (558, 728, 30, 30));
    groupOctavesButton.setBounds (D (1120, 724, 247, 38));
    // A dedicated gap separates context, per-note states and physical keys.
    const auto keyboard = D (57, 790, 1334, 154).toFloat();
    const auto statusStrip = D (57, 770, 1334, 16).toFloat();
    const auto whiteWidth = keyboard.getWidth() / 35.0f;
    // Quantise both visual layers through one coordinate pipeline.  A key and
    // its rule marker now own the same physical half-pixel centre, preventing
    // the alternating one-pixel drift that appeared at scaled host sizes.
    const auto snapHalfPixel = [] (float value)
    {
        return std::round (value * 2.0f) * 0.5f;
    };
    int whiteIndex = 0;
    for (int note = firstKey; note <= lastKey; ++note)
        if (! blackKey (note))
        {
            const auto x0 = snapHalfPixel (keyboard.getX() + (float) whiteIndex * whiteWidth);
            const auto x1 = snapHalfPixel (keyboard.getX() + (float) (++whiteIndex) * whiteWidth);
            auto& slot = noteMapSlots[(size_t) (note - firstKey)];
            slot.midiNote = note;
            slot.isBlack = false;
            slot.keyBounds = { x0, keyboard.getY(), x1 - x0, keyboard.getHeight() };
        }
    const auto blackWidth = snapHalfPixel (juce::jmax (7.0f * layout.scale, whiteWidth * .61f));
    for (int note = firstKey; note <= lastKey; ++note)
        if (blackKey (note))
        {
            const auto& left = noteMapSlots[(size_t) (note - 1 - firstKey)].keyBounds;
            auto& slot = noteMapSlots[(size_t) (note - firstKey)];
            slot.midiNote = note;
            slot.isBlack = true;
            const auto centreX = snapHalfPixel (left.getRight());
            slot.keyBounds = { centreX - blackWidth * .5f, left.getY(),
                               blackWidth, keyboard.getHeight() * .66f };
        }
    for (int note = firstKey; note <= lastKey; ++note)
    {
        auto& slot = noteMapSlots[(size_t) (note - firstKey)];
        const auto nominalWidth = (slot.isBlack ? 15.0f : 18.0f) * layout.scale;
        const auto markerWidth = snapHalfPixel (juce::jlimit (8.0f * layout.scale, nominalWidth,
                                                              slot.keyBounds.getWidth() - 2.0f * layout.scale));
        const auto centreX = snapHalfPixel (slot.keyBounds.getCentreX());
        slot.keyBounds.setCentre (centreX, slot.keyBounds.getCentreY());
        slot.indicatorBounds = juce::Rectangle<float> (markerWidth, statusStrip.getHeight())
                                   .withCentre ({ centreX, statusStrip.getCentreY() });
        noteRuleHitTargets[(size_t) (note - firstKey)].setBounds (
            slot.indicatorBounds.expanded (2.0f * layout.scale, 1.0f * layout.scale).getSmallestIntegerContainer());
    }

   #if JUCE_DEBUG
    // Layout contract: every visual surface is geometrically contained by
    // its parent at every host scale because all bounds share the artboard.
    jassert (safeFaceplate.contains (headerBounds));
    jassert (safeFaceplate.contains (correctionBounds));
    jassert (safeFaceplate.contains (monitorBounds));
    jassert (safeFaceplate.contains (setupBounds));
    jassert (safeFaceplate.contains (pianoBounds));
    jassert (pianoBounds.toFloat().contains (keyboard));
    jassert (pianoBounds.toFloat().contains (statusStrip));
    for (const auto& slot : noteMapSlots)
    {
        jassert (slot.midiNote >= firstKey && slot.midiNote <= lastKey);
        jassert (std::abs (slot.keyBounds.getCentreX() - slot.indicatorBounds.getCentreX()) < .01f);
    }
   #endif
}
void VibeAutotuneEditor::paintOverChildren (juce::Graphics& g)
{
   #if JUCE_DEBUG
    if (showTargetOverlay && targetReferenceImage.isValid())
    {
        const LiveTuneLayoutMetrics layout (getLocalBounds());
        g.setOpacity (.50f);
        g.drawImage (targetReferenceImage,
                     layout.originX, layout.originY,
                     LiveTuneLayoutMetrics::designWidth * layout.scale,
                     LiveTuneLayoutMetrics::designHeight * layout.scale,
                     0.0f, 0.0f,
                     (float) targetReferenceImage.getWidth(),
                     (float) targetReferenceImage.getHeight());
        g.setOpacity (1.0f);
    }
   #else
    juce::ignoreUnused (g);
   #endif
}

bool VibeAutotuneEditor::keyPressed (const juce::KeyPress& key)
{
   #if JUCE_DEBUG
    if (key.getTextCharacter() == 'o' || key.getTextCharacter() == 'O')
    {
        showTargetOverlay = ! showTargetOverlay;
        repaint();
        return true;
    }
    if (key.getTextCharacter() == 'r' || key.getTextCharacter() == 'R')
    {
        useReferenceVisualState = ! useReferenceVisualState;
        if (useReferenceVisualState)
        {
            retuneSpeedSlider.setValue (100.0, juce::dontSendNotification);
            pitchFlexibleSlider.setValue (50.0, juce::dontSendNotification);
            humanizeSlider.setValue (30.0, juce::dontSendNotification);
            naturalVibratoSlider.setValue (2.4, juce::dontSendNotification);
            trackingSlider.setValue (50.0, juce::dontSendNotification);
            voicedColourSlider.setValue (25.0, juce::dontSendNotification);
            inputTypeSelector.setSelectedItemIndex (2, juce::dontSendNotification);
            keySelector.setSelectedItemIndex (7, juce::dontSendNotification);
            scaleSelector.setSelectedItemIndex (1, juce::dontSendNotification);
            for (auto* slider : { static_cast<juce::Slider*> (&retuneSpeedSlider), static_cast<juce::Slider*> (&pitchFlexibleSlider), static_cast<juce::Slider*> (&humanizeSlider), static_cast<juce::Slider*> (&naturalVibratoSlider), static_cast<juce::Slider*> (&trackingSlider), static_cast<juce::Slider*> (&voicedColourSlider) })
                slider->updateText();
        }
        repaint (monitorBounds);
        repaint (correctionBounds);
        repaint (setupBounds);
        return true;
    }
   #endif
    return AudioProcessorEditor::keyPressed (key);
}

int VibeAutotuneEditor::pianoNoteAt (juce::Point<int> position) const noexcept
{
    // Rule markers live above both black and white keys.  Checking this
    // first makes every visible symbol directly editable, including sharps.
    for (int note = firstKey; note <= lastKey; ++note)
        if (noteMapSlots[(size_t) (note - firstKey)].indicatorBounds.contains (position.toFloat()))
            return note;
    for (int note = firstKey; note <= lastKey; ++note)
        if (blackKey (note) && noteMapSlots[(size_t) (note - firstKey)].keyBounds.contains (position.toFloat()))
            return note;
    for (int note = firstKey; note <= lastKey; ++note)
        if (! blackKey (note)
            && noteMapSlots[(size_t) (note - firstKey)].keyBounds.contains (position.toFloat()))
            return note;
    return -1;
}

bool VibeAutotuneEditor::isNoteInSelectedVoiceRange (int midiNote) const noexcept
{
    // Match the target-range table used by CorrectionTrajectory exactly.
    const auto* parameter = processor.parameters.getRawParameterValue ("inputType");
    const auto voice = parameter != nullptr ? juce::jlimit (0, 3, juce::roundToInt (parameter->load (std::memory_order_relaxed))) : 1;
    const auto range = vocalTargetRangeForInputType (voice);
    return midiNote >= range.firstMidi && midiNote <= range.lastMidi;
}

void VibeAutotuneEditor::applyNoteRule (int midiNote, NoteRule rule)
{
    if (processor.getGroupOctaves())
    {
        processor.setNoteRuleForPitchClass (midiNote % 12, rule);
    }
    else
    {
        processor.setNoteRuleForMidiNote (midiNote, rule);
    }
    repaint (pianoBounds);
}

void VibeAutotuneEditor::showNoteRuleMenu (int midiNote, juce::Rectangle<int> targetBounds)
{
    const auto current = processor.getGroupOctaves()
                       ? processor.getNoteRuleForPitchClass (midiNote % 12)
                       : processor.getNoteRuleForMidiNote (midiNote);
    juce::PopupMenu menu;
    const auto addRule = [&] (int id, const juce::String& name, NoteRule rule)
    {
        menu.addItem (id, name, true, current == rule);
    };
    addRule (1, "Legal", NoteRule::Legal);
    addRule (2, "Excluded / nearest legal", NoteRule::IllegalNearest);
    addRule (3, "Force up", NoteRule::IllegalUp);
    addRule (4, "Force down", NoteRule::IllegalDown);
    addRule (5, "Bypass", NoteRule::BypassLegal);
    menu.addSeparator();
    menu.addItem (6, "Reset this note to scale");
    menu.addItem (7, "Reset all notes to Key / Scale");

    auto safeThis = juce::Component::SafePointer<VibeAutotuneEditor> (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (targetBounds),
                        [safeThis, midiNote] (int result)
                        {
                            auto* editor = safeThis.getComponent();
                            if (editor == nullptr || result == 0)
                                return;
                            if (result >= 1 && result <= 5)
                                editor->applyNoteRule (midiNote, static_cast<NoteRule> (result - 1));
                            else if (result == 6)
                            {
                                if (editor->processor.getGroupOctaves())
                                {
                                    editor->processor.resetNoteRuleForPitchClass (midiNote % 12);
                                }
                                else
                                {
                                    editor->processor.resetNoteRuleForMidiNote (midiNote);
                                }
                                editor->repaint (editor->pianoBounds);
                            }
                            else if (result == 7)
                            {
                                editor->processor.resetNotePolicyToCurrentScale();
                                editor->repaint (editor->pianoBounds);
                            }
                        });
}

void VibeAutotuneEditor::mouseDown (const juce::MouseEvent& event)
{
    const auto midiNote = pianoNoteAt (event.getPosition());
    if (midiNote < 0)
        return;

    if (editNotesEnabled)
    {
        handleNoteRuleGesture (midiNote, event.mods);
        return;
    }

    uiPressedPianoNote = midiNote;
    uiPressFramesRemaining = 8;

    // Normal piano interaction selects an explicit target.  Rule editing is
    // intentionally kept behind the Edit Note toggle so the two workflows do
    // not mutate each other's state.
    const auto selectedTarget = processor.getMidiTargetNote();
    processor.setMidiTargetNoteFromUi (selectedTarget == midiNote ? -1 : midiNote);
    targetPianoNote = selectedTarget == midiNote ? -1 : midiNote;
    repaint (pianoBounds);
}

void VibeAutotuneEditor::handleNoteRuleGesture (int midiNote, const juce::ModifierKeys& modifiers)
{
    if (! editNotesEnabled || midiNote < firstKey || midiNote > lastKey)
        return;

    if (modifiers.isPopupMenu())
    {
        const auto localTarget = noteMapSlots[(size_t) (midiNote - firstKey)].indicatorBounds
                                     .expanded (3.0f, 2.0f).getSmallestIntegerContainer();
        showNoteRuleMenu (midiNote, localAreaToGlobal (localTarget));
        return;
    }

    const auto current = processor.getGroupOctaves()
                       ? processor.getNoteRuleForPitchClass (midiNote % 12)
                       : processor.getNoteRuleForMidiNote (midiNote);
    // Click advances through all five rules; Shift-click walks backwards.
    const auto direction = modifiers.isShiftDown() ? 4 : 1;
    applyNoteRule (midiNote, static_cast<NoteRule> ((static_cast<int> (current) + direction) % 5));
}
void VibeAutotuneEditor::timerCallback()
{
    if (uiPressFramesRemaining > 0)
    {
        --uiPressFramesRemaining;
        if (uiPressFramesRemaining == 0)
            uiPressedPianoNote = -1;
        repaint (pianoBounds);
    }

    monitor = processor.getPitchMonitorData();
    monitorScopeFrameCount = processor.copyWaveformScopeFrames (monitorScopeFrames.data(), (int) monitorScopeFrames.size());
    const auto tracking = processor.getPitchTrackingData();
    monitorHasValidPitch = monitor.voiced && tracking.voiced && std::isfinite (tracking.stableF0Hz)
                           && tracking.stableF0Hz > 0 && std::isfinite (tracking.confidence) && tracking.confidence >= .4;
    const auto wantedConfidence = monitorHasValidPitch ? (float) tracking.confidence : 0.0f;
    displayedPitchConfidence += (wantedConfidence - displayedPitchConfidence) * (monitorHasValidPitch ? .40f : .28f);
    const auto wantedCorrection = monitorHasValidPitch ? juce::jlimit (-100.0f, 100.0f, monitor.correctionCents) : 0.0f;
    displayedAppliedCorrectionCents += (wantedCorrection - displayedAppliedCorrectionCents) * (monitorHasValidPitch ? .40f : .28f);
    float scopeEnergy = 0.0f;
    for (int i = 0; i < monitorScopeFrameCount; ++i)
    {
        const auto& frame = monitorScopeFrames[(size_t) i];
        scopeEnergy += juce::jlimit (0.0f, 1.0f, (frame.maximum - frame.minimum) * 2.8f);
    }
    scopeEnergy /= (float) juce::jmax (1, monitorScopeFrameCount);
    const auto wantedRibbonMotion = monitorHasValidPitch
                                  ? juce::jlimit (.12f, .95f, .18f + scopeEnergy * .48f
                                                                  + std::abs (displayedAppliedCorrectionCents) / 250.0f)
                                  : 0.0f;
    displayedRibbonMotion += (wantedRibbonMotion - displayedRibbonMotion)
                           * (monitorHasValidPitch ? .22f : .12f);
    ribbonMotionPhase = std::fmod (ribbonMotionPhase + .12f + displayedRibbonMotion * .19f,
                                   juce::MathConstants<float>::twoPi);

    const auto previousInputNote = activePianoNote;
    const auto previousTargetNote = targetPianoNote;
    const auto inputNote = processor.getLatestDetectedMidiNote();
    const auto targetNote = processor.getLatestTargetMidiNote();
    activePianoNote = monitorHasValidPitch && inputNote >= 0 ? inputNote : -1;
    targetPianoNote = monitorHasValidPitch && targetNote >= 0 ? targetNote : -1;
    groupOctavesButton.setToggleState (processor.getGroupOctaves(), juce::dontSendNotification);
    const auto caption = processor.hasCustomNotePolicy() ? "SCALE *" : "SCALE";
    if (scaleLabel.getText() != caption) scaleLabel.setText (caption, juce::dontSendNotification);

    repaint (monitorBounds);
    if (previousInputNote != activePianoNote || previousTargetNote != targetPianoNote)
        repaint (pianoBounds);
}
