#include "TunerView.h"

#include "PluginProcessor.h"
#include "common/ui/DustifyLookAndFeel.h"

namespace
{
constexpr int window = 2048;
}

TunerView::TunerView (DampAudioProcessor& p) : processor (p)
{
    using P = DustifyLookAndFeel::Palette;

    mute.setToggleState (processor.tunerMute.load(), juce::dontSendNotification);
    mute.onClick = [this] { processor.tunerMute = mute.getToggleState(); };
    addAndMakeVisible (mute);
    muteLabel.setText ("Mute output while tuning", juce::dontSendNotification);
    muteLabel.setColour (juce::Label::textColourId, juce::Colour (P::dimText));
    addAndMakeVisible (muteLabel);

    reference.setRange (415.0, 466.0, 1.0);
    reference.setValue (processor.tunerReference.load(), juce::dontSendNotification);
    reference.setTextValueSuffix (" Hz");
    reference.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 64, 24);
    reference.onValueChange = [this] { processor.tunerReference = (float) reference.getValue(); };
    addAndMakeVisible (reference);
    referenceLabel.setText ("A4 =", juce::dontSendNotification);
    referenceLabel.setColour (juce::Label::textColourId, juce::Colour (P::dimText));
    referenceLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (referenceLabel);

    setInterceptsMouseClicks (true, true); // the page underneath stays untouched while tuning
}

TunerView::~TunerView()
{
    processor.setTunerActive (false);
}

void TunerView::visibilityChanged()
{
    processor.setTunerActive (isVisible());
    if (isVisible())
    {
        std::vector<float> discard (4096);
        while (processor.readTunerSamples (discard.data(), (int) discard.size()) > 0) {} // stale audio
        history.assign (window, 0.0f);
        sinceReading = 10.0f;
    }
}

void TunerView::update (float secondsSinceLast)
{
    if (! isVisible())
        return;
    const double rate = processor.tunerSampleRate();
    if (! detector || std::abs (detector->sampleRate() - rate) > 1.0e-6)
    {
        detector.emplace (rate, window);
        history.assign (window, 0.0f);
    }

    incoming.resize (8192);
    const int got = processor.readTunerSamples (incoming.data(), (int) incoming.size());
    if (got >= window)
    {
        std::copy (incoming.begin() + (got - window), incoming.begin() + got, history.begin());
    }
    else if (got > 0)
    {
        std::move (history.begin() + got, history.end(), history.begin());
        std::copy_n (incoming.begin(), got, history.end() - got);
    }

    sinceReading += secondsSinceLast;
    const auto result = detector->detect (history.data());
    if (result.hz > 0.0 && result.clarity > 0.85)
    {
        const auto reading = damp::nearestNote (result.hz, processor.tunerReference.load());
        if (reading.midiNote == shownNote && sinceReading < 0.5f)
        {
            // Settle the needle on the same note; jump when the note changes
            shownCents += 0.35 * (reading.cents - shownCents);
            shownHz += 0.35 * (result.hz - shownHz);
        }
        else
        {
            shownCents = reading.cents;
            shownHz = result.hz;
        }
        shownNote = reading.midiNote;
        sinceReading = 0.0f;
    }
    repaint();
}

