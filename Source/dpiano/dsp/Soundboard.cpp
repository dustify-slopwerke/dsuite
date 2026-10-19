#include "Soundboard.h"

#include "common/dsp/Noise.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dpiano
{
namespace
{
constexpr double twoPi = 2.0 * std::numbers::pi;
} // namespace

void Soundboard::prepare (double newRate)
{
    sampleRate = newRate;

    // Mode frequencies: about 10 Hz apart near the bottom, widening with frequency, jittered so
    // they don't line up with the strings' harmonic series. Fixed seed: every instance (and every
    // render) has the same board.
    dustify::Random rng (0x50a7d0u);
    double f = 48.0;
    for (int k = 0; k < numModes; ++k)
    {
        const auto i = (size_t) k;
        const double q = 30.0 + 0.02 * f; // loss factor of a spruce board, a little lower at the top
        const double sigma = std::numbers::pi * f / q;
        const double r = std::exp (-sigma / sampleRate), theta = twoPi * f / sampleRate;
        pRe[i] = (float) (r * std::cos (theta));
        pIm[i] = (float) (r * std::sin (theta));
        gain[i] = (float) (2.0 * (1.0 - r)); // unit gain at resonance

        // Each mode reaches the two microphones with its own strength and sign
        auto weight = [&rng] { const float w = rng.bipolar(); return std::copysign (0.35f + 0.65f * std::abs (w), w); };
        toLeft[i] = weight();
        toRight[i] = weight();

        f += (8.0 + 0.035 * f) * (1.0 + 0.6 * rng.bipolar());
    }

    // Diffuse part: line lengths in ms, mutually prime-ish, 61 ms in all
    constexpr double lengthsMs[numLines] { 1.07, 1.39, 1.73, 2.11, 2.53, 2.83, 3.17, 3.47,
                                           3.79, 4.13, 4.43, 4.79, 5.21, 5.59, 6.17, 6.83 };
    for (int k = 0; k < numLines; ++k)
    {
        const auto i = (size_t) k;
        lengths[i] = std::max (2, (int) std::lround (lengthsMs[i] * 0.001 * sampleRate));
        lines[i].assign ((size_t) lengths[i], 0.0f);
    }
    updateLineGains();
    dampCoeff = (float) std::exp (-twoPi * 3500.0 / sampleRate);

    lidCoeff = std::exp (-twoPi * 900.0 / sampleRate);
    updateRadiationFilters();
    reset();
}

void Soundboard::setRadiation (float bassHz, float warmthDb, float airHz)
{
    if (bassHz == radiationBassHz && warmthDb == radiationWarmthDb && airHz == radiationAirHz)
        return;
    radiationBassHz = bassHz;
    radiationWarmthDb = warmthDb;
    radiationAirHz = airHz;
    updateRadiationFilters();
}

void Soundboard::updateRadiationFilters()
{
    // 2nd-order Butterworth high-pass (bilinear), keeping the filter state
    const double k = std::tan (std::numbers::pi * std::min ((double) radiationBassHz, 0.4 * sampleRate) / sampleRate);
    const double q = std::numbers::sqrt2 / 2.0;
    const double norm = 1.0 / (1.0 + k / q + k * k);
    for (auto* hp : { &bassLeft, &bassRight })
    {
        hp->b0 = norm;
        hp->b1 = -2.0 * norm;
        hp->b2 = norm;
        hp->a1 = 2.0 * (k * k - 1.0) * norm;
        hp->a2 = (1.0 - k / q + k * k) * norm;
    }
    mobilityCoeff = std::exp (-twoPi * 450.0 / sampleRate);
    mobilityHighGain = std::pow (10.0, -radiationWarmthDb / 20.0);
    airCoeff = std::exp (-twoPi * std::min ((double) radiationAirHz, 0.45 * sampleRate) / sampleRate);
}

void Soundboard::reset()
{
    re.fill (0.0f);
    im.fill (0.0f);
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);
    writePos.fill (0);
    damp.fill (0.0f);
    for (int c = 0; c < 2; ++c)
        lidState[c] = airState[c] = mobilityState[c] = 0.0;
    bassLeft.z1 = bassLeft.z2 = bassRight.z1 = bassRight.z2 = 0.0;
}

void Soundboard::updateLineGains()
{
    for (size_t i = 0; i < lines.size(); ++i)
        lineGain[i] = (float) std::pow (10.0, -3.0 * lengths[i] / (diffuseT60 * sampleRate));
}

