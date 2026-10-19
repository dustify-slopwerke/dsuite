#pragma once

#include "ParamTable.h"

#include <string>
#include <string_view>
#include <vector>

namespace dustify
{

// The text preset format shared by every plugin: one `id = value` per line, choices written by
// name, bools as on/off, `#` starts a comment. A `[section]` line starts a plugin-defined section.
// Parsing is forgiving: anything malformed becomes a warning and is skipped.

struct PresetEntry
{
    std::string_view key, value; // key is the whole "[...]" line for a section header
    int line = 0;
    bool section = false;
};

std::vector<PresetEntry> splitPresetText (std::string_view text, std::vector<std::string>& warnings);

bool equalsIgnoreCase (std::string_view, std::string_view);

// Text -> plain value, clamped to the parameter's range. False if the text doesn't parse.
bool parseParamValue (const ParamSpec&, std::string_view text, float& out);
std::string formatParamValue (const ParamSpec&, float value);

} // namespace dustify
