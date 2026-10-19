#pragma once

#include "Params.h"

#include <string>
#include <vector>

namespace dsynth
{

// Text preset format, one `id = value` per line. Choices are written by name, bools as on/off.
// Parameters a preset doesn't mention keep their defaults, so factory presets only list what
// they change. `#` starts a comment; `name = ...` is the display name and `category = ...` groups
// factory presets in the preset menu.
//
// A `[layer b]` line starts the second layer's section. Layer-setup parameters (ParamInfo::global)
// always belong to the main section, wherever they appear.
struct PresetParseResult
{
    Patch patch = Patch::defaults();  // layer A, plus the layer setup
    Patch layerB = Patch::defaults();
    bool hasLayerB = false;
    std::string name;
    std::string category;
    std::vector<std::string> warnings; // unknown ids / bad values (ignored, not fatal)
};

PresetParseResult parsePreset (std::string_view text);
std::string serialisePreset (const Patch& layerA, std::string_view name, const Patch* layerB = nullptr);

} // namespace dsynth
