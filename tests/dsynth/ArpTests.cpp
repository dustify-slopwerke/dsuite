#include "dsp/Arpeggiator.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace dsynth;

namespace
{
constexpr double rate = 48000.0;

struct Timed
{
    int64_t sample;
    bool on;
    int note;
};

// Runs the arpeggiator the way the engine does and records every event with its sample time
std::vector<Timed> run (Arpeggiator& arp, int64_t samples, int64_t& clock)
{
    std::vector<Timed> events;
    const int64_t end = clock + samples;
    while (clock < end)
    {
        const int k = arp.samplesUntilNextEvent ((int) (end - clock));
        arp.advance (k);
        clock += k;
        Arpeggiator::Event e[3];
        const int n = arp.popEvents (e);
        for (int i = 0; i < n; ++i)
            events.push_back ({ clock, e[i].on, e[i].note });
        if (k == 0 && n == 0)
            break;
    }
    return events;
}

std::vector<int> onNotes (const std::vector<Timed>& events)
{
    std::vector<int> notes;
    for (const auto& e : events)
        if (e.on)
            notes.push_back (e.note);
    return notes;
}

Arpeggiator makeArp (ArpMode mode, int octaves, float gate = 0.5f, bool latch = false)
{
    Arpeggiator arp;
    arp.prepare (rate);
    arp.setParams (mode, octaves, 0.25, gate, latch); // 1/16 notes
    arp.setClock (120.0, false, -1.0);                // 6000 samples per step
    return arp;
}
} // namespace

TEST_CASE ("Arp modes order the notes")
{
    int64_t clock = 0;

    SECTION ("Up over two octaves, starting immediately")
    {
        auto arp = makeArp (ArpMode::Up, 2);
        arp.keyDown (64, 1.0f);
        arp.keyDown (60, 1.0f);
        arp.keyDown (67, 1.0f);
        const auto events = run (arp, 6 * 6000 - 1, clock);
        CHECK (onNotes (events) == std::vector<int> { 60, 64, 67, 72, 76, 79 });
        CHECK (events.front().sample == 0);
    }

    SECTION ("Down")
    {
        auto arp = makeArp (ArpMode::Down, 1);
        for (int n : { 60, 64, 67 })
            arp.keyDown (n, 1.0f);
        CHECK (onNotes (run (arp, 4 * 6000 - 1, clock)) == std::vector<int> { 67, 64, 60, 67 });
    }

    SECTION ("Up & Down doesn't repeat the ends")
    {
        auto arp = makeArp (ArpMode::UpDown, 1);
        for (int n : { 60, 64, 67, 71 })
            arp.keyDown (n, 1.0f);
        CHECK (onNotes (run (arp, 8 * 6000 - 1, clock)) == std::vector<int> { 60, 64, 67, 71, 67, 64, 60, 64 });
    }

    SECTION ("As Played keeps key order")
    {
        auto arp = makeArp (ArpMode::AsPlayed, 1);
        for (int n : { 67, 60, 64 })
            arp.keyDown (n, 1.0f);
        CHECK (onNotes (run (arp, 3 * 6000 - 1, clock)) == std::vector<int> { 67, 60, 64 });
    }

    SECTION ("Random stays within the held notes")
    {
        auto arp = makeArp (ArpMode::Random, 1);
        for (int n : { 60, 64, 67 })
            arp.keyDown (n, 1.0f);
        for (int n : onNotes (run (arp, 50 * 6000, clock)))
            CHECK ((n == 60 || n == 64 || n == 67));
    }
}

TEST_CASE ("Arp timing and gate")
{
    int64_t clock = 0;
    auto arp = makeArp (ArpMode::Up, 1, 0.25f);
    arp.keyDown (60, 1.0f);
    arp.keyDown (62, 1.0f);
    const auto events = run (arp, 3 * 6000 - 1, clock);

    // on @0, off @1500, on @6000, off @7500, on @12000, off @13500
    std::vector<std::pair<int64_t, bool>> expected { { 0, true }, { 1500, false }, { 6000, true }, { 7500, false }, { 12000, true }, { 13500, false } };
    REQUIRE (events.size() == expected.size());
    for (size_t i = 0; i < events.size(); ++i)
    {
        CHECK (events[i].sample == expected[i].first);
        CHECK (events[i].on == expected[i].second);
    }
}

TEST_CASE ("Arp full gate is legato: next note starts before the last ends")
{
    int64_t clock = 0;
    auto arp = makeArp (ArpMode::Up, 1, 1.0f);
    arp.keyDown (60, 1.0f);
    arp.keyDown (62, 1.0f);
    const auto events = run (arp, 6001, clock);
    REQUIRE (events.size() == 3);
    CHECK ((events[1].on && events[1].note == 62 && events[1].sample == 6000));
    CHECK ((! events[2].on && events[2].note == 60 && events[2].sample == 6000));
}

TEST_CASE ("Arp stops on release, latch keeps playing")
{
    int64_t clock = 0;

    SECTION ("No latch")
    {
        auto arp = makeArp (ArpMode::Up, 1, 1.0f);
        arp.keyDown (60, 1.0f);
        run (arp, 3000, clock);
        arp.keyUp (60);
        const auto events = run (arp, 20000, clock);
        REQUIRE (events.size() == 1);
        CHECK ((! events[0].on && events[0].note == 60));
        CHECK_FALSE (arp.isRunning());
    }

    SECTION ("Latch: keeps going, and a new chord replaces the old one")
    {
        auto arp = makeArp (ArpMode::Up, 1, 0.5f, true);
        arp.keyDown (60, 1.0f);
        arp.keyDown (64, 1.0f);
        arp.keyUp (60);
        arp.keyUp (64);
        CHECK (onNotes (run (arp, 4 * 6000 - 1, clock)) == std::vector<int> { 60, 64, 60, 64 });

        arp.keyDown (70, 1.0f);
        const auto notes = onNotes (run (arp, 2 * 6000, clock));
        CHECK (notes == std::vector<int> { 70, 70 });
    }
}

TEST_CASE ("Arp locks to the host grid")
{
    Arpeggiator arp;
    arp.prepare (rate);
    arp.setParams (ArpMode::Up, 1, 0.25, 0.5f, false);
    arp.setClock (120.0, true, 0.1); // playing, a little past the start of beat 1

    arp.keyDown (60, 1.0f);
    int64_t clock = 0;
    const auto events = run (arp, 6000, clock);

    // Next 1/16 boundary is ppq 0.25: 0.15 beats = 0.075 s = 3600 samples away
    REQUIRE_FALSE (events.empty());
    CHECK (events.front().on);
    CHECK (events.front().sample == 3600);
}
