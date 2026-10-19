#include "LooperView.h"

#include "PluginProcessor.h"
#include "common/plugin/ParamLayout.h"
#include "common/ui/DustifyLookAndFeel.h"

namespace
{
using P = DustifyLookAndFeel::Palette;
using State = damp::Looper::State;
using Action = damp::Looper::Action;

constexpr int transportHeight = 34, rowHeight = 30, rowGap = 4, gap = 6;
constexpr juce::uint32 recordRed = 0xffe5484d;

juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }

juce::Colour stateColour (State s)
{
    switch (s)
    {
        case State::Recording: return c (recordRed);
        case State::Playing: return c (P::envelope);
        case State::Overdubbing: return c (P::source);
        case State::Stopped: return c (P::global);
        case State::Empty: break;
    }
    return c (P::control);
}

const char* stateName (State s)
{
    switch (s)
    {
        case State::Recording: return "recording";
        case State::Playing: return "playing";
        case State::Overdubbing: return "overdubbing";
        case State::Stopped: return "stopped";
        case State::Empty: break;
    }
    return "empty";
}

bool active (State s) { return s == State::Recording || s == State::Playing || s == State::Overdubbing; }

juce::String paramId (damp::PID pid) { return dustify::toJuce (damp::paramInfo (pid).id); }
} // namespace

// ======================================================================================
// A transport button: a glyph (record dot, play triangle, stop square) and its name, lit in its
// colour while that's what the looper is doing

class LooperView::TransportButton final : public juce::Button
{
public:
    enum class Glyph { Rec, Dub, Play, Stop };

    TransportButton (const juce::String& name, Glyph g, juce::Colour colour) : juce::Button (name), glyph (g), litColour (colour)
    {
        setComponentID ("looper" + name.removeCharacters (" "));
    }

    void setGlyph (Glyph g)
    {
        if (g != glyph)
        {
            glyph = g;
            repaint();
        }
    }

