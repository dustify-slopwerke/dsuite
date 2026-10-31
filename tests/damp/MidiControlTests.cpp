#include "dsp/MidiControl.h"

#include "AllocationCounter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace damp;
using namespace damp::midi;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr int block = 64;

struct Event
{
    std::vector<uint8_t> bytes;
    int offset = 0;
};

Event cc (int ch, int number, int value, int offset = 0) { return { { (uint8_t) (0xb0 | (ch - 1)), (uint8_t) number, (uint8_t) value }, offset }; }
Event noteOn (int ch, int note, int velocity = 100, int offset = 0) { return { { (uint8_t) (0x90 | (ch - 1)), (uint8_t) note, (uint8_t) velocity }, offset }; }
Event noteOff (int ch, int note, int offset = 0) { return { { (uint8_t) (0x80 | (ch - 1)), (uint8_t) note, 0 }, offset }; }
Event program (int ch, int number, int offset = 0) { return { { (uint8_t) (0xc0 | (ch - 1)), (uint8_t) number }, offset }; }

Mapping map (Source::Type type, int ch, int number, Target target, SwitchMode mode = SwitchMode::Momentary)
{
    return { { type, ch, number }, target, mode };
}

// The processor's part, in one thread: `host` holds the host's parameter values; each block lays
// MIDI's over them, then the "message thread" sets the host's values from the changes posted and
// acknowledges them (unless `pumping` is off), and collects the requests
struct Harness
{
    MidiControl midi;
    Looper looper;
    Patch host = Patch::defaults();
    Patch heard = Patch::defaults(); // the last block's values
    std::vector<MidiControl::Request> requests;
    bool pumping = true, learnAutomatically = true;
    int64_t frame = 0;

    Harness()
    {
        midi.prepare (rate);
        looper.setIdleTimeout (0.0);
        looper.prepare (rate);
        host[PID::LooperOn] = 1.0f;
        looper.setPatch (host);
        looper.createPool (rate, 20.0);
        for (int t = 0; t < Looper::numTracks; ++t)
            looper.setTrackCrossfade (t, 0.0f);
    }

    void run (const std::vector<Event>& events = {}, int blocks = 1)
    {
        for (int b = 0; b < blocks; ++b)
        {
            auto patch = host;
            midi.beginBlock (patch);
            if (b == 0)
                for (const auto& e : events)
                    midi.handle (e.bytes.data(), (int) e.bytes.size(), e.offset, patch, looper);
            midi.endBlock (block);
            heard = patch;
            std::array<float, block> l {}, r {};
            looper.setPatch (patch);
            looper.process (l.data(), r.data(), block);
            frame += block;
            pump();
        }
    }

    void pump()
    {
        if (! pumping)
            return;
        MidiControl::ParamChange c;
        while (midi.popParamChange (c))
        {
            host[c.pid] = fromNormalised (paramInfo (c.pid), c.normalised);
            midi.acknowledge (c);
        }
        MidiControl::Request r;
        while (midi.popRequest (r))
        {
            if (r.type == MidiControl::Request::Type::Learned && learnAutomatically)
                midi.addMapping (r.mapping);
            requests.push_back (r);
        }
    }

    std::vector<int> presets() const
    {
        std::vector<int> out;
        for (const auto& r : requests)
            if (r.type == MidiControl::Request::Type::Preset)
                out.push_back (r.value);
        return out;
    }
};
} // namespace

