#include "MidiControl.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace damp::midi
{
namespace
{
struct CommandInfo
{
    Command command;
    std::string_view key, name;
};

constexpr CommandInfo commands[] {
    { Command::LooperRec, "looper_rec", "Looper: Rec" },
    { Command::LooperOverdub, "looper_overdub", "Looper: Overdub" },
    { Command::LooperPlay, "looper_play", "Looper: Play track" },
    { Command::LooperStop, "looper_stop", "Looper: Stop track" },
    { Command::LooperUndo, "looper_undo", "Looper: Undo" },
    { Command::LooperRedo, "looper_redo", "Looper: Redo" },
    { Command::LooperClear, "looper_clear", "Looper: Clear track" },
    { Command::LooperPlayStopAll, "looper_play_stop_all", "Looper: Play / Stop all" },
    { Command::LooperClearAll, "looper_clear_all", "Looper: Clear all (press twice)" },
    { Command::TrackNext, "track_next", "Looper: Next track" },
    { Command::TrackPrevious, "track_previous", "Looper: Previous track" },
    { Command::Track1, "track_1", "Looper: Select track 1" },
    { Command::Track2, "track_2", "Looper: Select track 2" },
    { Command::Track3, "track_3", "Looper: Select track 3" },
    { Command::Track4, "track_4", "Looper: Select track 4" },
    { Command::Track5, "track_5", "Looper: Select track 5" },
    { Command::Track6, "track_6", "Looper: Select track 6" },
    { Command::Track7, "track_7", "Looper: Select track 7" },
    { Command::Track8, "track_8", "Looper: Select track 8" },
    { Command::TapTempo, "tap_tempo", "Tap tempo" },
    { Command::Metronome, "metronome", "Metronome on / off" },
    { Command::PresetNext, "preset_next", "Next preset" },
    { Command::PresetPrevious, "preset_previous", "Previous preset" },
    { Command::Tuner, "tuner", "Tuner on / off" },
};
static_assert (std::size (commands) == (size_t) numCommands);

constexpr std::string_view commandPrefix = "cmd:";

std::vector<std::string_view> tokens (std::string_view line)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            ++i;
        if (i > start)
            out.push_back (line.substr (start, i - start));
    }
    return out;
}

bool parseInt (std::string_view s, int lo, int hi, int& out)
{
    if (s.empty() || s.size() > 4)
        return false;
    int v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi)
        return false;
    out = v;
    return true;
}

float skewFor (const ParamSpec& p)
{
    if (p.centre <= 0.0f || p.centre <= p.min || p.centre >= p.max) // 0: linear (see ParamLayout)
        return 1.0f;
    return (float) (std::log (0.5) / std::log ((double) (p.centre - p.min) / (double) (p.max - p.min)));
}
} // namespace

std::string_view commandKey (Command c) { return commands[(size_t) c].key; }
std::string_view commandName (Command c) { return commands[(size_t) c].name; }

std::optional<Command> findCommand (std::string_view key)
{
    for (const auto& c : commands)
        if (c.key == key)
            return c.command;
    return std::nullopt;
}

bool Target::valid() const
{
    return kind == Kind::Param ? index >= 0 && index < numParams : index >= 0 && index < numCommands;
}

bool Target::isSwitch() const
{
    return kind == Kind::Command || paramInfo (pid()).type == ParamType::Bool;
}

std::string targetKey (Target t)
{
    if (t.isParam())
        return std::string (paramInfo (t.pid()).id);
    return std::string (commandPrefix) + std::string (commandKey (t.cmd()));
}

std::string targetName (Target t)
{
    return t.isParam() ? std::string (paramInfo (t.pid()).name) : std::string (commandName (t.cmd()));
}

std::optional<Target> parseTarget (std::string_view key)
{
    if (key.starts_with (commandPrefix))
    {
        if (const auto c = findCommand (key.substr (commandPrefix.size())))
            return Target::command (*c);
        return std::nullopt;
    }
    if (const auto* info = findParam (key))
        return Target::param (info->pid);
    return std::nullopt;
}

std::string sourceName (Source s)
{
    if (s.type == Source::Type::Cc)
        return "CC " + std::to_string (s.number) + " (ch " + std::to_string (s.channel) + ")";
    constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string ("Note ") + names[s.number % 12] + std::to_string (s.number / 12 - 1) + " (ch " + std::to_string (s.channel) + ")";
}

