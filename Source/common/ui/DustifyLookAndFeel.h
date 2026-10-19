#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The suite's visual style: dark panels, knobs with a value arc in their section's colour
// (bipolar knobs draw from the centre), LED toggles, flat tabs. Section colours reach the
// widgets through their standard colour ids, set by ParamSection.
class DustifyLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    struct Palette
    {
        static constexpr juce::uint32 background = 0xff17191d;
        static constexpr juce::uint32 panel = 0xff23262c;
        static constexpr juce::uint32 panelEdge = 0xff33373f;
        static constexpr juce::uint32 text = 0xffd6d9de;
        static constexpr juce::uint32 dimText = 0xff8a9099;
        static constexpr juce::uint32 control = 0xff2f333a;

        // Section accents (dsynth names; other plugins pick whichever reads best)
        static constexpr juce::uint32 source = 0xffe0a040;    // oscillators, mixer
        static constexpr juce::uint32 filter = 0xffe8673c;    // filters
        static constexpr juce::uint32 envelope = 0xff3cc4b0;  // envelopes, voicing
        static constexpr juce::uint32 modulation = 0xffa47de8; // LFO, matrix, arp
        static constexpr juce::uint32 effects = 0xff4f9de8;   // chorus, delay, reverb
        static constexpr juce::uint32 global = 0xff9aa3ad;    // analog, layers, MPE
    };

    DustifyLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float startAngle, float endAngle, juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& background,
                               bool highlighted, bool down) override;

    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool mouseOver, bool mouseDown) override;
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) override {}
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}

    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (13.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::FontOptions (13.5f); }
};
