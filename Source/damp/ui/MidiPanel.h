#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "dsp/MidiControl.h"

class DampAudioProcessor;

// The MIDI tab: the channel, whether Program Change picks presets, the mappings (source, what it
// controls, momentary / latching for switches, remove), Learn... for any target (every command
// and parameter, from a menu), and the last message received.
class MidiPanel final : public juce::Component,
                        private juce::ListBoxModel
{
public:
    explicit MidiPanel (DampAudioProcessor&);

    // From the editor's timer
    void update();

    void paint (juce::Graphics&) override;
    void resized() override;

    // Every target, in a menu (item ids 1 + index into `targets`)
    static void addTargetMenu (juce::PopupMenu&, std::vector<damp::midi::Target>& targets);

private:
    int getNumRows() override { return (int) shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void chooseLearnTarget();
    void changed();

    DampAudioProcessor& processor;
    damp::midi::MidiControl& midi;
    juce::Label channelLabel, programLabel, status, lastMessage, help;
    juce::ComboBox channel;
    juce::ToggleButton programChange;
    juce::TextButton learn { "Learn..." }, cancel { "Cancel" };
    juce::ListBox list { "mappings", this };
    std::vector<damp::midi::Mapping> shown;
    int versionShown = -1;
    uint32_t activityShown = 0;
    std::vector<damp::midi::Target> menuTargets;
};

// Learn mode: over the editor, outlining every control MIDI can drive (with the source already
// mapped to it). Click one to learn it (the next CC or note maps to it); right-click to forget it
// or set its switch mode. Clicks elsewhere go through to the editor (tabs, scrolling).
class MidiLearnOverlay final : public juce::Component
{
public:
    MidiLearnOverlay (DampAudioProcessor&, juce::Component& content);

    // From the editor's timer: finds the controls again (tabs, scrolling, folding move them)
    void update();

    bool hitTest (int x, int y) override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Spot
    {
        juce::Rectangle<int> bounds;
        juce::Component::SafePointer<juce::Component> component;
        std::vector<damp::midi::Target> targets;
    };
    void collect (juce::Component&, juce::Rectangle<int> clip);
    const Spot* spotAt (juce::Point<int>) const;
    void showMenu (const Spot&);

    DampAudioProcessor& processor;
    juce::Component& content;
    std::vector<Spot> spots;
    std::vector<damp::midi::Target> menuTargets;
};