    void setLit (bool on)
    {
        if (on != lit)
        {
            lit = on;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto fill = lit ? litColour.withAlpha (0.55f) : c (P::control);
        getLookAndFeel().drawButtonBackground (g, *this, fill, highlighted && isEnabled(), down);

        const float alpha = isEnabled() ? 1.0f : 0.35f;
        auto r = getLocalBounds().toFloat().reduced (6.0f, 4.0f);
        const bool withText = getWidth() > 40;
        const float s = std::min (r.getHeight(), 14.0f) * 0.75f;
        auto glyphArea = (withText ? r.removeFromLeft (s + 2.0f) : r).withSizeKeepingCentre (s, s);
        const auto glyphColour = (lit ? c (P::text) : litColour).withMultipliedAlpha (alpha);
        g.setColour (glyphColour);
        switch (glyph)
        {
            case Glyph::Rec: g.fillEllipse (glyphArea); break;
            case Glyph::Dub:
                g.fillEllipse (glyphArea.reduced (s * 0.18f).translated (-s * 0.18f, 0.0f));
                g.drawEllipse (glyphArea.reduced (s * 0.12f).translated (s * 0.2f, 0.0f), 1.2f);
                break;
            case Glyph::Play:
            {
                juce::Path p;
                p.addTriangle (glyphArea.getTopLeft(), glyphArea.getBottomLeft(), { glyphArea.getRight(), glyphArea.getCentreY() });
                g.fillPath (p);
                break;
            }
            case Glyph::Stop: g.fillRect (glyphArea.reduced (s * 0.06f)); break;
        }
        if (withText)
        {
            g.setColour (c (P::text).withMultipliedAlpha (alpha));
            g.setFont (juce::FontOptions (13.5f, lit ? juce::Font::bold : juce::Font::plain));
            g.drawText (getButtonText(), r.withTrimmedLeft (5.0f), juce::Justification::centredLeft);
        }
    }

private:
    Glyph glyph;
    juce::Colour litColour;
    bool lit = false;
};

// ======================================================================================
// One track: its number in its state's colour, a strip of its loop, level, the switches, play /
// stop and the WAV menu. Clicking the row selects the track.

class LooperView::TrackRow final : public juce::Component,
                                   public juce::SettableTooltipClient
{
public:
    TrackRow (LooperView& v, int t) : owner (v), track (t), playStop ("Play", TransportButton::Glyph::Play, c (P::envelope))
    {
        auto& apvts = owner.processor.apvts;
        using TP = damp::TrackParam;
        auto id = [t] (TP p) { return paramId (damp::trackParam (t, p)); };

        level.setSliderStyle (juce::Slider::LinearBar);
        level.setColour (juce::Slider::trackColourId, c (P::effects).withAlpha (0.4f));
        level.setColour (juce::Slider::backgroundColourId, c (P::control));
        level.setTooltip ("Track " + juce::String (t + 1) + " level");
        addAndMakeVisible (level);
        levelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id (TP::Level), level);

        struct Switch { juce::TextButton* button; TP param; juce::uint32 colour; const char* tip; };
        for (const Switch& s : { Switch { &mute, TP::Mute, recordRed, "Mute" },
                                 Switch { &reverse, TP::Reverse, P::modulation, "Play backwards (no overdubs while it's on)" },
                                 Switch { &half, TP::Half, P::modulation, "Half speed: an octave down and twice as long (no overdubs while it's on)" },
                                 Switch { &sync, TP::Sync, P::envelope, "Sync: locked to the first track's loop (its length rounded to a multiple of it); off: loops its own length freely" } })
        {
            s.button->setClickingTogglesState (true);
            s.button->setColour (juce::TextButton::buttonOnColourId, c (s.colour).withAlpha (0.5f));
            s.button->setTooltip (s.tip);
            s.button->setComponentID ("looperTrack" + juce::String (t + 1) + s.button->getButtonText());
            addAndMakeVisible (*s.button);
            attachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id (s.param), *s.button));
        }

        playStop.setComponentID ("looperTrack" + juce::String (t + 1) + "PlayStop");
        playStop.setTooltip ("Play or stop this track");
        playStop.onClick = [this]
        {
            const auto s = owner.statuses[(size_t) track].state;
            owner.looper.press (s == State::Stopped ? Action::Play : Action::Stop, track);
        };
        addAndMakeVisible (playStop);

        wav.setTooltip ("Export this track as a WAV file, or import one");
        wav.setComponentID ("looperTrack" + juce::String (t + 1) + "Wav");
        wav.onClick = [this] { owner.showWavMenu (track, wav); };
        addAndMakeVisible (wav);
    }

    void update (const damp::Looper::TrackStatus& s, bool overviewChanged, bool isSelected, bool poolReady)
    {
        const bool changed = overviewChanged || isSelected != selected || s.state != shown.state || s.start != shown.start
                             || s.end != shown.end || s.length != shown.length || (active (s.state) && s.sourcePosition != shown.sourcePosition)
                             || s.ending || s.ending != shown.ending; // ending: the badge blinks
        shown = s;
        selected = isSelected;
        playStop.setGlyph (active (s.state) ? TransportButton::Glyph::Stop : TransportButton::Glyph::Play);
        playStop.setEnabled (poolReady && s.state != State::Empty);
        wav.setEnabled (poolReady);
        setTooltip ("Track " + juce::String (track + 1) + ": " + (s.ending ? "recording on to the end of the bar" : stateName (s.state))
                    + (s.master ? " (sets the loop length)" : "")
                    + (s.length > 0 ? ". Click the waveform to play from there." : ""));
        if (changed)
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (selected ? c (P::panel).brighter (0.08f) : c (P::panel));
        g.fillRoundedRectangle (r, 5.0f);
        if (selected)
        {
            g.setColour (c (P::effects).withAlpha (0.8f));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.2f);
        }

        // The number, in its state's colour (blinking while a recording waits for its bar to close)
        const auto badge = badgeArea.toFloat();
        const bool blink = shown.ending && (juce::Time::getMillisecondCounter() / 200) % 2 == 0;
        g.setColour (stateColour (shown.state).withAlpha (shown.state == State::Empty ? 1.0f : blink ? 0.35f : 0.85f));
        g.fillRoundedRectangle (badge, 4.0f);
        g.setColour (shown.state == State::Empty ? c (P::dimText) : c (P::background));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (juce::String (track + 1), badge, juce::Justification::centred);
        if (shown.master)
        {
            // The master (its length sets every sync track's) has a dot in the corner
            g.fillEllipse (badge.getRight() - 6.0f, badge.getY() + 2.5f, 3.5f, 3.5f);
        }

        g.setColour (c (P::background).withAlpha (0.7f));
        g.fillRoundedRectangle (waveArea.toFloat(), 3.0f);
        if (shown.length == 0)
        {
            g.setColour (c (P::dimText).withAlpha (0.7f));
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (shown.state == State::Recording ? "recording..." : "empty", waveArea, juce::Justification::centred);
            return;
        }
        const damp::loopedit::Edits edits { shown.start, shown.end };
        const auto inner = waveArea.reduced (2, 1);
        const auto view = damp::loopedit::fit (damp::loopedit::domainFor (edits, shown.length, 0.0), inner.getWidth());
        const auto colour = shown.state == State::Recording ? c (recordRed) : (shown.muted ? c (P::dimText) : c (P::effects));
        drawLoop (g, inner, view, { &owner.overviews[(size_t) track], edits, shown.length, colour,
                                    active (shown.state) ? (double) shown.sourcePosition : -1.0 });
        if (shown.reverse || shown.half || shown.ending)
        {
            const auto text = shown.ending ? juce::String ("ENDING AT BAR")
                                           : juce::String (shown.reverse ? "REV" : "") + (shown.reverse && shown.half ? " " : "") + (shown.half ? "1/2" : "");
            const juce::Font font (juce::FontOptions (10.5f, juce::Font::bold));
            auto tag = waveArea.reduced (3, 2).removeFromRight (juce::GlyphArrangement::getStringWidthInt (font, text) + 6).removeFromTop (12);
            g.setColour (c (P::background).withAlpha (0.85f));
            g.fillRoundedRectangle (tag.toFloat(), 2.0f);
            g.setColour (shown.ending ? c (recordRed) : c (P::modulation));
            g.setFont (font);
            g.drawText (text, tag, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        badgeArea = r.removeFromLeft (24);
        r.removeFromLeft (4);
        wav.setBounds (r.removeFromRight (38));
        r.removeFromRight (2);
        playStop.setBounds (r.removeFromRight (28));
        r.removeFromRight (6);
        for (auto* b : { &sync, &half, &reverse, &mute })
        {
            b->setBounds (r.removeFromRight (b == &reverse ? 34 : 38));
            r.removeFromRight (2);
        }
        r.removeFromRight (4);
        level.setBounds (r.removeFromRight (76));
        r.removeFromRight (6);
        waveArea = r;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        owner.select (track);
        // A click on the strip plays the track from there
        const auto inner = waveArea.reduced (2, 1);
        if (! inner.contains (e.getPosition()) || shown.length == 0 || shown.state == State::Recording)
            return;
        const damp::loopedit::Edits edits { shown.start, shown.end };
        const auto view = damp::loopedit::fit (damp::loopedit::domainFor (edits, shown.length, 0.0), inner.getWidth());
        if (const auto offset = damp::loopedit::seekOffset (view.frameAt ((double) (e.x - inner.getX())), edits))
            owner.looper.seek (track, *offset);
    }

private:
    LooperView& owner;
    const int track;
    juce::Slider level;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAttachment;
    juce::TextButton mute { "Mute" }, reverse { "Rev" }, half { "Half" }, sync { "Sync" };
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> attachments;
    TransportButton playStop;
    juce::TextButton wav { "WAV" };
    juce::Rectangle<int> badgeArea, waveArea;
    damp::Looper::TrackStatus shown;
    bool selected = false;
};

