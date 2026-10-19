#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

#include "common/ui/DustifyLookAndFeel.h"
#include "common/ui/PresetBar.h"
#include "common/ui/SectionPage.h"

class DpianoAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit DpianoAudioProcessorEditor (DpianoAudioProcessor&);
    ~DpianoAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int designWidth = 1240, designHeight = 550;

private:
    void layoutContent();

    DpianoAudioProcessor& processor;
    DustifyLookAndFeel lookAndFeel; // declared before every child, so it outlives them

    // Everything is laid out at the design size inside this component, which is scaled to the
    // window, so resizing zooms the whole UI evenly
    struct Content final : juce::Component
    {
        std::function<void()> onResize;
        void resized() override { if (onResize) onResize(); }
    } content;

    juce::Label title;
    PresetBar presets;
    std::unique_ptr<SectionPage<dpiano::PID>> mainPage, voicingPage;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::MidiKeyboardComponent keyboard;
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DpianoAudioProcessorEditor)
};