std::string serialise (const Settings& s)
{
    std::ostringstream out;
    out << "channel " << (s.channel == 0 ? std::string ("omni") : std::to_string (s.channel)) << "\n";
    out << "program_change " << (s.programChange ? "on" : "off") << "\n";
    for (const auto& m : s.mappings)
    {
        out << "map " << (m.source.type == Source::Type::Cc ? "cc " : "note ") << m.source.channel << " " << m.source.number << " "
            << targetKey (m.target);
        if (m.target.isSwitch())
            out << (m.mode == SwitchMode::Latching ? " latching" : " momentary");
        out << "\n";
    }
    return out.str();
}

Settings parseSettings (std::string_view text, std::vector<std::string>* warnings)
{
    Settings s;
    int lineNumber = 0;
    auto warn = [&] (const std::string& what)
    {
        if (warnings != nullptr)
            warnings->push_back ("line " + std::to_string (lineNumber) + ": " + what);
    };
    while (! text.empty())
    {
        const auto end = text.find ('\n');
        const auto line = text.substr (0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr (end + 1);
        ++lineNumber;
        const auto t = tokens (line);
        if (t.empty() || t[0].starts_with ("#"))
            continue;
        if (t[0] == "channel" && t.size() == 2)
        {
            int ch = 0;
            if (t[1] == "omni")
                s.channel = 0;
            else if (parseInt (t[1], 1, 16, ch))
                s.channel = ch;
            else
                warn ("bad channel");
        }
        else if (t[0] == "program_change" && t.size() == 2 && (t[1] == "on" || t[1] == "off"))
        {
            s.programChange = t[1] == "on";
        }
        else if (t[0] == "map" && (t.size() == 5 || t.size() == 6))
        {
            Mapping m;
            if (t[1] == "cc")
                m.source.type = Source::Type::Cc;
            else if (t[1] == "note")
                m.source.type = Source::Type::Note;
            else
            {
                warn ("bad source type");
                continue;
            }
            const auto target = parseTarget (t[4]);
            if (! parseInt (t[2], 1, 16, m.source.channel) || ! parseInt (t[3], 0, 127, m.source.number) || ! target)
            {
                warn ("bad mapping");
                continue;
            }
            m.target = *target;
            if (t.size() == 6)
                m.mode = t[5] == "latching" ? SwitchMode::Latching : SwitchMode::Momentary;
            // One target per source
            std::erase_if (s.mappings, [&m] (const Mapping& o) { return o.source == m.source; });
            if ((int) s.mappings.size() < MidiControl::maxMappings)
                s.mappings.push_back (m);
        }
        else
        {
            warn ("unknown line");
        }
    }
    return s;
}

float toNormalised (const ParamSpec& p, float value)
{
    if (p.max <= p.min)
        return 0.0f;
    const float proportion = std::clamp ((value - p.min) / (p.max - p.min), 0.0f, 1.0f);
    const float skew = p.type == ParamType::Float ? skewFor (p) : 1.0f;
    return skew == 1.0f ? proportion : std::pow (proportion, skew);
}

float fromNormalised (const ParamSpec& p, float normalised)
{
    normalised = std::clamp (normalised, 0.0f, 1.0f);
    switch (p.type)
    {
        case ParamType::Bool: return normalised >= 0.5f ? 1.0f : 0.0f;
        case ParamType::Choice: return std::nearbyint (normalised * (p.max - p.min)) + p.min; // halves to even, as JUCE's roundToInt
        case ParamType::Float: break;
    }
    const float skew = skewFor (p);
    if (skew != 1.0f && normalised > 0.0f)
        normalised = std::exp (std::log (normalised) / skew);
    float v = p.min + (p.max - p.min) * normalised;
    if (p.step > 0.0f)
        v = p.min + p.step * std::floor ((v - p.min) / p.step + 0.5f);
    return std::clamp (v, p.min, p.max);
}

// ---- The engine ----

MidiControl::MidiControl()
{
    for (auto& s : slots)
        s.store (0, std::memory_order_relaxed);
    for (auto& a : acknowledged)
        a.store (0, std::memory_order_relaxed);
}

// valid (1) | type (1) | channel - 1 (4) | number (7) | kind (1) | index (16) | mode (1)
uint64_t MidiControl::pack (const Mapping& m)
{
    return 1u | (uint64_t) (m.source.type == Source::Type::Note) << 1 | (uint64_t) ((m.source.channel - 1) & 15) << 2
         | (uint64_t) (m.source.number & 127) << 6 | (uint64_t) (m.target.kind == Target::Kind::Command) << 13
         | (uint64_t) (m.target.index & 0xffff) << 14 | (uint64_t) (m.mode == SwitchMode::Latching) << 30;
}

std::optional<Mapping> MidiControl::unpack (uint64_t v)
{
    if ((v & 1u) == 0)
        return std::nullopt;
    Mapping m;
    m.source.type = (v >> 1 & 1u) != 0 ? Source::Type::Note : Source::Type::Cc;
    m.source.channel = (int) (v >> 2 & 15u) + 1;
    m.source.number = (int) (v >> 6 & 127u);
    m.target.kind = (v >> 13 & 1u) != 0 ? Target::Kind::Command : Target::Kind::Param;
    m.target.index = (int) (v >> 14 & 0xffffu);
    m.mode = (v >> 30 & 1u) != 0 ? SwitchMode::Latching : SwitchMode::Momentary;
    if (! m.target.valid())
        return std::nullopt;
    return m;
}

void MidiControl::setSettings (const Settings& s)
{
    setChannel (s.channel);
    setProgramChange (s.programChange);
    // Emptied first, then filled in order: the audio thread may see some missing for a moment,
    // never one twice
    for (auto& slot : slots)
        slot.store (0, std::memory_order_release);
    for (const auto& m : s.mappings)
        addMapping (m);
}

Settings MidiControl::settings() const
{
    Settings s;
    s.channel = channel();
    s.programChange = programChange();
    for (const auto& slot : slots)
        if (const auto m = unpack (slot.load (std::memory_order_acquire)))
            s.mappings.push_back (*m);
    return s;
}

void MidiControl::setChannel (int ch)
{
    channelSetting.store (std::clamp (ch, 0, 16), std::memory_order_relaxed);
}

int MidiControl::findSlot (const Source& source) const
{
    for (int i = 0; i < maxMappings; ++i)
        if (const auto m = unpack (slots[(size_t) i].load (std::memory_order_acquire)); m && m->source == source)
            return i;
    return -1;
}

bool MidiControl::addMapping (const Mapping& m)
{
    if (! m.target.valid() || m.source.channel < 1 || m.source.channel > 16 || m.source.number < 0 || m.source.number > 127)
        return false;
    int slot = findSlot (m.source);
    for (int i = 0; slot < 0 && i < maxMappings; ++i)
        if (slots[(size_t) i].load (std::memory_order_acquire) == 0)
            slot = i;
    if (slot < 0)
        return false;
    slots[(size_t) slot].store (pack (m), std::memory_order_release);
    return true;
}

void MidiControl::removeMapping (const Mapping& m)
{
    if (const int slot = findSlot (m.source); slot >= 0)
        slots[(size_t) slot].store (0, std::memory_order_release);
}

void MidiControl::removeMappings (Target t)
{
    for (auto& slot : slots)
        if (const auto m = unpack (slot.load (std::memory_order_acquire)); m && m->target == t)
            slot.store (0, std::memory_order_release);
}

std::vector<Mapping> MidiControl::mappingsFor (Target t) const
{
    std::vector<Mapping> out;
    for (const auto& slot : slots)
        if (const auto m = unpack (slot.load (std::memory_order_acquire)); m && m->target == t)
            out.push_back (*m);
    return out;
}

void MidiControl::setMode (const Mapping& m, SwitchMode mode)
{
    if (const int slot = findSlot (m.source); slot >= 0)
        if (auto current = unpack (slots[(size_t) slot].load (std::memory_order_acquire)))
        {
            current->mode = mode;
            slots[(size_t) slot].store (pack (*current), std::memory_order_release);
        }
}

void MidiControl::learn (Target t)
{
    if (t.valid())
        learnTarget.store (1 + ((t.kind == Target::Kind::Command ? 1 : 0) << 16 | t.index), std::memory_order_relaxed);
}

std::optional<Target> MidiControl::learning() const
{
    const int v = learnTarget.load (std::memory_order_relaxed);
    if (v == 0)
        return std::nullopt;
    return Target { ((v - 1) >> 16) != 0 ? Target::Kind::Command : Target::Kind::Param, (v - 1) & 0xffff };
}

void MidiControl::acknowledge (const ParamChange& c)
{
    auto& a = acknowledged[(size_t) c.pid];
    // Sequences only grow (wrapping is decades away at any plausible rate)
    if (c.sequence > a.load (std::memory_order_relaxed))
        a.store (c.sequence, std::memory_order_release);
}

std::array<uint8_t, 3> MidiControl::lastMessage() const
{
    const auto v = lastBytes.load (std::memory_order_relaxed);
    return { (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
}

// ---- Audio thread ----

void MidiControl::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void MidiControl::beginBlock (Patch& patch)
{
    const auto timeout = (int64_t) (overrideTimeoutSeconds * rate);
    for (size_t i = 0; i < overrides.size(); ++i)
    {
        auto& o = overrides[i];
        if (o.sequence == 0)
            continue;
        if (acknowledged[i].load (std::memory_order_acquire) >= o.sequence || elapsed - o.since > timeout)
            o.sequence = 0; // the host's value carries it now
        else
            patch.v[i] = o.value;
    }
}

void MidiControl::post (Request::Type type, int value, const Mapping& m)
{
    requests.push ({ type, value, m });
}

void MidiControl::setParam (PID pid, float value, Patch& patch)
{
    const auto& info = paramInfo (pid);
    const float normalised = toNormalised (info, value);
    value = fromNormalised (info, normalised); // as the host parameter will hold it
    patch[pid] = value;
    auto& o = overrides[(size_t) pid];
    o.value = value;
    o.sequence = ++sequence;
    o.since = elapsed;
    if (! paramChanges.push ({ pid, normalised, o.sequence }))
        o.since = elapsed - (int64_t) (overrideTimeoutSeconds * rate) + (int64_t) (0.05 * rate); // lost: lapses soon
}

bool MidiControl::tryLearn (Source source)
{
    int v = learnTarget.load (std::memory_order_relaxed);
    if (v == 0)
        return false;
    if (! learnTarget.compare_exchange_strong (v, 0, std::memory_order_relaxed))
        return false; // changed meanwhile: the next message learns
    Mapping m;
    m.source = source;
    m.target = { ((v - 1) >> 16) != 0 ? Target::Kind::Command : Target::Kind::Param, (v - 1) & 0xffff };
    post (Request::Type::Learned, 0, m);
    return true;
}

void MidiControl::handle (const uint8_t* data, int size, int offset, Patch& patch, Looper& looper)
{
    if (size < 2 || data[0] < 0x80 || data[0] >= 0xf0)
        return; // system messages (clock, sysex...) aren't used
    const int status = data[0] & 0xf0, ch = (data[0] & 0x0f) + 1;
    const int filter = channel();
    if (filter != 0 && ch != filter)
        return;
    const int d1 = data[1] & 0x7f, d2 = size > 2 ? data[2] & 0x7f : 0;
    const double ms = (double) (elapsed + offset) * 1000.0 / rate;

    bool used = true;
    switch (status)
    {
        case 0xb0: handleController (ch, d1, d2, ms, patch, looper); break;
        case 0x90: handleNote (ch, d1, d2, ms, patch, looper); break;
        case 0x80: handleNote (ch, d1, 0, ms, patch, looper); break;
        case 0xc0:
            if (programChange())
                post (Request::Type::Preset, (bankMsb * 128 + bankLsb) * 128 + d1);
            break;
        default: used = false; break;
    }
    if (used)
    {
        lastBytes.store ((uint32_t) data[0] << 16 | (uint32_t) d1 << 8 | (uint32_t) d2, std::memory_order_relaxed);
        activityCount.fetch_add (1, std::memory_order_relaxed);
    }
}

void MidiControl::handleController (int ch, int number, int value, double ms, Patch& patch, Looper& looper)
{
    if (programChange() && (number == 0 || number == 32))
    {
        (number == 0 ? bankMsb : bankLsb) = value;
        return;
    }
    if (number >= 120)
        return; // channel mode messages
    const Source source { Source::Type::Cc, ch, number };
    if (tryLearn (source))
        return;
    const bool high = value >= 64;
    for (int i = 0; i < maxMappings; ++i)
    {
        const auto packed = slots[(size_t) i].load (std::memory_order_acquire);
        if (packed == 0)
            continue;
        const auto m = unpack (packed);
        if (! m || m->source != source)
            continue;
        auto& state = slotStates[(size_t) i];
        if (state.seen != packed)
            state = { packed, false, false };
        const bool press = m->mode == SwitchMode::Latching ? ! state.known || high != state.high
                                                           : high && (! state.known || ! state.high);
        state.known = true;
        state.high = high;
        act (*m, press, (float) value / 127.0f, ms, patch, looper);
    }
}

void MidiControl::handleNote (int ch, int number, int velocity, double ms, Patch& patch, Looper& looper)
{
    const Source source { Source::Type::Note, ch, number };
    if (velocity > 0 && tryLearn (source))
        return;
    for (int i = 0; i < maxMappings; ++i)
    {
        const auto packed = slots[(size_t) i].load (std::memory_order_acquire);
        if (packed == 0)
            continue;
        const auto m = unpack (packed);
        if (! m || m->source != source)
            continue;
        auto& state = slotStates[(size_t) i];
        state = { packed, true, velocity > 0 };
        if (velocity > 0) // note offs only end the press
            act (*m, true, (float) velocity / 127.0f, ms, patch, looper);
    }
}

void MidiControl::act (const Mapping& m, bool press, float normalised, double ms, Patch& patch, Looper& looper)
{
    if (! m.target.isSwitch())
    {
        setParam (m.target.pid(), fromNormalised (paramInfo (m.target.pid()), normalised), patch);
        return;
    }
    if (! press)
        return;
    if (m.target.isParam())
        setParam (m.target.pid(), patch.on (m.target.pid()) ? 0.0f : 1.0f, patch);
    else
        runCommand (m.target.cmd(), ms, patch, looper);
}

void MidiControl::runCommand (Command c, double ms, Patch& patch, Looper& looper)
{
    using A = Looper::Action;
    const int track = std::clamp (patch.index (PID::LooperTrack), 0, Looper::numTracks - 1);
    switch (c)
    {
        case Command::LooperRec: looper.trigger (A::Rec, track); break;
        case Command::LooperOverdub:
            if (looper.trackStatus (track).state != Looper::State::Empty)
                looper.trigger (A::Rec, track);
            break;
        case Command::LooperPlay: looper.trigger (A::Play, track); break;
        case Command::LooperStop: looper.trigger (A::Stop, track); break;
        case Command::LooperUndo: looper.trigger (A::Undo, track); break;
        case Command::LooperRedo: looper.trigger (A::Redo, track); break;
        case Command::LooperClear: looper.trigger (A::Clear, track); break;
        case Command::LooperPlayStopAll: looper.trigger (A::PlayStop); break;
        case Command::LooperClearAll:
            if (ms - clearAllAskedMs <= clearAllConfirmSeconds * 1000.0)
            {
                clearAllAskedMs = -1.0e9;
                looper.trigger (A::ClearAll);
                post (Request::Type::ClearAllAsking, 0);
            }
            else
            {
                clearAllAskedMs = ms;
                post (Request::Type::ClearAllAsking, 1);
            }
            break;
        case Command::TrackNext: setParam (PID::LooperTrack, (float) ((track + 1) % Looper::numTracks), patch); break;
        case Command::TrackPrevious:
            setParam (PID::LooperTrack, (float) ((track + Looper::numTracks - 1) % Looper::numTracks), patch);
            break;
        case Command::Track1: case Command::Track2: case Command::Track3: case Command::Track4:
        case Command::Track5: case Command::Track6: case Command::Track7: case Command::Track8:
            setParam (PID::LooperTrack, (float) ((int) c - (int) Command::Track1), patch);
            break;
        case Command::TapTempo:
            if (const auto bpm = tapper.tap (ms))
            {
                if (patch.on (PID::TempoFollowHost))
                    setParam (PID::TempoFollowHost, 0.0f, patch);
                setParam (PID::Tempo, (float) (std::round (*bpm * 10.0) / 10.0), patch);
            }
            break;
        case Command::Metronome: setParam (PID::MetronomeOn, patch.on (PID::MetronomeOn) ? 0.0f : 1.0f, patch); break;
        case Command::PresetNext: post (Request::Type::PresetStep, 1); break;
        case Command::PresetPrevious: post (Request::Type::PresetStep, -1); break;
        case Command::Tuner: post (Request::Type::Tuner); break;
        case Command::Count: break;
    }
}

} // namespace damp::midi