// ======================================================================================

LooperView::LooperView (DampAudioProcessor& p) : processor (p), looper (p.getLooper()), editor (p.getLooper())
{
    using G = TransportButton::Glyph;

    title.setText ("Looper", juce::dontSendNotification);
    title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, c (P::effects));
    addAndMakeVisible (title);
    onSwitch.setComponentID ("looperOn");
    onSwitch.setColour (juce::ToggleButton::tickColourId, c (P::effects));
    onSwitch.setTooltip ("The looper, at the end of the chain. On reserves its memory (5 minutes of stereo shared by every track and undo).");
    addAndMakeVisible (onSwitch);
    onAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, paramId (damp::PID::LooperOn), onSwitch);

    for (auto* l : { &poolLabel, &infoLabel })
    {
        l->setFont (juce::FontOptions (12.5f));
        l->setColour (juce::Label::textColourId, c (P::dimText));
        addAndMakeVisible (*l);
    }
    infoLabel.setJustificationType (juce::Justification::centredRight);

    rec = std::make_unique<TransportButton> ("Rec", G::Rec, c (recordRed));
    dub = std::make_unique<TransportButton> ("Overdub", G::Dub, c (P::source));
    play = std::make_unique<TransportButton> ("Play", G::Play, c (P::envelope));
    stop = std::make_unique<TransportButton> ("Stop", G::Stop, c (P::global));
    rec->setTooltip ("Record on the selected track; press again to close the loop and play it (a sync track records on to the "
                     "end of its bar first)");
    dub->setTooltip ("Overdub the selected track (press again to stop overdubbing). Overdubbing over padding records into it.");
    play->setTooltip ("Play every track");
    stop->setTooltip ("Stop every track");
    rec->onClick = [this] { looper.press (Action::Rec, selected); };
    dub->onClick = [this] { looper.press (Action::Rec, selected); }; // Rec on a track with a loop overdubs it
    play->onClick = [this] { looper.press (Action::PlayStop); };
    stop->onClick = [this] { looper.press (Action::PlayStop); };
    for (auto* b : { rec.get(), dub.get(), play.get(), stop.get() })
        addAndMakeVisible (*b);

    undo.setTooltip ("Undo the selected track's last recording or overdub");
    redo.setTooltip ("Redo on the selected track");
    clear.setTooltip ("Clear the selected track (undo brings it back)");
    undo.onClick = [this] { looper.press (Action::Undo, selected); };
    redo.onClick = [this] { looper.press (Action::Redo, selected); };
    clear.onClick = [this] { looper.press (Action::Clear, selected); };
    for (auto* b : { &undo, &redo, &clear })
    {
        b->setComponentID ("looper" + b->getButtonText());
        addAndMakeVisible (*b);
    }

    for (int t = 0; t < damp::Looper::numTracks; ++t)
        addAndMakeVisible (rows.add (new TrackRow (*this, t)));
    addAndMakeVisible (editor);

    if (auto* param = p.apvts.getParameter (paramId (damp::PID::LooperTrack)))
    {
        selectedAttachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v)
                                                                          {
                                                                              selected = juce::jlimit (0, damp::Looper::numTracks - 1, juce::roundToInt (v));
                                                                              update();
                                                                          });
        selectedAttachment->sendInitialUpdate();
    }
    update();
}

