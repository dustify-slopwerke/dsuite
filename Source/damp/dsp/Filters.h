#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace damp
{

// One-pole filters, topology-preserving (trapezoidal) form
struct OnePole
{
    float g = 0.0f, s = 0.0f;

    void setCutoff (double hz, double rate)
    {
        const double w = std::tan (std::numbers::pi * std::min (hz, 0.49 * rate) / rate);
        g = (float) (w / (1.0 + w));
    }
    // Without prewarping: accurate well below the corner however high it is, even past Nyquist
    // (prewarping a corner near Nyquist pushes the analog prototype's corner off to infinity)
    void setCutoffUnwarped (double hz, double rate)
    {
        const double w = std::numbers::pi * hz / rate;
        g = (float) (w / (1.0 + w));
    }
    float lowpass (float x)
    {
        const float v = (x - s) * g;
        const float y = v + s;
        s = y + v;
        return y;
    }
    float highpass (float x) { return x - lowpass (x); }
    void reset() { s = 0.0f; }
};

// Second-order section (RBJ cookbook designs), transposed direct form II
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0f; }

    void setLowpass (double hz, double q, double rate) { design (hz, q, rate, Kind::Lowpass, 0.0); }
    void setHighpass (double hz, double q, double rate) { design (hz, q, rate, Kind::Highpass, 0.0); }
    void setBandpass (double hz, double q, double rate) { design (hz, q, rate, Kind::Bandpass, 0.0); } // 0 dB peak
    void setPeak (double hz, double q, double db, double rate) { design (hz, q, rate, Kind::Peak, db); }
    void setLowShelf (double hz, double db, double rate) { design (hz, 0.7071, rate, Kind::LowShelf, db); }
    void setHighShelf (double hz, double db, double rate) { design (hz, 0.7071, rate, Kind::HighShelf, db); }

private:
    enum class Kind { Lowpass, Highpass, Bandpass, Peak, LowShelf, HighShelf };

    void design (double hz, double q, double rate, Kind kind, double db)
    {
        const double w = 2.0 * std::numbers::pi * std::clamp (hz, 1.0, 0.49 * rate) / rate;
        const double c = std::cos (w), s = std::sin (w), alpha = s / (2.0 * q), a = std::pow (10.0, db / 40.0);
        double n0 = 1, n1 = 0, n2 = 0, d0 = 1, d1 = 0, d2 = 0;
        switch (kind)
        {
            case Kind::Lowpass:  n0 = n2 = (1 - c) / 2; n1 = 1 - c; d0 = 1 + alpha; d1 = -2 * c; d2 = 1 - alpha; break;
            case Kind::Highpass: n0 = n2 = (1 + c) / 2; n1 = -(1 + c); d0 = 1 + alpha; d1 = -2 * c; d2 = 1 - alpha; break;
            case Kind::Bandpass: n0 = alpha; n1 = 0; n2 = -alpha; d0 = 1 + alpha; d1 = -2 * c; d2 = 1 - alpha; break;
            case Kind::Peak:
                n0 = 1 + alpha * a; n1 = -2 * c; n2 = 1 - alpha * a; d0 = 1 + alpha / a; d1 = -2 * c; d2 = 1 - alpha / a;
                break;
            case Kind::LowShelf:
            {
                const double r = 2 * std::sqrt (a) * alpha;
                n0 = a * ((a + 1) - (a - 1) * c + r); n1 = 2 * a * ((a - 1) - (a + 1) * c); n2 = a * ((a + 1) - (a - 1) * c - r);
                d0 = (a + 1) + (a - 1) * c + r; d1 = -2 * ((a - 1) + (a + 1) * c); d2 = (a + 1) + (a - 1) * c - r;
                break;
            }
            case Kind::HighShelf:
            {
                const double r = 2 * std::sqrt (a) * alpha;
                n0 = a * ((a + 1) + (a - 1) * c + r); n1 = -2 * a * ((a - 1) + (a + 1) * c); n2 = a * ((a + 1) + (a - 1) * c - r);
                d0 = (a + 1) - (a - 1) * c + r; d1 = 2 * ((a - 1) - (a + 1) * c); d2 = (a + 1) - (a - 1) * c - r;
                break;
            }
        }
        b0 = (float) (n0 / d0);
        b1 = (float) (n1 / d0);
        b2 = (float) (n2 / d0);
        a1 = (float) (d1 / d0);
        a2 = (float) (d2 / d0);
    }
};

// A control value that moves to its new target in a straight line across one block
struct Ramp
{
    float value = 0.0f, step = 0.0f;
    void snap (float v) { value = v; step = 0.0f; }
    void toward (float target, int samples) { step = samples > 0 ? (target - value) / (float) samples : 0.0f; if (samples <= 0) value = target; }
    float next() { value += step; return value; }
};

} // namespace damp
