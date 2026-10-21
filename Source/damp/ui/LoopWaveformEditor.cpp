#include "LoopWaveformEditor.h"

#include "common/ui/DustifyLookAndFeel.h"

namespace
{
using P = DustifyLookAndFeel::Palette;
using namespace damp::loopedit;
using State = damp::Looper::State;

constexpr int binFrames = damp::Looper::overviewBinFrames;
constexpr double deepestZoom = 0.125;   // frames per pixel: 8 pixels a sample
constexpr int handleReach = 7;          // pixels either side of a handle that grab it
constexpr juce::uint32 askedTimeoutMs = 400; // after which the status wins over an edit not taken

juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }

// Diagonal lines over a region: the loop's padding (silence that costs no memory)
void hatch (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    if (r.getWidth() <= 0.0f)
        return;
    g.setColour (colour.withAlpha (0.10f));
    g.fillRect (r);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (r.toNearestIntEdges());
    g.setColour (colour.withAlpha (0.35f));
    const float step = 6.0f;
    const float start = r.getX() - r.getHeight();
    const float offset = std::fmod (start, step); // lines stay put as the region grows
    for (float x = start - offset; x < r.getRight(); x += step)
        g.drawLine (x, r.getBottom(), x + r.getHeight(), r.getY(), 1.0f);
}
} // namespace

// ======================================================================================

bool LoopOverview::refresh (damp::Looper& looper, int track)
{
    const auto v = looper.overviewVersion (track);
    const int bins = looper.overviewBins (track);
    if (known && v == version && bins == (int) mins.size())
        return false;
    known = true;
    version = v;
    mins.resize ((size_t) bins);
    maxs.resize ((size_t) bins);
    const int got = bins > 0 ? looper.readOverview (track, 0, bins, mins.data(), maxs.data()) : 0;
    mins.resize ((size_t) got);
    maxs.resize ((size_t) got);
    if (haveSamples && samplesVersion != v)
    {
        haveSamples = false;
        left.clear();
        right.clear();
    }
    return true;
}

void LoopOverview::fetchSamples (damp::Looper& looper, int track)
{
    if (haveSamples && samplesVersion == version)
        return;
    haveSamples = looper.copyTrackAudio (track, left, right);
    samplesVersion = version;
}

bool LoopOverview::range (double from, double to, float& lo, float& hi) const
{
    if (haveSamples && ! left.empty())
    {
        const auto n = (int64_t) left.size();
        auto a = (int64_t) std::floor (from), b = std::max (a + 1, (int64_t) std::ceil (to));
        a = std::max<int64_t> (a, 0);
        b = std::min (b, n);
        if (a >= b)
            return false;
        lo = hi = left[(size_t) a];
        for (auto i = a; i < b; ++i)
        {
            lo = std::min ({ lo, left[(size_t) i], right[(size_t) i] });
            hi = std::max ({ hi, left[(size_t) i], right[(size_t) i] });
        }
        return true;
    }
    const auto bins = (int64_t) mins.size();
    auto a = (int64_t) std::floor (from / binFrames), b = std::max (a + 1, (int64_t) std::ceil (to / binFrames));
    a = std::max<int64_t> (a, 0);
    b = std::min (b, bins);
    if (a >= b)
        return false;
    lo = mins[(size_t) a];
    hi = maxs[(size_t) a];
    for (auto i = a + 1; i < b; ++i)
    {
        lo = std::min (lo, mins[(size_t) i]);
        hi = std::max (hi, maxs[(size_t) i]);
    }
    return true;
}

