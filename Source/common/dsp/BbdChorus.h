#pragma once

#include "FastMath.h"
#include "Noise.h"
#include "Svf.h"

#include <cmath>
#include <vector>

namespace dustify
{

// Only ever append: presets and sessions store the index
enum class ChorusMode : int { Off, I, II, Both };

// Juno-60/106-style bucket-brigade chorus. The mono sum feeds one BBD line; two taps modulated by
// a triangle LFO in opposite directions give the left and right wet signals, which are added to
// the dry signal on each side. The BBD's anti-aliasing / reconstruction filters, a little clock
// noise and soft saturation are modelled; the delay itself is a clean interpolated line.
class BbdChorus
{
public:
    void prepare (double newRate)
    {
        sampleRate = newRate;
        size_t size = 1;
        while (size < (size_t) (0.02 * sampleRate))
            size <<= 1;
        buffer.assign (size, 0.0f);
        mask = size - 1;
        write = 0;
        phase = 0.0;
        for (auto* f : { &preFilter, &postL, &postR })
        {
            f->setup (sampleRate, 9000.0, 0.7071);
            f->reset();
        }
    }

    void setMode (ChorusMode newMode)
    {
        if (newMode != mode && newMode != ChorusMode::Off && mode == ChorusMode::Off)
            std::fill (buffer.begin(), buffer.end(), 0.0f); // no stale audio when switching on
        mode = newMode;
    }

    ChorusMode getMode() const { return mode; }

    void process (float* left, float* right, int n)
    {
        if (mode == ChorusMode::Off || buffer.empty())
            return;

        // Rates and delay sweep from Juno-60 measurements
        const double rate = mode == ChorusMode::I ? 0.513 : mode == ChorusMode::II ? 0.863 : 9.75;
        const double centreMs = mode == ChorusMode::Both ? 3.3 : 3.5;
        const double depthMs = mode == ChorusMode::Both ? 0.3 : 1.85;
        const double inc = rate / sampleRate;

        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (left[i] + right[i]);
            buffer[write] = preFilter.process (mono).low + 2.0e-5f * noise.bipolar();

            phase += inc;
            if (phase >= 1.0)
                phase -= 1.0;
            const double tri = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;

            const float wetL = postL.process (read ((centreMs + depthMs * tri) * 0.001 * sampleRate)).low;
            const float wetR = postR.process (read ((centreMs - depthMs * tri) * 0.001 * sampleRate)).low;

            left[i] = 0.75f * (left[i] + fastTanh (wetL));
            right[i] = 0.75f * (right[i] + fastTanh (wetR));

            write = (write + 1) & mask;
        }
    }

private:
    // Cubic Hermite read, delay in samples behind the newest written sample
    float read (double delay) const
    {
        const double pos = (double) write - delay;
        const auto base = (long) std::floor (pos);
        const auto t = (float) (pos - (double) base);
        auto at = [&] (long i) { return buffer[(size_t) i & mask]; };
        const float y0 = at (base - 1), y1 = at (base), y2 = at (base + 1), y3 = at (base + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + y1;
    }

    double sampleRate = 48000.0;
    ChorusMode mode = ChorusMode::Off;
    std::vector<float> buffer;
    size_t mask = 0, write = 0;
    double phase = 0.0;
    Svf preFilter, postL, postR;
    Random noise { 0xbbdu };
};

} // namespace dustify
