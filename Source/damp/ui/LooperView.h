#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "LoopWaveformEditor.h"

#include <array>

class DampAudioProcessor;

// The looper tab: the transport across the top (on switch, Rec / Overdub / Play / Stop, Undo /
// Redo / Clear, tempo and position), a row per track (state, a strip of its loop, level, mute,
// reverse, half speed, sync, play / stop, WAV export and import), and the selected track's
// waveform editor below. Buttons act through Looper::press; the per-track switches and levels
// are their parameters. Everything shown is polled from the engine's published status.
class LooperView final : public juce::Component
{
public:
    explicit LooperView (DampAudioProcessor&);
    ~LooperView() override;

    // From the editor's timer
    void update();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class TrackRow;
    class TransportButton;

    void select (int track);
    void showWavMenu (int track, juce::Component& from);
    void exportTrack (int track, bool asPlayed);
    void importTrack (int track);
    void updateTransport();

    DampAudioProcessor& processor;
    damp::Looper& looper;

    juce::ToggleButton onSwitch { "Looper" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    juce::Label title, poolLabel, infoLabel;
    std::unique_ptr<TransportButton> rec, dub, play, stop;
    juce::TextButton undo { "Undo" }, redo { "Redo" }, clear { "Clear" };
    juce::OwnedArray<TrackRow> rows;
    LoopWaveformEditor editor;

    std::unique_ptr<juce::ParameterAttachment> selectedAttachment;
    int selected = 0;
    std::array<LoopOverview, damp::Looper::numTracks> overviews;
    std::array<damp::Looper::TrackStatus, damp::Looper::numTracks> statuses;
    damp::Looper::Status global;
    int poolFullSeen = -1;
    juce::uint32 poolFullAt = 0;
    std::unique_ptr<juce::FileChooser> chooser;
};
