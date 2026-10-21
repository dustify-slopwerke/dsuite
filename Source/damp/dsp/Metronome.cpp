#include "Metronome.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace damp
{

void Metronome::prepare (double sampleRate)
{
    rate = sampleRate;
    smoothing = dustify::Smoother::coefficient (0.02, rate);
    attack = std::max (1, (int) std::lround (0.0005 * rate));
    length = (int) std::lround (0.040 * rate);
    decay = (float) std::exp (-1.0 / (0.007 * rate));
    reset();
}

void Metronome::reset()
{
    active = false;
    level.snap();
}

void Metronome::setPatch (float levelDb, bool accent)
{
    level.target = levelDb <= -48.0f ? 0.0f : std::pow (10.0f, levelDb / 20.0f);
    accentOn = accent;
}

void Metronome::start (bool accent)
{
    const bool loud = accent && accentOn;
    const double w = 2.0 * std::numbers::pi * (loud ? 1500.0 : 1000.0) / rate;
    rc = (float) std::cos (w);
    rs = (float) std::sin (w);
    c = 1.0f;
    s = 0.0f;
    gain = loud ? std::pow (10.0f, 4.0f / 20.0f) : 1.0f;
    age = 0;
    env = 0.0f;
    active = true;
}

void Metronome::render (float* left, float* right, int n, const tempo::Click* clicks, int count)
{
    if (! active && count == 0)
    {
        level.snap(); // nothing sounds: the level may as well be there already
        return;
    }
    int next = 0;
    for (int i = 0; i < n; ++i)
    {
        while (next < count && clicks[next].offset <= i)
            start (clicks[next++].accent);
        const float g = level.next (smoothing);
        if (! active)
            continue;
        env = age < attack ? (float) (age + 1) / (float) attack : env * decay;
        const float x = s * env * gain * g;
        left[i] += x;
        if (right != nullptr)
            right[i] += x;
        const float c2 = c * rc - s * rs;
        s = s * rc + c * rs;
        c = c2;
        if (++age >= length)
            active = false;
    }
}

} // namespace damp