LooperView::~LooperView() = default;

void LooperView::select (int track)
{
    if (track == selected)
        return;
    selected = track;
    if (selectedAttachment != nullptr)
        selectedAttachment->setValueAsCompleteGesture ((float) track); // footswitches act on it too
    update();
}

void LooperView::update()
{
    global = looper.status();
    for (int t = 0; t < damp::Looper::numTracks; ++t)
    {
        statuses[(size_t) t] = looper.trackStatus (t);
        const bool changed = overviews[(size_t) t].refresh (looper, t);
        rows[t]->update (statuses[(size_t) t], changed, t == selected, global.poolReady);
    }
    editor.update (selected, statuses[(size_t) selected], global, overviews[(size_t) selected]);
    updateTransport();
}

void LooperView::updateTransport()
{
    const auto& s = statuses[(size_t) selected];
    const bool ready = global.poolReady;
    bool anyActive = false, anyAudio = false;
    for (const auto& t : statuses)
    {
        anyActive = anyActive || active (t.state);
        anyAudio = anyAudio || t.state != State::Empty;
    }

    rec->setEnabled (ready && (s.state == State::Empty || s.state == State::Recording));
    rec->setLit (s.state == State::Recording && ! (s.ending && (juce::Time::getMillisecondCounter() / 200) % 2 == 0));
    dub->setEnabled (ready && s.canOverdub && (s.state == State::Playing || s.state == State::Stopped || s.state == State::Overdubbing));
    dub->setLit (s.state == State::Overdubbing);
    play->setEnabled (ready && anyAudio && ! anyActive);
    play->setLit (anyActive);
    stop->setEnabled (ready && anyActive);
    undo.setEnabled (ready && (s.undoLevels > 0 || s.state == State::Recording));
    redo.setEnabled (ready && s.redoLevels > 0);
    clear.setEnabled (ready && s.state != State::Empty);

    // Memory, and a warning for a while when a recording stopped because it ran out
    const auto now = juce::Time::getMillisecondCounter();
    if (poolFullSeen >= 0 && global.poolFullCount > poolFullSeen)
        poolFullAt = now;
    poolFullSeen = global.poolFullCount;
    juce::String pool;
    auto colour = c (P::dimText);
    if (! onSwitch.getToggleState())
        pool = "Off";
    else if (! ready)
        pool = "Reserving memory...";
    else if (poolFullAt != 0 && now - poolFullAt < 4000)
    {
        pool = "Memory full: recording stopped";
        colour = c (recordRed);
    }
    else
        pool = "Memory " + juce::String (global.poolUsedSeconds, 1) + " of " + juce::String (juce::roundToInt (global.poolTotalSeconds)) + " s";
    poolLabel.setText (pool, juce::dontSendNotification);
    poolLabel.setColour (juce::Label::textColourId, colour);

    // Tempo, the master loop and where it's at (bar.beat, beats of the host tempo from its start)
    const double rate = global.sampleRate > 0.0 ? global.sampleRate : 48000.0;
    juce::String info = juce::String (global.tempo, 1) + " BPM";
    if (global.masterLength > 0)
    {
        info << "   Loop " << juce::String ((double) global.masterLength / rate, 2) << " s = "
             << juce::String (damp::loopedit::toBeats (global.masterLength, rate, global.tempo), 2) << " beats";
        if (global.masterTrack >= 0)
        {
            const auto& m = statuses[(size_t) global.masterTrack];
            if (active (m.state) && m.period > 0)
            {
                const auto beat = (int) std::floor (damp::loopedit::toBeats ((int64_t) (m.position * (double) m.period), rate, global.tempo));
                info << "   Bar " << (beat / 4 + 1) << "." << (beat % 4 + 1);
            }
        }
    }
    infoLabel.setText (info, juce::dontSendNotification);
}

