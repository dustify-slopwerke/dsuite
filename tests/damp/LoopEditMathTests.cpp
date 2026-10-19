#include "ui/LoopEditMath.h"

#include "dsp/looper/Looper.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

using namespace damp::loopedit;

namespace
{
const Limits limits { 48000, 96000, damp::Looper::minLoop };
}

TEST_CASE ("Loop edits are clamped as the engine clamps them", "[looper][ui]")
{
    CHECK (clamp ({ 0, 48000 }, limits) == Edits { 0, 48000 });
    CHECK (clamp ({ -200000, 48000 }, limits) == Edits { -96000, 48000 });              // padding up to maxPad
    CHECK (clamp ({ 0, 400000 }, limits) == Edits { 0, 48000 + 96000 });
    CHECK (clamp ({ 1000, 1005 }, limits) == Edits { 1000, 1000 + damp::Looper::minLoop }); // a minimum length
    CHECK (clamp ({ 200000, 300000 }, limits).start == 48000 + 96000 - damp::Looper::minLoop);
}

TEST_CASE ("Moving a loop handle leaves the other where it is", "[looper][ui]")
{
    const Edits e { 1000, 20000 };
    CHECK (withStart (e, -500, limits) == Edits { -500, 20000 });
    CHECK (withStart (e, 25000, limits) == Edits { 20000 - damp::Looper::minLoop, 20000 }); // can't pass the end
    CHECK (withStart (e, -500000, limits) == Edits { -96000, 20000 });
    CHECK (withEnd (e, 60000, limits) == Edits { 1000, 60000 });                          // padding after
    CHECK (withEnd (e, 0, limits) == Edits { 1000, 1000 + damp::Looper::minLoop });
    CHECK (withEnd (e, 900000, limits) == Edits { 1000, 48000 + 96000 });

    CHECK (padBefore ({ -480, 100 }) == 480);
    CHECK (padBefore ({ 480, 1000 }) == 0);
    CHECK (padAfter ({ 0, 50000 }, 48000) == 2000);
    CHECK (padAfter ({ 0, 40000 }, 48000) == 0);
}

TEST_CASE ("The loop view maps pixels to frames, zooms about a point and stays in range", "[looper][ui]")
{
    // The domain covers padding before and after the recording, plus a margin
    const auto domain = domainFor ({ -1000, 12000 }, 10000, 0.0);
    CHECK (domain.first == -1000.0);
    CHECK (domain.last == 12000.0);
    const auto wide = domainFor ({ 0, 10000 }, 10000, 0.1);
    CHECK (wide.first == Catch::Approx (-1000.0));
    CHECK (wide.last == Catch::Approx (11000.0));

    auto v = fit (domain, 1300);
    CHECK (v.framesPerPixel == Catch::Approx (10.0));
    CHECK (v.frameAt (0.0) == Catch::Approx (-1000.0));
    CHECK (v.frameAt (1300.0) == Catch::Approx (12000.0));
    CHECK (v.xAt (0.0) == Catch::Approx (100.0));
    CHECK (v.xAt (v.frameAt (437.0)) == Catch::Approx (437.0));
    CHECK_FALSE (zoomedIn (v, domain));

    // Zooming keeps the frame under the anchor in place
    const double anchorFrame = v.frameAt (300.0);
    auto z = zoom (v, 4.0, 300.0, domain, 0.25);
    CHECK (z.framesPerPixel == Catch::Approx (2.5));
    CHECK (z.frameAt (300.0) == Catch::Approx (anchorFrame));
    CHECK (zoomedIn (z, domain));

    // ... unless that would show beyond the domain: then it's pushed back inside
    auto edge = zoom (v, 4.0, 0.0, domain, 0.25);
    CHECK (edge.first == Catch::Approx (domain.first));
    edge.first = 1.0e9;
    edge = constrain (edge, domain);
    CHECK (edge.last() == Catch::Approx (domain.last));

    // Zoom stops at the closest allowed, and zooming out stops at the whole domain
    auto deep = zoom (v, 1.0e6, 650.0, domain, 0.25);
    CHECK (deep.framesPerPixel == Catch::Approx (0.25));
    auto out = zoom (deep, 1.0e-6, 650.0, domain, 0.25);
    CHECK (out.framesPerPixel == Catch::Approx (v.framesPerPixel));
    CHECK (out.first == Catch::Approx (domain.first));
    CHECK_FALSE (zoomedIn (out, domain));
}

