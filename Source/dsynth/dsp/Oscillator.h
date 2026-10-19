#pragma once

#include "Params.h"

#include "common/dsp/FastMath.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dsynth
{

// Band-limited oscillator using polyBLEP (steps) and polyBLAMP (slope changes).
// Corrections are applied to both sides of each discontinuity, which costs one sample of
// latency but handles any jump, including hard-sync resets at arbitrary phases.
class Oscillator
{
public:
    void reset (double startPhase = 0.0)
    {
        phase = startPhase - std::floor (startPhase);
        pending = 0.0;
        wrapped = false;
        wrapDistance = 0.0;
    }

    // dt = frequency / sampleRate. syncDistance < 0: no sync this sample; otherwise the master
    // wrapped syncDistance samples before this sample (in [0, 1)) and this osc resets to phase 0.
    float process (double dt, Wave wave, float pulseWidth, double syncDistance = -1.0)
    {
        dt = std::clamp (dt, 1.0e-9, 0.45);
        const double pw = wave == Wave::Square ? 0.5 : std::clamp ((double) pulseWidth, 0.02, 0.98);
        double current = 0.0;

        // a = distance from the event to the current sample, in samples
        auto step = [&] (double jump, double a)
        {
            pending += 0.5 * jump * a * a;
            current -= 0.5 * jump * (1.0 - a) * (1.0 - a);
        };
        auto bend = [&] (double slopeChange, double a)
        {
            const double b = 1.0 - a;
            pending += slopeChange * a * a * a / 6.0;
            current += slopeChange * b * b * b / 6.0;
        };

        // Discontinuities crossed while the phase runs from p0 to p1 (p1 < p0 + 1); endOffset is
        // the distance from the end of this segment to the current sample.
        auto segment = [&] (double p0, double p1, double endOffset)
        {
            auto crossed = [&] (double edge, auto&& handle)
            {
                for (double e : { edge, edge + 1.0 })
                    if (p0 < e && e <= p1)
                        handle ((p1 - e) / dt + endOffset);
            };

            // Corrections are linear, so combined waves just add their components' corrections
            for (auto component : components (wave))
            {
                switch (component)
                {
                    case Wave::Saw: crossed (1.0, [&] (double a) { step (-2.0, a); }); break;
                    case Wave::Ramp: crossed (1.0, [&] (double a) { step (2.0, a); }); break;
                    case Wave::Square:
                    case Wave::Pulse:
                        crossed (pw, [&] (double a) { step (-2.0, a); });
                        crossed (1.0, [&] (double a) { step (2.0, a); });
                        break;
                    case Wave::Triangle:
                        crossed (0.5, [&] (double a) { bend (-8.0 * dt, a); });
                        crossed (1.0, [&] (double a) { bend (8.0 * dt, a); });
                        break;
                    case Wave::Sine:
                    case Wave::SawPulse: // combined waves never appear as components
                    case Wave::SawTri:
                    case Wave::TriPulse: break;
                }
            }
        };

        wrapped = false;

        if (syncDistance < 0.0)
        {
            const double p1 = phase + dt;
            segment (phase, p1, 0.0);
            phase = p1;
            if (phase >= 1.0)
            {
                phase -= 1.0;
                wrapped = true;
                wrapDistance = phase / dt;
            }
        }
        else
        {
            const double atReset = phase + dt * (1.0 - syncDistance);
            segment (phase, atReset, syncDistance);
            step (naive (wave, 0.0, pw) - naive (wave, atReset - std::floor (atReset), pw), syncDistance);
            phase = dt * syncDistance;
            wrapped = true;
            wrapDistance = syncDistance;
        }

        current += naive (wave, phase, pw);
        const auto out = (float) pending;
        pending = current;
        return out;
    }

    // Valid after process(): whether the phase wrapped during the last sample, and how far
    // before the sample it happened (used to hard-sync another oscillator).
    bool didWrap() const { return wrapped; }
    double wrapDistanceSamples() const { return wrapDistance; }

    static double naive (Wave wave, double p, double pw)
    {
        switch (wave)
        {
            case Wave::SawPulse: return naive (Wave::Saw, p, pw) + naive (Wave::Pulse, p, pw);
            case Wave::SawTri: return naive (Wave::Saw, p, pw) + naive (Wave::Triangle, p, pw);
            case Wave::TriPulse: return naive (Wave::Triangle, p, pw) + naive (Wave::Pulse, p, pw);
            case Wave::Sine: return fastSin2Pi (p);
            case Wave::Triangle: return p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p;
            case Wave::Saw: return 2.0 * p - 1.0;
            case Wave::Ramp: return 1.0 - 2.0 * p;
            case Wave::Square:
            case Wave::Pulse: return p < pw ? 1.0 : -1.0;
        }
        return 0.0;
    }

private:
    struct Components
    {
        Wave items[2];
        int count;
        const Wave* begin() const { return items; }
        const Wave* end() const { return items + count; }
    };

    static Components components (Wave wave)
    {
        switch (wave)
        {
            case Wave::SawPulse: return { { Wave::Saw, Wave::Pulse }, 2 };
            case Wave::SawTri: return { { Wave::Saw, Wave::Triangle }, 2 };
            case Wave::TriPulse: return { { Wave::Triangle, Wave::Pulse }, 2 };
            case Wave::Sine:
            case Wave::Triangle:
            case Wave::Saw:
            case Wave::Ramp:
            case Wave::Square:
            case Wave::Pulse: break;
        }
        return { { wave, wave }, 1 };
    }

    double phase = 0.0;
    double pending = 0.0;
    bool wrapped = false;
    double wrapDistance = 0.0;
};

} // namespace dsynth
