#include "Preset.h"

#include "common/dsp/PresetText.h"

#include <sstream>

namespace dsynth
{

PresetParseResult parsePreset (std::string_view text)
{
    PresetParseResult result;

    for (const auto& entry : splitPresetText (text, result.warnings))
    {
        if (entry.section)
        {
            if (equalsIgnoreCase (entry.key, "[layer b]"))
                result.hasLayerB = true;
            else
                result.warnings.push_back ("line " + std::to_string (entry.line) + ": unknown section " + std::string (entry.key));
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
        if (info == nullptr)
        {
            result.warnings.push_back ("line " + std::to_string (entry.line) + ": unknown parameter '" + std::string (entry.key) + "'");
            continue;
        }

        float v = 0.0f;
        if (parseParamValue (*info, entry.value, v))
            (result.hasLayerB && ! info->global ? result.layerB : result.patch)[info->pid] = v;
        else
            result.warnings.push_back ("line " + std::to_string (entry.line) + ": bad value '" + std::string (entry.value) + "' for " + std::string (entry.key));
    }

    return result;
}

std::string serialisePreset (const Patch& layerA, std::string_view name, const Patch* layerB)
{
    std::ostringstream out;
    out << "# dsynth preset\n";
    out << "name = " << name << "\n";

    auto write = [&out] (const Patch& patch, bool includeLayerSetup)
    {
        for (const auto& info : allParams())
            if (! info.global || includeLayerSetup)
                out << info.id << " = " << formatParamValue (info, patch[info.pid]) << "\n";
    };

    write (layerA, true);
    if (layerB != nullptr)
    {
        out << "\n[layer b]\n";
        write (*layerB, false);
    }
    return out.str();
}

} // namespace dsynth
