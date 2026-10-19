#include "PresetText.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace dustify
{
namespace
{
std::string_view trim (std::string_view s)
{
    const auto ws = " \t\r\n";
    const auto b = s.find_first_not_of (ws);
    if (b == std::string_view::npos)
        return {};
    return s.substr (b, s.find_last_not_of (ws) - b + 1);
}

bool parseNumber (std::string_view s, float& out)
{
    const auto res = std::from_chars (s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}
} // namespace

bool equalsIgnoreCase (std::string_view a, std::string_view b)
{
    return std::ranges::equal (a, b, [] (char x, char y) { return std::tolower ((unsigned char) x) == std::tolower ((unsigned char) y); });
}

std::vector<PresetEntry> splitPresetText (std::string_view text, std::vector<std::string>& warnings)
{
    std::vector<PresetEntry> entries;
    int lineNo = 0;

    while (! text.empty())
    {
        const auto nl = text.find ('\n');
        auto line = text.substr (0, nl);
        text = nl == std::string_view::npos ? std::string_view {} : text.substr (nl + 1);
        ++lineNo;

        if (const auto hash = line.find ('#'); hash != std::string_view::npos)
            line = line.substr (0, hash);
        line = trim (line);
        if (line.empty())
            continue;

        if (line.front() == '[' && line.back() == ']')
        {
            entries.push_back ({ line, {}, lineNo, true });
            continue;
        }

        const auto eq = line.find ('=');
        if (eq == std::string_view::npos)
        {
            warnings.push_back ("line " + std::to_string (lineNo) + ": expected id = value");
            continue;
        }

        entries.push_back ({ trim (line.substr (0, eq)), trim (line.substr (eq + 1)), lineNo, false });
    }

    return entries;
}

bool parseParamValue (const ParamSpec& info, std::string_view s, float& out)
{
    switch (info.type)
    {
        case ParamType::Bool:
            if (equalsIgnoreCase (s, "on") || equalsIgnoreCase (s, "true") || s == "1") { out = 1.0f; return true; }
            if (equalsIgnoreCase (s, "off") || equalsIgnoreCase (s, "false") || s == "0") { out = 0.0f; return true; }
            return false;

        case ParamType::Choice:
            for (size_t i = 0; i < info.choices.size(); ++i)
                if (equalsIgnoreCase (s, info.choices[i])) { out = (float) i; return true; }
            if (parseNumber (s, out) && out >= 0.0f && out <= info.max) { out = std::round (out); return true; }
            return false;

        case ParamType::Float:
            if (! parseNumber (s, out))
                return false;
            out = std::clamp (out, info.min, info.max);
            return true;
    }
    return false;
}

std::string formatParamValue (const ParamSpec& info, float value)
{
    switch (info.type)
    {
        case ParamType::Bool: return value >= 0.5f ? "on" : "off";
        case ParamType::Choice:
            return std::string (info.choices[(size_t) std::clamp ((int) (value + 0.5f), 0, (int) info.choices.size() - 1)]);
        case ParamType::Float: break;
    }
    char buf[32];
    const auto res = std::to_chars (buf, buf + sizeof (buf), value);
    return std::string (buf, (size_t) (res.ptr - buf));
}

} // namespace dustify