TEST_CASE ("Loop handles snap to beats, and nudges are sized by the rate and tempo", "[looper][ui]")
{
    const double beat = framesPerBeat (120.0, 48000.0);
    CHECK (beat == Catch::Approx (24000.0));
    CHECK (framesPerBeat (0.0, 48000.0) == Catch::Approx (48000.0)); // no tempo: a second

    CHECK (snapToGrid (11000, 0.0, beat) == 0);
    CHECK (snapToGrid (13000, 0.0, beat) == 24000);
    CHECK (snapToGrid (-13000, 0.0, beat) == -24000); // into the padding
    CHECK (snapToGrid (1234, 0.0, 0.0) == 1234);
    CHECK (snapToGrid (10100, 100.0, 1000.0) == 10100);

    // The start snaps to the grid from the recording's first frame, the end to whole beats after
    // the start: the loop's length is a whole number of beats
    CHECK (snapStart (-14000, beat) == -24000);
    const int64_t start = 3000;
    CHECK (snapEnd (60000, start, beat) == start + 48000);
    CHECK (snapEnd (start + 100, start, beat) == start + 24000); // at least one step
    CHECK (snapEnd (start + 5 * 24000 + 9000, start, 4.0 * beat) == start + 96000);

    CHECK (nudgeFrames (Nudge::Ms1, 48000.0, 120.0) == 48);
    CHECK (nudgeFrames (Nudge::Ms10, 44100.0, 120.0) == 441);
    CHECK (nudgeFrames (Nudge::Beat, 48000.0, 90.0) == 32000);
    CHECK (nudgeFrames (Nudge::Ms1, 500.0, 120.0) == 1); // never zero

    CHECK (toMs (24000, 48000.0) == Catch::Approx (500.0));
    CHECK (toBeats (-12000, 48000.0, 120.0) == Catch::Approx (-0.5));
}

TEST_CASE ("A click seeks within the loop; a seam audition starts just before the end", "[looper][ui]")
{
    CHECK (isClick (0, 0));
    CHECK (isClick (-clickSlop, clickSlop));
    CHECK_FALSE (isClick (clickSlop + 1, 0));
    CHECK_FALSE (isClick (0, -clickSlop - 1));

    const Edits e { -4800, 52800 }; // 100 ms of padding either side of a second
    CHECK (seekOffset (-4800.0, e) == 0);   // the start handle, in the padding
    CHECK (seekOffset (-1.5, e) == 4798);    // still padding
    CHECK (seekOffset (0.0, e) == 4800);    // the recording's first frame
    CHECK (seekOffset (52799.9, e) == 57599);
    CHECK_FALSE (seekOffset (52800.0, e).has_value()); // past the end handle
    CHECK_FALSE (seekOffset (-4801.0, e).has_value());

    CHECK (seamOffset (e, 48000.0, 0.5) == 57600 - 24000);
    CHECK (seamOffset (e, 48000.0, 1.5) == 57600 / 2); // no more than half the loop
    CHECK (seamOffset ({ 0, 1000 }, 48000.0, 0.0) == 999); // the last frame at least
}

TEST_CASE ("The engine takes the edits the editor computes, padding included", "[looper][ui]")
{
    damp::Looper looper;
    looper.setIdleTimeout (0.0);
    looper.prepare (48000.0);
    auto patch = damp::Patch::defaults();
    patch[damp::PID::LooperOn] = 1.0f;
    looper.setPatch (patch);
    looper.createPool (48000.0, 10.0);

    std::vector<float> audio (48000, 0.25f);
    REQUIRE (looper.setTrackAudio (0, audio.data(), audio.data(), (int64_t) audio.size()) == 48000);

    const auto status = looper.status();
    const Limits l { 48000, std::llround (status.poolTotalSeconds * status.sampleRate), damp::Looper::minLoop };
    CHECK (l.maxPad >= 10 * 48000);

    for (const Edits wanted : { Edits { -4800, 52800 }, withStart ({ 0, 48000 }, -10'000'000, l), withEnd ({ 0, 48000 }, 99'000'000, l),
                                withEnd ({ 500, 48000 }, 0, l) })
    {
        looper.setTrackEdits (0, wanted.start, wanted.end);
        std::vector<float> left (64), right (64);
        looper.process (left.data(), right.data(), 64); // publishes the status
        const auto s = looper.trackStatus (0);
        CHECK (Edits { s.start, s.end } == clamp (wanted, l));
        CHECK (Edits { s.start, s.end } == wanted); // already within the engine's limits
    }
}
