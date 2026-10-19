#include "Cabinet.h"

#include "common/dsp/Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace damp
{
namespace
{
using Complex = std::complex<double>;
constexpr double pi = std::numbers::pi;

// Analog prototype sections, evaluated at s = j 2 pi f
Complex at (double f) { return { 0.0, 2.0 * pi * f }; }

double highpass2 (double f, double f0, double q)
{
    const auto s = at (f);
    const double w = 2.0 * pi * f0;
    return std::abs (s * s / (s * s + s * w / q + w * w));
}

double lowpass2 (double f, double f0, double q)
{
    const auto s = at (f);
    const double w = 2.0 * pi * f0;
    return std::abs (w * w / (s * s + s * w / q + w * w));
}

double highpass1 (double f, double f0)
{
    const auto s = at (f);
    return std::abs (s / (s + 2.0 * pi * f0));
}

double bell (double f, double f0, double db, double q)
{
    const auto s = at (f);
    const double w = 2.0 * pi * f0, a = std::pow (10.0, db / 40.0);
    return std::abs ((s * s + s * (a * w / q) + w * w) / (s * s + s * (w / (a * q)) + w * w));
}

// Bass boost below f0 by db (a microphone's proximity effect)
double lowShelf (double f, double f0, double db)
{
    const auto s = at (f);
    const double w = 2.0 * pi * f0, g = std::pow (10.0, db / 20.0);
    return std::abs ((s + w * g) / (s + w));
}

struct Bell
{
    double hz, db, q;
};

struct CabSpec
{
    double resonanceHz, resonanceQ; // the speaker's free-air resonance in the box
    double cancelHz;                // open back: the rear wave cancels the bass below about here
    double rolloffHz, rolloffQ;     // where the cone stops radiating on axis
    std::array<Bell, 4> breakup;    // cone breakup peaks and dips
    double neighbourMs, neighbourGain; // a second speaker heard slightly later
};

CabSpec specFor (CabType cab)
{
    switch (cab)
    {
        case CabType::Open1x12:
            return { 85.0, 1.1, 160.0, 5200.0, 0.9, { { { 2300.0, 4.0, 3.0 }, { 4100.0, 3.0, 4.0 }, { 900.0, -2.0, 1.5 }, { 1400.0, 1.0, 2.0 } } }, 0.0, 0.0 };
        case CabType::Open2x12:
            return { 90.0, 1.2, 120.0, 5000.0, 1.0, { { { 2000.0, 3.0, 3.0 }, { 3500.0, 4.0, 4.0 }, { 750.0, -2.0, 1.5 }, { 1200.0, 1.0, 2.0 } } }, 0.11, 0.35 };
        case CabType::Closed4x12:
        case CabType::Off:
        case CabType::User:
            break;
    }
    return { 105.0, 1.0, 0.0, 5400.0, 1.1, { { { 1900.0, 3.0, 2.5 }, { 3300.0, 5.0, 3.5 }, { 650.0, -2.5, 1.2 }, { 4300.0, 2.0, 4.0 } } }, 0.15, 0.5 };
}

double magnitude (const CabSettings& c, const CabSpec& spec, double f)
{
    const double p = std::clamp ((double) c.position, 0.0, 1.0), d = std::clamp ((double) c.distance, 0.0, 1.0);

    double m = highpass2 (f, spec.resonanceHz, spec.resonanceQ);
    if (spec.cancelHz > 0.0)
        m *= highpass1 (f, spec.cancelHz);

    // Off axis (towards the cone's edge) and further back, the treble falls away earlier
    const double rolloff = spec.rolloffHz * (1.0 - 0.5 * p) * (1.0 - 0.2 * d);
    m *= lowpass2 (f, rolloff, spec.rolloffQ) * lowpass2 (f, rolloff * 1.15, 0.6);
    for (const auto& b : spec.breakup)
        m *= bell (f, b.hz, b.db * (1.0 - 0.6 * p), b.q);
    m *= bell (f, 2500.0, -4.0 * p, 1.0);

    double proximity = 0.0;
    switch (c.mic)
    {
        case MicType::Dynamic:
            m *= bell (f, 5500.0, 4.0, 1.2) * lowpass2 (f, 14000.0, 0.7);
            proximity = 4.0;
            break;
        case MicType::Ribbon:
            m *= lowpass2 (f, 7500.0, 0.6) * bell (f, 1000.0, 1.0, 0.7);
            proximity = 9.0;
            break;
        case MicType::Condenser:
            m *= bell (f, 9000.0, 3.0, 0.8);
            proximity = 3.0;
            break;
    }
    m *= lowShelf (f, 200.0, proximity * (1.0 - d) * (1.0 - d));
    return m;
}

// Minimum-phase impulse response with the given magnitude at bins 0 .. n/2 (real cepstrum)
std::vector<float> minimumPhase (const std::vector<double>& mag, int n)
{
    const dustify::Fft fft (n);
    std::vector<std::complex<float>> x ((size_t) n);
    for (int k = 0; k <= n / 2; ++k)
    {
        const float l = (float) std::log (std::max (mag[(size_t) k], 1.0e-5));
        x[(size_t) k] = l;
        if (k > 0 && k < n / 2)
            x[(size_t) (n - k)] = l;
    }
    fft.inverse (x.data()); // real cepstrum (times n)
    for (int i = 0; i < n; ++i)
    {
        const float fold = i == 0 || i == n / 2 ? 1.0f : i < n / 2 ? 2.0f : 0.0f;
        x[(size_t) i] = x[(size_t) i].real() * fold / (float) n;
    }
    fft.forward (x.data());
    for (auto& v : x)
        v = std::exp (v);
    fft.inverse (x.data());

    std::vector<float> h ((size_t) n);
    for (int i = 0; i < n; ++i)
        h[(size_t) i] = x[(size_t) i].real() / (float) n;
    return h;
}

// Fades out the last `fraction` of the response
void fadeTail (std::vector<float>& h, double fraction)
{
    const auto n = (int) h.size(), fade = std::max (1, (int) (fraction * n));
    for (int i = 0; i < fade; ++i)
        h[(size_t) (n - fade + i)] *= (float) (0.5 + 0.5 * std::cos (pi * (i + 1) / fade));
}

void addEcho (std::vector<float>& h, const std::vector<float>& source, double delaySamples, float gain)
{
    // Fractional delay by linear interpolation is plenty for a reflection
    const int whole = (int) delaySamples;
    const float frac = (float) (delaySamples - whole);
    for (size_t i = 0; i < source.size(); ++i)
    {
        const size_t at1 = i + (size_t) whole;
        if (at1 < h.size())
            h[at1] += gain * (1.0f - frac) * source[i];
        if (at1 + 1 < h.size())
            h[at1 + 1] += gain * frac * source[i];
    }
}
} // namespace

CabSettings cabSettings (const Patch& patch)
{
    return { patch.choice<CabType> (PID::Cab), patch.choice<MicType> (PID::Mic), patch[PID::MicPosition], patch[PID::MicDistance] };
}

std::vector<float> modelledCabinet (const CabSettings& c, double sampleRate)
{
    if (c.cab == CabType::Off || c.cab == CabType::User)
        return { 1.0f };

    const auto spec = specFor (c.cab);
    const int length = (int) std::lround (0.1 * sampleRate);
    int n = 1;
    while (n < 4 * length)
        n *= 2;

    std::vector<double> mag ((size_t) n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k)
        mag[(size_t) k] = magnitude (c, spec, k * sampleRate / n);
    auto full = minimumPhase (mag, n);
    full.resize ((size_t) length);

    // A neighbouring speaker, then the floor and (further back) the far wall
    auto h = full;
    const double ms = sampleRate / 1000.0, d = std::clamp ((double) c.distance, 0.0, 1.0);
    if (spec.neighbourGain > 0.0)
        addEcho (h, full, spec.neighbourMs * (0.5 + c.position) * ms, (float) spec.neighbourGain);
    addEcho (h, full, (0.4 + 2.6 * d) * ms, (float) (0.15 + 0.3 * d));
    if (d > 0.3)
        addEcho (h, full, (5.0 + 3.0 * d) * ms, (float) (0.2 * (d - 0.3)));

    fadeTail (h, 0.2);
    levelImpulse (h, sampleRate);
    return h;
}

std::vector<float> prepareUserImpulse (const std::vector<float>& impulse, double impulseRate, double sampleRate, double maxSeconds)
{
    if (impulse.empty() || impulseRate <= 0.0)
        return { 1.0f };

    std::vector<float> h;
    const double ratio = sampleRate / impulseRate;
    const auto inLength = (int) std::min<size_t> (impulse.size(), (size_t) (maxSeconds * impulseRate) + 1);
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        h.assign (impulse.begin(), impulse.begin() + inLength);
    }
    else
    {
        // Windowed sinc, cut off below the lower of the two Nyquists
        constexpr int zeros = 24;
        const double cutoff = 0.95 * std::min (1.0, ratio);
        const auto outLength = (size_t) std::ceil (inLength * ratio);
        h.resize (outLength);
        for (size_t i = 0; i < outLength; ++i)
        {
            const double t = (double) i / ratio; // position in input samples
            const double reach = zeros / cutoff;
            double sum = 0.0;
            for (auto j = (int) std::ceil (t - reach); j <= (int) std::floor (t + reach); ++j)
            {
                if (j < 0 || j >= inLength)
                    continue;
                const double x = t - j;
                const double sinc = x == 0.0 ? 1.0 : std::sin (pi * cutoff * x) / (pi * cutoff * x);
                const double w = 0.42 + 0.5 * std::cos (pi * x / reach) + 0.08 * std::cos (2.0 * pi * x / reach); // Blackman
                sum += impulse[(size_t) j] * cutoff * sinc * w;
            }
            h[i] = (float) sum;
        }
    }

    const auto maxLength = (size_t) (maxSeconds * sampleRate);
    if (h.size() > maxLength)
    {
        h.resize (maxLength);
        fadeTail (h, 0.05);
    }
    levelImpulse (h, sampleRate);
    return h;
}

void levelImpulse (std::vector<float>& h, double sampleRate)
{
    // Average dB of the response over log-spaced frequencies
    constexpr int points = 48;
    double sumDb = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double f = 100.0 * std::pow (50.0, (double) i / (points - 1));
        const double w = 2.0 * pi * f / sampleRate;
        Complex acc = 0.0;
        for (size_t n = 0; n < h.size(); ++n)
            acc += (double) h[n] * std::polar (1.0, -w * (double) n);
        sumDb += 20.0 * std::log10 (std::max (std::abs (acc), 1.0e-9));
    }
    const auto scale = (float) std::pow (10.0, -sumDb / points / 20.0);
    for (auto& s : h)
        s *= scale;
}

} // namespace damp
