#include "Preset.h"

#include "common/dsp/PresetText.h"

#include <sstream>

namespace dpiano
{

PresetParseResult parsePreset (std::string_view text)
{
    PresetParseResult result;

    for (const auto& entry : splitPresetText (text, result.warnings))
    {
        const auto where = "line " + std::to_string (entry.line) + ": ";
        if (entry.section)
        {
            result.warnings.push_back (where + "unknown section " + std::string (entry.key));
            continue;
        }
        if (entry.key == "name")
        {
            result.name = entry.value;
            continue;
        }
        if (entry.key == "category")
        {
            result.category = entry.value;
            continue;
        }

        const auto* info = findParam (entry.key);
        float v = 0.0f;
        if (info == nullptr)
            result.warnings.push_back (where + "unknown parameter '" + std::string (entry.key) + "'");
        else if (parseParamValue (*info, entry.value, v))
            result.patch[info->pid] = v;
        else
            result.warnings.push_back (where + "bad value '" + std::string (entry.value) + "' for " + std::string (entry.key));
    }

    return result;
}

std::string serialisePreset (const Patch& patch, std::string_view name)
{
    std::ostringstream out;
    out << "# dpiano preset\n";
    out << "name = " << name << "\n";
    for (const auto& info : allParams())
        out << info.id << " = " << formatParamValue (info, patch[info.pid]) << "\n";
    return out.str();
}

} // namespace dpiano
