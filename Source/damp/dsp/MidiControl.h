#pragma once

#include "Params.h"
#include "looper/Looper.h"
#include "looper/TempoGrid.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace damp::midi
{

// MIDI control: Program Change picks a preset, and learned CCs and notes drive parameters,
// pedal switches and commands (the looper's transport, track selection, tap tempo, presets, the
// tuner), so damp can be played hands-free from a controller or a MIDI footswitch.
//
// Sources. A CC or a note, on one channel (1-16); the channel setting (omni: 0) filters every
// message first. With Program Change on, CC 0 and 32 are Bank Select (MSB, LSB) and never map:
// a program change selects preset (bank * 128 + program), bank = MSB * 128 + LSB, counting the
// preset menu's list (factory presets, then the user's).
//
// Targets. A parameter, or a Command. A continuous parameter (a number or a choice) follows a
// CC's value (0-127 across its range, as the host's normalised value) or a note's velocity.
// Switches (on / off parameters and commands) act on a press: Momentary, a CC going from below 64
// to 64 or more (a footswitch sending 127 on press and 0 on release acts once), or a note on;
// Latching, every change across 64 (a switch that sends 127 on one press and 0 on the next acts on
// both) or a note on. A switch parameter toggles; Clear all asks first: a second press within
// clearAllConfirmSeconds clears.
//
// Learning. learn (target) arms it; the next CC (not Bank Select) or note on, on an accepted
// channel, is posted back as a Request::Learned instead of acting, and the message thread adds it
// (addMapping). A source maps to one target: learning it again replaces the mapping.
//
// Threads. The mappings live in a fixed table of atomics (one per mapping) written only by the
// message thread, so the audio thread reads them without locks, and handles messages without
// allocating. What it can't do itself goes to the message thread through two lock-free queues:
// Requests (presets, the tuner, learned mappings, Clear all asking) and ParamChanges. A parameter
// MIDI sets takes effect at once (beginBlock lays it over the patch the host's values make) and is
// posted as a ParamChange; the message thread sets the host parameter (setValueNotifyingHost)
// and acknowledges it, and from then on the host's value carries it. Unacknowledged after
// overrideTimeoutSeconds (a message thread that never runs), the override lapses.

enum class Command : int
{
    LooperRec,         // the selected track: record, close and play, overdub, play...
    LooperOverdub,     // the selected track, if it has a loop: overdub on / off (else nothing)
    LooperPlay,        // the selected track
    LooperStop,        // the selected track
    LooperUndo, LooperRedo, LooperClear,
    LooperPlayStopAll, // every track
    LooperClearAll,    // every track and its history, and the loop length (asks first)
    TrackNext, TrackPrevious,
    Track1, Track2, Track3, Track4, Track5, Track6, Track7, Track8,
    TapTempo,
    Metronome,         // the click on / off
    PresetNext, PresetPrevious,
    Tuner,             // the editor's tuner (only while the editor is open)
    Count
};
constexpr int numCommands = (int) Command::Count;
std::string_view commandKey (Command);  // persisted: never rename
std::string_view commandName (Command); // shown
std::optional<Command> findCommand (std::string_view key);

struct Target
{
    enum class Kind : uint8_t { Param, Command };
    Kind kind = Kind::Param;
    int index = 0; // a PID or a Command

    static Target param (PID p) { return { Kind::Param, (int) p }; }
    static Target command (Command c) { return { Kind::Command, (int) c }; }
    bool isParam() const { return kind == Kind::Param; }
    PID pid() const { return (PID) index; }
    Command cmd() const { return (Command) index; }
    bool valid() const;
    bool isSwitch() const; // a command or an on / off parameter: acts on presses
    bool operator== (const Target&) const = default;
};
std::string targetKey (Target);  // "wah_position", "cmd:looper_rec"
std::string targetName (Target); // "Wah: Pedal Position", "Looper: Rec"
std::optional<Target> parseTarget (std::string_view key);

struct Source
{
    enum class Type : uint8_t { Cc, Note };
    Type type = Type::Cc;
    int channel = 1; // 1-16
    int number = 0;  // 0-127
    bool operator== (const Source&) const = default;
};
std::string sourceName (Source); // "CC 64 (ch 1)", "Note C1 (ch 10)"

enum class SwitchMode : uint8_t { Momentary, Latching };

struct Mapping
{
    Source source;
    Target target;
    SwitchMode mode = SwitchMode::Momentary;
    bool operator== (const Mapping&) const = default;
};

// Everything MIDI control keeps: in the machine-wide settings file and in sessions
struct Settings
{
    int channel = 0; // 0: omni
    bool programChange = true;
    std::vector<Mapping> mappings;
    bool operator== (const Settings&) const = default;
};
// Text, a line each: "channel omni|1-16", "program_change on|off", "map cc|note <ch> <number>
// <target> [momentary|latching]". Unknown or bad lines are skipped (with a warning).
std::string serialise (const Settings&);
Settings parseSettings (std::string_view, std::vector<std::string>* warnings = nullptr);

// Host-normalised (0..1) values for a parameter, as JUCE's ranges convert them (skew for the
// centre, steps; choices and switches by index)
float toNormalised (const ParamSpec&, float value);
float fromNormalised (const ParamSpec&, float normalised);

// A single-producer, single-consumer queue of N - 1 items
template <typename T, size_t N>
class SpscQueue
{
public:
    bool push (const T& item)
    {
        const auto w = write.load (std::memory_order_relaxed);
        const auto next = (w + 1) % N;
        if (next == read.load (std::memory_order_acquire))
            return false;
        items[w] = item;
        write.store (next, std::memory_order_release);
        return true;
    }
    bool pop (T& item)
    {
        const auto r = read.load (std::memory_order_relaxed);
        if (r == write.load (std::memory_order_acquire))
            return false;
        item = items[r];
        read.store ((r + 1) % N, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> items {};
    std::atomic<size_t> write { 0 }, read { 0 };
};

class MidiControl
{
public:
    static constexpr int maxMappings = 128;
    static constexpr double clearAllConfirmSeconds = 3.0;
    static constexpr double overrideTimeoutSeconds = 2.0;

    struct Request
    {
        enum class Type : uint8_t { Preset, PresetStep, Tuner, Learned, ClearAllAsking };
        Type type = Type::Preset;
        int value = 0;   // Preset: the index; PresetStep: +1 / -1
        Mapping mapping; // Learned
    };
    struct ParamChange
    {
        PID pid = PID {};
        float normalised = 0.0f;
        uint32_t sequence = 0;
    };

    MidiControl();

    // ---- Message thread ----
    void setSettings (const Settings&); // replaces everything
    Settings settings() const;          // mappings in table order
    void setChannel (int channel);      // 0: omni
    int channel() const { return channelSetting.load (std::memory_order_relaxed); }
    void setProgramChange (bool on) { programChangeOn.store (on, std::memory_order_relaxed); }
    bool programChange() const { return programChangeOn.load (std::memory_order_relaxed); }
    // Replaces any mapping with the same source; false when the table is full
    bool addMapping (const Mapping&);
    void removeMapping (const Mapping&);
    void removeMappings (Target); // "Forget": every source mapped to it
    std::vector<Mapping> mappingsFor (Target) const;
    void setMode (const Mapping&, SwitchMode);

    void learn (Target);
    void cancelLearn() { learnTarget.store (0, std::memory_order_relaxed); }
    std::optional<Target> learning() const;

    bool popRequest (Request& r) { return requests.pop (r); }
    // Set the host parameter, then acknowledge
    bool popParamChange (ParamChange& c) { return paramChanges.pop (c); }
    void acknowledge (const ParamChange&);

    // Messages accepted so far (any thread), and the last one (status, data1, data2)
    uint32_t activity() const { return activityCount.load (std::memory_order_relaxed); }
    std::array<uint8_t, 3> lastMessage() const;

    // ---- Audio thread ----
    void prepare (double sampleRate);
    // Start of a block: lays the values MIDI set, not yet carried by the host's, over the patch
    void beginBlock (Patch&);
    // One MIDI message, `offset` frames into the block: may change the patch (the block's values),
    // trigger the looper and post requests
    void handle (const uint8_t* data, int size, int offset, Patch&, Looper&);
    void endBlock (int frames) { elapsed += frames; }

private:
    static uint64_t pack (const Mapping&);
    static std::optional<Mapping> unpack (uint64_t);
    int findSlot (const Source&) const;

    void handleController (int channel, int number, int value, double ms, Patch&, Looper&);
    void handleNote (int channel, int number, int velocity, double ms, Patch&, Looper&);
    void act (const Mapping&, bool press, float normalised, double ms, Patch&, Looper&);
    void runCommand (Command, double ms, Patch&, Looper&);
    void setParam (PID, float value, Patch&);
    void post (Request::Type, int value = 0, const Mapping& = {});
    bool tryLearn (Source);

    // The table (message thread writes, audio thread reads); 0: an empty slot
    std::array<std::atomic<uint64_t>, maxMappings> slots;
    std::atomic<int> channelSetting { 0 };
    std::atomic<bool> programChangeOn { true };
    std::atomic<int> learnTarget { 0 }; // 0: not learning, else 1 + (kind << 16 | index)
    std::atomic<uint32_t> activityCount { 0 }, lastBytes { 0 };

    SpscQueue<Request, 64> requests;
    SpscQueue<ParamChange, 1024> paramChanges;
    std::array<std::atomic<uint32_t>, numParams> acknowledged;

    // Audio thread
    double rate = 48000.0;
    int64_t elapsed = 0;
    struct SlotState
    {
        uint64_t seen = 0; // the slot this state is for (reset when it changes)
        bool known = false, high = false;
    };
    std::array<SlotState, maxMappings> slotStates {};
    struct Override
    {
        float value = 0.0f;
        uint32_t sequence = 0; // 0: none
        int64_t since = 0;
    };
    std::array<Override, numParams> overrides {};
    uint32_t sequence = 0;
    int bankMsb = 0, bankLsb = 0;
    double clearAllAskedMs = -1.0e9;
    tempo::TapTempo tapper;
};

} // namespace damp::midi
