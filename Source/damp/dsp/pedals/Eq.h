#pragma once

#include "Drives.h"

namespace damp::pedals
{

// BOSS GE-7-style graphic EQ: seven octave-spaced bands (100 Hz .. 6.4 kHz), each +-15 dB, plus
// an output level. Each band is a constant-Q peaking filter (Q 1.41, an octave wide): two
// neighbours boosted alike make a fairly flat shelf between them, as on the hardware, and a band
// moves the response little two octaves away. With every slider at 0 the bands drop out exactly.
class Eq final : public DrivePedal
{
public:
    static constexpr int numBands = 7;

    void reset() override;
    void setPatch (const Patch&, double rate, int blockSize) override;
    void process (int channel, float* x, int n) override;

private:
    // Cytomic's trapezoidal state-variable bell in double precision: unlike a float biquad it
    // stays accurate with a 100 Hz band at the oversampled rate, and it can be retuned per sample
    struct Band
    {
        double ic1 = 0.0, ic2 = 0.0;
        double a1 = 1.0, a2 = 0.0, a3 = 0.0, m1 = 0.0;
        double gain = 1.0; // smoothed sqrt of the linear gain (the filter's "A")
    };
    struct State
    {
        std::array<Band, numBands> bands;
        int countdown = 0; // samples until the coefficients are next updated
    };
    void updateBand (Band&, int index) const;

    std::array<State, 2> state;
    std::array<double, numBands> g {}, target {}; // per band: prewarped tan(pi f / rate), and A's target
    double smoothing = 1.0;
    int controlInterval = 1;
};

} // namespace damp::pedals
