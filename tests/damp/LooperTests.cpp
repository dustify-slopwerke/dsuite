#include "dsp/looper/Looper.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "common/dsp/Noise.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <utility>
#include <vector>

// Counts allocations made while `counting` is set on this thread, to show the audio thread
// doesn't allocate
namespace
{
thread_local bool counting = false;
thread_local long allocations = 0;
} // namespace

void* operator new (std::size_t size)
{
    if (counting)
        ++allocations;
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t size) { return operator new (size); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace damp;
using Action = Looper::Action;
using State = Looper::State;

namespace
{
constexpr double rate = 48000.0;

// One looper driven from a single thread: message calls act at once (no idle wait), audio runs
// in fixed blocks so actions land on known frames
struct Harness
{
    Looper looper;
    Patch patch = Patch::defaults();
    int block = 64;
    // The transport each block gets, and the clicks heard (frame since construction, accent)
    double bpm = 120.0, ppq = 0.0;
    bool host = false;
    int64_t frame = 0;
    std::vector<std::pair<int64_t, bool>> clicks;

    explicit Harness (double poolSeconds = 20.0)
    {
        looper.setIdleTimeout (0.0);
        looper.prepare (rate);
        patch[PID::LooperOn] = 1.0f;
        for (int t = 0; t < Looper::numTracks; ++t)
            looper.setTrackCrossfade (t, 0.0f);
        looper.setPatch (patch);
        looper.createPool (rate, poolSeconds);
        for (int t = 0; t < Looper::numTracks; ++t)
            looper.setTrackCrossfade (t, 0.0f);
    }

    // n frames of input (silence if shorter); returns the left output (right in `outRight`)
    std::vector<float> run (const std::vector<float>& in, size_t n, std::vector<float>* outRight = nullptr, bool mono = false)
    {
        std::vector<float> l (n, 0.0f), r (n, 0.0f);
        std::copy_n (in.begin(), std::min (n, in.size()), l.begin());
        r = l;
        for (size_t pos = 0; pos < n; pos += (size_t) block)
        {
            const int m = (int) std::min<size_t> ((size_t) block, n - pos);
            looper.setPatch (patch);
            looper.setTransport (bpm, host, ppq);
            looper.process (l.data() + pos, mono ? nullptr : r.data() + pos, m);
            for (int c = 0; c < looper.clickCount(); ++c)
                clicks.emplace_back (frame + looper.clicks()[c].offset, looper.clicks()[c].accent);
            frame += m;
            ppq += m * bpm / (60.0 * rate);
        }
        if (outRight != nullptr)
            *outRight = r;
        return l;
    }
    std::vector<float> silence (size_t n) { return run ({}, n); }
    void press (Action a, int track) { looper.press (a, track); }
    // After applying what was pressed (a block of no frames serves the queue)
    Looper::TrackStatus status (int t)
    {
        float dummy = 0.0f;
        looper.setPatch (patch);
        looper.setTransport (bpm, host, ppq);
        looper.process (&dummy, &dummy, 0);
        return looper.trackStatus (t);
    }
};

std::vector<float> noise (size_t n, uint32_t seed, float amp = 0.3f)
{
    dustify::Random rng (seed);
    std::vector<float> x (n);
    for (auto& s : x)
        s = amp * rng.bipolar();
    return x;
}

std::vector<float> sine (double f, size_t n, float amp = 0.5f)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = amp * (float) std::sin (2.0 * std::numbers::pi * f * (double) i / rate);
    return x;
}

float maxStep (const std::vector<float>& x, size_t from = 1)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (1, from); i < x.size(); ++i)
        m = std::max (m, std::abs (x[i] - x[i - 1]));
    return m;
}

// Records `in` on a track: Rec before the first frame, Rec after the last
void recordTrack (Harness& h, int track, const std::vector<float>& in)
{
    h.press (Action::Rec, track);
    h.run (in, in.size());
    h.press (Action::Rec, track);
}
} // namespace

TEST_CASE ("looper: a recording repeats sample for sample", "[looper]")
{
    Harness h;
    const auto in = noise (10000, 1);
    recordTrack (h, 0, in);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.status (0).length == 10000);
    CHECK (h.status (0).master);
    const auto out = h.silence (40000);
    // After the 3 ms fade-in at the first wrap, every cycle is the recording
    for (size_t i = 10000; i < out.size(); ++i)
        REQUIRE (out[i] == in[i % 10000]);
}

TEST_CASE ("looper: overdub sums, undo and redo restore", "[looper]")
{
    Harness h;
    const auto a = noise (4800, 2), b = noise (4800, 3);
    recordTrack (h, 0, a);
    h.press (Action::Rec, 0); // overdub one cycle
    h.run (b, 4800);
    h.press (Action::Rec, 0);
    CHECK (h.status (0).undoLevels == 2);
    auto out = h.silence (9600);
    for (size_t i = 300; i < 4500; ++i) // clear of the 5 ms punch ramps
        REQUIRE (out[4800 + i] == Catch::Approx (a[i] + b[i]).margin (1.0e-6));

    h.press (Action::Undo, 0);
    out = h.silence (9600);
    for (size_t i = 0; i < 4800; ++i)
        REQUIRE (out[4800 + i] == a[i]);
    CHECK (h.status (0).redoLevels == 1);

    h.press (Action::Redo, 0);
    out = h.silence (9600);
    for (size_t i = 300; i < 4500; ++i)
        REQUIRE (out[4800 + i] == Catch::Approx (a[i] + b[i]).margin (1.0e-6));

    // Undoing the first recording empties the track; redo brings it back
    h.press (Action::Undo, 0);
    h.press (Action::Undo, 0);
    h.silence (4800);
    CHECK (h.status (0).state == State::Empty);
    h.press (Action::Redo, 0);
    h.silence (480);
    CHECK (h.status (0).length == 4800);
}

TEST_CASE ("looper: trims and padding", "[looper]")
{
    Harness h;
    const std::vector<float> in (10000, 0.5f);
    recordTrack (h, 0, in);
    h.looper.setTrackEdits (0, -2000, 12000); // 2000 frames of silence each side
    h.silence (64);
    CHECK (h.status (0).loopLength == 14000);
    CHECK (h.status (0).period == 14000);
    CHECK (h.looper.status().masterLength == 14000);
    const auto out = h.silence (14000 * 4);
    int loud = 0, quiet = 0;
    for (size_t i = 14000 * 2; i < 14000 * 3; ++i)
        (out[i] == 0.5f ? loud : out[i] == 0.0f ? quiet : loud) += 1;
    CHECK (loud == 10000);
    CHECK (quiet == 4000);

    // A trim inside the recording
    h.looper.setTrackEdits (0, 1000, 4000);
    h.silence (64);
    CHECK (h.status (0).period == 3000);
    const auto trimmed = h.silence (12000);
    for (size_t i = 6000; i < 12000; ++i)
        REQUIRE (trimmed[i] == 0.5f);
}

