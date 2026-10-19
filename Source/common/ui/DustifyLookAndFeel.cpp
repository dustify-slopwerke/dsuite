#include "DustifyLookAndFeel.h"

namespace
{
using P = DustifyLookAndFeel::Palette;
juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }
} // namespace

DustifyLookAndFeel::DustifyLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, c (P::background));
    setColour (juce::Label::textColourId, c (P::text));
    setColour (juce::Slider::textBoxTextColourId, c (P::text));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::rotarySliderFillColourId, c (P::source));
    setColour (juce::ComboBox::backgroundColourId, c (P::control));
    setColour (juce::ComboBox::textColourId, c (P::text));
    setColour (juce::ComboBox::outlineColourId, c (P::panelEdge));
    setColour (juce::ComboBox::arrowColourId, c (P::source));
    setColour (juce::PopupMenu::backgroundColourId, c (P::panel));
    setColour (juce::PopupMenu::textColourId, c (P::text));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, c (P::control).brighter (0.15f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::buttonColourId, c (P::control));
    setColour (juce::TextButton::textColourOffId, c (P::text));
    setColour (juce::ToggleButton::tickColourId, c (P::source));
    setColour (juce::TooltipWindow::backgroundColourId, c (P::panel).brighter (0.1f));
    setColour (juce::TooltipWindow::textColourId, c (P::text));
}

void DustifyLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                          float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto centre = bounds.getCentre();
    const float arcRadius = radius - 2.0f;
    const float thickness = juce::jmax (2.5f, radius * 0.16f);
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const float angle = startAngle + sliderPos * (endAngle - startAngle);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (c (P::control).brighter (0.05f));
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc: from the start, or from 12 o'clock for bipolar parameters
    const bool bipolar = slider.getProperties()["bipolar"];
    const float from = bipolar ? (startAngle + endAngle) / 2.0f : startAngle;
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (slider.isEnabled() ? accent : accent.withSaturation (0.0f));
        g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Knob body
    const float bodyRadius = arcRadius - thickness - 2.0f;
    g.setGradientFill (juce::ColourGradient (c (0xff3a3f48), centre.x, centre.y - bodyRadius,
                                             c (0xff22252b), centre.x, centre.y + bodyRadius, false));
    g.fillEllipse (juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre));
    g.setColour (c (P::panelEdge).brighter (0.2f));
    g.drawEllipse (juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre), 1.0f);

    // Pointer
    const auto tip = centre.getPointOnCircumference (bodyRadius - 2.0f, angle);
    const auto base = centre.getPointOnCircumference (bodyRadius * 0.35f, angle);
    g.setColour (slider.isMouseOverOrDragging() ? juce::Colours::white : c (P::text));
    g.drawLine ({ base, tip }, 2.0f);
}

void DustifyLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // An LED switch: rounded pill, lit in the section colour when on
    const auto bounds = button.getLocalBounds().toFloat().withSizeKeepingCentre (30.0f, 16.0f);
    const auto accent = button.findColour (juce::ToggleButton::tickColourId);
    const bool on = button.getToggleState();

    g.setColour (on ? accent.withAlpha (0.25f) : c (P::control));
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (on ? accent : c (P::panelEdge).brighter (highlighted ? 0.4f : 0.2f));
    g.drawRoundedRectangle (bounds, 8.0f, 1.0f);

    const auto led = juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ on ? bounds.getRight() - 8.0f : bounds.getX() + 8.0f, bounds.getCentreY() });
    if (on)
    {
        g.setColour (accent.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.0f)); // glow
    }
    g.setColour (on ? accent : c (P::dimText));
    g.fillEllipse (led);
}

void DustifyLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (box.hasKeyboardFocus (true) || box.isMouseOver (true) ? c (P::panelEdge).brighter (0.5f) : c (P::panelEdge));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    // Small chevron in the section colour
    const float cx = (float) width - 12.0f, cy = (float) height * 0.5f;
    juce::Path arrow;
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void DustifyLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                              bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = background;
    if (down)
        fill = fill.brighter (0.2f);
    else if (highlighted)
        fill = fill.brighter (0.08f);
    g.setColour (fill);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (c (P::panelEdge).brighter (highlighted ? 0.4f : 0.15f));
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

void DustifyLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool mouseOver, bool)
{
    auto bounds = button.getLocalBounds();
    const bool active = button.isFrontTab();

    g.setColour (active ? c (P::text) : (mouseOver ? c (P::text).withAlpha (0.85f) : c (P::dimText)));
    g.setFont (juce::FontOptions (14.0f, active ? juce::Font::bold : juce::Font::plain));
    g.drawText (button.getButtonText(), bounds.withTrimmedBottom (3), juce::Justification::centred);

    if (active)
    {
        g.setColour (c (P::source));
        g.fillRoundedRectangle (bounds.removeFromBottom (3).reduced (10, 0).toFloat(), 1.5f);
    }
}

juce::Label* DustifyLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (juce::FontOptions (12.0f));
    label->setColour (juce::Label::textColourId, c (P::dimText).brighter (0.25f));
    label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    label->setColour (juce::TextEditor::backgroundColourId, c (P::control));
    label->setColour (juce::TextEditor::outlineColourId, c (P::panelEdge));
    return label;
}
