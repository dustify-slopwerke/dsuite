#include "ParamLayout.h"

namespace dustify
{
namespace
{
// Human-readable values with units ("1.00 kHz", "350 ms", "-6.0 dB", "C4"...). Each format picks
// its precision from the rounded value, and text parses back in the parameter's own unit, so
// value -> text -> value -> text is stable (hosts and validators check this).
juce::String formatValue (float v, std::string_view unit, bool wholeNumbers)
{
    auto rounded = [] (double x, int decimals) { const double s = std::pow (10.0, decimals); return std::round (x * s) / s; };

    if (unit == "note")
        return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 4);
    if (wholeNumbers)
        return juce::String (juce::roundToInt (v)) + (unit.empty() ? "" : " " + juce::String (unit.data(), unit.size()));
    if (unit == "Hz")
    {
        if (rounded (v, 1) < 100.0) return juce::String (rounded (v, 1), 1) + " Hz";
        if (rounded (v, 0) < 1000.0) return juce::String (juce::roundToInt (v)) + " Hz";
        return juce::String (rounded (v / 1000.0, 2), 2) + " kHz";
    }
    if (unit == "s" || unit == "ms")
    {
        const double ms = unit == "s" ? v * 1000.0 : v;
        if (rounded (ms, 1) < 100.0) return juce::String (rounded (ms, 1), 1) + " ms";
        if (rounded (ms, 0) < 1000.0) return juce::String (juce::roundToInt (ms)) + " ms";
        return juce::String (rounded (ms / 1000.0, 2), 2) + " s";
    }
    if (unit == "dB") return v <= -48.0f ? juce::String ("-inf dB") : juce::String (rounded (v, 1), 1) + " dB";
    if (unit == "dBFS") return juce::String (rounded (v, 1), 1) + " dB"; // a level, never "-inf" (thresholds)
    if (unit == "st") return juce::String (rounded (v, 2), 2) + " st";
    if (unit == "ct") return juce::String (juce::roundToInt (v)) + " ct";
    if (unit == "cents") return juce::String (rounded (v, 1), 1) + " ct"; // fine detuning
    if (unit == "bpm") return juce::String (juce::roundToInt (v)) + " bpm";
    return juce::String (rounded (v, 2), 2);
}

float parseValue (const juce::String& input, std::string_view unit)
{
    const auto t = input.trim().toLowerCase();

    if (unit == "note" && ! t.containsOnly ("0123456789.-"))
    {
        // Note name with middle C = C4: C, C#, Db ... B, then the octave (may be negative)
        constexpr int semitones[] { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
        if (t.isEmpty() || t[0] < 'a' || t[0] > 'g')
            return 0.0f;
        int note = semitones[t[0] - 'a'];
        int pos = 1;
        if (t[pos] == '#') { ++note; ++pos; }
        else if (t[pos] == 'b') { --note; ++pos; }
        return (float) (note + 12 * (t.substring (pos).getIntValue() + 1));
    }

    if (t.startsWith ("-inf"))
        return -1000.0f; // clamps to the bottom of the range

    double v = t.getDoubleValue();
    if (unit == "Hz" && t.endsWith ("khz")) v *= 1000.0;
    if (unit == "s" && t.endsWith ("ms")) v /= 1000.0;
    if (unit == "ms" && t.endsWith ("s") && ! t.endsWith ("ms")) v *= 1000.0;
    return (float) v;
}

juce::AudioParameterFloatAttributes floatAttributes (std::string_view unit, bool wholeNumbers)
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([unit, wholeNumbers] (float v, int) { return formatValue (v, unit, wholeNumbers); })
        .withValueFromStringFunction ([unit] (const juce::String& text) { return parseValue (text, unit); });
}
} // namespace

std::unique_ptr<juce::RangedAudioParameter> createParameter (const ParamSpec& info)
{
    const juce::ParameterID id { toJuce (info.id), 1 };
    const auto name = toJuce (info.name);

    switch (info.type)
    {
        case ParamType::Bool:
            return std::make_unique<juce::AudioParameterBool> (id, name, info.def >= 0.5f);

        case ParamType::Choice:
        {
            juce::StringArray choices;
            for (auto c : info.choices)
                choices.add (toJuce (c));
            return std::make_unique<juce::AudioParameterChoice> (id, name, choices, (int) info.def);
        }

        case ParamType::Float:
            break;
    }

    juce::NormalisableRange<float> range (info.min, info.max, info.step);
    if (info.centre > 0.0f)
        range.setSkewForCentre (info.centre);
    return std::make_unique<juce::AudioParameterFloat> (id, name, range, info.def, floatAttributes (info.unit, info.step >= 1.0f));
}

float snapToSaveGrid (const juce::RangedAudioParameter& param, float value)
{
    const float normalised = (float) (std::round ((double) param.convertTo0to1 (value) * 1.0e6) / 1.0e6);
    return param.convertFrom0to1 (normalised);
}

} // namespace dustify