TEST_CASE ("midi: program change selects a preset, with bank select", "[midi]")
{
    Harness h;
    h.run ({ program (1, 5) });
    CHECK (h.presets() == std::vector<int> { 5 });

    // Bank select: MSB 1, LSB 2 -> bank 130
    h.run ({ cc (1, 0, 1), cc (1, 32, 2), program (1, 3) });
    CHECK (h.presets() == std::vector<int> { 5, (1 * 128 + 2) * 128 + 3 });
    h.run ({ cc (1, 0, 0), cc (1, 32, 1), program (1, 0) });
    CHECK (h.presets().back() == 128);

    // Bank select never maps while program change is on
    h.midi.learn (Target::param (PID::WahPosition));
    h.run ({ cc (1, 0, 0) });
    CHECK (h.midi.learning().has_value());
    h.midi.cancelLearn();

    // Off: program changes are ignored
    h.midi.setProgramChange (false);
    h.run ({ program (1, 7) });
    CHECK (h.presets().size() == 3);
}

TEST_CASE ("midi: the channel setting filters every message", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Cc, 1, 11, Target::param (PID::WahPosition)));
    h.midi.addMapping (map (Source::Type::Cc, 2, 11, Target::param (PID::WahPosition)));

    h.midi.setChannel (2);
    h.run ({ cc (1, 11, 0), program (1, 4) });
    CHECK (h.host[PID::WahPosition] == Approx (5.0f)); // untouched
    CHECK (h.presets().empty());
    CHECK (h.midi.activity() == 0);

    h.run ({ cc (2, 11, 127), program (2, 4) });
    CHECK (h.host[PID::WahPosition] == Approx (10.0f));
    CHECK (h.presets() == std::vector<int> { 4 });
    CHECK (h.midi.activity() == 2);

    // Omni: both channels, each through its own mapping; a mapping only hears its own channel
    h.midi.setChannel (0);
    h.run ({ cc (1, 11, 0) });
    CHECK (h.host[PID::WahPosition] == Approx (0.0f));
    h.midi.removeMapping (map (Source::Type::Cc, 1, 11, {}));
    h.run ({ cc (1, 11, 127) });
    CHECK (h.host[PID::WahPosition] == Approx (0.0f));
    h.run ({ cc (3, 11, 127) });
    CHECK (h.host[PID::WahPosition] == Approx (0.0f));
}

TEST_CASE ("midi: learning maps the next CC or note to the target", "[midi]")
{
    Harness h;
    h.midi.learn (Target::param (PID::WahPosition));
    REQUIRE (h.midi.learning() == Target::param (PID::WahPosition));
    h.run ({ cc (3, 11, 100) });
    CHECK_FALSE (h.midi.learning().has_value());
    CHECK (h.heard[PID::WahPosition] == Approx (5.0f)); // learning doesn't act
    const auto mappings = h.midi.settings().mappings;
    REQUIRE (mappings.size() == 1);
    CHECK (mappings[0].source == Source { Source::Type::Cc, 3, 11 });
    CHECK (mappings[0].target == Target::param (PID::WahPosition));
    h.run ({ cc (3, 11, 127) });
    CHECK (h.heard[PID::WahPosition] == Approx (10.0f));

    // A note on (a pad); note offs never learn
    h.midi.learn (Target::command (Command::LooperRec));
    h.run ({ noteOff (10, 40) });
    CHECK (h.midi.learning().has_value());
    h.run ({ noteOn (10, 36) });
    CHECK (h.midi.mappingsFor (Target::command (Command::LooperRec)).size() == 1);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Empty); // learning doesn't act

    // Learning a source again moves it to the new target; Forget removes every source of a target
    h.midi.learn (Target::param (PID::MasterOutput));
    h.run ({ cc (3, 11, 10) });
    CHECK (h.midi.mappingsFor (Target::param (PID::WahPosition)).empty());
    CHECK (h.midi.mappingsFor (Target::param (PID::MasterOutput)).size() == 1);
    CHECK (h.midi.settings().mappings.size() == 2);
    h.midi.removeMappings (Target::param (PID::MasterOutput));
    CHECK (h.midi.settings().mappings.size() == 1);
}

