#pragma once

#include "Params.h"
#include "pedals/Pedal.h"

#include <string>
#include <vector>

namespace damp
{

// Text presets (common/dsp/PresetText.h): `id = value` lines; parameters a preset doesn't mention
// keep their defaults. `name = ...` is the display name, `category = ...` groups factory presets.
struct PresetParseResult
{
    Patch patch = Patch::defaults();
    std::string name;
    std::string category;
    BoardOrder board = BoardOrder::defaults(); // `board = ...`: the pedal order
    std::vector<std::string> warnings; // unknown ids / bad values (ignored, not fatal)
};

PresetParseResult parsePreset (std::string_view text);
std::string serialisePreset (const Patch&, std::string_view name, const BoardOrder& = BoardOrder::defaults());

} // namespace damp
