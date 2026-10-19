#pragma once

#include <array>
#include <span>
#include <string_view>

namespace dustify
{

// Building blocks for a plugin's parameter table: each plugin declares an `enum class PID` (ending
// in Count), a constexpr table of ParamInfoT<PID>, and a `paramTable (PID)` function (found by
// argument-dependent lookup) returning it. The plugin builds its host parameters from the table,
// the DSP reads plain values indexed by PID, and presets refer to the string ids.

enum class ParamType { Float, Choice, Bool };

// Everything about a parameter except which plugin it belongs to, so UI and host-side code can
// handle any plugin's parameters
struct ParamSpec
{
    std::string_view id;    // persisted identifier: never rename or reuse one
    std::string_view name;  // full name shown to hosts
    std::string_view label; // short label used inside a UI section
    ParamType type;
    float min, max, def;
    float centre = 0.0f;    // skew so this value sits mid-knob; 0 = linear
    float step = 0.0f;      // 0 = continuous
    std::string_view unit = {};
    std::span<const std::string_view> choices = {};
    bool global = false;    // plugin-defined grouping (dsynth: shared by both layers)
};

template <typename PID>
struct ParamInfoT : ParamSpec
{
    PID pid;
};

template <typename PID>
constexpr ParamInfoT<PID> boolean (PID p, std::string_view id, std::string_view name, std::string_view label, bool def)
{
    return { { id, name, label, ParamType::Bool, 0.0f, 1.0f, def ? 1.0f : 0.0f }, p };
}

template <typename PID, size_t N>
constexpr ParamInfoT<PID> choice (PID p, std::string_view id, std::string_view name, std::string_view label,
                                 const std::string_view (&list)[N], int def)
{
    return { { id, name, label, ParamType::Choice, 0.0f, (float) (N - 1), (float) def, 0.0f, 1.0f, {}, list }, p };
}

template <typename PID>
constexpr ParamInfoT<PID> number (PID p, std::string_view id, std::string_view name, std::string_view label,
                                 float min, float max, float def, std::string_view unit = {}, float centre = 0.0f,
                                 float step = 0.0f)
{
    return { { id, name, label, ParamType::Float, min, max, def, centre, step, unit }, p };
}

template <typename PID>
const ParamInfoT<PID>& paramInfo (PID p)
{
    return paramTable (p)[(size_t) p];
}

template <typename PID>
const ParamInfoT<PID>* findParam (std::string_view id)
{
    for (const auto& p : paramTable (PID {}))
        if (p.id == id)
            return &p;
    return nullptr;
}

// Plain (denormalised) values for every parameter. Choices hold their index, bools 0/1.
template <typename PID>
struct PatchT
{
    static constexpr int size = (int) PID::Count;
    std::array<float, (size_t) size> v {};

    static PatchT defaults()
    {
        PatchT patch;
        for (const auto& p : paramTable (PID {}))
            patch[p.pid] = p.def;
        return patch;
    }

    float operator[] (PID p) const { return v[(size_t) p]; }
    float& operator[] (PID p) { return v[(size_t) p]; }

    bool on (PID p) const { return v[(size_t) p] >= 0.5f; }
    int index (PID p) const { return (int) (v[(size_t) p] + 0.5f); }
    template <typename E> E choice (PID p) const { return (E) index (p); }
};

} // namespace dustify
