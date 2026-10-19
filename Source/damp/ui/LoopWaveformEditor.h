#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "LoopEditMath.h"
#include "dsp/looper/Looper.h"

#include <optional>
#include <vector>

// A track's waveform overview as the UI keeps it: re-read from the engine only when its version
// moves. Deep zoom draws real samples, copied (once per version) when it's first needed.
struct LoopOverview
{
    // True when the overview changed
    bool refresh (damp::Looper&, int track);
    // Lowest and highest sample over recording frames [from, to): from the samples if they're
    // here, else from the overview's bins. False when there's no audio there.
    bool range (double from, double to, float& lo, float& hi) const;
    // Fetches the samples (message thread; waits for a block) unless they're current
    void fetchSamples (damp::Looper&, int track);
    bool hasSamples() const { return haveSamples; }
    int64_t frames() const { return (int64_t) mins.size() * damp::Looper::overviewBinFrames; }

    uint32_t version = 0;
    bool known = false;
    std::vector<float> mins, maxs; // per bin of Looper::overviewBinFrames
    bool haveSamples = false;
    uint32_t samplesVersion = 0;
    std::vector<float> left, right;
};

// Draws a loop the same way everywhere (the track rows' strips and the editor): the recording's
// waveform, bright inside [start, end) and dim outside it, padding hatched, and the playhead
struct LoopPicture
{
    const LoopOverview* overview = nullptr;
    damp::loopedit::Edits edits;
    int64_t recorded = 0;
    juce::Colour colour;
    double playhead = -1.0; // recording frame, < 0: none
};
void drawLoop (juce::Graphics&, juce::Rectangle<int> area, const damp::loopedit::View&, const LoopPicture&);

// The selected track's editor: its waveform with draggable start and end handles, zoom (wheel,
// buttons) and scroll, nudges, snapping, the seam crossfade and the numbers. Edits go to the
// engine as they're made (it applies them non-destructively); the handles show what was asked
// until the engine's status catches up.
class LoopWaveformEditor final : public juce::Component,
                                 private juce::ScrollBar::Listener
{
public:
    explicit LoopWaveformEditor (damp::Looper&);
    ~LoopWaveformEditor() override;

    // Polled with the selected track's state
    void update (int track, const damp::Looper::TrackStatus&, const damp::Looper::Status&, LoopOverview&);

    void paint (juce::Graphics&) override;
    void resized() override;

    enum class Snap { Off, ZeroCrossing, Beat, Bar };

private:
    class Strip;

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    damp::loopedit::Edits shownEdits() const;
    damp::loopedit::Limits limits() const;
    bool editable() const;
    void sendEdits (damp::loopedit::Edits);
    void nudge (int direction, damp::loopedit::Nudge);
    void seekAt (double x); // plays from the loop frame under x
    void hearSeam();
    void zoomBy (double factor, std::optional<double> anchorX = {});
    void refit();
    void fetchDetail(); // the samples themselves, when zoomed in past the overview's bins
    void updateScrollBar();
    void updateReadouts();
    // Snapping for a dragged handle (zero crossings are found when it's let go)
    int64_t snapped (bool start, int64_t frame, const damp::loopedit::Edits&) const;
    int64_t toZeroCrossing (int64_t frame);
    double beatFrames() const;

    damp::Looper& looper;
    std::unique_ptr<Strip> strip;
    juce::ScrollBar scrollBar { false };
    juce::Label trackLabel, startLabel, endLabel, lengthLabel, padLabel;
    juce::ComboBox snapBox;
    juce::TextButton targetStart { "Start" }, targetEnd { "End" };
    juce::OwnedArray<juce::TextButton> nudgeButtons;
    juce::TextButton zoomIn { "+" }, zoomOut { "-" }, zoomFit { "Fit" };
    juce::TextButton seamButton { "Hear seam" };
    juce::ComboBox preRoll;
    juce::Label crossfadeLabel;
    juce::Slider crossfade;

    int track = 0;
    damp::Looper::TrackStatus status;
    damp::Looper::Status global;
    LoopOverview* overview = nullptr;
    damp::loopedit::View view;
    bool fitted = true; // follow the domain (until the user zooms or scrolls)
    std::optional<damp::loopedit::Edits> asked; // sent, not yet in the status
    juce::uint32 askedAt = 0;
    bool editingStart = true; // which handle the nudges move

    friend class Strip;
};