TEST_CASE ("looper: the seam crossfade removes the wrap click", "[looper]")
{
    for (float fade : { 0.0f, 5.0f })
    {
        Harness h;
        h.looper.setTrackCrossfade (0, fade);
        recordTrack (h, 0, sine (441.0, 10037)); // not a whole number of cycles
        const auto out = h.silence (10037 * 4);
        const float step = maxStep (out, 10037);
        const float natural = 0.5f * (float) (2.0 * std::numbers::pi * 441.0 / rate);
        INFO ("fade " << fade << " ms, largest step " << step);
        if (fade == 0.0f)
            CHECK (step > 5.0f * natural);
        else
            CHECK (step < 1.5f * natural);
    }
}

TEST_CASE ("looper: edits and toggles while playing don't click", "[looper]")
{
    Harness h;
    h.looper.setTrackCrossfade (0, 5.0f);
    recordTrack (h, 0, sine (200.0, 24000));
    const float natural = 0.5f * (float) (2.0 * std::numbers::pi * 200.0 / rate);
    std::vector<float> all;
    for (int k = 0; k < 6; ++k)
    {
        h.looper.setTrackEdits (0, 317 * k, 24000 - 211 * k);
        if (k == 3)
            h.patch[trackParam (0, TrackParam::Reverse)] = 1.0f;
        if (k == 4)
            h.patch[trackParam (0, TrackParam::Half)] = 1.0f;
        const auto out = h.silence (9000);
        all.insert (all.end(), out.begin(), out.end());
    }
    CHECK (maxStep (all) < 2.0f * natural);
}

TEST_CASE ("looper: reverse plays backwards, half speed an octave down and twice as long", "[looper]")
{
    Harness h;
    std::vector<float> ramp (10000);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = (float) i / 10000.0f;
    recordTrack (h, 0, ramp);
    h.patch[trackParam (0, TrackParam::Reverse)] = 1.0f;
    auto out = h.silence (30000);
    int down = 0;
    for (size_t i = 20001; i < 30000; ++i)
        down += out[i] - out[i - 1] == Catch::Approx (-1.0e-4f).margin (1.0e-6) ? 1 : 0;
    CHECK (down >= 9990);

    Harness s;
    recordTrack (s, 0, sine (1000.0, 4800)); // 100 whole cycles
    s.patch[trackParam (0, TrackParam::Half)] = 1.0f;
    out = s.silence (4800 * 6);
    CHECK (s.status (0).period == 9600);
    int crossings = 0;
    for (size_t i = 9601; i < 9600 * 2; ++i)
        crossings += (out[i - 1] < 0.0f) != (out[i] < 0.0f) ? 1 : 0;
    CHECK (std::abs (crossings - 200) <= 2); // 500 Hz over 0.2 s
}

TEST_CASE ("looper: sync tracks round to the master and stay aligned; free tracks keep their length", "[looper]")
{
    Harness h;
    std::vector<float> master (4800, 0.0f);
    master[100] = 1.0f;
    recordTrack (h, 0, master);
    // Track 2 starts recording mid-cycle and runs a little short of two cycles; it hears the
    // master's click at frames 100 and 4900 after the start of its cycle, and adds its own at the first
    h.silence (1000);
    std::vector<float> second (9500, 0.0f);
    second[100 + 4800 - 1000] = 0.5f;
    recordTrack (h, 1, second);
    CHECK (h.status (1).ending); // records on to the bar: two master loops
    h.silence (100);
    CHECK (h.status (1).length == 9600);
    CHECK (h.status (1).multiple == 2);
    CHECK (h.status (1).period == 9600);

    // Free track: its own 7000
    h.patch[trackParam (2, TrackParam::Sync)] = 0.0f;
    h.patch[trackParam (2, TrackParam::Mute)] = 1.0f;
    recordTrack (h, 2, std::vector<float> (7000, 0.0f));
    CHECK (h.status (2).period == 7000);
    CHECK (h.status (2).multiple == 0);

    // Many cycles later the clicks still coincide: 1.5 every other cycle, 1.0 between
    h.silence (4800 * 50 + 123);
    const auto out = h.silence (9600 * 3);
    int big = 0, single = 0;
    for (float s : out)
    {
        big += s > 1.4f ? 1 : 0;
        single += s > 0.9f && s < 1.1f ? 1 : 0;
    }
    CHECK (big == 3);
    CHECK (single == 3);
}

TEST_CASE ("looper: seeking a sync track moves every sync track together; a free track alone", "[looper]")
{
    Harness h;
    recordTrack (h, 0, noise (4800, 11));
    h.silence (1000);
    recordTrack (h, 1, noise (9500, 12)); // two master loops
    h.patch[trackParam (2, TrackParam::Sync)] = 0.0f;
    recordTrack (h, 2, noise (7000, 13));
    h.silence (777);

    auto offsets = [&]
    {
        std::array<int64_t, 3> o {};
        for (int t = 0; t < 3; ++t)
            o[(size_t) t] = h.status (t).sourcePosition - h.status (t).start;
        return o;
    };
    // The clock positions of the sync tracks relative to each other: track 1 spans two master loops
    auto lag = [] (const std::array<int64_t, 3>& o) { return ((o[1] - o[0]) % 4800 + 4800) % 4800; };
    const auto before = offsets();

    h.looper.seek (0, 1234);
    const auto after = offsets();
    CHECK (after[0] == 1234);
    CHECK (lag (after) == lag (before)); // still aligned
    CHECK (after[2] == before[2]);        // the free track didn't move

    // Seeking the longer sync track moves the master with it
    h.looper.seek (1, 6000);
    const auto later = offsets();
    CHECK (later[1] == 6000);
    CHECK (lag (later) == lag (before));
    CHECK (later[2] == before[2]);

    // A free track seeks alone
    h.looper.seek (2, 3500);
    const auto free = offsets();
    CHECK (free[2] == 3500);
    CHECK (free[0] == later[0]);
    CHECK (free[1] == later[1]);

    // Out of range offsets are held to the loop
    h.looper.seek (2, 1'000'000);
    CHECK (offsets()[2] == 6999);

    // A stopped track starts playing from the seek (and the padding counts: offset 0 is the start handle)
    h.looper.setTrackEdits (0, -500, 4800);
    h.press (Action::PlayStop, -1);
    h.silence (2000);
    CHECK (h.status (0).state == State::Stopped);
    h.looper.seek (0, 100);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.status (0).sourcePosition == -400);
}