TEST_CASE ("midi: a CC scales across a parameter's range as the host normalises it", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Cc, 1, 1, Target::param (PID::WahPosition)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 2, Target::param (PID::DelayTime)));  // skewed: 300 ms mid-way
    h.midi.addMapping (map (Source::Type::Cc, 1, 3, Target::param (PID::Model)));      // a choice
    h.midi.addMapping (map (Source::Type::Cc, 1, 4, Target::param (PID::DelayMix)));

    h.run ({ cc (1, 1, 0), cc (1, 2, 0), cc (1, 3, 0) });
    CHECK (h.heard[PID::WahPosition] == Approx (0.0f));
    CHECK (h.heard[PID::DelayTime] == Approx (20.0f));
    CHECK (h.heard[PID::Model] == 0.0f);
    h.run ({ cc (1, 1, 127), cc (1, 2, 127), cc (1, 3, 127) });
    CHECK (h.heard[PID::WahPosition] == Approx (10.0f));
    CHECK (h.heard[PID::DelayTime] == Approx (2000.0f));
    CHECK (h.heard[PID::Model] == 4.0f);
    h.run ({ cc (1, 1, 64), cc (1, 2, 64), cc (1, 3, 64) });
    CHECK (h.heard[PID::WahPosition] == Approx (10.0f * 64.0f / 127.0f));
    CHECK (h.heard[PID::DelayTime] == Approx (300.0f).epsilon (0.03)); // about the centre (64 is just past it)
    CHECK (h.heard[PID::Model] == 2.0f);

    // Every parameter: normalised and back is where it was (on its steps)
    for (const auto& info : allParams())
        for (float x : { 0.0f, 0.1f, 0.37f, 0.5f, 0.81f, 1.0f })
        {
            const float v = fromNormalised (info, x);
            CHECK (fromNormalised (info, toNormalised (info, v)) == Approx (v).margin (1.0e-3 * (info.max - info.min)));
        }

    // The host follows (what it holds carries on after the change is acknowledged)
    h.run ({ cc (1, 4, 127) });
    const float mix = h.heard[PID::DelayMix];
    CHECK (h.host[PID::DelayMix] == Approx (mix));
    h.run();
    CHECK (h.heard[PID::DelayMix] == Approx (mix));
}

TEST_CASE ("midi: a value MIDI sets holds until the host carries it", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Cc, 1, 7, Target::param (PID::MasterOutput)));
    h.pumping = false; // a message thread that hasn't run yet
    h.run ({ cc (1, 7, 0) });
    const float set = h.heard[PID::MasterOutput];
    CHECK (set != Approx (h.host[PID::MasterOutput]));
    h.run ({}, 100); // still unacknowledged: still heard
    CHECK (h.heard[PID::MasterOutput] == Approx (set));
    h.pumping = true;
    h.run();
    CHECK (h.host[PID::MasterOutput] == Approx (set));

    // Never acknowledged: it lapses
    h.pumping = false;
    h.run ({ cc (1, 7, 127) });
    CHECK (h.heard[PID::MasterOutput] != Approx (set));
    h.run ({}, (int) (MidiControl::overrideTimeoutSeconds * rate / block) + 2);
    CHECK (h.heard[PID::MasterOutput] == Approx (set));
}

