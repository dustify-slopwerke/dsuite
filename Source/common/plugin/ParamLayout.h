#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "common/dsp/ParamTable.h"

namespace dustify
{

inline juce::String toJuce (std::string_view s) { return juce::String (s.data(), s.size()); }

// A host parameter for one table entry: bool, choice or float, the float with unit-aware
// value <-> text conversion ("1.00 kHz", "350 ms", "C4"...)
std::unique_ptr<juce::RangedAudioParameter> createParameter (const ParamSpec&);

// Host parameters for a plugin's whole parameter table
template <typename Table>
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout (const Table& table)
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const ParamSpec& info : table)
        layout.add (createParameter (info));
    return layout;
}

// Restoring a value goes through the parameter's normalised range, which isn't bit-exact for
// skewed ranges, so a reloaded session would save slightly different digits. Saving on a fine
// grid in the normalised domain makes the round trip land on the same grid point every time.
float snapToSaveGrid (const juce::RangedAudioParameter&, float value);

} // namespace dustify