void drawLoop (juce::Graphics& g, juce::Rectangle<int> area, const View& view, const LoopPicture& pic)
{
    const auto r = area.toFloat();
    auto xOf = [&] (double frame) { return r.getX() + (float) view.xAt (frame); };
    auto span = [&] (double from, double to)
    {
        const float a = juce::jlimit (r.getX(), r.getRight(), xOf (from)), b = juce::jlimit (r.getX(), r.getRight(), xOf (to));
        return juce::Rectangle<float> (a, r.getY(), b - a, r.getHeight());
    };

    // Padding: silence before the recording and after it
    if (pic.edits.start < 0)
        hatch (g, span ((double) pic.edits.start, 0.0), pic.colour);
    if (pic.edits.end > pic.recorded)
        hatch (g, span ((double) pic.recorded, (double) pic.edits.end), pic.colour);

    g.setColour (c (P::panelEdge));
    g.fillRect (span (0.0, (double) pic.recorded).withY (r.getCentreY()).withHeight (1.0f));

    // The waveform, a column per pixel; neighbouring columns join up when zoomed in past a sample
    if (pic.overview != nullptr && pic.recorded > 0)
    {
        const float mid = r.getCentreY(), half = r.getHeight() * 0.5f - 1.0f;
        g.setColour (pic.colour);
        float lastLo = 0.0f, lastHi = 0.0f;
        bool joined = false;
        const int x0 = std::max (0, (int) std::floor (view.xAt (0.0))), x1 = std::min (area.getWidth(), (int) std::ceil (view.xAt ((double) pic.recorded)) + 1);
        for (int x = x0; x < x1; ++x)
        {
            float lo = 0.0f, hi = 0.0f;
            if (! pic.overview->range (view.frameAt (x), view.frameAt (x + 1), lo, hi))
            {
                joined = false;
                continue;
            }
            float top = hi, bottom = lo;
            if (joined)
            {
                top = std::max (top, lastLo);
                bottom = std::min (bottom, lastHi);
            }
            lastLo = lo;
            lastHi = hi;
            joined = true;
            const float yTop = mid - juce::jlimit (-1.0f, 1.0f, top) * half;
            const float yBottom = mid - juce::jlimit (-1.0f, 1.0f, bottom) * half;
            g.fillRect ((float) area.getX() + (float) x, yTop, 1.0f, std::max (1.0f, yBottom - yTop));
        }
    }

    // Outside the loop: dimmed
    g.setColour (c (P::background).withAlpha (0.62f));
    g.fillRect (span (view.first - 1.0, (double) pic.edits.start));
    g.fillRect (span ((double) pic.edits.end, view.last() + 1.0));

    // The loop's ends, and the playhead
    g.setColour (pic.colour.brighter (0.3f));
    for (auto f : { pic.edits.start, pic.edits.end })
        if (const float x = xOf ((double) f); x >= r.getX() - 1.0f && x <= r.getRight() + 1.0f)
            g.fillRect (x - 0.5f, r.getY(), 1.0f, r.getHeight());
    if (pic.playhead >= 0.0)
        if (const float x = xOf (pic.playhead); x >= r.getX() && x <= r.getRight())
        {
            g.setColour (c (P::text));
            g.fillRect (x - 0.75f, r.getY(), 1.5f, r.getHeight());
        }
}

// ======================================================================================
// The waveform: handles, grid, playhead. Drag a handle to move it (beyond the recording adds
// silence), drag elsewhere to scroll, wheel to zoom, double-click to see everything.

