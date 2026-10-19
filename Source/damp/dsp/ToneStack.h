#pragma once

#include <array>
#include <complex>

namespace damp
{

// The passive treble / middle / bass network of Fender, Marshall and most of their descendants,
// from D. T. Yeh and J. O. Smith's analysis ("Discretization of the '59 Fender Bassman tone
// stack", DAFx 2006): an exact third-order transfer function of the component values and the
// three pot positions, bilinear-transformed. Runs in double precision: its poles crowd z = 1 at
// oversampled rates.
struct ToneStackParts
{
    double r1, r2, r3, r4; // treble pot, bass pot, mid pot, slope resistor (ohms)
    double c1, c2, c3;     // treble, bass, mid caps (farads)
};

class ToneStack
{
public:
    // H(s) = (b1 s + b2 s^2 + b3 s^3) / (1 + a1 s + a2 s^2 + a3 s^3)
    struct Analog
    {
        double b1, b2, b3, a1, a2, a3;
    };

    // Pot positions 0..1 as the knobs read; the bass pot has an audio taper
    static Analog analog (const ToneStackParts&, double treble, double middle, double bass);
    static std::complex<double> response (const Analog&, double hz);

    void prepare (double sampleRate) { rate = sampleRate; reset(); }
    void reset() { s1 = s2 = s3 = 0.0; }

    void set (const ToneStackParts&, double treble, double middle, double bass);

    float process (float x)
    {
        // Transposed direct form II
        const double y = b[0] * x + s1;
        s1 = b[1] * x - a[1] * y + s2;
        s2 = b[2] * x - a[2] * y + s3;
        s3 = b[3] * x - a[3] * y;
        return (float) y;
    }

private:
    double rate = 48000.0;
    std::array<double, 4> b {}, a {};
    double s1 = 0.0, s2 = 0.0, s3 = 0.0;
};

} // namespace damp