void TunerView::paint (juce::Graphics& g)
{
    using P = DustifyLookAndFeel::Palette;
    auto area = getLocalBounds().toFloat();
    g.setColour (juce::Colour (P::background));
    g.fillRoundedRectangle (area, 8.0f);
    g.setColour (juce::Colour (P::panelEdge));
    g.drawRoundedRectangle (area.reduced (0.5f), 8.0f, 1.0f);

    const bool live = sinceReading < 1.0f && shownNote >= 0;
    const bool inTune = live && std::abs (shownCents) < 3.0;
    const auto accent = inTune ? juce::Colour (P::envelope) : juce::Colour (P::source);
    const float alpha = live ? 1.0f : juce::jmax (0.25f, 1.0f - (sinceReading - 1.0f)); // fades after the note ends

    auto content = area.reduced (24.0f, 16.0f);
    content.removeFromBottom (40.0f); // controls

    // Note name, big, with the octave small after it
    auto noteArea = content.removeFromTop (content.getHeight() * 0.55f);
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (shownNote >= 0)
    {
        g.setColour (accent.withAlpha (alpha));
        g.setFont (juce::FontOptions (noteArea.getHeight() * 0.9f, juce::Font::bold));
        const juce::String name (names[shownNote % 12]);
        g.drawText (name, noteArea, juce::Justification::centred);
        g.setFont (juce::FontOptions (noteArea.getHeight() * 0.3f));
        const float nameWidth = (float) juce::GlyphArrangement::getStringWidthInt (juce::Font (juce::FontOptions (noteArea.getHeight() * 0.9f, juce::Font::bold)), name);
        g.drawText (juce::String (shownNote / 12 - 1), noteArea.withX (noteArea.getCentreX() + nameWidth * 0.5f + 4.0f).withWidth (60.0f).withTrimmedBottom (noteArea.getHeight() * 0.15f),
                    juce::Justification::bottomLeft);
    }
    else
    {
        g.setColour (juce::Colour (P::dimText));
        g.setFont (juce::FontOptions (22.0f));
        g.drawText ("Play a string", noteArea, juce::Justification::centred);
    }

    // Cents scale: -50 .. +50 with ticks, a green zone and the needle
    auto scale = content.removeFromTop (44.0f).reduced (40.0f, 0.0f);
    const float mid = scale.getCentreX(), half = scale.getWidth() * 0.5f;
    g.setColour (juce::Colour (P::envelope).withAlpha (0.15f));
    g.fillRect (juce::Rectangle<float> (mid - half * 0.06f, scale.getY(), half * 0.12f, scale.getHeight()));
    for (int c = -50; c <= 50; c += 10)
    {
        const float x = mid + half * (float) c / 50.0f;
        const float h = c == 0 ? scale.getHeight() : c % 50 == 0 ? scale.getHeight() * 0.7f : scale.getHeight() * 0.45f;
        g.setColour (juce::Colour (c == 0 ? P::text : P::dimText));
        g.fillRect (juce::Rectangle<float> (x - 0.75f, scale.getCentreY() - h * 0.5f, 1.5f, h));
    }
    if (shownNote >= 0)
    {
        const float x = mid + half * (float) juce::jlimit (-50.0, 50.0, shownCents) / 50.0f;
        g.setColour (accent.withAlpha (alpha));
        g.fillRoundedRectangle (juce::Rectangle<float> (x - 3.0f, scale.getY() - 4.0f, 6.0f, scale.getHeight() + 8.0f), 3.0f);
    }

    // Scale labels, then the reading
    g.setColour (juce::Colour (P::dimText));
    g.setFont (juce::FontOptions (12.0f));
    auto labels = content.removeFromTop (18.0f);
    for (int c : { -50, 0, 50 })
    {
        const float x = mid + half * (float) c / 50.0f;
        g.drawText (c == 0 ? juce::String ("0") : juce::String::formatted ("%+d", c), juce::Rectangle<float> (x - 30.0f, labels.getY(), 60.0f, labels.getHeight()),
                    juce::Justification::centred);
    }
    g.drawText ("flat", juce::Rectangle<float> (mid - half * 0.5f - 30.0f, labels.getY(), 60.0f, labels.getHeight()), juce::Justification::centred);
    g.drawText ("sharp", juce::Rectangle<float> (mid + half * 0.5f - 30.0f, labels.getY(), 60.0f, labels.getHeight()), juce::Justification::centred);

    if (shownNote >= 0)
    {
        g.setColour (juce::Colour (P::text).withAlpha (alpha));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ((shownCents >= 0.0 ? "+" : "") + juce::String (shownCents, 1) + " cents      " + juce::String (shownHz, 2) + " Hz",
                    content.removeFromTop (30.0f), juce::Justification::centred);
    }
}

void TunerView::resized()
{
    auto controls = getLocalBounds().reduced (24, 16).removeFromBottom (32);
    mute.setBounds (controls.removeFromLeft (44));
    muteLabel.setBounds (controls.removeFromLeft (200));
    reference.setBounds (controls.removeFromRight (150));
    referenceLabel.setBounds (controls.removeFromRight (50));
}
