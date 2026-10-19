#pragma once

#include <array>
#include <cmath>
#include <vector>

namespace damp
{

// A common-cathode 12AX7 stage's static transfer curve: Koren's triode equations solved on the
// stage's load line, tabulated against the grid voltage (relative to its bias point). Built once,
// off the audio thread.
struct TriodeCircuit
{
    double plateResistor = 100.0e3;
    double cathodeResistor = 1.5e3;
    double supply = 300.0;
    bool cathodeBypassed = true; // a bypass cap holds the cathode at its quiescent voltage
};

class TriodeCurve
{
public:
    static constexpr double lowest = -12.0, highest = 6.0; // grid volts around the bias point
    static constexpr int size = 4096;

    void build (const TriodeCircuit&);

    // Plate voltage swing (volts, inverted like the real stage) for a grid swing of v volts
    float operator() (float v) const
    {
        const float x = (v - (float) lowest) * scale;
        if (x <= 0.0f)
            return table.front();
        if (x >= (float) (size - 1))
            return table.back();
        const int i = (int) x;
        const float f = x - (float) i;
        return table[(size_t) i] + f * (table[(size_t) i + 1] - table[(size_t) i]);
    }

    double gain() const { return smallSignalGain; }    // dVp/dVg at the bias point (negative)
    double bias() const { return quiescentGridToCathode; } // Vgk at rest (negative)
    double platePoint() const { return quiescentPlate; }

    // Koren's 12AX7 model: plate current (A) for plate and grid-cathode voltages
    static double plateCurrent (double vp, double vgk);

private:
    // Plate voltage when the grid sits at vg (cathode handled per the circuit)
    static double solve (const TriodeCircuit&, double vg, double fixedCathode);

    std::vector<float> table = std::vector<float> ((size_t) size, 0.0f);
    float scale = (float) (size - 1) / (float) (highest - lowest);
    double smallSignalGain = 0.0, quiescentGridToCathode = 0.0, quiescentPlate = 0.0;
};

} // namespace damp
