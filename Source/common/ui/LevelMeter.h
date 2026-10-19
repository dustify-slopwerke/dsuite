#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "DustifyLookAndFeel.h"

// Horizontal peak meter, -60 .. 0 dBFS, with a falling bar and a peak hold. Optionally shades a
// recommended range (an input level sweet spot). Feed it with setPeak() from a timer.
class LevelMeter final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    explicit LevelMeter (juce::String labelText) : label (std::move (labelText)) {}

    void setSweetSpot (float lowDb, float highDb)
    {
        sweetLow = lowDb;
        sweetHigh = highDb;
    }

    // Call regularly (e.g. 30 Hz) with the peak since the last call
    void setPeak (float linear, float secondsSinceLast)
    {
        const float db = juce::Decibels::gainToDecibels (linear, -100.0f);
        level = juce::jmax (db, level - 30.0f * secondsSinceLast); // falls at 30 dB/s
        if (db >= hold)
        {
            hold = db;
            holdAge = 0.0f;
        }
        else if ((holdAge += secondsSinceLast) > 1.5f)
        {
            hold = juce::jmax (level, hold - 30.0f * secondsSinceLast);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using P = DustifyLookAndFeel::Palette;
        auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (P::dimText));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label, area.removeFromLeft (30.0f), juce::Justification::centredLeft);

        const auto bar = area.reduced (0.0f, 3.0f);
        g.setColour (juce::Colour (P::control));
        g.fillRoundedRectangle (bar, 3.0f);
        auto x = [&] (float db) { return bar.getX() + bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); };

        if (sweetHigh > sweetLow)
        {
            g.setColour (juce::Colour (P::envelope).withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x (sweetLow), bar.getY(), x (sweetHigh), bar.getBottom()));
        }

        const auto colourFor = [] (float db)
        {
            return db > -1.0f ? juce::Colour (0xffe5484d) : db > -6.0f ? juce::Colour (0xffe0b040) : juce::Colour (P::envelope);
        };
        if (level > -60.0f)
        {
            g.setColour (colourFor (level));
            g.fillRoundedRectangle (bar.withRight (x (level)), 3.0f);
        }
        if (hold > -60.0f)
        {
            g.setColour (colourFor (hold).brighter (0.3f));
            g.fillRect (juce::Rectangle<float> (x (hold) - 1.0f, bar.getY(), 2.0f, bar.getHeight()));
        }
    }

private:
    juce::String label;
    float level = -100.0f, hold = -100.0f, holdAge = 0.0f;
    float sweetLow = 0.0f, sweetHigh = 0.0f;
};
