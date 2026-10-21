#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "common/dsp/ParamTable.h"

// A titled panel of controls. The widget for each parameter follows its type in the table:
// rotary knob for floats, combo box for choices, toggle for bools. The accent colours the title
// and the controls (knob arcs, LEDs, combo chevrons). Compact sections (damp's pedal tiles) have
// smaller controls and a taller title strip, with room for a switch on its right.
class ParamSection final : public juce::Component
{
public:
    enum class Size { Normal, Compact };

    ParamSection (juce::String title, juce::AudioProcessorValueTreeState&, std::vector<const dustify::ParamSpec*>,
                  juce::Colour accent, Size = Size::Normal);
    ~ParamSection() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Width this section wants when laid out on one row of controls
    int preferredWidth() const;
    // Height of the title strip, and of the whole section with one row of controls
    int titleHeight() const;
    int preferredHeight() const;
    // Width the title strip needs for the accent bar and the title
    int titleWidth() const;

    // Collapsed, only the title strip is shown
    void setCollapsed (bool);

    // Dims (but doesn't disable) a control whose parameter currently has no effect
    void setActive (const dustify::ParamSpec&, bool active);

    // Replaces a control's tooltip (its parameter's name by default)
    void setTooltip (const dustify::ParamSpec&, const juce::String& text);

private:
    class Control;

    juce::String title;
    juce::Colour accent;
    Size size;
    bool collapsed = false;
    std::vector<std::unique_ptr<Control>> controls;
};
