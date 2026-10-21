#include "KeyboardPanel.h"

#include "PluginProcessor.h"

#include "common/ui/DustifyLookAndFeel.h"

namespace
{
using P = DustifyLookAndFeel::Palette;
juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }

constexpr juce::uint32 accent = P::global;
constexpr int titleHeight = 22, padding = 6;
const juce::FontOptions titleFont { 13.0f, juce::Font::bold };

int midi (float velocity) { return juce::roundToInt (velocity * 127.0f); }
double now() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }
} // namespace

KeyboardPanel::KeyboardPanel (DpianoAudioProcessor& p) : processor (p)
{
    setComponentID ("keyboardPanel");
    calibrate.setComponentID ("calibrateButton");
    calibrate.setTooltip ("Learn your keyboard's velocity range: play your softest notes, then your hardest. "
                          "Saved for every dpiano on this computer, and with the project");
    calibrate.setColour (juce::TextButton::buttonOnColourId, c (P::modulation).withAlpha (0.5f));
    calibrate.onClick = [this] { capturing ? finishCalibration() : startCalibration(); };
    addAndMakeVisible (calibrate);

    reset.setComponentID ("resetVelocityButton");
    reset.setTooltip ("Use velocities as played again (clears the calibration and the sensitivity)");
    reset.onClick = [this]
    {
        capturing = false;
        calibrate.setButtonText ("Calibrate");
        calibrate.setToggleState (false, juce::dontSendNotification);
        processor.setVelocityMap ({}, true);
        sensitivity.setValue (0.0, juce::dontSendNotification);
        sensitivityChangedAt = -1.0;
        result = {};
        refreshMeter();
        updateStatus();
    };
    addAndMakeVisible (reset);

    sensitivityLabel.setText ("Sensitivity", juce::dontSendNotification);
    sensitivityLabel.setJustificationType (juce::Justification::centred);
    sensitivityLabel.setFont (juce::FontOptions (12.0f));
    sensitivityLabel.setColour (juce::Label::textColourId, c (P::dimText));
    addAndMakeVisible (sensitivityLabel);

    sensitivity.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sensitivity.setColour (juce::Slider::rotarySliderFillColourId, c (accent));
    sensitivity.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    sensitivity.setColour (juce::Slider::textBoxTextColourId, c (P::text).withAlpha (0.8f));
    sensitivity.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 66, 16);
    sensitivity.getProperties().set ("bipolar", true);
    sensitivity.setRange (-100.0, 100.0, 1.0);
    sensitivity.setTextValueSuffix (" %");
    sensitivity.setDoubleClickReturnValue (true, 0.0);
    sensitivity.setTooltip ("Keyboard sensitivity: above 0, softer playing gives louder notes; below 0, it takes harder playing");
    sensitivity.setValue (processor.getVelocityMap().sensitivity * 100.0, juce::dontSendNotification);
    sensitivity.onValueChange = [this]
    {
        auto map = processor.getVelocityMap();
        map.sensitivity = (float) sensitivity.getValue() / 100.0f;
        processor.setVelocityMap (map, false);
        sensitivityChangedAt = now();
        refreshMeter();
    };
    addAndMakeVisible (sensitivity);

    status.setFont (juce::FontOptions (12.5f));
    status.setJustificationType (juce::Justification::topLeft);
    status.setColour (juce::Label::textColourId, c (P::text));
    status.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (status);

    // Velocities played before the editor opened aren't a calibration
    for (float v; processor.popPlayedVelocity (v);)
        ;
    refreshMeter();
    updateStatus();
    startTimerHz (30);
}

KeyboardPanel::~KeyboardPanel()
{
    if (sensitivityChangedAt >= 0.0)
        processor.setVelocityMap (processor.getVelocityMap(), true);
}

void KeyboardPanel::startCalibration()
{
    for (float v; processor.popPlayedVelocity (v);)
        ;
    capture = {};
    capturing = true;
    lastExtremeTime = now();
    result = {};
    calibrate.setButtonText ("Done");
    calibrate.setToggleState (true, juce::dontSendNotification);
    updateStatus();
    repaint();
}

void KeyboardPanel::finishCalibration()
{
    capturing = false;
    calibrate.setButtonText ("Calibrate");
    calibrate.setToggleState (false, juce::dontSendNotification);

    if (capture.enoughRange())
    {
        processor.setVelocityMap (capture.result ((float) sensitivity.getValue() / 100.0f), true);
        sensitivityChangedAt = -1.0; // just saved
        result = "Calibrated: your softest note now plays as pp, your hardest as ff.";
    }
    else if (capture.notes == 0)
    {
        result = "No notes played: calibration unchanged.";
    }
    else
    {
        result = "Not enough range (" + juce::String (midi (capture.lowest)) + juce::String::fromUTF8 ("\xe2\x80\x93")
                 + juce::String (midi (capture.highest)) + "): calibration unchanged. Try again, from your "
                 "very softest to your hardest.";
    }
    refreshMeter();
    updateStatus();
    repaint();
}

void KeyboardPanel::timerCallback()
{
    for (float v; processor.popPlayedVelocity (v);)
        if (capturing && capture.add (v))
            lastExtremeTime = now();

    if (capturing)
    {
        if (now() - lastExtremeTime > autoFinishSeconds)
            finishCalibration();
        else
            updateStatus();
    }

    if (sensitivityChangedAt >= 0.0 && now() - sensitivityChangedAt > 0.5)
    {
        processor.setVelocityMap (processor.getVelocityMap(), true);
        sensitivityChangedAt = -1.0;
    }

    // The map can also change under us (a session loaded while the editor is open)
    if (const double target = std::round (processor.getVelocityMap().sensitivity * 100.0f);
        sensitivityChangedAt < 0.0 && ! sensitivity.isMouseButtonDown() && ! juce::exactlyEqual (sensitivity.getValue(), target))
        sensitivity.setValue (target, juce::dontSendNotification);

    if (processor.playedNoteCount() != seenNotes)
        refreshMeter();
}

