#include "ParamSection.h"

#include "DustifyLookAndFeel.h"

namespace
{
struct Metrics
{
    int titleHeight, padding, labelHeight, knobWidth, comboWidth, toggleWidth, controlHeight;
    float labelFont;
};
constexpr Metrics normalMetrics { 22, 6, 16, 66, 112, 62, 74, 12.0f };
constexpr Metrics compactMetrics { 28, 5, 14, 56, 86, 56, 58, 11.0f }; // the labels still fit: "Sensitivity", "Ping-Pong"

const Metrics& metricsFor (ParamSection::Size size) { return size == ParamSection::Size::Compact ? compactMetrics : normalMetrics; }
const juce::FontOptions titleFont { 13.0f, juce::Font::bold };

juce::String toJuce (std::string_view s) { return juce::String (s.data(), s.size()); }
} // namespace

class ParamSection::Control final : public juce::Component
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    Control (APVTS& state, const dustify::ParamSpec& info, juce::Colour accent, const Metrics& sizes)
        : spec (&info), type (info.type), m (sizes)
    {
        const auto id = toJuce (info.id);

        label.setText (toJuce (info.label), juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (m.labelFont));
        label.setColour (juce::Label::textColourId, juce::Colour (DustifyLookAndFeel::Palette::dimText));
        addAndMakeVisible (label);

        slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
        slider.getProperties().set ("bipolar", info.min < 0.0f && info.max > 0.0f);
        toggle.setColour (juce::ToggleButton::tickColourId, accent);
        combo.setColour (juce::ComboBox::arrowColourId, accent);

        switch (type)
        {
            case dustify::ParamType::Float:
                slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
                // Before the text box exists, so it's created with these colours
                slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
                slider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (DustifyLookAndFeel::Palette::text).withAlpha (0.8f));
                slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, m.knobWidth, m.labelHeight);
                slider.setDoubleClickReturnValue (true, info.def);
                slider.setTooltip (toJuce (info.name));
                addAndMakeVisible (slider);
                sliderAttachment = std::make_unique<APVTS::SliderAttachment> (state, id, slider);
                break;

            case dustify::ParamType::Choice:
                for (size_t i = 0; i < info.choices.size(); ++i)
                    combo.addItem (toJuce (info.choices[i]), (int) i + 1);
                combo.setTooltip (toJuce (info.name));
                addAndMakeVisible (combo);
                comboAttachment = std::make_unique<APVTS::ComboBoxAttachment> (state, id, combo);
                break;

            case dustify::ParamType::Bool:
                toggle.setTooltip (toJuce (info.name));
                addAndMakeVisible (toggle);
                buttonAttachment = std::make_unique<APVTS::ButtonAttachment> (state, id, toggle);
                break;
        }
    }

    const dustify::ParamSpec* getSpec() const { return spec; }

    void setTooltip (const juce::String& text)
    {
        slider.setTooltip (text);
        combo.setTooltip (text);
        toggle.setTooltip (text);
    }

    void setActive (bool active)
    {
        const float alpha = active ? 1.0f : 0.35f;
        if (! juce::approximatelyEqual (getAlpha(), alpha))
            setAlpha (alpha);
    }

    int preferredWidth() const
    {
        switch (type)
        {
            case dustify::ParamType::Float: return m.knobWidth;
            case dustify::ParamType::Choice: return m.comboWidth;
            case dustify::ParamType::Bool: return m.toggleWidth;
        }
        return m.knobWidth;
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (m.labelHeight));

        switch (type)
        {
            case dustify::ParamType::Float: slider.setBounds (area); break;
            case dustify::ParamType::Choice: combo.setBounds (area.removeFromTop (24).reduced (2, 0)); break;
            case dustify::ParamType::Bool: toggle.setBounds (area.removeFromTop (26).withSizeKeepingCentre (40, 24)); break;
        }
    }

private:
    const dustify::ParamSpec* spec;
    dustify::ParamType type;
    const Metrics& m;
    juce::Label label;
    juce::Slider slider;
    juce::ComboBox combo;
    juce::ToggleButton toggle;
    std::unique_ptr<APVTS::SliderAttachment> sliderAttachment;
    std::unique_ptr<APVTS::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<APVTS::ButtonAttachment> buttonAttachment;
};

ParamSection::ParamSection (juce::String sectionTitle, juce::AudioProcessorValueTreeState& state,
                            std::vector<const dustify::ParamSpec*> params, juce::Colour accentColour, Size sectionSize)
    : title (std::move (sectionTitle)), accent (accentColour), size (sectionSize)
{
    for (const auto* info : params)
    {
        controls.push_back (std::make_unique<Control> (state, *info, accent, metricsFor (size)));
        addAndMakeVisible (*controls.back());
    }
}

ParamSection::~ParamSection() = default;

void ParamSection::setActive (const dustify::ParamSpec& info, bool active)
{
    for (auto& c : controls)
        if (c->getSpec() == &info)
            c->setActive (active);
}

void ParamSection::setTooltip (const dustify::ParamSpec& info, const juce::String& text)
{
    for (auto& c : controls)
        if (c->getSpec() == &info)
            c->setTooltip (text);
}

int ParamSection::preferredWidth() const
{
    int width = 2 * metricsFor (size).padding;
    for (const auto& c : controls)
        width += c->preferredWidth();
    return width;
}

int ParamSection::titleHeight() const
{
    return metricsFor (size).titleHeight;
}

int ParamSection::preferredHeight() const
{
    const auto& m = metricsFor (size);
    return m.titleHeight + m.controlHeight + m.padding;
}

int ParamSection::titleWidth() const
{
    // As paint() lays the strip out: padding, accent bar, gap, title
    return metricsFor (size).padding + 2 + 3 + 6 + juce::GlyphArrangement::getStringWidthInt (juce::Font (titleFont), title.toUpperCase());
}

void ParamSection::setCollapsed (bool shouldCollapse)
{
    collapsed = shouldCollapse;
    for (auto& c : controls)
        c->setVisible (! collapsed);
}

void ParamSection::paint (juce::Graphics& g)
{
    using P = DustifyLookAndFeel::Palette;
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (juce::Colour (P::panel));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour (P::panelEdge));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    // Accent bar and title
    const auto& m = metricsFor (size);
    auto header = getLocalBounds().removeFromTop (m.titleHeight).reduced (m.padding + 2, 0);
    g.setColour (accent);
    g.fillRoundedRectangle (header.removeFromLeft (3).withSizeKeepingCentre (3, 12).toFloat(), 1.5f);
    header.removeFromLeft (6);
    g.setFont (titleFont);
    g.drawText (title.toUpperCase(), header, juce::Justification::centredLeft);
}

void ParamSection::resized()
{
    juce::FlexBox flex;
    flex.flexWrap = juce::FlexBox::Wrap::wrap;
    flex.justifyContent = juce::FlexBox::JustifyContent::flexStart;
    flex.alignContent = juce::FlexBox::AlignContent::flexStart;
    const auto& m = metricsFor (size);

    for (auto& c : controls)
        flex.items.add (juce::FlexItem (*c).withWidth ((float) c->preferredWidth()).withHeight ((float) m.controlHeight));

    flex.performLayout (getLocalBounds().reduced (m.padding).withTrimmedTop (m.titleHeight - m.padding + 2));
}