class LoopWaveformEditor::Strip final : public juce::Component,
                                       public juce::SettableTooltipClient
{
public:
    explicit Strip (LoopWaveformEditor& e) : editor (e)
    {
        setComponentID ("looperWaveform");
        setTooltip ("Click to play from there; drag a handle to move it (past the recording adds silence), drag elsewhere to "
                    "scroll, wheel to zoom, double-click to see it all");
    }

    enum class Drag { None, Start, End, Scroll };
    Drag drag = Drag::None;

    juce::Rectangle<int> waveArea() const { return getLocalBounds().withTrimmedTop (rulerHeight).reduced (0, 2); }

    void paint (juce::Graphics& g) override
    {
        const auto& v = editor.view;
        const auto bounds = getLocalBounds();
        g.setColour (c (P::panel).darker (0.35f));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);

        const auto wave = waveArea();
        const auto& s = editor.status;
        const bool hasAudio = s.length > 0;
        const auto accent = s.state == State::Recording ? juce::Colour (0xffe5484d) : c (P::effects);

        // The beat grid from the recording's first frame (where Rec was pressed), bars brighter
        const double beat = editor.beatFrames();
        if (hasAudio && beat / v.framesPerPixel >= 6.0)
        {
            const auto first = (int64_t) std::floor (v.first / beat), last = (int64_t) std::ceil (v.last() / beat);
            const int bpb = editor.beatsPerBar();
            const bool labelBars = beat * bpb / v.framesPerPixel >= 28.0;
            g.setFont (juce::FontOptions (10.5f));
            for (auto k = first; k <= last; ++k)
            {
                const float x = (float) v.xAt ((double) k * beat);
                const bool bar = k % bpb == 0;
                g.setColour (c (P::panelEdge).withAlpha (bar ? 1.0f : 0.5f));
                g.fillRect (x, (float) wave.getY(), 1.0f, (float) wave.getHeight());
                if (bar && labelBars)
                {
                    g.setColour (c (P::dimText));
                    const auto barNumber = k / bpb + 1; // bar 1 starts at the first frame
                    g.drawText (juce::String (barNumber), juce::Rectangle<float> (x + 2.0f, 0.0f, 30.0f, (float) rulerHeight),
                                juce::Justification::centredLeft);
                }
            }
        }

        if (! hasAudio)
        {
            g.setColour (c (P::dimText));
            g.setFont (juce::FontOptions (13.0f));
            const bool on = editor.global.on && editor.global.poolReady;
            g.drawText (on ? (s.state == State::Recording ? "Recording..." : "Empty: press Rec to record on this track, or import a WAV")
                           : "Switch the looper on to record",
                        wave, juce::Justification::centred);
            return;
        }

        const auto edits = editor.shownEdits();
        const bool playing = s.state == State::Playing || s.state == State::Overdubbing;
        drawLoop (g, wave, v, { editor.overview, edits, s.length, accent, playing ? (double) s.sourcePosition : -1.0 });

        // Handles: a flag on the ruler, the one the nudges move filled
        for (const bool start : { true, false })
        {
            const float x = (float) v.xAt ((double) (start ? edits.start : edits.end));
            if (x < -8.0f || x > (float) getWidth() + 8.0f)
                continue;
            const bool target = start == editor.editingStart;
            const auto colour = start ? c (P::envelope) : c (P::source);
            g.setColour (colour);
            g.fillRect (x - 1.0f, (float) wave.getY(), 2.0f, (float) wave.getHeight());
            juce::Path flag;
            const float w = 9.0f, h = (float) rulerHeight;
            if (start)
                flag.addTriangle (x, 0.0f, x + w, h * 0.5f, x, h);
            else
                flag.addTriangle (x, 0.0f, x - w, h * 0.5f, x, h);
            if (target)
                g.fillPath (flag);
            else
                g.strokePath (flag, juce::PathStrokeType (1.2f));
        }

        if (s.state == State::Recording)
        {
            g.setColour (accent);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("REC", wave.reduced (6, 4), juce::Justification::topRight);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (handleAt (e.x) != Drag::None ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = handleAt (e.x);
        scrolling = false;
        if (drag == Drag::Start || drag == Drag::End)
        {
            editor.editingStart = drag == Drag::Start;
            editor.updateReadouts();
            repaint();
        }
        else if (editor.fitted)
        {
            drag = Drag::None; // nothing to scroll
        }
        else
        {
            drag = Drag::Scroll;
            scrollFrom = editor.view.first;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& v = editor.view;
        if (drag == Drag::Scroll)
        {
            if (! scrolling && isClick (e.getDistanceFromDragStartX(), e.getDistanceFromDragStartY()))
                return; // still a click (a seek) so far
            scrolling = true;
            v.first = scrollFrom - (double) e.getDistanceFromDragStartX() * v.framesPerPixel;
            v = constrain (v, domainFor (editor.shownEdits(), editor.status.length));
            editor.updateScrollBar();
            repaint();
            return;
        }
        if (drag == Drag::None)
            return;
        // Past the edges the mapping carries on, so padding can be dragged beyond the view
        const auto frame = (int64_t) std::llround (v.frameAt ((double) e.x));
        const auto e0 = editor.shownEdits();
        const bool start = drag == Drag::Start;
        const auto at = editor.snapped (start, frame, e0);
        editor.sendEdits (start ? withStart (e0, at, editor.limits()) : withEnd (e0, at, editor.limits()));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (drag != Drag::Start && drag != Drag::End && ! scrolling && editor.editable()
            && isClick (e.getDistanceFromDragStartX(), e.getDistanceFromDragStartY()))
            editor.seekAt ((double) e.x);
        if ((drag == Drag::Start || drag == Drag::End) && (Snap) editor.snapBox.getSelectedItemIndex() == Snap::ZeroCrossing)
        {
            const auto e0 = editor.shownEdits();
            if (drag == Drag::Start)
                editor.sendEdits (withStart (e0, editor.toZeroCrossing (e0.start), editor.limits()));
            else
                editor.sendEdits (withEnd (e0, editor.toZeroCrossing (e0.end), editor.limits()));
        }
        drag = Drag::None;
        if (editor.fitted)
            editor.refit(); // the loop may have grown past the view
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { editor.refit(); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (editor.status.length == 0)
            return;
        auto& v = editor.view;
        if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            const float delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
            v.first -= (double) delta * 0.6 * (double) v.width * v.framesPerPixel;
            v = constrain (v, domainFor (editor.shownEdits(), editor.status.length));
            editor.fitted = ! zoomedIn (v, domainFor (editor.shownEdits(), editor.status.length));
            editor.updateScrollBar();
            repaint();
            return;
        }
        editor.zoomBy (std::pow (2.0, (double) w.deltaY * 4.0), (double) e.x);
    }

    static constexpr int rulerHeight = 13;

private:
    Drag handleAt (int x) const
    {
        if (! editor.editable())
            return Drag::None;
        const auto e = editor.shownEdits();
        const double ds = std::abs (editor.view.xAt ((double) e.start) - x), de = std::abs (editor.view.xAt ((double) e.end) - x);
        if (std::min (ds, de) > handleReach)
            return Drag::Scroll;
        return ds <= de ? Drag::Start : Drag::End;
    }

    LoopWaveformEditor& editor;
    double scrollFrom = 0.0;
    bool scrolling = false;
};

// ======================================================================================

LoopWaveformEditor::LoopWaveformEditor (damp::Looper& l) : looper (l), strip (std::make_unique<Strip> (*this))
{
    addAndMakeVisible (*strip);
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    scrollBar.setColour (juce::ScrollBar::thumbColourId, c (P::dimText).withAlpha (0.6f));
    addAndMakeVisible (scrollBar);

    trackLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    trackLabel.setColour (juce::Label::textColourId, c (P::effects));
    addAndMakeVisible (trackLabel);

    for (auto* l2 : { &startLabel, &endLabel, &lengthLabel, &padLabel })
    {
        l2->setFont (juce::FontOptions (12.5f));
        l2->setColour (juce::Label::textColourId, c (P::dimText));
        l2->setBorderSize ({ 0, 2, 0, 2 });
        addAndMakeVisible (*l2);
    }
    startLabel.setColour (juce::Label::textColourId, c (P::envelope));
    endLabel.setColour (juce::Label::textColourId, c (P::source));

    snapBox.addItemList ({ "Snap: off", "Snap: zero", "Snap: beat", "Snap: bar" }, 1);
    snapBox.setSelectedItemIndex (0, juce::dontSendNotification);
    snapBox.setTooltip ("Where a dragged handle lands: anywhere, on the nearest zero crossing (when it's let go), or on the "
                        "tempo's beats or bars (the start from the first recorded frame, the end in whole beats or bars "
                        "after the start)");
    addAndMakeVisible (snapBox);

    for (auto* b : { &targetStart, &targetEnd })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        b->setColour (juce::TextButton::buttonOnColourId, (b == &targetStart ? c (P::envelope) : c (P::source)).withAlpha (0.45f));
        b->setTooltip ("Which end of the loop the nudge buttons move (clicking a handle picks it too)");
        addAndMakeVisible (*b);
    }
    targetStart.setComponentID ("looperTargetStart");
    targetEnd.setComponentID ("looperTargetEnd");
    targetStart.onClick = [this] { editingStart = true; updateReadouts(); strip->repaint(); };
    targetEnd.onClick = [this] { editingStart = false; updateReadouts(); strip->repaint(); };

    struct NudgeSpec { const char* text; int direction; Nudge size; const char* tip; };
    for (const NudgeSpec& n : { NudgeSpec { "-beat", -1, Nudge::Beat, "Earlier by a beat of the tempo" },
                                NudgeSpec { "-10ms", -1, Nudge::Ms10, "Earlier by 10 ms" },
                                NudgeSpec { "-1ms", -1, Nudge::Ms1, "Earlier by 1 ms" },
                                NudgeSpec { "+1ms", 1, Nudge::Ms1, "Later by 1 ms" },
                                NudgeSpec { "+10ms", 1, Nudge::Ms10, "Later by 10 ms" },
                                NudgeSpec { "+beat", 1, Nudge::Beat, "Later by a beat of the tempo" } })
    {
        auto* b = nudgeButtons.add (new juce::TextButton (n.text));
        b->setTooltip (juce::String (n.tip) + ". Before the recording's first frame or after its last adds silence.");
        b->onClick = [this, d = n.direction, s = n.size] { nudge (d, s); };
        addAndMakeVisible (b);
    }

    zoomIn.setComponentID ("looperZoomIn");
    zoomOut.setComponentID ("looperZoomOut");
    zoomFit.setComponentID ("looperZoomFit");
    zoomIn.setTooltip ("Zoom in on the handle the nudges move (or use the mouse wheel over the waveform)");
    zoomOut.setTooltip ("Zoom out");
    zoomFit.setTooltip ("Show the whole recording and loop (or double-click the waveform)");
    zoomIn.onClick = [this] { zoomBy (2.0); };
    zoomOut.onClick = [this] { zoomBy (0.5); };
    zoomFit.onClick = [this] { refit(); };
    for (auto* b : { &zoomIn, &zoomOut, &zoomFit })
        addAndMakeVisible (*b);

    seamButton.setComponentID ("looperHearSeam");
    seamButton.setTooltip ("Play from just before the end handle, to hear the end run into the start");
    seamButton.onClick = [this] { hearSeam(); };
    addAndMakeVisible (seamButton);
    preRoll.addItemList ({ "0.5 s", "1 s", "1.5 s", "2 s" }, 1);
    preRoll.setSelectedItemIndex (2, juce::dontSendNotification);
    preRoll.setTooltip ("How long before the end Hear seam starts (at most half the loop)");
    addAndMakeVisible (preRoll);

    crossfadeLabel.setText ("Xfade", juce::dontSendNotification);
    crossfadeLabel.setFont (juce::FontOptions (12.5f));
    crossfadeLabel.setColour (juce::Label::textColourId, c (P::dimText));
    crossfadeLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (crossfadeLabel);
    crossfade.setSliderStyle (juce::Slider::LinearBar);
    crossfade.setRange (0.0, (double) damp::Looper::maxCrossfadeMs, 0.5);
    crossfade.setTextValueSuffix (" ms");
    crossfade.setNumDecimalPlacesToDisplay (1);
    crossfade.setColour (juce::Slider::trackColourId, c (P::effects).withAlpha (0.45f));
    crossfade.setColour (juce::Slider::backgroundColourId, c (P::control));
    crossfade.setTooltip ("The crossfade across the loop's seam (end into start), 0-50 ms");
    crossfade.onValueChange = [this] { looper.setTrackCrossfade (track, (float) crossfade.getValue()); };
    addAndMakeVisible (crossfade);

    targetStart.setToggleState (true, juce::dontSendNotification);
    updateReadouts();
}

LoopWaveformEditor::~LoopWaveformEditor()
{
    scrollBar.removeListener (this);
}

damp::loopedit::Edits LoopWaveformEditor::shownEdits() const
{
    return asked.value_or (Edits { status.start, status.end });
}

damp::loopedit::Limits LoopWaveformEditor::limits() const
{
    return { status.length, (int64_t) std::llround (global.poolTotalSeconds * global.sampleRate), damp::Looper::minLoop };
}

bool LoopWaveformEditor::editable() const
{
    return status.length > 0 && status.state != State::Recording && status.state != State::Empty;
}

int LoopWaveformEditor::beatsPerBar() const
{
    return std::max (1, global.beatsPerBar);
}

double LoopWaveformEditor::beatFrames() const
{
    return framesPerBeat (global.tempo, global.sampleRate > 0.0 ? global.sampleRate : 48000.0);
}

void LoopWaveformEditor::sendEdits (Edits e)
{
    if (! editable())
        return;
    e = clamp (e, limits());
    asked = e;
    askedAt = juce::Time::getMillisecondCounter();
    looper.setTrackEdits (track, e.start, e.end, status.front); // follows the recording should an overdub renumber it
    updateReadouts();
    strip->repaint();
}

void LoopWaveformEditor::nudge (int direction, Nudge size)
{
    const auto e = shownEdits();
    const auto n = direction * nudgeFrames (size, global.sampleRate > 0.0 ? global.sampleRate : 48000.0, global.tempo);
    sendEdits (editingStart ? withStart (e, e.start + n, limits()) : withEnd (e, e.end + n, limits()));
    if (fitted)
        refit();
    else
        updateScrollBar();
}

void LoopWaveformEditor::seekAt (double x)
{
    if (const auto offset = seekOffset (view.frameAt (x), shownEdits()))
        looper.seek (track, *offset);
}

void LoopWaveformEditor::hearSeam()
{
    static constexpr double preRolls[] { 0.5, 1.0, 1.5, 2.0 };
    const auto rate = global.sampleRate > 0.0 ? global.sampleRate : 48000.0;
    looper.seek (track, seamOffset (shownEdits(), rate, preRolls[juce::jlimit (0, 3, preRoll.getSelectedItemIndex())]));
}

int64_t LoopWaveformEditor::snapped (bool start, int64_t frame, const Edits& e) const
{
    const double beat = beatFrames();
    switch ((Snap) snapBox.getSelectedItemIndex())
    {
        case Snap::Beat: return start ? snapStart (frame, beat) : snapEnd (frame, e.start, beat);
        case Snap::Bar: return start ? snapStart (frame, beatsPerBar() * beat) : snapEnd (frame, e.start, beatsPerBar() * beat);
        case Snap::Off: case Snap::ZeroCrossing: break;
    }
    return frame;
}

int64_t LoopWaveformEditor::toZeroCrossing (int64_t frame)
{
    if (frame <= 0 || frame >= status.length)
        return frame; // in the padding: silence everywhere
    const auto radius = (int64_t) std::llround ((global.sampleRate > 0.0 ? global.sampleRate : 48000.0) * 0.005);
    return looper.nearestZeroCrossing (track, frame, radius);
}

void LoopWaveformEditor::zoomBy (double factor, std::optional<double> anchorX)
{
    if (status.length == 0)
        return;
    const auto domain = domainFor (shownEdits(), status.length);
    if (! anchorX.has_value())
    {
        // About the handle the nudges move, if it's in view
        const auto e = shownEdits();
        const double x = view.xAt ((double) (editingStart ? e.start : e.end));
        anchorX = x >= 0.0 && x <= (double) view.width ? x : (double) view.width * 0.5;
    }
    view = zoom (view, factor, *anchorX, domain, deepestZoom);
    fitted = ! zoomedIn (view, domain);
    fetchDetail();
    updateScrollBar();
    strip->repaint();
}

void LoopWaveformEditor::fetchDetail()
{
    if (overview != nullptr && view.framesPerPixel < binFrames * 0.5 && status.length > 0 && status.state != State::Recording
        && status.state != State::Overdubbing)
        overview->fetchSamples (looper, track);
}

void LoopWaveformEditor::refit()
{
    fitted = true;
    view = fit (domainFor (shownEdits(), status.length), strip->waveArea().getWidth());
    updateScrollBar();
    strip->repaint();
}

void LoopWaveformEditor::updateScrollBar()
{
    const auto domain = domainFor (shownEdits(), status.length);
    scrollBar.setRangeLimits (domain.first, domain.last, juce::dontSendNotification);
    scrollBar.setCurrentRange (view.first, view.framesPerPixel * (double) view.width, juce::dontSendNotification);
    scrollBar.setVisible (! fitted && status.length > 0);
}

void LoopWaveformEditor::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    view.first = newRangeStart;
    strip->repaint();
}

