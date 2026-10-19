#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"

#include "ui/LooperView.h"
#include "ui/PedalboardView.h"
#include "ui/TunerView.h"

#include "common/ui/DustifyLookAndFeel.h"
#include "common/ui/LevelMeter.h"
#include "common/ui/PresetBar.h"
#include "common/ui/SectionPage.h"

class DampAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                       public juce::DragAndDropContainer,
                                       private juce::Timer
{
public:
    explicit DampAudioProcessorEditor (DampAudioProcessor&);
    ~DampAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int designWidth = 1048, designHeight = 452;

private:
    void layoutContent();
    void timerCallback() override;
    void chooseImpulse();
    void updateRelevance();

    DampAudioProcessor& processor;
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
    std::unique_ptr<SectionPage<damp::PID>> page;
    juce::Component ampTab;
    juce::Viewport pedalViewport;
    PedalboardView pedalboard;
    LooperView looperView;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::TextButton loadImpulse { "Load IR..." };
    juce::Label impulseName;
    juce::TextButton tunerButton { "Tuner" };
    LevelMeter inputMeter { "IN" }, outputMeter { "OUT" };
    TunerView tuner;
    juce::uint32 lastTick = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DampAudioProcessorEditor)
};
