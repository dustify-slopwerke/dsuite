#pragma once

#include "Filters.h"
#include "Triode.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace damp
{

// A 12AX7 gain stage at the oversampled rate: input coupling cap, cathode-bypass shelf, grid
// conduction with the bias shift it leaves on the coupling cap (the "blocking" that makes an
// overdriven stage sputter and compress), the tube's own curve, and the Miller / plate-cap
// treble loss.
struct StageSpec
{
    TriodeCircuit circuit;
    double bypassCap = 22.0e-6;   // across the cathode resistor; 0 = unbypassed
    double couplingHz = 20.0;     // coupling cap into this grid
    double lowpassHz = 16000.0;   // Miller capacitance and any plate bypass cap
    double gridLeakSeconds = 0.05; // how long a bias shift takes to bleed off (coupling cap x grid leak)
    double outputScale = 1.0;     // fixed divider after the plate
};

class PreampStage
{
public:
    // The curve must outlive the stage (they're built once and shared)
    void prepare (const StageSpec& spec, const TriodeCurve& curve, const TriodeCurve& unbypassed, double rate)
    {
        tube = &curve;
        coupling.setCutoff (spec.couplingHz, rate);
        plate.setCutoff (spec.lowpassHz, rate);
        outputScale = (float) spec.outputScale;
        gridHeadroom = (float) -curve.bias();

        // Partial bypass: below fz = 1 / (2 pi Rk Ck) the stage falls to its unbypassed gain
        shelfRatio = 1.0f;
        if (spec.circuit.cathodeBypassed && spec.bypassCap > 0.0)
        {
            const double fz = 1.0 / (2.0 * std::numbers::pi * spec.circuit.cathodeResistor * spec.bypassCap);
            shelfRatio = (float) (unbypassed.gain() / curve.gain());
            shelf.setCutoff (fz / shelfRatio, rate); // the pole sits above the zero by the gain ratio
        }

        charge = (float) (1.0 - std::exp (-1.0 / (0.0025 * rate))); // coupling cap through the source impedance: ~100k x 22n
        bleed = (float) std::exp (-1.0 / (spec.gridLeakSeconds * rate));
        reset();
    }

    void reset()
    {
        coupling.reset();
        plate.reset();
        shelf.reset();
        shift = 0.0f;
    }

    float process (float x)
    {
        x = coupling.highpass (x);
        if (shelfRatio < 1.0f)
            x = shelfRatio * x + (1.0f - shelfRatio) * shelf.highpass (x); // low shelf down to the unbypassed gain

        // The grid conducts once it rises above the cathode, which clamps it; the current it
        // draws charges the coupling cap, shifting the bias colder until it bleeds away
        float v = x - shift;
        if (v > gridHeadroom)
        {
            const float over = v - gridHeadroom;
            const float clamped = 0.4f * std::tanh (over * 2.5f);
            shift += (over - clamped) * charge;
            v = gridHeadroom + clamped;
        }
        shift *= bleed;

        return outputScale * plate.lowpass ((*tube) (v));
    }

    float biasShift() const { return shift; }

private:
    const TriodeCurve* tube = nullptr;
    OnePole coupling, plate, shelf;
    float shelfRatio = 1.0f, outputScale = 1.0f, gridHeadroom = 1.5f;
    float shift = 0.0f, charge = 0.0f, bleed = 1.0f;
};

// Push-pull power section with its negative feedback loop. The loop has no delay in it (it's
// solved for each sample by Newton's method), so heavy feedback stays stable at any rate. The
// presence and resonance (depth) controls take treble and bass out of the feedback, which turns
// them up at the output; supply sag lowers the ceiling as the amp draws current.
struct PowerSpec
{
    double drive = 1.0;          // grid drive per volt from the tone stack, at full master
    double openLoopGain = 4.0;
    double feedback = 0.75;      // 0 = no feedback loop (presence becomes a treble cut)
    double idle = 0.3;           // class AB idle current: crossover smoothness
    double mismatch = 0.04;      // imbalance between the two sides (even harmonics)
    double sag = 0.3;            // ceiling drop at full draw
    double sagSeconds = 0.08;
    double presenceHz = 2500.0, resonanceHz = 90.0;
    double bandwidthHz = 18000.0; // output transformer
};

#ifndef DAMP_NEWTON_ITERATIONS
 #define DAMP_NEWTON_ITERATIONS 4
#endif

class PowerAmp
{
public:
    static constexpr int newtonIterations = DAMP_NEWTON_ITERATIONS;

    void prepare (const PowerSpec& newSpec, double sampleRate)
    {
        spec = newSpec;
        rate = sampleRate;
        transformer.setCutoff (spec.bandwidthHz, rate);
        presenceFilter.setCutoff (spec.presenceHz, rate);
        resonanceFilter.setCutoff (spec.resonanceHz, rate);
        sagAttack = (float) (1.0 - std::exp (-1.0 / (0.25 * spec.sagSeconds * rate)));
        sagRelease = (float) (1.0 - std::exp (-1.0 / (spec.sagSeconds * rate)));
        k = 4.0f;
        bias = (float) spec.idle;
        norm = 1.0f / (2.0f * slope (0.0f, 1.0f)); // unity small-signal gain
        reset();
    }

    void reset()
    {
        transformer.reset();
        presenceFilter.reset();
        resonanceFilter.reset();
        y = 0.0f;
        draw = 0.0f;
    }

    // presence, resonance 0..1; sagAmount scales the model's sag
    void setControls (float presence, float resonance, float sagAmount)
    {
        p = presence;
        r = resonance;
        sagScale = sagAmount;
    }

    float process (float u)
    {
        const float ceiling = std::max (0.3f, 1.0f - sagScale * (float) spec.sag * draw);
        const float A = (float) spec.openLoopGain, beta = (float) spec.feedback;

        if (beta > 0.0f)
        {
            // Every filter in the loop is linear in this sample's output: feedback = c y + d
            const float G = transformer.g, Gp = presenceFilter.g, Gr = resonanceFilter.g;
            const float mix = (1.0f - p) + p * Gp - r * Gr;
            const float c = G * mix;
            const float d = (1.0f - G) * transformer.s * mix + p * (1.0f - Gp) * presenceFilter.s - r * (1.0f - Gr) * resonanceFilter.s;
            const float base = A * (u - beta * d), slopeIn = A * beta * c;
            for (int i = 0; i < newtonIterations; ++i)
            {
                const float x = base - slopeIn * y;
                const float g = y - norm * curve (x, ceiling);
                const float dg = 1.0f + slopeIn * norm * (slope (x, ceiling) + slope (-x, ceiling));
                y -= g / dg;
            }
            const float out = transformer.lowpass (y);
            presenceFilter.lowpass (out);
            resonanceFilter.lowpass (out);
            draw += (std::abs (y) - draw) * (std::abs (y) > draw ? sagAttack : sagRelease);
            return out;
        }

        // No loop (class A, cathode biased): presence acts as a treble cut instead
        y = norm * curve (A * u, ceiling);
        draw += (std::abs (y) - draw) * (std::abs (y) > draw ? sagAttack : sagRelease);
        const float out = transformer.lowpass (y);
        const float dark = presenceFilter.lowpass (out);
        return out + (1.0f - p) * 0.7f * (dark - out);
    }

private:
    // One side's plate current for grid drive z: softplus turn-on, saturating at the ceiling
    float side (float z, float ceiling) const
    {
        const float a = k * (z + bias);
        const float softplus = a > 20.0f ? a : std::log1p (std::exp (a));
        return ceiling * std::tanh (softplus / (k * ceiling));
    }
    float sideSlope (float z, float ceiling) const
    {
        const float a = k * (z + bias);
        const float softplus = a > 20.0f ? a : std::log1p (std::exp (a));
        const float t = std::tanh (softplus / (k * ceiling));
        return (1.0f - t * t) / (1.0f + std::exp (-a));
    }
    // Push-pull: the two sides' currents subtract. Mismatched sides leave an offset at rest,
    // which the output transformer wouldn't pass: take it out.
    float curve (float x, float ceiling) const
    {
        const float m = 1.0f - (float) spec.mismatch;
        return side (x, ceiling) - m * side (-x, ceiling) - (1.0f - m) * side (0.0f, ceiling);
    }
    // d curve / dx = side'(x) + (1 - mismatch) side'(-x); slope (x) + slope (-x) above is close enough for Newton
    float slope (float x, float ceiling) const { return sideSlope (x, ceiling); }

    PowerSpec spec;
    double rate = 48000.0;
    OnePole transformer, presenceFilter, resonanceFilter;
    float k = 4.0f, bias = 0.3f, norm = 1.0f;
    float p = 0.5f, r = 0.5f, sagScale = 1.0f;
    float y = 0.0f, draw = 0.0f, sagAttack = 0.0f, sagRelease = 0.0f;
};

} // namespace damp
