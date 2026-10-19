#include "Triode.h"

namespace damp
{

double TriodeCurve::plateCurrent (double vp, double vgk)
{
    // Norman Koren's 12AX7 fit
    constexpr double mu = 100.0, ex = 1.4, kg1 = 1060.0, kp = 600.0, kvb = 300.0;
    if (vp <= 0.0)
        return 0.0;
    const double x = kp * (1.0 / mu + vgk / std::sqrt (kvb + vp * vp));
    const double softplus = x > 30.0 ? x : std::log1p (std::exp (x));
    const double e1 = vp / kp * softplus;
    return e1 <= 0.0 ? 0.0 : 2.0 * std::pow (e1, ex) / kg1;
}

double TriodeCurve::solve (const TriodeCircuit& c, double vg, double fixedCathode)
{
    // The tube's current rises with plate voltage while the load line's falls: bisect the crossing
    double lo = 0.0, hi = c.supply;
    for (int i = 0; i < 60; ++i)
    {
        const double vp = 0.5 * (lo + hi);
        const double load = (c.supply - vp) / c.plateResistor;
        const double cathode = fixedCathode >= 0.0 ? fixedCathode : load * c.cathodeResistor;
        if (plateCurrent (vp, vg - cathode) > load)
            hi = vp;
        else
            lo = vp;
    }
    return 0.5 * (lo + hi);
}

void TriodeCurve::build (const TriodeCircuit& c)
{
    // At rest the grid sits at 0 V and the cathode wherever its own current puts it
    quiescentPlate = solve (c, 0.0, -1.0);
    const double cathode = (c.supply - quiescentPlate) / c.plateResistor * c.cathodeResistor;
    quiescentGridToCathode = -cathode;

    const double fixedCathode = c.cathodeBypassed ? cathode : -1.0;
    for (int i = 0; i < size; ++i)
    {
        const double v = lowest + (highest - lowest) * i / (size - 1);
        table[(size_t) i] = (float) (solve (c, v, fixedCathode) - quiescentPlate);
    }

    constexpr double dv = 0.01;
    smallSignalGain = (solve (c, dv, fixedCathode) - solve (c, -dv, fixedCathode)) / (2.0 * dv);
}

} // namespace damp
