#include "ToneStack.h"

#include <cmath>

namespace damp
{

ToneStack::Analog ToneStack::analog (const ToneStackParts& p, double t, double m, double bass)
{
    const double l = std::exp ((bass - 1.0) * 3.4); // audio-taper bass pot
    const double R1 = p.r1, R2 = p.r2, R3 = p.r3, R4 = p.r4, C1 = p.c1, C2 = p.c2, C3 = p.c3;

    Analog h;
    h.b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    h.b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
           + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
           + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
           + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
           + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    h.b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
           - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
           + m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
           + t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4
           + t * l * C1 * C2 * C3 * R1 * R2 * R4;
    h.a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    h.a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
           + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3)
           - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
           + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
           + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
    h.a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
           - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
           + m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4)
           + l * C1 * C2 * C3 * R1 * R2 * R4
           + C1 * C2 * C3 * R1 * R3 * R4;
    return h;
}

std::complex<double> ToneStack::response (const Analog& h, double hz)
{
    const std::complex<double> s (0.0, 2.0 * 3.14159265358979323846 * hz);
    return (h.b1 * s + h.b2 * s * s + h.b3 * s * s * s) / (1.0 + h.a1 * s + h.a2 * s * s + h.a3 * s * s * s);
}

void ToneStack::set (const ToneStackParts& parts, double treble, double middle, double bass)
{
    const auto h = analog (parts, treble, middle, bass);

    // Bilinear transform, s = c (1 - z^-1) / (1 + z^-1): each s^k term becomes
    // c^k (1 - z^-1)^k (1 + z^-1)^(3 - k)
    const double c = 2.0 * rate;
    const double num[4] { 0.0, h.b1 * c, h.b2 * c * c, h.b3 * c * c * c };
    const double den[4] { 1.0, h.a1 * c, h.a2 * c * c, h.a3 * c * c * c };
    // Coefficients of (1 - z^-1)^k (1 + z^-1)^(3 - k), k = 0..3
    constexpr double expand[4][4] { { 1, 3, 3, 1 }, { 1, 1, -1, -1 }, { 1, -1, -1, 1 }, { 1, -3, 3, -1 } };

    std::array<double, 4> nb {}, na {};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 4; ++i)
        {
            nb[(size_t) i] += num[k] * expand[k][i];
            na[(size_t) i] += den[k] * expand[k][i];
        }
    for (size_t i = 0; i < 4; ++i)
    {
        b[i] = nb[i] / na[0];
        a[i] = na[i] / na[0];
    }
}

} // namespace damp
