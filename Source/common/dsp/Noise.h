#pragma once

#include <cmath>
#include <cstdint>

namespace dustify
{

// Small deterministic PRNG (xorshift32): real-time safe and reproducible for tests.
class Random
{
public:
    explicit Random (uint32_t seed = 0x9e3779b9u) : state (seed != 0 ? seed : 1u) {}

    uint32_t nextInt()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // Uniform in [-1, 1)
    float bipolar() { return (float) ((double) nextInt() / 2147483648.0 - 1.0); }

    // Uniform in [0, 1)
    double unipolar() { return (double) nextInt() / 4294967296.0; }

private:
    uint32_t state;
};

class NoiseSource
{
public:
    void seed (uint32_t s) { rng = Random (s); }

    float white() { return rng.bipolar(); }

    // Paul Kellet's economy pink filter, normalised to roughly the level of the white output
    float pink()
    {
        const float w = white();
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
    }

private:
    Random rng;
    float b0 = 0, b1 = 0, b2 = 0;
};

// Slow random wander with unit variance: white noise through a one-pole low-pass,
// updated at control rate.
class Wander
{
public:
    void prepare (double updateRate, double cornerHz, uint32_t seed)
    {
        rng = Random (seed);
        coeff = 1.0 - std::exp (-2.0 * 3.14159265358979 * cornerHz / updateRate);
        // variance of a one-pole low-pass of uniform noise: (1/3) * c / (2 - c)
        scale = std::sqrt (3.0 * (2.0 - coeff) / coeff);
        value = 0.0;
    }

    float next()
    {
        value += (rng.bipolar() * scale - value) * coeff;
        return (float) value;
    }

private:
    Random rng;
    double coeff = 0.0, scale = 1.0, value = 0.0;
};

} // namespace dustify