TEST_CASE ("midi: footswitches toggle once per press, momentary or latching", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Cc, 1, 64, Target::param (PID::DelayOn), SwitchMode::Momentary));
    h.midi.addMapping (map (Source::Type::Cc, 1, 65, Target::param (PID::ReverbOn), SwitchMode::Latching));

    // Momentary: 127 on press, 0 on release (each in its own block, or both in one)
    h.run ({ cc (1, 64, 127) });
    CHECK (h.host.on (PID::DelayOn));
    h.run ({ cc (1, 64, 0) });
    CHECK (h.host.on (PID::DelayOn));
    h.run ({ cc (1, 64, 127, 3), cc (1, 64, 0, 40) });
    CHECK_FALSE (h.host.on (PID::DelayOn));
    h.run ({ cc (1, 64, 127), cc (1, 64, 127) }); // a repeated value isn't a press
    CHECK (h.host.on (PID::DelayOn));
    h.run ({ cc (1, 64, 0) });
    CHECK (h.host.on (PID::DelayOn));

    // Latching: 127 on one press, 0 on the next: each acts
    h.run ({ cc (1, 65, 127) });
    CHECK (h.host.on (PID::ReverbOn));
    h.run ({ cc (1, 65, 0) });
    CHECK_FALSE (h.host.on (PID::ReverbOn));
    h.run ({ cc (1, 65, 0) }); // no change, no press
    CHECK_FALSE (h.host.on (PID::ReverbOn));
    h.run ({ cc (1, 65, 127) });
    CHECK (h.host.on (PID::ReverbOn));

    // Switching the mode starts its press detection afresh
    h.midi.setMode (map (Source::Type::Cc, 1, 64, {}), SwitchMode::Latching);
    h.run ({ cc (1, 64, 127) });
    h.run ({ cc (1, 64, 0) });
    CHECK (h.host.on (PID::DelayOn)); // two changes: on, off, on (it started on)
}

TEST_CASE ("midi: notes trigger commands on note on", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Note, 10, 36, Target::command (Command::LooperRec)));
    h.midi.addMapping (map (Source::Type::Note, 10, 37, Target::command (Command::TrackNext)));
    h.midi.addMapping (map (Source::Type::Note, 10, 38, Target::command (Command::Track8)));
    h.midi.addMapping (map (Source::Type::Note, 10, 39, Target::command (Command::Metronome)));
    h.midi.addMapping (map (Source::Type::Note, 10, 40, Target::param (PID::WahPosition)));
    h.midi.addMapping (map (Source::Type::Note, 10, 41, Target::command (Command::Tuner)));
    h.midi.addMapping (map (Source::Type::Note, 10, 42, Target::command (Command::PresetNext)));
    h.midi.addMapping (map (Source::Type::Note, 10, 43, Target::command (Command::PresetPrevious)));

    h.run ({ noteOn (10, 37) });
    CHECK (h.host.index (PID::LooperTrack) == 1);
    h.run ({ noteOff (10, 37) }); // note offs don't act
    CHECK (h.host.index (PID::LooperTrack) == 1);
    h.run ({ noteOn (10, 37, 1) }); // any velocity
    CHECK (h.host.index (PID::LooperTrack) == 2);
    h.run ({ noteOn (10, 38), noteOn (10, 37) }); // to 8, then wraps to 1
    CHECK (h.host.index (PID::LooperTrack) == 0);

    h.run ({ noteOn (10, 39) });
    CHECK (h.host.on (PID::MetronomeOn));
    h.run ({ noteOn (10, 40, 127) }); // a continuous target follows the velocity
    CHECK (h.host[PID::WahPosition] == Approx (10.0f));

    h.run ({ noteOn (10, 41), noteOn (10, 42), noteOn (10, 43) });
    using T = MidiControl::Request::Type;
    REQUIRE (h.requests.size() == 3);
    CHECK (h.requests[0].type == T::Tuner);
    CHECK ((h.requests[1].type == T::PresetStep && h.requests[1].value == 1));
    CHECK ((h.requests[2].type == T::PresetStep && h.requests[2].value == -1));

    // Rec on the selected track (track 1)
    h.run ({ noteOn (10, 36) });
    CHECK (h.looper.trackStatus (0).state == Looper::State::Recording);
}

