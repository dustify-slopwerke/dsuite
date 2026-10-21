#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/VelocityMap.h"

class DpianoAudioProcessor;

// The Voicing tab's "Keyboard" section: velocity calibration for the player's MIDI keyboard.
// Calibrate captures the softest and hardest note-on velocities and maps that range onto the
// full one (stored machine-wide and in the session); Sensitivity bends the curve on top. A meter
// shows the last note's velocity as played and as mapped, over the calibrated range.
class KeyboardPanel final : public juce::Component,
                            private juce::Timer
{
public:
    explicit KeyboardPanel (DpianoAudioProcessor&);
    ~KeyboardPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr double autoFinishSeconds = 15.0; // calibration ends this long after the last new extreme

    void timerCallback() override; // public so the snapshot tool can run it once

private:
    void startCalibration();
    void finishCalibration();
    void refreshMeter();
    void updateStatus();

    DpianoAudioProcessor& processor;
    juce::TextButton calibrate { "Calibrate" }, reset { "Reset" };
    juce::Slider sensitivity;
    juce::Label sensitivityLabel, status;

    bool capturing = false;
    dpiano::VelocityCapture capture;
    double lastExtremeTime = 0.0;
    juce::String result; // the outcome of the last calibration, shown until the next one
    int seenNotes = -1;
    float played = -1.0f, mapped = -1.0f; // the last note, or -1 before any
    double sensitivityChangedAt = -1.0;   // saved machine-wide once it settles

    juce::Rectangle<int> meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KeyboardPanel)
};
