#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace damp
{

PitchDetector::PitchDetector (double sampleRate, int windowSize)
    : rate (sampleRate), window (windowSize)
{
    maxLag = std::min (window / 2, (int) (sampleRate / 40.0));
    minLag = std::max (2, (int) (sampleRate / 1500.0));
    difference.assign ((size_t) maxLag + 2, 0.0);
    normalised.assign ((size_t) maxLag + 2, 0.0);
}

PitchDetector::Result PitchDetector::detect (const float* x)
{
    // Squared difference between the window and itself shifted by each lag, over the half window
    // the largest lag still fits in
    const int span = window - maxLag - 1;
    double energy = 0.0;
    for (int i = 0; i < span; ++i)
        energy += (double) x[i] * x[i];
    if (energy < 1.0e-9 * span) // silence
        return {};

    for (int lag = 1; lag <= maxLag + 1; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i < span; ++i)
        {
            const double d = (double) x[i] - x[i + lag];
            sum += d * d;
        }
        difference[(size_t) lag - 1] = sum;
    }

    // Cumulative mean normalised difference; take the first dip below the threshold
    double running = 0.0;
    for (int lag = 1; lag <= maxLag + 1; ++lag)
    {
        running += difference[(size_t) lag - 1];
        normalised[(size_t) lag - 1] = running > 0.0 ? difference[(size_t) lag - 1] * lag / running : 1.0;
    }
    auto cmnd = [&] (int lag) { return normalised[(size_t) lag - 1]; };
    auto raw = [&] (int lag) { return difference[(size_t) lag - 1]; };

    constexpr double threshold = 0.15;
    int best = -1;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        if (cmnd (lag) < threshold)
        {
            while (lag + 1 <= maxLag && cmnd (lag + 1) < cmnd (lag))
                ++lag;
            best = lag;
            break;
        }
    }
    if (best < 0)
        return {};
    const double clarity = 1.0 - cmnd (best);

    // Parabola through a dip of the raw difference
    auto refine = [&] (int lag)
    {
        const double a = raw (lag - 1), b = raw (lag), c = raw (lag + 1);
        const double denominator = a - 2.0 * b + c;
        return lag + (std::abs (denominator) > 1.0e-12 ? std::clamp (0.5 * (a - c) / denominator, -0.5, 0.5) : 0.0);
    };
    const double period = refine (best);

    // Precision grows with the lag measured: refine the dip at the largest multiple of the period
    // that fits, and divide back down
    const int cycles = (int) ((maxLag - 1) / period);
    if (cycles >= 2)
    {
        int lag = (int) std::lround (cycles * period);
        for (int step = 0; step < 3; ++step) // walk to the bottom of that dip
        {
            if (lag > 2 && raw (lag - 1) < raw (lag))
                --lag;
            else if (lag + 1 <= maxLag && raw (lag + 1) < raw (lag))
                ++lag;
        }
        return { rate * cycles / refine (lag), clarity };
    }
    return { rate / period, clarity };
}

NoteReading nearestNote (double hz, double a4)
{
    if (hz <= 0.0)
        return {};
    const double semitones = 12.0 * std::log2 (hz / a4) + 69.0;
    const int note = (int) std::lround (semitones);
    return { note, 100.0 * (semitones - note) };
}

} // namespace damp
