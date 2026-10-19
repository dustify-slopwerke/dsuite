#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/PitchDetector.h"

#include <optional>

class DampAudioProcessor;

// Pedal-style chromatic tuner: note name, a cents scale with a needle, the frequency, and the
// mute and A4 settings. Pulls the processor's tuner feed and analyses it on the message thread.
class TunerView final : public juce::Component
{
public:
    explicit TunerView (DampAudioProcessor&);
    ~TunerView() override;

    // Call from the editor's timer while visible
    void update (float secondsSinceLast);

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    DampAudioProcessor& processor;
    std::optional<damp::PitchDetector> detector;
    std::vector<float> history; // the latest window of the feed, oldest first
    std::vector<float> incoming;

    double shownCents = 0.0, shownHz = 0.0;
    int shownNote = -1;
    float sinceReading = 10.0f; // seconds since the last confident reading

    juce::ToggleButton mute;
    juce::Label muteLabel;
    juce::Slider reference { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label referenceLabel;
};
