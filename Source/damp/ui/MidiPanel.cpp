#include "MidiPanel.h"
#include "MidiTags.h"

#include "PluginProcessor.h"
#include "common/plugin/ParamLayout.h"
#include "common/ui/DustifyLookAndFeel.h"


namespace
{
using P = DustifyLookAndFeel::Palette;
using namespace damp::midi;
juce::Colour c (juce::uint32 argb) { return juce::Colour (argb); }
constexpr juce::uint32 learnRed = 0xffe0505a;

juce::String str (const std::string& s) { return juce::String (s); }
juce::String shortSource (Source s) { return str (sourceName (s)).upToFirstOccurrenceOf (" (", false, false); }

std::optional<Target> targetFor (const juce::String& key) { return parseTarget (key.toStdString()); }

// Column edges in the mapping list
constexpr int sourceWidth = 170, modeWidth = 110, removeWidth = 30;

juce::String messageText (std::array<uint8_t, 3> m)
{
    const int status = m[0] & 0xf0, ch = (m[0] & 0x0f) + 1;
    const auto on = " (ch " + juce::String (ch) + ")";
    switch (status)
    {
        case 0xb0: return "CC " + juce::String (m[1]) + " = " + juce::String (m[2]) + on;
        case 0x90:
            return m[2] > 0 ? str (sourceName ({ Source::Type::Note, ch, m[1] })).upToFirstOccurrenceOf (" (", false, false) + " on, velocity "
                                  + juce::String (m[2]) + on
                            : str (sourceName ({ Source::Type::Note, ch, m[1] })).upToFirstOccurrenceOf (" (", false, false) + " off" + on;
        case 0x80: return str (sourceName ({ Source::Type::Note, ch, m[1] })).upToFirstOccurrenceOf (" (", false, false) + " off" + on;
        case 0xc0: return "Program " + juce::String (m[1]) + on;
        default: return {};
    }
}
} // namespace

// ======================================================================================

void MidiPanel::addTargetMenu (juce::PopupMenu& menu, std::vector<Target>& targets)
{
    auto add = [&targets] (juce::PopupMenu& m, Target t, const juce::String& name)
    {
        targets.push_back (t);
        m.addItem ((int) targets.size(), name);
    };
    juce::PopupMenu commands;
    for (int i = 0; i < numCommands; ++i)
        add (commands, Target::command ((Command) i), str (std::string (commandName ((Command) i))));
    menu.addSubMenu ("Looper, tempo, presets, tuner", commands);

    // Parameters, grouped by the part of their name before ':' (in table order)
    std::vector<std::pair<juce::String, juce::PopupMenu>> groups;
    for (const auto& info : damp::allParams())
    {
        const auto name = dustify::toJuce (info.name);
        const auto group = name.containsChar (':') ? name.upToFirstOccurrenceOf (":", false, false) : juce::String ("Amp and output");
        const auto item = name.containsChar (':') ? name.fromFirstOccurrenceOf (":", false, false).trim() : name;
        auto it = std::find_if (groups.begin(), groups.end(), [&group] (const auto& g) { return g.first == group; });
        if (it == groups.end())
        {
            groups.emplace_back (group, juce::PopupMenu());
            it = groups.end() - 1;
        }
        add (it->second, Target::param (info.pid), item);
    }
    menu.addSeparator();
    for (auto& [name, sub] : groups)
        menu.addSubMenu (name, sub);
}

