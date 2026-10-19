#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "common/plugin/PresetHost.h"

// Preset menu (factory presets under category headings, then the user's), previous / next
// buttons and Save... Follows presets loaded from elsewhere (host program changes, sessions).
class PresetBar final : public juce::Component,
                        private juce::Timer
{
public:
    explicit PresetBar (dustify::PresetHost&);

    void resized() override;

private:
    void timerCallback() override;
    void refreshList();
    void chosen();
    void save();

    dustify::PresetHost& host;
    juce::ComboBox box;
    juce::TextButton saveButton { "Save..." }, prevButton { "<" }, nextButton { ">" };
    juce::Array<juce::File> userPresets;
    std::unique_ptr<juce::FileChooser> fileChooser;
};