TEST_CASE ("looper: seeking crossfades, and leaves recordings alone", "[looper]")
{
    Harness h;
    h.looper.setTrackCrossfade (0, 5.0f);
    recordTrack (h, 0, sine (200.0, 24000));
    const float natural = 0.5f * (float) (2.0 * std::numbers::pi * 200.0 / rate);
    std::vector<float> all;
    for (int64_t to : { 5000, 17111, 333, 23900, 12000 })
    {
        h.looper.seek (0, to);
        const auto out = h.silence (3000);
        all.insert (all.end(), out.begin(), out.end());
    }
    CHECK (maxStep (all) < 2.0f * natural);

    // While another track records, a sync seek would misplace the recording: it's ignored
    h.patch[trackParam (1, TrackParam::Sync)] = 0.0f; // free: it closes when asked
    h.press (Action::Rec, 1);
    h.silence (1000);
    const auto position = h.status (0).sourcePosition;
    h.looper.seek (0, 100);
    h.looper.seek (1, 100);
    CHECK (h.status (0).sourcePosition == position);
    CHECK (h.status (1).state == State::Recording);
    h.silence (3000);
    h.press (Action::Rec, 1);
    CHECK (h.status (1).length == 4000);
    CHECK (h.status (1).state == State::Playing);

    // ... and so while overdubbing
    h.press (Action::Rec, 0);
    h.silence (100);
    const auto dubbing = h.status (0).sourcePosition;
    h.looper.seek (0, 9000);
    CHECK (h.status (0).sourcePosition == dubbing);
    h.press (Action::Rec, 0);
    h.silence (480); // the punch-out ramp
    h.looper.seek (0, 9000);
    CHECK (h.status (0).sourcePosition == 9000);
}

TEST_CASE ("looper: eight tracks play together; level and mute", "[looper]")
{
    Harness h;
    for (int t = 0; t < 8; ++t)
    {
        h.press (Action::Rec, t); // closes the previous track
        h.run (std::vector<float> (4800, 0.01f * (float) (t + 1)), 4800);
    }
    h.press (Action::Rec, 7);
    auto out = h.silence (4800 * 3);
    CHECK (out.back() == Catch::Approx (0.36f).margin (1.0e-5));
    for (int t = 0; t < 8; ++t)
        CHECK (h.status (t).state == State::Playing);

    h.patch[trackParam (7, TrackParam::Mute)] = 1.0f;
    h.patch[trackParam (0, TrackParam::Level)] = -20.0f * std::log10 (2.0f); // half
    out = h.silence (48000);
    CHECK (out.back() == Catch::Approx (0.36f - 0.08f - 0.005f).margin (1.0e-4));
}

TEST_CASE ("looper: a full pool stops the recording cleanly; clear gives it back", "[looper]")
{
    Harness h (1.0); // 3 chunks
    const auto chunks = (int64_t) std::ceil (rate / (double) looper::LoopPool::chunkFrames);
    h.press (Action::Rec, 0);
    h.run (noise (96000, 4), 96000);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.status (0).length == chunks * looper::LoopPool::chunkFrames);
    CHECK (h.looper.status().poolFullCount == 1);
    CHECK (h.looper.status().poolUsedSeconds == Catch::Approx (h.looper.status().poolTotalSeconds));

    // An overdub on a full pool gives up undo history, then stops
    h.press (Action::Rec, 0);
    h.run (noise (96000, 5), 96000);
    CHECK (h.status (0).state == State::Playing);

    h.press (Action::Clear, 0);
    h.silence (4800);
    CHECK (h.status (0).state == State::Empty);
    CHECK (h.looper.status().poolUsedSeconds == 0.0);
    CHECK (h.looper.status().masterTrack == -1);
}

TEST_CASE ("looper: mono output", "[looper]")
{
    Harness h;
    const auto in = noise (5000, 6);
    h.press (Action::Rec, 0);
    h.run (in, 5000, nullptr, true);
    h.press (Action::Rec, 0);
    const auto out = h.run ({}, 20000, nullptr, true);
    for (size_t i = 5000; i < out.size(); ++i)
        REQUIRE (out[i] == Catch::Approx (in[i % 5000]).margin (1.0e-7));
}

TEST_CASE ("looper: the audio thread doesn't allocate", "[looper]")
{
    Harness h;
    h.looper.setTrackCrossfade (0, 5.0f);
    h.looper.setTrackCrossfade (1, 5.0f);
    const auto in = noise (48000, 7);
    std::vector<float> l (in), r (in);
    auto audio = [&] (int frames)
    {
        counting = true;
        for (int pos = 0; pos + 64 <= frames; pos += 64)
        {
            h.looper.setPatch (h.patch);
            h.looper.process (l.data() + pos % 40000, r.data() + pos % 40000, 64);
        }
        counting = false;
    };
    allocations = 0;
    h.press (Action::Rec, 0);
    audio (20000);
    h.press (Action::Rec, 0);
    audio (20000);
    h.press (Action::Rec, 0); // overdub (copies chunks on write)
    audio (30000);
    h.press (Action::Rec, 1);
    audio (25000);
    h.press (Action::Rec, 1);
    h.looper.setTrackEdits (0, 300, 18000);
    h.patch[trackParam (1, TrackParam::Reverse)] = 1.0f;
    audio (20000);
    h.looper.setTrackEdits (0, -20000, 40000); // overdub into padding (grows the take both ways)
    h.press (Action::Rec, 0);
    audio (60000);
    h.press (Action::Rec, 0);
    h.press (Action::Rec, 2); // a sync recording waiting for its bar
    audio (5000);
    h.press (Action::Rec, 2);
    audio (60000);
    CHECK (h.status (0).front > 0);
    CHECK (h.status (2).length == 60000);
    h.press (Action::Undo, 0);
    audio (5000);
    h.press (Action::Redo, 0);
    h.press (Action::PlayStop, -1);
    audio (5000);
    h.press (Action::PlayStop, -1);
    h.press (Action::Clear, 1);
    audio (5000);
    CHECK (allocations == 0);
    CHECK (h.status (0).state == State::Playing);
}

TEST_CASE ("looper: a session round trip gives identical audio and edits", "[looper]")
{
    Harness h;
    recordTrack (h, 0, noise (9000, 8));
    h.silence (777);
    recordTrack (h, 3, noise (18100, 9));
    h.looper.setTrackEdits (3, -500, 17000);
    h.looper.setTrackCrossfade (3, 12.5f);
    h.silence (64);
    const auto saved = h.looper.saveSession();
    CHECK (saved.masterTrack == 0);
    CHECK (saved.masterLength == 9000);

    std::vector<std::byte> bytes;
    looper::encodeSession (saved, bytes);
    const auto decoded = looper::decodeSession (bytes.data(), bytes.size());
    REQUIRE (decoded.has_value());

    Harness other;
    REQUIRE (other.looper.loadSession (*decoded));
    CHECK (other.status (3).state == State::Stopped);
    CHECK (other.status (3).start == -500);
    CHECK (other.status (3).end == 17000);
    CHECK (other.status (3).crossfadeMs == 12.5f);
    const auto again = other.looper.saveSession();
    std::vector<std::byte> bytes2;
    looper::encodeSession (again, bytes2);
    CHECK (bytes2 == bytes);

    // And it plays the same: both from the top
    h.press (Action::PlayStop, -1);
    h.silence (4800);
    h.press (Action::PlayStop, -1);
    other.press (Action::PlayStop, -1);
    const auto a = h.silence (40000), b = other.silence (40000);
    CHECK (a == b);

    // Junk isn't a session
    bytes[0] = std::byte { 0 };
    CHECK (! looper::decodeSession (bytes.data(), bytes.size()).has_value());
    CHECK (! looper::decodeSession (bytes.data(), 10).has_value());
}