void LoopWaveformEditor::updateReadouts()
{
    targetStart.setToggleState (editingStart, juce::dontSendNotification);
    targetEnd.setToggleState (! editingStart, juce::dontSendNotification);
    const bool canEdit = editable();
    for (auto* b : nudgeButtons)
        b->setEnabled (canEdit);
    for (auto* comp : std::initializer_list<juce::Component*> { &targetStart, &targetEnd, &crossfade, &snapBox, &seamButton, &preRoll })
        comp->setEnabled (canEdit);
    for (auto* b : { &zoomIn, &zoomOut, &zoomFit })
        b->setEnabled (status.length > 0);

    trackLabel.setText ("Track " + juce::String (track + 1), juce::dontSendNotification);
    if (status.length == 0)
    {
        for (auto* l : { &startLabel, &endLabel, &lengthLabel, &padLabel })
            l->setText ({}, juce::dontSendNotification);
        return;
    }
    const double rate = global.sampleRate > 0.0 ? global.sampleRate : 48000.0;
    const auto e = shownEdits();
    auto both = [&] (int64_t frames)
    {
        return juce::String (toMs (frames, rate), 1) + " ms  (" + juce::String (toBeats (frames, rate, global.tempo), 2) + " beats)";
    };
    startLabel.setText ((editingStart ? "> Start " : "Start ") + both (e.start), juce::dontSendNotification);
    endLabel.setText ((editingStart ? "End " : "> End ") + both (e.end), juce::dontSendNotification);
    lengthLabel.setText ("Loop " + both (e.length()), juce::dontSendNotification);
    const auto before = padBefore (e), after = padAfter (e, status.length);
    padLabel.setText (before == 0 && after == 0 ? juce::String ("No padding")
                                                : "Padding " + juce::String (toMs (before, rate), 1) + " / " + juce::String (toMs (after, rate), 1) + " ms",
                      juce::dontSendNotification);
}

