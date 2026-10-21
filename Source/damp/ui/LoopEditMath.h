#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

// The looper editor's arithmetic, kept free of JUCE so it can be tested: start / end edits and
// the limits the engine applies to them, the waveform view (pixels <-> recording frames, with
// zoom and scroll), snapping to the tempo grid and nudging.
namespace damp::loopedit
{

// A track's edits, in recording frames: start may be negative and end past the recording (the
// loop is padded with silence there)
struct Edits
{
    int64_t start = 0, end = 0;
    int64_t length() const { return end - start; }
    bool operator== (const Edits&) const = default;
};

// What Looper::setEdits accepts: padding up to the pool's length on either side, and a loop of at
// least minLoop frames
struct Limits
{
    int64_t recorded = 0; // frames in the recording
    int64_t maxPad = 0;
    int64_t minLoop = 16;
};

// As the engine clamps them (start first, then end)
inline Edits clamp (Edits e, const Limits& l)
{
    e.start = std::clamp (e.start, -l.maxPad, l.recorded + l.maxPad - l.minLoop);
    e.end = std::clamp (e.end, e.start + l.minLoop, l.recorded + l.maxPad);
    return e;
}

// Moving one handle leaves the other where it is: the moved one stops minLoop short of it
inline Edits withStart (Edits e, int64_t start, const Limits& l)
{
    e.start = std::clamp (start, -l.maxPad, std::min (e.end, l.recorded + l.maxPad) - l.minLoop);
    return clamp (e, l);
}

inline Edits withEnd (Edits e, int64_t end, const Limits& l)
{
    e.end = std::clamp (end, std::max (e.start, -l.maxPad) + l.minLoop, l.recorded + l.maxPad);
    return clamp (e, l);
}

// Silence before the recording, and after it
inline int64_t padBefore (const Edits& e) { return std::max<int64_t> (0, -e.start); }
inline int64_t padAfter (const Edits& e, int64_t recorded) { return std::max<int64_t> (0, e.end - recorded); }

// ---- The view: `first` is the frame at the left edge, `framesPerPixel` the zoom ----

struct Range
{
    double first = 0.0, last = 0.0;
    double span() const { return last - first; }
};

struct View
{
    double first = 0.0, framesPerPixel = 1.0;
    int width = 1;