namespace
{
// A version 1 block (float32, before the loops were packed), byte by byte as that build wrote it
std::vector<std::byte> versionOneSession (const std::vector<float>& left, const std::vector<float>& right)
{
    std::vector<std::byte> out;
    auto put = [&out] (const auto& v)
    {
        const auto* p = reinterpret_cast<const std::byte*> (&v);
        out.insert (out.end(), p, p + sizeof (v));
    };
    put (looper::sessionMagic);
    put (uint32_t { 1 });
    put (uint64_t { 0 });
    put (44100.0);
    put (int32_t { 1 });       // master track
    put (int64_t { 3000 });    // master length
    put (int32_t { 8 });
    for (int t = 0; t < 8; ++t)
    {
        const bool audio = t == 1;
        put ((int64_t) (audio ? left.size() : 0));
        put (int64_t { audio ? -200 : 0 });
        put (int64_t { audio ? 2800 : 0 });
        put (audio ? 7.5f : 5.0f);
        put (int64_t { audio ? 123 : 0 });
        for (const auto* x : { &left, &right })
            for (size_t i = 0; audio && i < x->size(); ++i)
                put ((*x)[i]);
    }
    const uint64_t size = out.size() - 16;
    std::memcpy (out.data() + 8, &size, sizeof (size));
    return out;
}
} // namespace

TEST_CASE ("looper: version 1 (float32) sessions still load", "[looper]")
{
    const auto l = noise (3000, 21, 1.7f), r = noise (3000, 22); // louder than full scale
    const auto bytes = versionOneSession (l, r);
    const auto s = looper::decodeSession (bytes.data(), bytes.size());
    REQUIRE (s.has_value());
    CHECK (s->sampleRate == 44100.0);
    CHECK (s->masterTrack == 1);
    CHECK (s->masterLength == 3000);
    const auto& t = s->tracks[1];
    CHECK (t.left == l);
    CHECK (t.right == r);
    CHECK (t.start == -200);
    CHECK (t.end == 2800);
    CHECK (t.crossfadeMs == 7.5f);
    CHECK (t.anchor == 123);
    CHECK (s->tracks[0].empty());

    // Cut short: not a session
    CHECK (! looper::decodeSession (bytes.data(), bytes.size() - 1).has_value());
}

TEST_CASE ("looper: sessions pack audio as 24 bits at a power-of-two scale", "[looper]")
{
    looper::LoopSession s;
    s.sampleRate = 48000.0;
    s.masterTrack = 0;
    s.masterLength = 5000;
    auto& loud = s.tracks[0];
    loud.left = noise (5000, 31, 3.7f); // an overdub well past full scale
    loud.right = noise (5000, 32, 0.01f);
    loud.left[17] = std::numeric_limits<float>::quiet_NaN(); // can't be stored: silence
    loud.end = 5000;
    auto& quiet = s.tracks[5];
    quiet.left = noise (3000, 33, 1.0e-4f);
    quiet.right = quiet.left;
    quiet.start = -100;
    quiet.end = 4000;
    quiet.anchor = 77;

    std::vector<std::byte> packed, raw;
    looper::encodeSession (s, packed, &looper::plainPacker());
    looper::encodeSession (s, raw);
    CHECK (packed.size() < raw.size() * 0.8);
    const auto d = looper::decodeSession (packed.data(), packed.size());
    REQUIRE (d.has_value());

    // Within half a step of 2^(e - 23), e from the track's peak: nothing clips
    for (size_t t : { 0u, 5u })
    {
        const auto &in = s.tracks[t], &out = d->tracks[t];
        REQUIRE (out.left.size() == in.left.size());
        float peak = 0.0f, error = 0.0f;
        for (size_t i = 0; i < in.left.size(); ++i)
        {
            const float l = std::isfinite (in.left[i]) ? in.left[i] : 0.0f;
            peak = std::max ({ peak, std::abs (l), std::abs (in.right[i]) });
            error = std::max ({ error, std::abs (out.left[i] - l), std::abs (out.right[i] - in.right[i]) });
        }
        int e = 0;
        std::frexp (peak, &e);
        INFO ("track " << t << " peak " << peak << " error " << error);
        CHECK (error <= std::ldexp (1.0f, e - 24));
        CHECK (error > 0.0f);
        CHECK (out.start == in.start);
        CHECK (out.end == in.end);
        CHECK (out.anchor == in.anchor);
    }
    CHECK (d->tracks[0].left[17] == 0.0f);
    CHECK (d->masterLength == 5000);

    // Decoded audio sits on the grid: saving it again gives the same bytes
    std::vector<std::byte> again;
    looper::encodeSession (*d, again, &looper::plainPacker());
    CHECK (again == packed);

    // A track packed by a packer the reader doesn't have can't be read
    struct Other final : looper::SamplePacker
    {
        uint32_t id() const override { return 9; }
        bool pack (const int32_t* l, const int32_t* r, size_t n, std::vector<std::byte>& out) const override { return looper::plainPacker().pack (l, r, n, out); }
        bool unpack (const std::byte* p, size_t size, size_t n, int32_t* l, int32_t* r) const override { return looper::plainPacker().unpack (p, size, n, l, r); }
    } other;
    std::vector<std::byte> foreign;
    looper::encodeSession (s, foreign, &other);
    CHECK (! looper::decodeSession (foreign.data(), foreign.size()).has_value());
    CHECK (looper::decodeSession (foreign.data(), foreign.size(), &other).has_value());
}

TEST_CASE ("looper: closing a sync recording waits for its bar", "[looper]")
{
    Harness h;
    recordTrack (h, 0, std::vector<float> (4800, 0.0f)); // the master: 4800, silent
    h.silence (1000);                                      // track 2 starts mid-loop

    // Pressed 7000 frames in: it records on to 9600 (two master loops), closing mid-block, and
    // plays on from the frame after with nothing lost
    const auto in = noise (12000, 41);
    h.press (Action::Rec, 1);
    h.run (in, 7000);
    h.press (Action::Rec, 1);
    CHECK (h.status (1).state == State::Recording);
    CHECK (h.status (1).ending);
    h.press (Action::Rec, 1); // a second press changes nothing
    CHECK (h.status (1).ending);
    std::vector<float> rest (in.begin() + 7000, in.begin() + 9600);
    rest.resize (2600 + 1000, 0.0f);
    const auto out = h.run (rest, rest.size()); // 9600 isn't on a block boundary
    CHECK (h.status (1).state == State::Playing);
    CHECK (! h.status (1).ending);
    CHECK (h.status (1).length == 9600);
    CHECK (h.status (1).multiple == 2);
    for (size_t k = 200; k < 1000; ++k) // past the 3 ms fade-in
        REQUIRE (out[2600 + k] == in[k]);
    const auto next = h.silence (9600);
    for (size_t k = 0; k < 9600; ++k)
        REQUIRE (next[k] == in[(1000 + k) % 9600]);

    // Pressed just after a bar (within 50 ms, or a quarter of the bar if less: 25 ms here): it
    // closes at that bar at once
    h.press (Action::Rec, 2);
    h.run (noise (4800 + 1000, 42), 4800 + 1000); // 21 ms past
    h.press (Action::Rec, 2);
    CHECK (h.status (2).state == State::Playing);
    CHECK (h.status (2).length == 4800);
    CHECK (h.status (2).multiple == 1);

    // ... and later than that, it waits for the next one; so does a recording under a bar long
    h.press (Action::Rec, 3);
    h.run (noise (4800 + 2500, 43), 4800 + 2500);
    h.press (Action::Rec, 3);
    CHECK (h.status (3).ending);
    h.silence (2300 - 1);
    CHECK (h.status (3).state == State::Recording);
    h.silence (1);
    CHECK (h.status (3).state == State::Playing);
    CHECK (h.status (3).length == 9600);
    h.press (Action::Rec, 4);
    h.silence (1000);
    h.press (Action::Rec, 4);
    h.silence (3799);
    CHECK (h.status (4).ending);
    h.silence (1);
    CHECK (h.status (4).length == 4800);
}

