#pragma once

#include <algorithm>
#include <array>
#include <iterator>
#include <string_view>

namespace dustify
{

// Historical 12-note temperaments. Only ever append: presets and sessions store the index.
enum class Tuning : int { Equal, Just, Pythagorean, Meantone, Werckmeister3, Kirnberger3, Vallotti };

// Choice lists for tuning parameters
inline constexpr std::string_view tuningNames[] { "Equal", "Just (5-limit)", "Pythagorean", "1/4-Comma Meantone",
                                                  "Werckmeister III", "Kirnberger III", "Vallotti" };
inline constexpr std::string_view noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// Offset of each pitch class (0 = the tuning root) from equal temperament, in semitones
inline const std::array<float, 12>& tuningOffsets (Tuning t)
{
    // Cents of each degree above the root, converted once to offsets from equal temperament
    static constexpr double cents[][12] {
        { 0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 },
        // 1, 16/15, 9/8, 6/5, 5/4, 4/3, 45/32, 3/2, 8/5, 5/3, 9/5, 15/8
        { 0, 111.731, 203.910, 315.641, 386.314, 498.045, 590.224, 701.955, 813.686, 884.359, 1017.596, 1088.269 },
        // Pure fifths, wolf between G# and Eb
        { 0, 90.225, 203.910, 294.135, 407.820, 498.045, 611.730, 701.955, 792.180, 905.865, 996.090, 1109.775 },
        { 0, 76.049, 193.157, 310.265, 386.314, 503.422, 579.471, 696.578, 772.627, 889.735, 1006.843, 1082.892 },
        { 0, 90.225, 192.180, 294.135, 390.225, 498.045, 588.270, 696.090, 792.180, 888.270, 996.090, 1092.180 },
        { 0, 90.225, 193.157, 294.135, 386.314, 498.045, 590.224, 696.578, 792.180, 889.735, 996.090, 1088.269 },
        { 0, 94.135, 196.090, 298.045, 392.180, 501.955, 592.180, 698.045, 796.090, 894.135, 1000.000, 1090.225 },
    };
    static const auto tables = []
    {
        std::array<std::array<float, 12>, std::size (cents)> result {};
        for (size_t k = 0; k < result.size(); ++k)
            for (size_t i = 0; i < 12; ++i)
                result[k][i] = (float) (cents[k][i] / 100.0 - (double) i);
        return result;
    }();
    return tables[std::min ((size_t) t, tables.size() - 1)];
}

} // namespace dustify
