#include "KeyData.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace dpiano
{
namespace
{
using Points = std::initializer_list<std::pair<double, double>>;

// Piecewise-linear interpolation through (key, value) points, held flat beyond the ends
double interpolate (Points points, double key)
{
    auto prev = *points.begin();
    if (key <= prev.first)
        return prev.second;
    for (const auto& p : points)
    {
        if (key <= p.first)
            return prev.second + (p.second - prev.second) * (key - prev.first) / (p.first - prev.first);
        prev = p;
    }
    return prev.second;
}

// The same in the log domain, for quantities that vary over orders of magnitude
double interpolateLog (Points points, double key)
{
    auto prev = *points.begin();
    auto lerp = [key] (std::pair<double, double> a, std::pair<double, double> b)
    {
        return std::exp (std::log (a.second) + (std::log (b.second) - std::log (a.second)) * (key - a.first) / (b.first - a.first));
    };
    if (key <= prev.first)
        return prev.second;
    for (const auto& p : points)
    {
        if (key <= p.first)
            return lerp (prev, p);
        prev = p;
    }
    return prev.second;
}

// Width of an octave tuned beatless on the lower string's partial 2p against the upper string's
// partial p, divided by 2: how much wider than pure the strings' inharmonicity makes it
double octaveStretch (double bLow, double bHigh, int p)
{
    // 2p f0low sqrt (1 + 4p^2 bLow) = p f0high sqrt (1 + p^2 bHigh), and the pitch heard is the
    // first partial f0 sqrt (1 + b)
    return std::sqrt ((1.0 + 4.0 * p * p * bLow) / (1.0 + p * p * bHigh)) * std::sqrt ((1.0 + bHigh) / (1.0 + bLow));
}
} // namespace

KeyData keyData (int key)
{
    key = std::clamp (key, lowestKey, highestKey);
    const double k = key;
    KeyData d;
    d.key = key;

    // Steinway D style stringing: single wound strings A0..G1, wound bichords to C#3, then trichords
    d.strings = key <= 31 ? 1 : key <= 49 ? 2 : 3;

    // B is lowest around the bass/tenor break and rises steeply into the treble
    d.inharmonicity = interpolateLog ({ { 21, 2.4e-4 }, { 33, 1.3e-4 }, { 43, 1.0e-4 }, { 52, 1.8e-4 }, { 60, 3.8e-4 },
                                        { 72, 7.5e-4 }, { 84, 1.6e-3 }, { 96, 4.0e-3 }, { 108, 1.2e-2 } }, k);

    // Heavy wound bass strings down to thin treble wire (C2 35 g, C4 3.9 g, G6 0.47 g)
    d.stringMass = interpolateLog ({ { 21, 0.120 }, { 36, 0.035 }, { 60, 3.93e-3 }, { 91, 4.67e-4 }, { 108, 1.5e-4 } }, k);
    d.tension = interpolate ({ { 21, 1300 }, { 36, 750 }, { 60, 670 }, { 91, 750 }, { 108, 700 } }, k);

    // Hammers get lighter towards the treble. Masses after Chaigne & Askenfelt; felt stiffness K
    // and exponent p fitted so the partial balance and loudness at mf and ff come close to
    // recordings of a Steinway (University of Iowa MIS) from C2 to C8. The treble's spectrum
    // barely brightens from mf to ff, hence the modest p up there.
    d.hammerMass = interpolateLog ({ { 21, 9.0e-3 }, { 36, 4.9e-3 }, { 60, 2.97e-3 }, { 72, 2.6e-3 }, { 84, 2.3e-3 },
                                     { 96, 2.0e-3 }, { 108, 1.6e-3 } }, k);
    d.hammerStiffness = interpolateLog ({ { 21, 2.0e9 }, { 36, 7.0e9 }, { 48, 8.0e9 }, { 60, 8.0e9 }, { 72, 4.0e9 }, { 84, 2.0e9 },
                                          { 96, 1.0e10 }, { 108, 4.0e12 } }, k);
    d.hammerExponent = interpolate ({ { 21, 2.2 }, { 36, 2.3 }, { 60, 2.5 }, { 72, 2.5 }, { 84, 2.4 }, { 96, 2.4 }, { 108, 2.5 } }, k);
    d.strikePosition = interpolate ({ { 21, 0.12 }, { 70, 0.12 }, { 91, 0.0625 }, { 108, 0.05 } }, k);

    d.t60 = interpolateLog ({ { 21, 20.0 }, { 36, 14.0 }, { 48, 10.0 }, { 60, 7.0 }, { 72, 4.5 }, { 84, 2.5 }, { 96, 1.2 }, { 108, 0.5 } }, k);
    // b3 levels off in the treble, so partials above ~10 kHz there still die within about half a second
    d.highLoss = interpolateLog ({ { 21, 8.0e-9 }, { 36, 6.25e-9 }, { 60, 2.7e-9 }, { 72, 1.8e-9 }, { 84, 1.4e-9 },
                                   { 96, 1.2e-9 }, { 108, 1.0e-9 } }, k);
    d.aftersoundRatio = interpolate ({ { 21, 2.5 }, { 48, 3.5 }, { 72, 4.0 }, { 96, 3.0 }, { 108, 2.0 } }, k);

    // The hammer's knock through bridge and frame: barely there in the bass, a big part of the
    // attack at the top (Askenfelt 1993)
    // Levels set so the first 30 ms of a note hold about as much non-tonal energy as recordings
    d.knock = interpolateLog ({ { 21, 0.05 }, { 60, 0.4 }, { 72, 0.8 }, { 84, 2.0 }, { 96, 4.0 }, { 108, 5.0 } }, k);

    // Loudness evened out along the scale, to the relative levels of the recorded Steinway
    d.voicingDb = interpolate ({ { 21, 0.0 }, { 60, 0.0 }, { 72, 5.0 }, { 84, 4.0 }, { 96, 4.0 }, { 108, 2.0 } }, k);

    // Bass dampers stop the heavy strings more slowly; the top 1.5 octaves have no dampers
    d.damperT60 = key > 88 ? 0.0 : interpolateLog ({ { 21, 0.8 }, { 48, 0.35 }, { 72, 0.2 }, { 88, 0.15 } }, k);
    return d;
}

std::array<double, numKeys> stretchCents (double inharmonicityScale)
{
    std::array<double, numKeys> cents {};
    auto b = [inharmonicityScale] (int key) { return keyData (std::clamp (key, lowestKey, highestKey)).inharmonicity * inharmonicityScale; };

    // Each octave is tuned beatless on 4:2 (6:3 in the low bass, where higher partials dominate
    // what's heard), so it's wider than 2:1 by the strings' inharmonicity. Spreading each
    // octave's extra width over its semitones and adding up from A4 gives the Railsback curve.
    auto semitoneStretch = [&b] (int key) // cents added going from key to key + 1
    {
        const int low = key - 6; // the octave centred on this step
        return 1200.0 * std::log2 (octaveStretch (b (low), b (low + 12), low < 40 ? 3 : 2)) / 12.0;
    };

    auto at = [&cents] (int key) -> double& { return cents[(size_t) (key - lowestKey)]; };
    at (69) = 0.0;
    for (int key = 70; key <= highestKey; ++key)
        at (key) = at (key - 1) + semitoneStretch (key - 1);
    for (int key = 68; key >= lowestKey; --key)
        at (key) = at (key + 1) - semitoneStretch (key);
    return cents;
}

} // namespace dpiano
