#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// What a control does, for MIDI Learn: the editor's learn mode finds controls by these
// properties. "midiTarget" holds target keys (damp::midi::targetKey, several separated by '|');
// the shared ParamSection marks its controls with "dustifyParam" (a parameter id).
namespace damp::ui
{
inline void setMidiTarget (juce::Component& c, const juce::String& keys) { c.getProperties().set ("midiTarget", keys); }

inline juce::StringArray midiTargetsOf (const juce::Component& c)
{
    const auto& props = c.getProperties();
    if (props.contains ("midiTarget"))
        return juce::StringArray::fromTokens (props["midiTarget"].toString(), "|", "");
    if (props.contains ("dustifyParam"))
        return { props["dustifyParam"].toString() };
    return {};
}
} // namespace damp::ui
