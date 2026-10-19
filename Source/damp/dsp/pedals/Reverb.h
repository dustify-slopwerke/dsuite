#pragma once

#include "Pedal.h"

#include "../Filters.h"

#include <array>
#include <vector>

namespace damp::pedals
{

// The reverb pedal: three tanks behind a pre-delay, the dry signal always at unity and the wet
// added on top.
//
//   Spring: a Fender-style three-spring tank. Each spring is a feedback loop around a cascade of
//           stretched first-order allpasses (after Välimäki, Parker and Abel's parametric spring
//           model), which delays the lows more than the highs: every echo arrives as a falling
//           "drip" chirp. The loops are 26-33 ms long and wobble a little, the tank is driven
//           through a soft clipper (loud picking splashes but stays bounded) and the whole thing
//           is band-limited to about 100 Hz - 4.5 kHz, like the real transducers.
//   Plate:  Dattorro's figure-of-eight plate tank: six input diffusers, then two cross-coupled
//           halves with modulated allpasses. Dense from the start, bright, no discrete echoes.
//   Hall:   early reflections (a tapped line fed through diffusers, spread across the stereo
//           field), then an
//           eight-line feedback delay network with an allpass in each line and slowly modulated
//           lengths: a slower, diffuse build-up and a smooth tail.
//
// The pedal has tails (see Pedal): switched off it stops feeding the tank and either lets it ring
// out (Trails) or fades the wet out over 10 ms. A mode change fades the wet out, clears the new
// tank and fades back in. Everything a block needs (gains, ramps, the mode) is settled in setPatch,
// so both channels of a block see the same values however they're called.
class Reverb final : public Pedal
{
public:
    void prepare (double maxRate) override;
    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;
    void processStereo (float* left, float* right, int n) override;

    bool hasTails() const override { return true; }
    void setEngaged (bool on) override;
    bool ringing() const override { return live; }

    // A delay line read back by whole or fractional samples. tap (0) is the newest sample.
    class Line
    {
    public:
        void allocate (int samples);
        void clear();
        void push (float x)
        {
            write = (write + 1) & mask;
            buffer[(size_t) write] = x;
        }
        float tap (int d) const { return buffer[(size_t) ((write - d) & mask)]; }
        float tapLinear (float d) const
        {
            const int whole = (int) d;
            const float f = d - (float) whole;
            const float a = tap (whole);
            return a + f * (tap (whole + 1) - a);
        }
        // Cubic (Hermite) interpolation; d >= 1
        float tapCubic (float d) const
        {
            const int whole = (int) d;
            const float f = d - (float) whole;
            const float ym1 = tap (whole - 1), y0 = tap (whole), y1 = tap (whole + 1), y2 = tap (whole + 2);
            const float c1 = 0.5f * (y1 - ym1);
            const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
            const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
            return ((c3 * f + c2) * f + c1) * f + y0;
        }

    private:
        std::vector<float> buffer = std::vector<float> (8, 0.0f);
        int mask = 7, write = 0;
    };

    // Schroeder allpass around a Line: delay `d` samples, coefficient g
    struct Allpass
    {
        Line line;
        float process (float x, int d, float g)
        {
            const float z = line.tap (d - 1);
            const float w = x + g * z;
            line.push (w);
            return z - g * w;
        }
        float processModulated (float x, float d, float g)
        {
            const float z = line.tapCubic (d - 1.0f);
            const float w = x + g * z;
            line.push (w);
            return z - g * w;
        }
    };

    // What each tank is told per block
    struct Settings
    {
        double rate = 48000.0;
        double rt60 = 2.0; // seconds
        float tone = 0.5f; // 0 .. 1
    };

    // A tank: mono in (after the pre-delay), stereo wet out
    class Spring
    {
    public:
        void allocate (double maxRate);
        void clear();
        void configure (const Settings&);
        void process (const float* in, float* left, float* right, int n);

    private:
        static constexpr int springs = 3, stages = 60, chunk = 64;
        struct Unit
        {
            Line loop;
            std::vector<float> cascade; // stages x stretch: the allpasses' states
            OnePole damp, block;
            float coefficient = -0.6f, gain = 0.0f, length = 1000.0f;
            double phase = 0.0;
        };
        void runCascade (Unit&, float* x, int n) const;

        std::array<Unit, springs> units;
        int stretch = 5, stretchCapacity = 5, position = 0;
        double rate = 48000.0;
        float wobble = 1.0f; // samples
        Biquad inputCut, outLowL1, outLowL2, outLowR1, outLowR2, outCutL, outCutR;
        OnePole inputDark;
    };

    class Plate
    {
    public:
        void allocate (double maxRate);
        void clear();
        void configure (const Settings&);
        void process (const float* in, float* left, float* right, int n);

    private:
        std::array<Allpass, 6> diffusers;
        std::array<Allpass, 2> modulated, decayDiffusers;
        std::array<Line, 4> delays;
        std::array<OnePole, 2> damping;
        OnePole inputCut, bandwidth, cutL, cutR;
        std::array<float, 2> carry {};
        std::array<int, 6> diffuserLength {};
        std::array<int, 4> delayLength {};
        std::array<int, 2> decayDiffuserLength {};
        std::array<float, 2> modulatedLength {};
        std::array<int, 14> outputTap {};
        double rate = 48000.0, phase = 0.0, phaseStep = 0.0;
        float decay = 0.5f, excursion = 4.0f;
    };

    class Hall
    {
    public:
        void allocate (double maxRate);
        void clear();
        void configure (const Settings&);
        void process (const float* in, float* left, float* right, int n);

    private:
        static constexpr int lines = 8, reflections = 12;
        Line early;
        std::array<Allpass, 6> diffusers;
        std::array<Line, lines> delay;
        std::array<Allpass, lines> inner;
        std::array<OnePole, lines> damping;
        std::array<float, lines> gain {}, length {}, feedback {};
        std::array<double, lines> phase {};
        std::array<int, reflections> reflectionTap {};
        std::array<int, 6> diffuserLength {};
        std::array<int, lines> innerLength {};
        OnePole erDark, cutL, cutR;
        double rate = 48000.0;
        float excursion = 10.0f;
    };

private:
    struct Channel
    {
        Line predelay;
        Spring spring;
        Plate plate;
        Hall hall;
    };
    void run (Channel&, const float* in, float* wetLeft, float* wetRight, int n);
    void clearTank (ReverbMode);
    void finish (float peak, int n, bool countQuiet);

    static constexpr int chunk = 128; // internal block
    std::array<Channel, 2> channels;
    std::array<float, chunk> feed {}, wetL {}, wetR {};

    double rate = 48000.0;
    ReverbMode mode = ReverbMode::Spring, wantMode = ReverbMode::Spring;
    Settings settings;
    bool engaged = false, trails = true, fresh = true, live = false;

    // Per block: start values and per-sample steps, shared by both channels
    float fade = 1.0f, fadeFrom = 1.0f, fadeStep = 0.0f;  // wet out (trails off, mode change)
    float gate = 0.0f, gateFrom = 0.0f, gateStep = 0.0f;  // tank input (engaged)
    float wetFrom = 0.0f, wetTo = 0.0f;                   // mix
    float delayFrom = 0.0f, delayTo = 0.0f;               // pre-delay, samples
    int blockSamples = 0;
    int quiet = 0, quietNeeded = 0;
};

} // namespace damp::pedals