    double frameAt (double x) const { return first + x * framesPerPixel; }
    double xAt (double frame) const { return (frame - first) / framesPerPixel; }
    double last() const { return first + (double) width * framesPerPixel; }
};

// What the view can show: the recording and the loop, whichever reaches further, with a margin
// either side (so a handle at the edge can be grabbed and padding added)
inline Range domainFor (const Edits& e, int64_t recorded, double marginFraction = 0.06)
{
    const double first = (double) std::min<int64_t> (0, e.start);
    const double last = std::max ((double) std::max (recorded, e.end), first + 1.0);
    const double margin = (last - first) * marginFraction;
    return { first - margin, last + margin };
}

// The whole domain in `width` pixels
inline View fit (const Range& domain, int width)
{
    width = std::max (1, width);
    return { domain.first, std::max (domain.span(), 1.0) / (double) width, width };
}

// Keeps the view inside the domain (when it's zoomed in) or centred on it (zoomed out)
inline View constrain (View v, const Range& domain)
{
    const double span = v.framesPerPixel * (double) v.width;
    if (span >= domain.span())
        v.first = domain.first - (span - domain.span()) * 0.5;
    else
        v.first = std::clamp (v.first, domain.first, domain.last - span);
    return v;
}

// Zooms by `factor` (> 1: in) keeping the frame under `anchorX` where it is, between
// `minFramesPerPixel` and the whole domain
inline View zoom (View v, double factor, double anchorX, const Range& domain, double minFramesPerPixel)
{
    const double anchor = v.frameAt (anchorX);
    const double widest = std::max (domain.span(), 1.0) / (double) std::max (1, v.width);
    v.framesPerPixel = std::clamp (v.framesPerPixel / factor, std::min (minFramesPerPixel, widest), widest);
    v.first = anchor - anchorX * v.framesPerPixel;
    return constrain (v, domain);
}

inline bool zoomedIn (const View& v, const Range& domain)
{
    return v.framesPerPixel * (double) v.width < domain.span() * 0.999;
}

// ---- Seeking ----

// A press that moves no further than this (pixels) is a click: it seeks; further, a drag
constexpr int clickSlop = 4;
inline bool isClick (int dx, int dy) { return std::abs (dx) <= clickSlop && std::abs (dy) <= clickSlop; }

// The offset into the loop (frames after the start handle, as Looper::seek takes it) of a click
// at recording frame `frame`; none outside the loop. Padding is part of the loop.
inline std::optional<int64_t> seekOffset (double frame, const Edits& e)
{
    const auto f = (int64_t) std::floor (frame);
    if (f < e.start || f >= e.end)
        return std::nullopt;
    return f - e.start;
}

// Where to seek to hear the seam (end into start) at once: `preRoll` seconds before the end, but
// no further back than half the loop
inline int64_t seamOffset (const Edits& e, double rate, double preRollSeconds)
{
    const auto pre = std::min<int64_t> (std::llround (preRollSeconds * rate), e.length() / 2);
    return e.length() - std::max<int64_t> (1, pre);
}

// ---- Tempo ----

inline double framesPerBeat (double bpm, double rate)
{
    return bpm > 0.0 ? rate * 60.0 / bpm : rate;
}

inline double framesPerBar (double bpm, double rate, int beatsPerBar)
{
    return framesPerBeat (bpm, rate) * std::max (1, beatsPerBar);
}

// Bar and beat (both from 1) at `frames` into a loop, e.g. bar 2 beat 3 ("2.3")
struct BarBeat
{
    int64_t bar = 1;
    int beat = 1;
    bool operator== (const BarBeat&) const = default;
};

inline BarBeat barBeat (int64_t frames, double rate, double bpm, int beatsPerBar)
{
    const auto beats = (int64_t) std::floor ((double) std::max<int64_t> (0, frames) / framesPerBeat (bpm, rate) + 1.0e-9);
    const int perBar = std::max (1, beatsPerBar);
    return { beats / perBar + 1, (int) (beats % perBar) + 1 };
}

// The grid line (origin + k * step) nearest `frame`
inline int64_t snapToGrid (int64_t frame, double origin, double step)
{
    if (step <= 0.0)
        return frame;
    return (int64_t) std::llround (origin + std::round (((double) frame - origin) / step) * step);
}

// Snapping a handle to the grid: the start to the beats counted from the recording's first frame
// (where Rec was pressed), the end to whole steps after the start, so the loop is a whole number
// of beats (or bars) wherever it starts
inline int64_t snapStart (int64_t frame, double step) { return snapToGrid (frame, 0.0, step); }

inline int64_t snapEnd (int64_t frame, int64_t start, double step)
{
    if (step <= 0.0)
        return frame;
    const auto k = std::max<int64_t> (1, std::llround ((double) (frame - start) / step));
    return start + (int64_t) std::llround ((double) k * step);
}

enum class Nudge { Ms1, Ms10, Beat };

inline int64_t nudgeFrames (Nudge n, double rate, double bpm)
{
    switch (n)
    {
        case Nudge::Ms1: return std::max<int64_t> (1, std::llround (rate * 0.001));
        case Nudge::Ms10: return std::max<int64_t> (1, std::llround (rate * 0.010));
        case Nudge::Beat: return std::max<int64_t> (1, std::llround (framesPerBeat (bpm, rate)));
    }
    return 1;
}

inline double toMs (int64_t frames, double rate) { return rate > 0.0 ? (double) frames * 1000.0 / rate : 0.0; }
inline double toBeats (int64_t frames, double rate, double bpm) { return (double) frames / framesPerBeat (bpm, rate); }

} // namespace damp::loopedit