TEST_CASE ("looper: stop, undo, clear or another track's Rec cancel the wait for the bar", "[looper]")
{
    auto waiting = [] (Harness& h, int track)
    {
        h.press (Action::Rec, track);
        h.run (noise (7000, 50), 7000);
        h.press (Action::Rec, track);
        REQUIRE (h.status (track).ending);
    };
    Harness h;
    recordTrack (h, 0, noise (4800, 51));

    // Stop: the recording closes where it is (its period rounded to the master, as ever), stopped
    waiting (h, 1);
    h.press (Action::Stop, 1);
    CHECK (h.status (1).state == State::Stopped);
    CHECK (! h.status (1).ending);
    CHECK (h.status (1).length == 7000);
    CHECK (h.status (1).period == 4800);

    // Play/Stop (every track) likewise
    waiting (h, 2);
    h.press (Action::PlayStop, -1);
    CHECK (h.status (2).state == State::Stopped);
    CHECK (h.status (2).length == 7000);

    // Undo: the recording goes
    h.press (Action::PlayStop, -1);
    waiting (h, 3);
    h.press (Action::Undo, 3);
    h.silence (480);
    CHECK (h.status (3).state == State::Empty);
    CHECK (! h.status (3).ending);

    // Clear: likewise
    waiting (h, 4);
    h.press (Action::Clear, 4);
    h.silence (480);
    CHECK (h.status (4).state == State::Empty);

    // Rec on another track: this one closes at once, the other starts
    waiting (h, 5);
    h.press (Action::Rec, 6);
    CHECK (h.status (5).state == State::Playing);
    CHECK (h.status (5).length == 7000);
    CHECK (h.status (6).state == State::Recording);
    h.press (Action::Rec, 6);
    h.silence (4800);

    // Going free while it waits: it closes then
    waiting (h, 7);
    h.patch[trackParam (7, TrackParam::Sync)] = 0.0f;
    h.silence (64);
    CHECK (h.status (7).state == State::Playing);
    CHECK (h.status (7).length == 7000);
}

TEST_CASE ("looper: free tracks and the master close at once", "[looper]")
{
    Harness h;
    h.press (Action::Rec, 0);
    h.run (noise (4321, 60), 4321);
    h.press (Action::Rec, 0);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.status (0).length == 4321);

    h.patch[trackParam (1, TrackParam::Sync)] = 0.0f;
    h.press (Action::Rec, 1);
    h.run (noise (7000, 61), 7000);
    h.press (Action::Rec, 1);
    CHECK (h.status (1).state == State::Playing);
    CHECK (h.status (1).length == 7000);
}

TEST_CASE ("looper: overdubbing over padding records into it", "[looper]")
{
    Harness h;
    const std::vector<float> a (10000, 0.25f);
    recordTrack (h, 0, a);
    h.looper.setTrackEdits (0, -3000, 13000); // 3000 frames of padding each side
    h.silence (16000 * 2 + 777);
    auto heard = [] (int64_t frame) { return frame >= 0 && frame < 10000 ? 0.25f : 0.0f; }; // recording frame -> audio

    // One cycle of overdub from wherever the loop is
    const auto before = h.status (0);
    const int64_t p0 = before.sourcePosition - before.start;
    const auto b = noise (16000, 70);
    h.press (Action::Rec, 0);
    h.run (b, 16000);
    h.press (Action::Rec, 0);
    const auto s = h.status (0);
    CHECK (s.front == looper::LoopPool::chunkFrames); // grown by a chunk at the front ...
    CHECK (s.start == -3000 + s.front);               // ... renumbering the frames
    CHECK (s.end == 13000 + s.front);
    CHECK (s.loopLength == 16000);
    CHECK (s.length >= s.end);
    CHECK (h.looper.status().masterLength == 16000);
    CHECK (s.sourcePosition - s.start == p0); // the loop didn't move

    // Every frame of the loop now holds the overdub, padding included
    const auto out = h.silence (16000);
    for (int64_t k = 300; k < 16000; ++k) // past the punch-in ramp
    {
        const int64_t phase = (p0 + k) % 16000;
        REQUIRE (out[(size_t) k] == Catch::Approx (heard (phase - 3000) + b[(size_t) k]).margin (1.0e-6));
    }

    // The overview shows it where the padding was
    std::vector<float> lo ((size_t) h.looper.overviewBins (0)), hi (lo.size());
    REQUIRE (h.looper.readOverview (0, 0, (int) lo.size(), lo.data(), hi.data()) == (int) lo.size());
    const auto bin = (size_t) ((s.start + 1000) / Looper::overviewBinFrames); // in the old front padding
    CHECK (hi[bin] - lo[bin] > 0.1f);
    CHECK (lo[0] == 0.0f);
    CHECK (hi[0] == 0.0f); // silence the overdub didn't reach

    // Saved, the silence it grew is plain zeros; it loads back the same
    std::vector<float> l, r;
    REQUIRE (h.looper.copyTrackAudio (0, l, r));
    CHECK ((int64_t) l.size() == s.length);
    CHECK (l[0] == 0.0f);
    Harness other;
    REQUIRE (other.looper.loadSession (h.looper.saveSession()));
    CHECK (other.status (0).start == s.start);
    CHECK (other.status (0).end == s.end);
    CHECK (other.status (0).length == s.length);

    // Undo gives back the padded recording, numbered as it was
    h.press (Action::Undo, 0);
    h.silence (480);
    CHECK (h.status (0).front == 0);
    CHECK (h.status (0).start == -3000);
    CHECK (h.status (0).end == 13000);
    CHECK (h.status (0).length == 10000);
    h.press (Action::Redo, 0);
    h.silence (480);
    CHECK (h.status (0).start == -3000 + s.front);
    CHECK (h.status (0).length == s.length);
}