void LooperView::showWavMenu (int track, juce::Component& from)
{
    const bool hasAudio = statuses[(size_t) track].length > 0;
    juce::PopupMenu menu;
    menu.addSectionHeader ("Track " + juce::String (track + 1));
    menu.addItem ("Export the recording...", hasAudio, false, [this, track] { exportTrack (track, false); });
    menu.addItem ("Export one loop as heard...", hasAudio, false, [this, track] { exportTrack (track, true); });
    menu.addSeparator();
    menu.addItem ("Import a WAV...", [this, track] { importTrack (track); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&from));
}

void LooperView::exportTrack (int track, bool asPlayed)
{
    const auto name = "damp loop " + juce::String (track + 1) + (asPlayed ? " (as heard)" : "") + ".wav";
    chooser = std::make_unique<juce::FileChooser> ("Export track " + juce::String (track + 1),
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer (this), track, asPlayed] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              juce::String error;
                              if (! safe->processor.exportTrack (track, fc.getResult().withFileExtension ("wav"), asPlayed, error))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", error);
                          });
}

void LooperView::importTrack (int track)
{
    chooser = std::make_unique<juce::FileChooser> ("Import into track " + juce::String (track + 1), juce::File(), "*.wav;*.aif;*.aiff;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer (this), track] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              juce::String error;
                              if (! safe->processor.importTrack (track, fc.getResult(), error))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Import", error);
                              safe->update();
                          });
}

void LooperView::paint (juce::Graphics& g)
{
    g.fillAll (c (P::background));
}

void LooperView::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    auto top = area.removeFromTop (transportHeight).reduced (0, 3);
    title.setBounds (top.removeFromLeft (62));
    onSwitch.setBounds (top.removeFromLeft (40));
    poolLabel.setBounds (top.removeFromLeft (172));
    rec->setBounds (top.removeFromLeft (66));
    top.removeFromLeft (4);
    dub->setBounds (top.removeFromLeft (92));
    top.removeFromLeft (4);
    play->setBounds (top.removeFromLeft (66));
    top.removeFromLeft (4);
    stop->setBounds (top.removeFromLeft (66));
    top.removeFromLeft (16);
    for (auto* b : { &undo, &redo, &clear })
    {
        b->setBounds (top.removeFromLeft (54));
        top.removeFromLeft (4);
    }
    infoLabel.setBounds (top.withTrimmedLeft (8));

    area.removeFromTop (gap);
    // Two columns of four tracks
    constexpr int perColumn = damp::Looper::numTracks / 2;
    auto grid = area.removeFromTop (perColumn * rowHeight + (perColumn - 1) * rowGap);
    const int columnWidth = (grid.getWidth() - 2 * gap) / 2;
    for (int column = 0; column < 2; ++column)
    {
        auto col = (column == 0 ? grid.removeFromLeft (columnWidth) : grid.removeFromRight (columnWidth));
        for (int i = 0; i < perColumn; ++i)
        {
            rows[column * perColumn + i]->setBounds (col.removeFromTop (rowHeight));
            col.removeFromTop (rowGap);
        }
    }

    area.removeFromTop (gap);
    editor.setBounds (area);
}
