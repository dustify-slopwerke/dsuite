#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

// Tempo arithmetic for the looper and its UI, free of JUCE so it can be tested: bar lengths, where
// a quantised loop closes, the tempo a loop implies, and tap tempo.
namespace damp::tempo
{

constexpr double minBpm = 40.0, maxBpm = 240.0;

// A metronome click, `offset` frames into the block it falls in; accented on the bar's first beat
struct Click
{
    int offset = 0;
    bool accent = false;
};

// Frames per beat / bar (exact, not rounded)
inline double beatFrames (double bpm, double rate) { return rate * 60.0 / (bpm > 0.0 ? bpm : 120.0); }
inline double barFrames (double bpm, double rate, int beatsPerBar) { return beatFrames (bpm, rate) * beatsPerBar; }

// Where a quantised recording of `length` frames closes: on the next whole bar (at least one),
// or on the bar it has just passed if that's no more than `toleranceFrames` ago (bar 1 or later).
// Bar k is at llround (k * barExact).
inline int64_t quantisedClose (int64_t length, double barExact, int64_t toleranceFrames)
{
    const auto passed = (int64_t) std::floor ((double) length / barExact);
    if (passed >= 1)
        if (const auto at = std::llround ((double) passed * barExact); at <= length && length - at <= toleranceFrames)
            return at;
    for (int64_t k = std::max<int64_t> (1, passed);; ++k)
        if (const auto at = std::llround ((double) k * barExact); at >= length)
            return at;
}

// The nearest whole number of bars (at least one) to `length`, in frames
inline int64_t wholeBars (int64_t length, double barExact)
{
    const auto k = std::max<int64_t> (1, std::llround ((double) length / barExact));
    return std::llround ((double) k * barExact);
}

// The tempo that makes a loop of `loopFrames` a whole number of bars (1 to 64): of those in
// [lo, hi] the nearest the current tempo (in ratio), else the one nearest that range. None if
// even that falls outside minBpm..maxBpm.
inline std::optional<double> tempoFromLoop (int64_t loopFrames, double rate, int beatsPerBar, double currentBpm,
                                            double lo = 70.0, double hi = 180.0)
{
    if (loopFrames <= 0 || rate <= 0.0 || beatsPerBar <= 0)
        return std::nullopt;
    const double seconds = (double) loopFrames / rate;
    const double current = currentBpm > 0.0 ? currentBpm : 120.0;
    std::optional<double> best, fallback;
    double bestScore = 0.0, fallbackScore = 0.0;
    for (int bars = 1; bars <= 64; ++bars)
    {
        const double bpm = 60.0 * beatsPerBar * bars / seconds;
        if (bpm >= lo && bpm <= hi)
        {
            const double score = std::abs (std::log (bpm / current));
            if (! best || score < bestScore)
            {
                best = bpm;
                bestScore = score;
            }
        }
        else
        {
            const double score = bpm < lo ? std::log (lo / bpm) : std::log (bpm / hi);
            if (! fallback || score < fallbackScore)
            {
                fallback = bpm;
                fallbackScore = score;
            }
        }
    }
    const auto bpm = best ? best : fallback;
    if (! bpm || *bpm < minBpm || *bpm > maxBpm)
        return std::nullopt;
    return bpm;
}

// Tap tempo: the mean of the last five intervals between taps (milliseconds). A gap of over two
// seconds starts again; an interval more than 40% off the running mean is dropped (a missed or
// doubled tap), unless the next one agrees with it: then the player changed tempo, and the two
// start the mean afresh. A tempo is known from the second tap, clamped to minBpm..maxBpm.
class TapTempo
{
public:
    static constexpr double resetMs = 2000.0, outlier = 0.4;
    static constexpr int maxIntervals = 5;

    // Returns the tempo after this tap, if known
    std::optional<double> tap (double ms)
    {
        if (taps > 0 && (ms - last > resetMs || ms <= last))
            reset();
        if (taps > 0)
        {
            const double interval = ms - last;
            const double mean = this->mean();
            if (count == 0 || std::abs (interval - mean) <= outlier * mean)
            {
                if (count == maxIntervals)
                {
                    std::rotate (intervals, intervals + 1, intervals + maxIntervals);
                    --count;
                }
                intervals[count++] = interval;
                rejected = 0.0;
            }
            else if (rejected > 0.0 && std::abs (interval - rejected) <= outlier * rejected)
            {
                intervals[0] = rejected;
                intervals[1] = interval;
                count = 2;
                rejected = 0.0;
            }
            else
            {
                rejected = interval;
            }
        }
        last = ms;
        ++taps;
        return bpm();
    }

    std::optional<double> bpm() const
    {
        if (count == 0)
            return std::nullopt;
        return std::clamp (60000.0 / mean(), minBpm, maxBpm);
    }

    void reset()
    {
        taps = count = 0;
        last = rejected = 0.0;
    }

private:
    double mean() const
    {
        double sum = 0.0;
        for (int i = 0; i < count; ++i)
            sum += intervals[i];
        return count > 0 ? sum / count : 0.0;
    }

    double intervals[maxIntervals] {};
    int count = 0, taps = 0;
    double last = 0.0, rejected = 0.0; // the last interval dropped (0: none)
};

} // namespace damp::tempo
