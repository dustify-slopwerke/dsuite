#pragma once

#include "Params.h"

#include <string>
#include <vector>

namespace dpiano
{

// Text presets (common/dsp/PresetText.h): `id = value` lines; parameters a preset doesn't mention
// keep their defaults. `name = ...` is the display name, `category = ...` groups factory presets.
struct PresetParseResult
{
    Patch patch = Patch::defaults();
    std::string name;
    std::string category;
    std::vector<std::string> warnings; // unknown ids / bad values (ignored, not fatal)
};

PresetParseResult parsePreset (std::string_view text);
std::string serialisePreset (const Patch&, std::string_view name);

} // namespace dpiano