void LoopWaveformEditor::update (int t, const damp::Looper::TrackStatus& s, const damp::Looper::Status& g, LoopOverview& ov)
{
    const bool trackChanged = t != track || overview != &ov;
    const auto oldEdits = shownEdits();
    const auto oldLength = status.length;
    const auto oldSource = status.sourcePosition;
    const auto oldState = status.state;
    const auto oldVersion = overview != nullptr ? overview->version : 0;
    const auto oldFront = status.front;

    track = t;
    status = s;
    global = g;
    overview = &ov;
    if (trackChanged)
    {
        asked.reset();
        fitted = true;
    }
    else if (s.front != oldFront)
    {
        // An overdub grew the recording at the front, renumbering its frames: the view and an edit
        // not yet taken follow them (so nothing on screen moves)
        const auto delta = s.front - oldFront;
        view.first += (double) delta;
        if (asked.has_value())
        {
            asked->start += delta;
            asked->end += delta;
        }
    }

    // An edit asked for shows until the engine has it (or plainly didn't take it)
    if (asked.has_value() && strip->drag != Strip::Drag::Start && strip->drag != Strip::Drag::End
        && (*asked == Edits { s.start, s.end } || juce::Time::getMillisecondCounter() - askedAt > askedTimeoutMs))
        asked.reset();

    const auto edits = shownEdits();
    const bool changed = trackChanged || edits != oldEdits || s.length != oldLength || s.sourcePosition != oldSource
                         || s.state != oldState || ov.version != oldVersion;
    const bool dragging = strip->drag == Strip::Drag::Start || strip->drag == Strip::Drag::End;
    if (fitted && ! dragging && (trackChanged || edits != oldEdits || s.length != oldLength))
        refit();
    else if (! fitted && s.length == 0)
        refit();

    fetchDetail();

    if (! crossfade.isMouseButtonDown())
        crossfade.setValue (s.crossfadeMs, juce::dontSendNotification);
    if (changed)
    {
        updateReadouts();
        strip->repaint();
    }
}

