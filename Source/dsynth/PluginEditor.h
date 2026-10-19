#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

#include "common/ui/DustifyLookAndFeel.h"
#include "common/ui/SectionPage.h"

class DsynthAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit DsynthAudioProcessorEditor (DsynthAudioProcessor&);
    ~DsynthAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int designWidth = 1560, designHeight = 540;

private:
    void timerCallback() override;
    void layoutContent();
    void refreshPresetList();
    void presetChosen();
    void savePreset();

    DsynthAudioProcessor& processor;
    DustifyLookAndFeel lookAndFeel; // declared before every child, so it outlives them

    // Everything is laid out at the design size inside this component, which is scaled to the
    // window, so resizing zooms the whole UI evenly
    struct Content final : juce::Component
    {
        std::function<void()> onResize;
        void resized() override { if (onResize) onResize(); }
    } content;

    juce::Label title;
    juce::ComboBox presetBox;
    juce::TextButton saveButton { "Save..." };
    juce::TextButton prevButton { "<" }, nextButton { ">" };
    juce::TextButton editA { "Edit A" }, editB { "Edit B" }, copyLayer { "Copy A > B" };
    juce::Array<juce::File> userPresets;
    std::unique_ptr<juce::FileChooser> fileChooser;

    std::unique_ptr<SectionPage<dsynth::PID>> voicePage, modPage, effectsPage;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::MidiKeyboardComponent keyboard;
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DsynthAudioProcessorEditor)
};