MidiPanel::MidiPanel (DampAudioProcessor& p) : processor (p), midi (p.getMidi())
{
    channelLabel.setText ("MIDI channel", juce::dontSendNotification);
    channelLabel.setFont (juce::FontOptions (13.0f));
    channelLabel.setColour (juce::Label::textColourId, c (P::dimText));
    channelLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (channelLabel);
    channel.setComponentID ("midiChannel");
    channel.addItem ("Omni", 1);
    for (int ch = 1; ch <= 16; ++ch)
        channel.addItem (juce::String (ch), ch + 1);
    channel.setTooltip ("Only messages on this channel are used (Omni: every channel). Each mapping also keeps the channel it was learned on.");
    channel.onChange = [this]
    {
        midi.setChannel (channel.getSelectedId() - 1);
        changed();
    };
    addAndMakeVisible (channel);

    programChange.setComponentID ("midiProgramChange");
    programChange.setColour (juce::ToggleButton::tickColourId, c (P::envelope));
    programChange.setTooltip ("Program Change n picks preset n + 1 of the preset menu (factory presets, then yours); Bank Select "
                              "(CC 0 / 32) first reaches past 128");
    programChange.onClick = [this]
    {
        midi.setProgramChange (programChange.getToggleState());
        changed();
    };
    addAndMakeVisible (programChange);
    programLabel.setText ("Program Change selects presets (Bank Select: CC 0 / 32)", juce::dontSendNotification);
    programLabel.setFont (juce::FontOptions (13.0f));
    programLabel.setColour (juce::Label::textColourId, c (P::text));
    programLabel.setTooltip (programChange.getTooltip());
    addAndMakeVisible (programLabel);

    learn.setComponentID ("midiLearnTarget");
    learn.setTooltip ("Choose anything to control, then move a knob or press a pad or footswitch. (Or use Learn at the top: click "
                      "any control.)");
    learn.onClick = [this] { chooseLearnTarget(); };
    addAndMakeVisible (learn);
    cancel.onClick = [this] { midi.cancelLearn(); };
    addChildComponent (cancel);

    for (auto* l : { &status, &lastMessage, &help })
    {
        l->setFont (juce::FontOptions (12.5f));
        l->setColour (juce::Label::textColourId, c (P::dimText));
        addAndMakeVisible (*l);
    }
    help.setText ("Switches (pedal on / off, looper buttons) act on a press. Momentary: a footswitch that sends 127 on press and 0 on "
                  "release. Latching: one that sends 127, then 0 on the next press. Click a mode to change it.",
                  juce::dontSendNotification);
    help.setJustificationType (juce::Justification::topLeft);
    help.setMinimumHorizontalScale (1.0f);

    list.setRowHeight (24);
    list.setColour (juce::ListBox::backgroundColourId, c (P::panel));
    list.setOutlineThickness (0);
    addAndMakeVisible (list);
    update();
}

void MidiPanel::changed()
{
    processor.midiSettingsChanged(); // machine-wide too
    update();
}

void MidiPanel::chooseLearnTarget()
{
    juce::PopupMenu menu;
    menuTargets.clear();
    addTargetMenu (menu, menuTargets);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (learn),
                        [safe = juce::Component::SafePointer (this)] (int id)
                        {
                            if (safe != nullptr && id > 0 && id <= (int) safe->menuTargets.size())
                                safe->midi.learn (safe->menuTargets[(size_t) id - 1]);
                        });
}

void MidiPanel::update()
{
    const auto settings = midi.settings();
    if (settings.mappings != shown || processor.midiSettingsVersion() != versionShown)
    {
        versionShown = processor.midiSettingsVersion();
        shown = settings.mappings;
        list.updateContent();
        list.repaint();
    }
    channel.setSelectedId (settings.channel + 1, juce::dontSendNotification);
    programChange.setToggleState (settings.programChange, juce::dontSendNotification);

    const auto learning = midi.learning();
    status.setText (learning ? "Learning " + str (targetName (*learning)) + ": move a knob or press a pad or footswitch"
                             : juce::String (shown.size()) + (shown.size() == 1 ? " mapping" : " mappings"),
                    juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, learning ? c (learnRed) : c (P::dimText));
    cancel.setVisible (learning.has_value());

    if (const auto activity = midi.activity(); activity != activityShown)
    {
        activityShown = activity;
        lastMessage.setText ("Last message: " + messageText (midi.lastMessage()), juce::dontSendNotification);
    }
    else if (activity == 0)
    {
        lastMessage.setText ("No MIDI received yet", juce::dontSendNotification);
    }
}

void MidiPanel::paint (juce::Graphics& g)
{
    // Column headings over the list
    const auto area = list.getBounds().withHeight (20).translated (0, -20);
    g.setColour (c (P::dimText));
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    auto r = area.withTrimmedLeft (8);
    g.drawText ("Source", r.removeFromLeft (sourceWidth), juce::Justification::centredLeft);
    r.removeFromRight (removeWidth);
    g.drawText ("Mode", r.removeFromRight (modeWidth), juce::Justification::centredLeft);
    g.drawText ("Controls", r, juce::Justification::centredLeft);
    if (shown.empty())
    {
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("No mappings yet: click Learn at the top, then a control, then move a knob or press a pad or footswitch.",
                    list.getBounds().reduced (12), juce::Justification::centred);
    }
}

void MidiPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= (int) shown.size())
        return;
    const auto& m = shown[(size_t) row];
    if (row % 2 == 1)
        g.fillAll (c (P::panelEdge).withAlpha (0.35f));
    auto r = juce::Rectangle<int> (0, 0, width, height).withTrimmedLeft (8);
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (c (P::text));
    g.drawText (str (sourceName (m.source)), r.removeFromLeft (sourceWidth), juce::Justification::centredLeft);
    auto remove = r.removeFromRight (removeWidth);
    auto mode = r.removeFromRight (modeWidth);
    g.drawText (str (targetName (m.target)), r, juce::Justification::centredLeft);
    if (m.target.isSwitch())
    {
        g.setColour (c (P::control));
        g.fillRoundedRectangle (mode.reduced (2, 3).toFloat(), 4.0f);
        g.setColour (c (P::envelope));
        g.drawText (m.mode == SwitchMode::Latching ? "Latching" : "Momentary", mode.reduced (8, 0), juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (c (P::dimText));
        g.drawText ("Continuous", mode.reduced (8, 0), juce::Justification::centredLeft);
    }
    g.setColour (c (P::dimText));
    g.drawText (juce::String::fromUTF8 ("\xc3\x97"), remove, juce::Justification::centred); // a multiplication sign
}

void MidiPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= (int) shown.size())
        return;
    const auto m = shown[(size_t) row];
    const int width = list.getVisibleRowWidth();
    if (e.x >= width - removeWidth)
    {
        midi.removeMapping (m);
        changed();
    }
    else if (e.x >= width - removeWidth - modeWidth && m.target.isSwitch())
    {
        midi.setMode (m, m.mode == SwitchMode::Latching ? SwitchMode::Momentary : SwitchMode::Latching);
        changed();
    }
}

void MidiPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 10);
    auto top = area.removeFromTop (28);
    channelLabel.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (6);
    channel.setBounds (top.removeFromLeft (90).reduced (0, 2));
    top.removeFromLeft (20);
    programChange.setBounds (top.removeFromLeft (44).reduced (0, 2));
    programLabel.setBounds (top.removeFromLeft (360));
    learn.setBounds (top.removeFromRight (96).reduced (0, 1));
    top.removeFromRight (8);
    cancel.setBounds (top.removeFromRight (70).reduced (0, 1));
    top.removeFromRight (8);
    status.setBounds (top);
    status.setJustificationType (juce::Justification::centredRight);

    auto bottom = area.removeFromBottom (36);
    lastMessage.setBounds (bottom.removeFromRight (300));
    help.setBounds (bottom);
    area.removeFromBottom (6);
    area.removeFromTop (28); // the column headings
    list.setBounds (area);
}

// ======================================================================================

MidiLearnOverlay::MidiLearnOverlay (DampAudioProcessor& p, juce::Component& c) : processor (p), content (c)
{
    setComponentID ("midiLearnOverlay");
    setWantsKeyboardFocus (false);
}

void MidiLearnOverlay::collect (juce::Component& parent, juce::Rectangle<int> clip)
{
    for (auto* child : parent.getChildren())
    {
        if (child == this || ! child->isVisible())
            continue;
        const auto bounds = content.getLocalArea (child, child->getLocalBounds()).getIntersection (clip);
        if (bounds.isEmpty())
            continue;
        if (const auto keys = damp::ui::midiTargetsOf (*child); ! keys.isEmpty())
        {
            Spot spot { bounds, child, {} };
            for (const auto& key : keys)
                if (const auto t = targetFor (key))
                    spot.targets.push_back (*t);
            if (! spot.targets.empty())
                spots.push_back (std::move (spot));
        }
        collect (*child, bounds);
    }
}

void MidiLearnOverlay::update()
{
    spots.clear();
    collect (content, content.getLocalBounds());
    repaint();
}

const MidiLearnOverlay::Spot* MidiLearnOverlay::spotAt (juce::Point<int> p) const
{
    // The innermost: the smallest containing it (a track row's buttons over the row)
    const Spot* best = nullptr;
    for (const auto& s : spots)
        if (s.bounds.contains (p) && (best == nullptr || s.bounds.getWidth() * s.bounds.getHeight() < best->bounds.getWidth() * best->bounds.getHeight()))
            best = &s;
    return best;
}

bool MidiLearnOverlay::hitTest (int x, int y)
{
    return spotAt ({ x, y }) != nullptr;
}