void LoopWaveformEditor::paint (juce::Graphics& g)
{
    g.setColour (c (P::panel));
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
}

void LoopWaveformEditor::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto bar = area.removeFromTop (24);
    trackLabel.setBounds (bar.removeFromLeft (76));
    snapBox.setBounds (bar.removeFromLeft (112));
    bar.removeFromLeft (14);
    targetStart.setBounds (bar.removeFromLeft (46));
    targetEnd.setBounds (bar.removeFromLeft (40));
    bar.removeFromLeft (6);
    for (auto* b : nudgeButtons)
    {
        b->setBounds (bar.removeFromLeft (b->getButtonText().contains ("beat") ? 50 : 48));
        bar.removeFromLeft (2);
    }
    zoomFit.setBounds (bar.removeFromRight (38));
    bar.removeFromRight (2);
    zoomIn.setBounds (bar.removeFromRight (26));
    bar.removeFromRight (2);
    zoomOut.setBounds (bar.removeFromRight (26));
    bar.removeFromRight (14);
    crossfade.setBounds (bar.removeFromRight (84));
    crossfadeLabel.setBounds (bar.removeFromRight (40));
    bar.removeFromLeft (10);
    seamButton.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (2);
    preRoll.setBounds (bar.removeFromLeft (66));

    area.removeFromTop (5);
    auto readouts = area.removeFromBottom (16);
    const int w = readouts.getWidth() / 4;
    startLabel.setBounds (readouts.removeFromLeft (w));
    endLabel.setBounds (readouts.removeFromLeft (w));
    lengthLabel.setBounds (readouts.removeFromLeft (w));
    padLabel.setBounds (readouts);
    area.removeFromBottom (2);
    scrollBar.setBounds (area.removeFromBottom (9));
    area.removeFromBottom (1);
    strip->setBounds (area);

    const int width = strip->waveArea().getWidth();
    if (fitted || width != view.width)
    {
        view.width = std::max (1, width);
        refit();
    }
}