void KeyboardPanel::refreshMeter()
{
    seenNotes = processor.playedNoteCount();
    if (seenNotes > 0)
    {
        played = processor.lastPlayedVelocity();
        mapped = processor.getVelocityMap().sanitised().apply (played);
    }
    repaint (meterArea);
}

void KeyboardPanel::updateStatus()
{
    juce::String text;
    if (capturing)
    {
        text = juce::String::fromUTF8 ("Play your softest notes, then your hardest \xe2\x80\x94 press Done.\n");
        text << (capture.notes == 0 ? juce::String ("Waiting for notes...")
                                    : "Softest " + juce::String (midi (capture.lowest)) + ", hardest "
                                          + juce::String (midi (capture.highest)) + " (" + juce::String (capture.notes)
                                          + (capture.notes == 1 ? " note)" : " notes)"));
    }
    else if (result.isNotEmpty())
    {
        text = result;
    }
    else if (! processor.getVelocityMap().isCalibrated())
    {
        text = "Not calibrated: velocities are used as played. Calibrate if your keyboard feels too soft or too hard.";
    }
    else
    {
        text = "Calibrated to your keyboard.";
    }
    if (status.getText() != text)
        status.setText (text, juce::dontSendNotification);
}

void KeyboardPanel::paint (juce::Graphics& g)
{
    // As ParamSection draws its panel and title
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (c (P::panel));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (c (P::panelEdge));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
    auto header = getLocalBounds().removeFromTop (titleHeight).reduced (padding + 2, 0);
    g.setColour (c (accent));
    g.fillRoundedRectangle (header.removeFromLeft (3).withSizeKeepingCentre (3, 12).toFloat(), 1.5f);
    header.removeFromLeft (6);
    g.setFont (titleFont);
    g.drawText ("KEYBOARD", header, juce::Justification::centredLeft);

    // Velocity meter: as played (with the calibrated or captured range shaded), and as mapped
    const auto map = processor.getVelocityMap();
    const float rangeLow = capturing ? (capture.notes > 0 ? capture.lowest : -1.0f) : map.inMin;
    const float rangeHigh = capturing ? capture.highest : map.inMax;
    const auto rangeColour = c (capturing ? P::modulation : accent);

    auto area = meterArea;
    auto drawBar = [&] (const juce::String& name, float value, bool withRange)
    {
        auto row = area.removeFromTop (20);
        area.removeFromTop (6);
        g.setColour (c (P::dimText));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (name, row.removeFromLeft (52), juce::Justification::centredLeft);
        g.setColour (c (P::text));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (value < 0.0f ? juce::String::fromUTF8 ("\xe2\x80\x93") : juce::String (midi (value)),
                    row.removeFromRight (30), juce::Justification::centredRight);
        row.removeFromRight (6);

        const auto bar = row.toFloat().reduced (0.0f, 4.0f);
        g.setColour (c (P::control));
        g.fillRoundedRectangle (bar, 3.0f);
        auto x = [&] (float v) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, v); };
        if (withRange && rangeLow >= 0.0f && rangeHigh >= rangeLow)
        {
            g.setColour (rangeColour.withAlpha (0.2f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x (rangeLow), bar.getY(), x (rangeHigh), bar.getBottom()));
            g.setColour (rangeColour.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x (rangeLow) - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
            g.fillRect (juce::Rectangle<float> (x (rangeHigh) - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
        }
        if (value >= 0.0f)
        {
            g.setColour (c (P::envelope));
            g.fillRoundedRectangle (bar.withRight (juce::jmax (bar.getX() + 3.0f, x (value))), 3.0f);
        }
    };
    drawBar ("PLAYED", played, true);
    drawBar ("MAPPED", mapped, false);

    g.setColour (c (P::dimText));
    g.setFont (juce::FontOptions (12.0f));
    const auto dash = juce::String::fromUTF8 ("\xe2\x80\x93");
    juce::String range;
    if (capturing)
        range = capture.notes > 0 ? "Captured " + juce::String (midi (capture.lowest)) + dash + juce::String (midi (capture.highest))
                                  : juce::String ("Capturing...");
    else
        range = "Range " + juce::String (midi (map.inMin)) + dash + juce::String (midi (map.inMax))
                + (map.isCalibrated() ? "" : " (full)");
    g.drawText (range, area.removeFromTop (16), juce::Justification::centredLeft);
}

void KeyboardPanel::resized()
{
    auto area = getLocalBounds().reduced (padding + 4, padding).withTrimmedTop (titleHeight - padding + 4);

    auto buttons = area.removeFromLeft (96);
    calibrate.setBounds (buttons.removeFromTop (28));
    buttons.removeFromTop (8);
    reset.setBounds (buttons.removeFromTop (28));
    area.removeFromLeft (14);

    auto knob = area.removeFromRight (70);
    sensitivityLabel.setBounds (knob.removeFromTop (16));
    sensitivity.setBounds (knob.removeFromTop (74));
    area.removeFromRight (10);

    status.setBounds (area.removeFromTop (48));
    area.removeFromTop (4);
    meterArea = area;
}