TEST_CASE ("looper: an overdub growing the take changes nothing heard", "[looper]")
{
    for (bool free : { false, true })
    {
        Harness h;
        recordTrack (h, 0, noise (4800, 80)); // the master
        const int t = free ? 1 : 0;
        h.looper.setTrackCrossfade (t, 5.0f);
        if (free)
        {
            h.patch[trackParam (1, TrackParam::Sync)] = 0.0f;
            h.patch[trackParam (0, TrackParam::Mute)] = 1.0f;
            recordTrack (h, 1, sine (300.0, 9000));
        }
        else
        {
            h.press (Action::Clear, 0);
            recordTrack (h, 0, sine (300.0, 9000));
        }
        h.looper.setTrackEdits (t, -2000, 11000);
        h.silence (13000 * 2);

        // Overdubbing silence over the whole loop: the take grows both ways, its edge fades are
        // written into the audio, and every cycle sounds as before
        const auto first = h.silence (13000);
        h.press (Action::Rec, t);
        const auto second = h.silence (13000);
        h.press (Action::Rec, t);
        const auto third = h.silence (13000);
        INFO ((free ? "free" : "sync") << " track");
        CHECK (h.status (t).front > 0);
        CHECK (h.status (t).length > 13000);
        float worst = 0.0f;
        for (size_t i = 0; i < 13000; ++i)
            worst = std::max ({ worst, std::abs (second[i] - first[i]), std::abs (third[i] - first[i]) });
        CHECK (worst < 1.0e-6f);

        // An edit made against the old numbering follows the recording
        const auto s = h.status (t);
        h.looper.setTrackEdits (t, -1000, 10000, 0);
        CHECK (h.status (t).start == -1000 + s.front);
        CHECK (h.status (t).end == 10000 + s.front);
    }
}

TEST_CASE ("looper: an overdub into padding stops cleanly when the pool runs out", "[looper]")
{
    Harness h (1.0); // 3 chunks
    recordTrack (h, 0, noise (40000, 90)); // 3 chunks
    h.looper.setTrackEdits (0, 0, 70000);
    h.silence (64);
    const int full = h.looper.status().poolFullCount;
    h.press (Action::Rec, 0);
    h.run (noise (70000, 91), 70000);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.looper.status().poolFullCount == full + 1);
    CHECK (h.status (0).end == 70000);
    h.silence (70000); // and plays on
}

TEST_CASE ("looper: overview, export and zero crossings", "[looper]")
{
    Harness h;
    recordTrack (h, 0, sine (100.0, 4800));
    CHECK (h.looper.overviewBins (0) == (4800 + 255) / 256);
    std::vector<float> lo (64), hi (64);
    const int got = h.looper.readOverview (0, 0, 64, lo.data(), hi.data());
    CHECK (got == 19);
    CHECK (lo[1] == Catch::Approx (-0.5f).margin (1.0e-3)); // the trough at frame 360
    CHECK (hi[1] == Catch::Approx (0.5f * std::sin (2.0 * std::numbers::pi * 31.0 / 480.0)).margin (1.0e-3));
    std::vector<float> l, r;
    REQUIRE (h.looper.copyTrackAudio (0, l, r));
    CHECK (l.size() == 4800);
    CHECK (std::abs (h.looper.nearestZeroCrossing (0, 230, 100) - 240) <= 1); // 100 Hz: every 240 frames
    h.looper.setTrackEdits (0, 0, 2400);
    h.silence (64);
    REQUIRE (h.looper.renderLoop (0, l, r));
    CHECK (l.size() == 2400);
}

TEST_CASE ("looper: resampling keeps a tone's pitch", "[looper]")
{
    looper::LoopSession s;
    s.sampleRate = 44100.0;
    s.masterTrack = 0;
    s.masterLength = 44100;
    auto& t = s.tracks[0];
    t.left.resize (44100);
    for (size_t i = 0; i < t.left.size(); ++i)
        t.left[i] = (float) std::sin (2.0 * std::numbers::pi * 1000.0 * (double) i / 44100.0);
    t.right = t.left;
    t.end = 44100;
    const auto r = looper::resampleSession (s, 48000.0);
    CHECK (r.tracks[0].left.size() == 48000);
    CHECK (r.tracks[0].end == 48000);
    double err = 0.0;
    for (size_t i = 1000; i < 47000; ++i)
        err = std::max (err, std::abs (r.tracks[0].left[i] - std::sin (2.0 * std::numbers::pi * 1000.0 * (double) i / 48000.0)));
    CHECK (err < 1.0e-3);
}

TEST_CASE ("looper: CPU of eight playing tracks", "[.][looper][bench]")
{
    Harness h (60.0);
    h.block = 256;
    for (int t = 0; t < 8; ++t)
    {
        h.looper.setTrackCrossfade (t, 5.0f);
        h.press (Action::Rec, t);
        h.run (noise (48000 * 2, (uint32_t) (10 + t)), 48000 * 2);
    }
    h.press (Action::Rec, 7);
    const auto started = std::chrono::steady_clock::now();
    h.silence (48000 * 20);
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    std::printf ("8 tracks at 48 kHz: %.2f%% of real time\n", 100.0 * seconds / 20.0);
}

// ======================================================================================
// The beat grid: count-in, quantise, clicks

namespace
{
std::vector<float> ramp (size_t n)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = (float) (i + 1) * 1.0e-6f;
    return x;
}

// The clicks heard from `from` on
std::vector<std::pair<int64_t, bool>> clicksFrom (const Harness& h, int64_t from)
{
    std::vector<std::pair<int64_t, bool>> out;
    for (const auto& c : h.clicks)
        if (c.first >= from)
            out.push_back (c);
    return out;
}
} // namespace

TEST_CASE ("looper: count-in clicks a bar, then records from the bar line exactly", "[looper][tempo]")
{
    Harness h;
    h.patch[PID::MetronomeOn] = 1.0f;
    h.patch[PID::CountIn] = 1.0f;
    h.silence (30000 - 13); // the free clock clicks at 0 and 24000; F is off its grid
    const int64_t f = h.frame;

    h.press (Action::Rec, 0);
    CHECK (h.status (0).armed);
    CHECK (h.status (0).state == State::Empty);
    CHECK (h.looper.status().countInBeatsLeft == 4);
    CHECK (h.looper.status().armedTrack == 0);
    const auto in = ramp (150000);
    h.run (in, 50000);
    CHECK (h.looper.status().countInBeatsLeft == 2);
    CHECK (h.status (0).state == State::Empty);
    h.run (std::vector<float> (in.begin() + 50000, in.end()), 100000);
    CHECK (! h.status (0).armed);
    CHECK (h.status (0).state == State::Recording);
    h.press (Action::Rec, 0);
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.status (0).length == 150000 - 96000);

    // Four beats (24000 frames apart at 120 BPM), the first accented; bar 1 on the first recorded frame
    const auto heard = clicksFrom (h, f);
    REQUIRE (heard.size() >= 5);
    for (int k = 0; k < 5; ++k)
    {
        CHECK (heard[(size_t) k].first == f + k * 24000);
        CHECK (heard[(size_t) k].second == (k % 4 == 0));
    }
    std::vector<float> l, r;
    REQUIRE (h.looper.copyTrackAudio (0, l, r));
    CHECK (l[0] == in[96000]);
    CHECK (l[1] == in[96001]);
}

