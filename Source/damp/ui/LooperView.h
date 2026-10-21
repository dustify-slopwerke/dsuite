#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "LoopWaveformEditor.h"
#include "dsp/looper/TempoGrid.h"

#include <array>

class DampAudioProcessor;

// The looper tab: the transport across the top (on switch, Rec / Overdub / Play / Stop, Undo /
// Redo / Clear, the loop's length and position), the tempo and metronome row (BPM, tap, follow
// host, tempo from the loop, quantise, click, count-in, beat dots), a row per track (state, a strip of its loop, level, mute,
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
    class BeatIndicator;

    void select (int track);
    void showWavMenu (int track, juce::Component& from);
    void exportTrack (int track, bool asPlayed);
    void importTrack (int track);
    void updateTransport();
    void updateTempo();
    void setTempo (double bpm);
    void tapTempo();
    void tempoFromLoop();

    DampAudioProcessor& processor;
    damp::Looper& looper;

    juce::ToggleButton onSwitch { "Looper" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttachment;
    juce::Label title, poolLabel, infoLabel;
    std::unique_ptr<TransportButton> rec, dub, play, stop;
    juce::TextButton undo { "Undo" }, redo { "Redo" }, clear { "Clear" };

    // Tempo and metronome
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    juce::Label tempoLabel, hostTempo;
    juce::Slider bpm, clickLevel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttachment, clickLevelAttachment;
    juce::TextButton tap { "Tap" }, fromLoop { "From loop" };
    juce::TextButton followHost { "Follow host" }, quantise { "Quantise" }, clickOn { "Click" }, accent { "Accent" },
        countIn { "Count-in" }, recOnly { "Rec only" };
    juce::ComboBox beats;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> beatsAttachment;
    std::vector<std::unique_ptr<ButtonAttachment>> tempoAttachments;
    std::unique_ptr<BeatIndicator> beatIndicator;
    damp::tempo::TapTempo tapper;

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
