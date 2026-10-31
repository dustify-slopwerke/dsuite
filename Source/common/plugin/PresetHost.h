#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <string>
#include <vector>

namespace dustify
{

struct FactoryPreset
{
    juce::String name;
    juce::String category;
    std::string text;
};

// What the shared preset bar needs from a plugin. Call from the message thread.
class PresetHost
{
public:
    virtual ~PresetHost() = default;

    virtual const std::vector<FactoryPreset>& getFactoryPresets() const = 0;
    virtual void loadFactoryPreset (int index) = 0;
    virtual void loadPresetText (const std::string&) = 0;
    virtual std::string savePresetText (const juce::String& name) const = 0;

    // Name of the last preset loaded or saved (may be called from any thread)
    virtual juce::String getCurrentPresetName() const = 0;
    virtual void setCurrentPresetName (const juce::String&) = 0;

    virtual juce::File userPresetDirectory() const = 0;
    virtual juce::String presetFileExtension() const = 0; // without the dot
};

// The preset selection in session state: the program (factory preset index) and the name the
// preset bar shows (a user preset's too), as attributes of the state's XML. Restoring brings back
// only the selection, never the preset itself: the session's parameter values stand. A session
// saved before these existed restores program 0 and no name.
inline void writePresetSelection (juce::XmlElement& xml, int program, const juce::String& name)
{
    xml.setAttribute ("program", program);
    xml.setAttribute ("presetName", name);
}

// Takes the attributes out of `xml` (before it becomes the parameters' state)
inline void readPresetSelection (juce::XmlElement& xml, int numPrograms, int& program, juce::String& name)
{
    program = juce::jlimit (0, juce::jmax (0, numPrograms - 1), xml.getIntAttribute ("program", 0));
    name = xml.getStringAttribute ("presetName");
    xml.removeAttribute ("program");
    xml.removeAttribute ("presetName");
}

// Factory presets embedded with juce_add_binary_data, in filename order (files are named so they
// sort into category order). parse (text) returns { name, category }.
template <typename Parse>
std::vector<FactoryPreset> loadFactoryPresets (int count, const char* const* resourceNames, const char* const* originalFilenames,
                                               const char* (*getResource) (const char*, int&), Parse parse)
{
    std::vector<std::pair<juce::String, int>> files; // original filename -> resource index
    for (int i = 0; i < count; ++i)
        files.emplace_back (originalFilenames[i], i);
    std::sort (files.begin(), files.end());

    std::vector<FactoryPreset> presets;
    for (const auto& [filename, index] : files)
    {
        int size = 0;
        const auto* data = getResource (resourceNames[index], size);
        std::string text (data, (size_t) size);
        const auto [name, category] = parse (text);
        presets.push_back ({ name.empty() ? filename : juce::String (name), juce::String (category), std::move (text) });
    }
    return presets;
}

} // namespace dustify