void Soundboard::setParameters (float body, float lid, float distance, float bloom)
{
    // Bloom: less direct sound, more (and longer-ringing) board resonance
    directGain = (1.0f - 0.4f * body) * (1.0f - 0.55f * distance) * (1.0f - 0.75f * bloom);
    modalGain = 0.9f * body * (0.8f + 0.4f * distance);
    diffuseGain = (0.35f * body + 1.6f * bloom) * (0.8f + 0.6f * distance);
    const float t60 = 0.05f + 0.15f * bloom;
    if (t60 != diffuseT60)
    {
        diffuseT60 = t60;
        if (sampleRate > 0.0 && ! lines[0].empty())
            updateLineGains();
    }
    // Closed lid: treble 10 dB down; distance loses up to 5 dB more
    lidHighGain = std::pow (10.0f, (-10.0f + 10.0f * lid - 5.0f * distance) / 20.0f);
}

void Soundboard::process (const float* force, const float* directLeft, const float* directRight, float* left, float* right,
                          int numSamples)
{
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = force[i];

        // Board modes
        float modesL = 0.0f, modesR = 0.0f;
#pragma omp simd reduction(+ : modesL, modesR)
        for (int m = 0; m < numModes; ++m)
        {
            const float r = pRe[(size_t) m] * re[(size_t) m] - pIm[(size_t) m] * im[(size_t) m] + gain[(size_t) m] * x;
            const float s = pRe[(size_t) m] * im[(size_t) m] + pIm[(size_t) m] * re[(size_t) m];
            re[(size_t) m] = r;
            im[(size_t) m] = s;
            modesL += toLeft[(size_t) m] * s;
            modesR += toRight[(size_t) m] * s;
        }

        // Radiating part: weak in the low bass, falling mobility above a few hundred Hz
        float direct[2] {};
        const double in[2] { directLeft[i], directRight[i] };
        for (int c = 0; c < 2; ++c)
        {
            mobilityState[c] = in[c] + mobilityCoeff * (mobilityState[c] - in[c]);
            direct[c] = (float) (mobilityState[c] + mobilityHighGain * (in[c] - mobilityState[c]));
        }

        // Diffuse ring: damped line outputs, mixed back through a 16x16 Hadamard matrix (every
        // line feeds every other equally, so no line rings on its own as a comb)
        std::array<float, numLines> out {};
        float diffuseL = 0.0f, diffuseR = 0.0f;
        for (size_t k = 0; k < numLines; ++k)
        {
            const float tap = lines[k][(size_t) writePos[k]];
            damp[k] = tap + dampCoeff * (damp[k] - tap);
            out[k] = damp[k];
            (k % 2 == 0 ? diffuseL : diffuseR) += tap;
        }
        for (size_t h = 1; h < numLines; h <<= 1)
            for (size_t a = 0; a < numLines; a += 2 * h)
                for (size_t b = a; b < a + h; ++b)
                {
                    const float x0 = out[b], x1 = out[b + h];
                    out[b] = x0 + x1;
                    out[b + h] = x0 - x1;
                }
        // Each side's sound enters its own half of the lines (and leaves from it), so the image
        // survives the board's resonance
        const float feed[2] { 0.125f * direct[0], 0.125f * direct[1] };
        for (size_t k = 0; k < numLines; ++k)
        {
            lines[k][(size_t) writePos[k]] = 0.25f * out[k] * lineGain[k] + feed[k % 2]; // 0.25: orthonormal
            if (++writePos[k] == lengths[k])
                writePos[k] = 0;
        }

        // Lid: shelve the treble
        const float modes[2] { modesL, modesR }, diffuse[2] { diffuseL, diffuseR };
        float channel[2] {};
        for (int c = 0; c < 2; ++c)
        {
            // The whole board radiates its lowest frequencies poorly (below coincidence)
            const double board = directGain * direct[c] + modalGain * modes[c] + diffuseGain * diffuse[c];
            const double mixed = (c == 0 ? bassLeft : bassRight).process (board);
            lidState[c] = mixed + lidCoeff * (lidState[c] - mixed);
            const double shelved = lidState[c] + lidHighGain * (mixed - lidState[c]);
            airState[c] = shelved + airCoeff * (airState[c] - shelved);
            channel[c] = (float) airState[c];
        }
        left[i] += channel[0];
        right[i] += channel[1];
    }
}

} // namespace dpiano
