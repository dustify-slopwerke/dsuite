#pragma once

#include "ParamSection.h"

#include <juce_gui_basics/juce_gui_basics.h>

// One tab of an editor: rows of ParamSections, for a plugin whose parameters are identified by
// the enum PID (looked up with its paramInfo(), see common/dsp/ParamTable.h). Sections in a row
// grow to fill it (never shrink below their preferred width); a row whose sections don't fill the
// width keeps them left-aligned.
template <typename PID>
class SectionPage final : public juce::Component
{
public:
    struct Spec
    {
        const char* title;
        int width;
        std::initializer_list<PID> params;
        juce::uint32 accent;
    };

    static constexpr int rowHeight = 180;

    SectionPage (juce::AudioProcessorValueTreeState& state, std::initializer_list<std::initializer_list<Spec>> rowSpecs,
                 int fillWidth)
        : fill (fillWidth)
    {
        for (const auto& specs : rowSpecs)
        {
            auto& row = rows.emplace_back();
            for (const auto& spec : specs)
            {
                std::vector<const dustify::ParamSpec*> params;
                for (auto pid : spec.params)
                    params.push_back (&paramInfo (pid));
                row.push_back ({ std::make_unique<ParamSection> (spec.title, state, std::move (params), juce::Colour (spec.accent)), spec.width });
                addAndMakeVisible (*row.back().section);
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        for (auto& row : rows)
        {
            auto bounds = area.removeFromTop (rowHeight);
            int natural = 0;
            for (auto& item : row)
                natural += item.width;

            if (natural >= fill)
            {
                juce::FlexBox flex;
                for (auto& item : row)
                    flex.items.add (juce::FlexItem (*item.section).withFlex (1.0f, 0.0f, (float) item.width));
                flex.performLayout (bounds);
            }
            else
            {
                for (auto& item : row)
                    item.section->setBounds (bounds.removeFromLeft (item.width));
            }
        }
    }

    int preferredHeight() const { return (int) rows.size() * rowHeight; }

    void setActive (PID pid, bool active)
    {
        for (auto& row : rows)
            for (auto& item : row)
                item.section->setActive (paramInfo (pid), active);
    }

    void setTooltip (PID pid, const juce::String& text)
    {
        for (auto& row : rows)
            for (auto& item : row)
                item.section->setTooltip (paramInfo (pid), text);
    }

private:
    struct Item
    {
        std::unique_ptr<ParamSection> section;
        int width;
    };

    int fill;
    std::vector<std::vector<Item>> rows;
};