void MidiLearnOverlay::paint (juce::Graphics& g)
{
    // Where the controls are now (a tab may have changed since the timer looked)
    spots.clear();
    collect (content, content.getLocalBounds());
    auto& midi = processor.getMidi();
    const auto learning = midi.learning();
    const auto settings = midi.settings();
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    for (const auto& s : spots)
    {
        const bool isLearning = learning && std::find (s.targets.begin(), s.targets.end(), *learning) != s.targets.end();
        juce::StringArray sources;
        for (const auto& m : settings.mappings)
            if (std::find (s.targets.begin(), s.targets.end(), m.target) != s.targets.end())
                sources.add (shortSource (m.source));
        const auto r = s.bounds.toFloat().reduced (1.0f);
        const auto colour = isLearning ? c (learnRed) : sources.isEmpty() ? c (P::envelope).withAlpha (0.55f) : c (P::source);
        g.setColour (colour.withAlpha (isLearning ? 0.3f : sources.isEmpty() ? 0.06f : 0.16f));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (r, 3.0f, isLearning ? 2.0f : 1.0f);
        if (! sources.isEmpty() || isLearning)
        {
            const auto text = isLearning ? juce::String ("Learning...") : sources.joinIntoString (", ");
            const int w = juce::jmin (s.bounds.getWidth(), juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 8);
            const auto tag = juce::Rectangle<int> (s.bounds.getX(), s.bounds.getY(), w, 14);
            g.setColour (colour.withAlpha (0.9f));
            g.fillRect (tag);
            g.setColour (c (P::background));
            g.drawText (text, tag.reduced (3, 0), juce::Justification::centredLeft, true);
        }
    }
}

void MidiLearnOverlay::mouseDown (const juce::MouseEvent& e)
{
    const auto* spot = spotAt (e.getPosition());
    if (spot == nullptr)
        return;
    auto& midi = processor.getMidi();
    if (e.mods.isPopupMenu() || spot->targets.size() > 1)
        return showMenu (*spot);
    const auto t = spot->targets.front();
    if (midi.learning() == t)
        midi.cancelLearn();
    else
        midi.learn (t);
    repaint();
}

void MidiLearnOverlay::showMenu (const Spot& spot)
{
    auto& midi = processor.getMidi();
    juce::PopupMenu menu;
    menuTargets.clear();
    std::vector<Mapping> mappings;
    for (const auto& t : spot.targets)
    {
        if (spot.targets.size() > 1)
            menu.addSectionHeader (str (targetName (t)));
        menuTargets.push_back (t);
        menu.addItem ((int) menuTargets.size(), "MIDI Learn" + (spot.targets.size() > 1 ? juce::String() : ": " + str (targetName (t))));
        for (const auto& m : midi.mappingsFor (t))
        {
            mappings.push_back (m);
            const int base = 1000 + 10 * ((int) mappings.size() - 1);
            menu.addItem (base, "Forget " + str (sourceName (m.source)));
            if (t.isSwitch())
            {
                menu.addItem (base + 1, "    Momentary (127 on press, 0 on release)", true, m.mode == SwitchMode::Momentary);
                menu.addItem (base + 2, "    Latching (127, then 0 on the next press)", true, m.mode == SwitchMode::Latching);
            }
        }
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (spot.bounds)),
                        [safe = juce::Component::SafePointer (this), mappings] (int id)
                        {
                            if (safe == nullptr || id <= 0)
                                return;
                            auto& m = safe->processor.getMidi();
                            if (id <= (int) safe->menuTargets.size())
                                m.learn (safe->menuTargets[(size_t) id - 1]);
                            else if (const int which = (id - 1000) / 10; id >= 1000 && which < (int) mappings.size())
                            {
                                const auto& mapping = mappings[(size_t) which];
                                switch ((id - 1000) % 10)
                                {
                                    case 0: m.removeMapping (mapping); break;
                                    case 1: m.setMode (mapping, SwitchMode::Momentary); break;
                                    case 2: m.setMode (mapping, SwitchMode::Latching); break;
                                    default: break;
                                }
                                safe->processor.midiSettingsChanged();
                            }
                            safe->repaint();
                        });
}

void MidiLearnOverlay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Scrolling still scrolls (the pedal board): pass it to what's under the spot
    if (const auto* spot = spotAt (e.getPosition()); spot != nullptr && spot->component != nullptr)
        for (auto* c = spot->component->getParentComponent(); c != nullptr && c != &content; c = c->getParentComponent())
            if (auto* viewport = dynamic_cast<juce::Viewport*> (c))
                return viewport->mouseWheelMove (e.getEventRelativeTo (viewport), wheel);
}