TEST_CASE ("midi: looper commands act within the block they arrive in", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Cc, 1, 20, Target::command (Command::LooperRec)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 21, Target::command (Command::LooperOverdub)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 22, Target::command (Command::LooperStop)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 23, Target::command (Command::LooperPlay)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 24, Target::command (Command::LooperUndo)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 25, Target::command (Command::LooperClear)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 26, Target::command (Command::LooperRedo)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 27, Target::command (Command::Track2)));

    // Overdub on an empty track does nothing
    h.run ({ cc (1, 21, 127) });
    h.run ({ cc (1, 21, 0) });
    CHECK (h.looper.trackStatus (0).state == Looper::State::Empty);

    // Late in the block: it still records the whole block
    h.run ({ cc (1, 20, 127, block - 1) });
    auto s = h.looper.trackStatus (0);
    CHECK (s.state == Looper::State::Recording);
    CHECK (s.length == block);
    h.run ({ cc (1, 20, 0) }, 99);
    h.run ({ cc (1, 20, 127, 10) }); // closes: the first loop closes at once
    s = h.looper.trackStatus (0);
    CHECK (s.state == Looper::State::Playing);
    CHECK (s.length == 100 * block);

    h.run ({ cc (1, 21, 127) });
    CHECK (h.looper.trackStatus (0).state == Looper::State::Overdubbing);
    h.run ({ cc (1, 21, 0) });
    h.run ({ cc (1, 21, 127) }, 10);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Playing);
    h.run ({ cc (1, 22, 127) }, 10);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Stopped);
    h.run ({ cc (1, 23, 127) }, 10);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Playing);

    // The selected track: select track 2 and record in the same block
    h.run ({ cc (1, 27, 127), cc (1, 20, 0), cc (1, 20, 127) });
    CHECK (h.looper.trackStatus (1).state == Looper::State::Recording);
    h.run ({ cc (1, 20, 0) }, 10);
    h.run ({ cc (1, 24, 127) }, 20); // undo: the recording goes
    CHECK (h.looper.trackStatus (1).state == Looper::State::Empty);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Playing);
}

TEST_CASE ("midi: Clear all asks first; tap tempo", "[midi]")
{
    Harness h;
    h.midi.addMapping (map (Source::Type::Note, 1, 60, Target::command (Command::LooperRec)));
    h.midi.addMapping (map (Source::Type::Note, 1, 61, Target::command (Command::LooperClearAll)));
    h.midi.addMapping (map (Source::Type::Note, 1, 62, Target::command (Command::TapTempo)));
    h.run ({ noteOn (1, 60) }, 50);
    h.run ({ noteOn (1, 60) }, 2);
    REQUIRE (h.looper.trackStatus (0).state == Looper::State::Playing);

    using T = MidiControl::Request::Type;
    h.run ({ noteOn (1, 61) }, 2);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Playing);
    REQUIRE (h.requests.size() == 1);
    CHECK ((h.requests[0].type == T::ClearAllAsking && h.requests[0].value == 1));
    // Too late: it asks again
    h.run ({}, (int) (MidiControl::clearAllConfirmSeconds * rate / block) + 10);
    h.run ({ noteOn (1, 61) }, 2);
    CHECK (h.looper.trackStatus (0).state == Looper::State::Playing);
    h.run ({ noteOn (1, 61) }, 300); // the second press clears (after a dip)
    CHECK (h.looper.trackStatus (0).state == Looper::State::Empty);
    CHECK ((h.requests.back().type == T::ClearAllAsking && h.requests.back().value == 0));

    // Taps half a second apart: 120 BPM, and Follow host goes off
    h.host[PID::TempoFollowHost] = 1.0f;
    const int beat = (int) (0.5 * rate / block); // 375 blocks
    for (int i = 0; i < 4; ++i)
        h.run ({ noteOn (1, 62) }, beat);
    CHECK (h.host[PID::Tempo] == Approx (120.0f));
    CHECK_FALSE (h.host.on (PID::TempoFollowHost));
}