TEST_CASE ("looper: the count-in clicks with the metronome off; only the count-in", "[looper][tempo]")
{
    for (const bool recOnly : { false, true })
    {
        Harness h;
        h.patch[PID::MetronomeOn] = 0.0f;
        h.patch[PID::MetronomeRecOnly] = recOnly ? 1.0f : 0.0f;
        h.patch[PID::CountIn] = 1.0f;
        h.silence (30000);
        CHECK (h.clicks.empty());
        const int64_t f = h.frame;
        h.press (Action::Rec, 0);
        h.silence (96000 + 60000); // the count-in bar, then recording
        CHECK (h.status (0).state == State::Recording);
        const auto heard = clicksFrom (h, f);
        REQUIRE (heard.size() == 4); // bar 1 of the recording and after: silent
        for (int k = 0; k < 4; ++k)
        {
            CHECK (heard[(size_t) k].first == f + k * 24000);
            CHECK (heard[(size_t) k].second == (k == 0));
        }
        h.press (Action::Rec, 0);
        h.silence (100000);
        CHECK (clicksFrom (h, f).size() == 4);
    }
}

TEST_CASE ("looper: Rec, Stop, Play/Stop, Clear, Undo or switching off cancel a count-in; Rec elsewhere moves it", "[looper][tempo]")
{
    Harness h;
    h.patch[PID::CountIn] = 1.0f;
    auto cancelled = [&] (Action a, int track)
    {
        h.press (Action::Rec, 0);
        h.silence (10000);
        REQUIRE (h.status (0).armed);
        h.press (a, track);
        CHECK (! h.status (0).armed);
        h.silence (100000);
        CHECK (h.status (0).state == State::Empty);
        CHECK (h.looper.status().armedTrack == -1);
    };
    cancelled (Action::Rec, 0);
    cancelled (Action::Stop, 0);
    cancelled (Action::PlayStop, -1);
    cancelled (Action::Clear, 0);
    cancelled (Action::Undo, 0);

    h.press (Action::Rec, 0);
    h.silence (10000);
    h.patch[PID::LooperOn] = 0.0f;
    CHECK (! h.status (0).armed);
    h.patch[PID::LooperOn] = 1.0f;
    h.silence (100000);
    CHECK (h.status (0).state == State::Empty);

    // Rec on another track: it records there, on the same bar line
    h.press (Action::Rec, 0);
    h.silence (20000);
    h.press (Action::Rec, 1);
    CHECK (! h.status (0).armed);
    CHECK (h.status (1).armed);
    h.silence (76000);
    CHECK (h.status (1).state == State::Empty);
    h.silence (64);
    CHECK (h.status (1).state == State::Recording);
    CHECK (h.status (1).length == 64);
    CHECK (h.status (0).state == State::Empty);
}

TEST_CASE ("looper: a quantised master closes on whole bars", "[looper][tempo]")
{
    // 100 BPM, 4/4: 115200 frames a bar
    constexpr int64_t bar = 115200;
    auto harness = []
    {
        auto h = std::make_unique<Harness>();
        h->bpm = 100.0;
        h->patch[PID::LooperQuantise] = 1.0f;
        return h;
    };
    auto record = [] (Harness& h, int64_t frames)
    {
        h.press (Action::Rec, 0);
        h.run (noise ((size_t) frames, 70), (size_t) frames);
        CHECK (h.status (0).state == State::Recording); // no clicks to wait for: at once
        h.press (Action::Rec, 0);
    };

    SECTION ("1.3 bars: on to bar 2")
    {
        auto h = harness();
        record (*h, bar * 13 / 10);
        CHECK (h->status (0).ending);
        h->silence ((size_t) (2 * bar - bar * 13 / 10 - 1));
        CHECK (h->status (0).state == State::Recording);
        h->silence (1);
        CHECK (h->status (0).state == State::Playing);
        CHECK (h->status (0).length == 2 * bar);
        CHECK (h->looper.status().masterLength == 2 * bar);
        CHECK (h->looper.status().masterBars == 2);
    }
    SECTION ("30 ms past bar 1: closes there at once")
    {
        auto h = harness();
        record (*h, bar + 1440);
        CHECK (h->status (0).state == State::Playing);
        CHECK (h->status (0).length == bar);
        CHECK (h->looper.status().masterLength == bar);
    }
    SECTION ("under a bar: on to bar 1")
    {
        auto h = harness();
        record (*h, bar / 5);
        CHECK (h->status (0).ending);
        h->silence ((size_t) (bar - bar / 5));
        CHECK (h->status (0).state == State::Playing);
        CHECK (h->looper.status().masterLength == bar);
    }
    SECTION ("Stop while it waits: the end rounds to whole bars, trimmed or padded")
    {
        auto h = harness();
        record (*h, bar * 13 / 10);
        h->silence ((size_t) (bar / 10)); // 1.4 bars
        h->press (Action::Stop, 0);
        auto s = h->status (0);
        CHECK (s.state == State::Stopped);
        CHECK (s.length == bar * 14 / 10);
        CHECK (s.end == bar);
        CHECK (h->looper.status().masterLength == bar);

        auto g = harness();
        record (*g, bar * 17 / 10);
        g->press (Action::PlayStop, -1);
        s = g->status (0);
        CHECK (s.length == bar * 17 / 10);
        CHECK (s.end == 2 * bar);
        CHECK (g->looper.status().masterLength == 2 * bar);
    }
    SECTION ("quantise off: the master closes where it is")
    {
        auto h = harness();
        h->patch[PID::LooperQuantise] = 0.0f;
        record (*h, bar * 13 / 10);
        CHECK (h->status (0).state == State::Playing);
        CHECK (h->status (0).length == bar * 13 / 10);
    }
}

TEST_CASE ("looper: with quantise and clicks, the first recording waits for the bar line", "[looper][tempo]")
{
    Harness h;
    h.patch[PID::LooperQuantise] = 1.0f;
    h.patch[PID::MetronomeOn] = 1.0f; // the free clock: bar lines at 0, 96000, ...
    h.silence (30000);
    h.press (Action::Rec, 0);
    CHECK (h.status (0).armed);
    CHECK (h.looper.status().countInBeatsLeft == 0);
    h.silence (96000 - 30000);
    CHECK (h.status (0).state == State::Empty);
    h.silence (64);
    CHECK (h.status (0).state == State::Recording);
    CHECK (h.status (0).length == 64);
    h.press (Action::Clear, 0);
    h.silence (480);
    CHECK (h.status (0).state == State::Empty);

    // Within 50 ms after a bar line: at once, bar 1 there
    Harness g;
    g.patch[PID::LooperQuantise] = 1.0f;
    g.patch[PID::MetronomeOn] = 1.0f;
    g.silence (96000 + 1000);
    g.press (Action::Rec, 0);
    CHECK (! g.status (0).armed);
    CHECK (g.status (0).state == State::Recording);
    const int64_t f = g.frame;
    g.silence (100000);
    const auto heard = clicksFrom (g, f - 2400);
    REQUIRE (heard.size() >= 2);
    CHECK (heard[0].first == 96000); // the bar line's click, not again at the press
    CHECK (heard[1].first == f + 24000);
}