TEST_CASE ("midi: settings round-trip as text", "[midi]")
{
    Settings s;
    s.channel = 10;
    s.programChange = false;
    s.mappings = { map (Source::Type::Cc, 1, 11, Target::param (PID::WahPosition)),
                   map (Source::Type::Cc, 16, 64, Target::param (PID::DelayOn), SwitchMode::Latching),
                   map (Source::Type::Note, 10, 36, Target::command (Command::LooperRec)),
                   map (Source::Type::Note, 10, 0, Target::command (Command::Tuner)),
                   map (Source::Type::Cc, 2, 127, Target::command (Command::LooperClearAll), SwitchMode::Latching) };
    const auto text = serialise (s);
    std::vector<std::string> warnings;
    CHECK (parseSettings (text, &warnings) == s);
    CHECK (warnings.empty());
    CHECK (serialise (parseSettings (text)) == text);

    // Through the engine (table order kept)
    MidiControl m;
    m.setSettings (s);
    CHECK (m.settings() == s);
    m.setSettings ({});
    CHECK (m.settings() == Settings {});

    // Every command and parameter key parses back
    for (int c = 0; c < numCommands; ++c)
        CHECK (parseTarget (targetKey (Target::command ((Command) c))) == Target::command ((Command) c));
    for (const auto& info : allParams())
        CHECK (parseTarget (targetKey (Target::param (info.pid))) == Target::param (info.pid));

    // Bad lines are skipped
    const auto parsed = parseSettings ("channel 17\nmap cc 1 200 wah_on\nmap cc 1 2 no_such_param\nmap foo 1 2 wah_on\nhello\n"
                                       "channel omni\nmap note 3 4 cmd:looper_undo\n",
                                       &warnings);
    CHECK (warnings.size() == 5);
    REQUIRE (parsed.mappings.size() == 1);
    CHECK (parsed.mappings[0] == map (Source::Type::Note, 3, 4, Target::command (Command::LooperUndo)));
    CHECK (parsed.channel == 0);
}

TEST_CASE ("midi: handling MIDI doesn't allocate", "[midi]")
{
    Harness h;
    for (int i = 0; i < 20; ++i)
        h.midi.addMapping (map (Source::Type::Cc, 1, 10 + i, Target::param ((PID) ((int) PID::DelayOn + i))));
    h.midi.addMapping (map (Source::Type::Note, 1, 60, Target::command (Command::LooperRec)));
    h.midi.addMapping (map (Source::Type::Note, 1, 61, Target::command (Command::TapTempo)));
    h.midi.addMapping (map (Source::Type::Note, 1, 62, Target::command (Command::LooperClearAll)));
    h.midi.addMapping (map (Source::Type::Note, 1, 63, Target::command (Command::PresetNext)));
    h.midi.addMapping (map (Source::Type::Cc, 1, 64, Target::command (Command::TrackNext), SwitchMode::Latching));

    std::vector<Event> events;
    for (int i = 0; i < 40; ++i)
        events.push_back (cc (1, 10 + i % 25, (i * 37) % 128, i));
    for (int n = 60; n < 64; ++n)
        events.push_back (noteOn (1, n, 90, n - 20));
    events.push_back (program (1, 3));
    events.push_back (cc (1, 0, 1));
    events.push_back (noteOff (1, 60));
    h.pump(); // the message side (allocates freely: not counted)

    allocation::allocations = 0;
    for (int b = 0; b < 200; ++b)
    {
        auto patch = h.host;
        allocation::counting = true;
        h.midi.beginBlock (patch);
        for (const auto& e : events)
            h.midi.handle (e.bytes.data(), (int) e.bytes.size(), e.offset, patch, h.looper);
        if (b == 50)
            h.midi.learn (Target::param (PID::WahPosition)); // learning, on the audio side
        h.midi.endBlock (block);
        std::array<float, block> l {}, r {};
        h.looper.setPatch (patch);
        h.looper.process (l.data(), r.data(), block);
        allocation::counting = false;
        if (b % 3 == 0)
            h.pump();
    }
    CHECK (allocation::allocations == 0);
    CHECK (h.midi.activity() == 200 * (unsigned) events.size());
}