TEST_CASE ("looper: clicks lock to the master loop; Play starts on an accented bar 1", "[looper][tempo]")
{
    Harness h;
    h.patch[PID::MetronomeOn] = 1.0f;
    h.silence (5000);
    // 200000 frames at 120 BPM: two bars of 4/4 (8 beats of 25000)
    constexpr int64_t m = 200000;
    const int64_t f = h.frame;
    recordTrack (h, 0, noise ((size_t) m, 80));
    CHECK (h.status (0).state == State::Playing);
    CHECK (h.looper.status().masterBars == 2);
    h.silence ((size_t) (2 * m + 12500));
    for (const auto& [frame, accent] : clicksFrom (h, f + m))
    {
        const int64_t phase = (frame - f) % m;
        const auto k = (int64_t) std::llround ((double) phase / 25000.0);
        CHECK (phase == std::llround ((double) k * m / 8.0));
        CHECK (accent == (k % 4 == 0));
    }
    CHECK (clicksFrom (h, f + m).size() == 16 + 1);

    // Stopped mid-beat, the free clock carries on from there; Play restarts on bar 1
    h.press (Action::PlayStop, -1);
    const int64_t s = h.frame;
    h.silence (72000);
    const auto free = clicksFrom (h, s);
    REQUIRE (free.size() == 3);
    CHECK (std::abs (free[0].first - (s + 12000)) < 100); // (the tracks' 3 ms fade out still runs the master's)
    CHECK (std::abs (free[2].first - free[1].first - 24000) <= 1);
    h.press (Action::PlayStop, -1);
    const int64_t p = h.frame;
    h.silence (30000);
    const auto played = clicksFrom (h, p);
    REQUIRE (played.size() >= 2);
    CHECK (played[0].first == p);
    CHECK (played[0].second);
    CHECK (played[1].first == p + 25000);
    CHECK (! played[1].second);
    CHECK (h.looper.status().beat == 1);
}

TEST_CASE ("looper: following a playing host, clicks fall on whole quarter notes", "[looper][tempo]")
{
    Harness h;
    h.block = 100;
    h.host = true;
    h.ppq = 3.5;
    h.patch[PID::MetronomeOn] = 1.0f;
    h.silence (100000);
    CHECK (h.looper.status().hostGrid);
    const auto heard = clicksFrom (h, 0);
    REQUIRE (heard.size() == 4);
    for (int k = 0; k < 4; ++k)
    {
        CHECK (heard[(size_t) k].first == 12000 + 24000 * k); // ppq 4, 5, 6, 7
        CHECK (heard[(size_t) k].second == (k == 0));
    }

    // Count-in: from the host's next bar line (ppq 8), recording on the one after (ppq 12)
    h.patch[PID::CountIn] = 1.0f;
    const int64_t f = h.frame; // ppq 7.67
    h.press (Action::Rec, 0);
    CHECK (h.status (0).armed);
    CHECK (h.looper.status().countInBeatsLeft == 5);
    const auto toRecord = (int64_t) std::llround ((12.0 - h.ppq) * 24000.0);
    h.silence ((size_t) (toRecord - 1));
    CHECK (h.status (0).state == State::Empty);
    h.silence (100);
    CHECK (h.status (0).state == State::Recording);
    for (const auto& [frame, accent] : clicksFrom (h, f))
    {
        const double beat = 3.5 + (double) frame / 24000.0;
        CHECK (std::abs (beat - std::round (beat)) < 1.0e-4);
        CHECK (accent == ((int64_t) std::llround (beat) % 4 == 0));
    }
}

TEST_CASE ("looper: the metronome runs without a pool; rec only gates it", "[looper][tempo]")
{
    Looper looper;
    looper.setIdleTimeout (0.0);
    looper.prepare (rate);
    Patch patch = Patch::defaults();
    patch[PID::MetronomeOn] = 1.0f;
    patch[PID::BeatsPerBar] = (float) BeatsPerBar::Three;
    std::vector<float> l (512), r (512);
    std::vector<int64_t> heard;
    for (int64_t at = 0; at < 100000; at += 512)
    {
        looper.setPatch (patch);
        looper.process (l.data(), r.data(), 512);
        for (int c = 0; c < looper.clickCount(); ++c)
        {
            heard.push_back (at + looper.clicks()[c].offset);
            CHECK (looper.clicks()[c].accent == (heard.size() % 3 == 1));
        }
    }
    CHECK (heard == std::vector<int64_t> { 0, 24000, 48000, 72000, 96000 });
    CHECK (looper.status().beatsPerBar == 3);

    Harness h;
    h.patch[PID::MetronomeOn] = 1.0f;
    h.patch[PID::MetronomeRecOnly] = 1.0f;
    h.silence (50000);
    CHECK (h.clicks.empty());
    h.press (Action::Rec, 0);
    h.silence (50000);
    CHECK (h.clicks.size() == 2);
    h.press (Action::Rec, 0);
    h.silence (50000);
    CHECK (h.clicks.size() == 2);
}

TEST_CASE ("looper: the beat grid doesn't allocate", "[looper][tempo]")
{
    Harness h;
    h.patch[PID::MetronomeOn] = 1.0f;
    h.patch[PID::CountIn] = 1.0f;
    h.patch[PID::LooperQuantise] = 1.0f;
    h.bpm = 137.0;
    std::vector<float> l (48000), r (48000);
    auto audio = [&] (int frames)
    {
        counting = true;
        for (int pos = 0; pos + 64 <= frames; pos += 64)
        {
            h.looper.setPatch (h.patch);
            h.looper.setTransport (h.bpm, h.host, h.ppq);
            h.looper.process (l.data() + pos % 40000, r.data() + pos % 40000, 64);
            h.ppq += 64 * h.bpm / (60.0 * rate);
        }
        counting = false;
    };
    allocations = 0;
    h.press (Action::Rec, 0); // count-in, then a quantised master
    audio (100000);
    h.press (Action::Rec, 0);
    audio (150000);
    h.press (Action::Rec, 1);
    audio (60000);
    h.press (Action::Rec, 1);
    h.press (Action::PlayStop, -1);
    audio (20000);
    h.press (Action::PlayStop, -1);
    h.host = true;
    audio (20000);
    h.press (Action::ClearAll, -1);
    h.patch[PID::CountIn] = 0.0f;
    audio (20000);
    h.press (Action::Rec, 2); // waits for the host's bar line
    audio (90000);
    h.press (Action::Stop, 2);
    audio (5000);
    CHECK (allocations == 0);
    CHECK (h.status (0).state == State::Empty);
    CHECK (h.status (2).state != State::Empty);
}
