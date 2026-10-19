#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace dustify
{

// Feedback-delay-network reverb: pre-delay, four series allpasses for early diffusion, then
// eight delay lines mixed through an orthogonal (Hadamard) matrix. Each line's gain is set from
// the RT60 decay time, a low-pass in each line makes highs die faster (damping), and slow
// modulation of the line lengths keeps the tail from ringing metallically.
class FdnReverb
{
public:
    void prepare (double newRate)
    {
        sampleRate = newRate;
        const double scale = sampleRate / 48000.0;

        predelay.assign ((size_t) (0.25 * sampleRate) + 2, 0.0f);
        predelayWrite = 0;

        constexpr int diffuserLengths[] { 142, 107, 379, 277 };
        for (size_t i = 0; i < diffusers.size(); ++i)
        {
            diffusers[i].buffer.assign ((size_t) (diffuserLengths[i] * scale) + 1, 0.0f);
            diffusers[i].pos = 0;
        }

        // Longest line at full size, plus room for modulation
        for (auto& line : lines)
        {
            line.buffer.assign ((size_t) (0.2 * sampleRate) + 64, 0.0f);
            line.write = 0;
            line.damp = 0.0f;
        }
        // Slow modulation LFOs as rotating phasors (a few multiplies instead of sin() per sample)
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const double start = 2.0 * std::numbers::pi * (double) i / (double) lines.size();
            const double step = 2.0 * std::numbers::pi * (0.07 + 0.031 * (double) i) / sampleRate;
            lfo[i] = { std::cos (start), std::sin (start), std::cos (step), std::sin (step) };
        }
        renormaliseCounter = 0;

        setParameters (size, decay, damping, predelayMs);
        mixSmoothed = 0.0f;
    }

    void setParameters (float newSize, float decaySeconds, float newDamping, float newPredelayMs)
    {
        size = newSize;
        decay = decaySeconds;
        damping = newDamping;
        predelayMs = newPredelayMs;

        // Mutually prime-ish lengths (ms), scaled by size from a small room to a large hall
        constexpr double baseMs[] { 29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.1, 73.3 };
        const double sizeScale = 0.35 + 1.65 * size;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const double length = baseMs[i] * sizeScale * 0.001 * sampleRate;
            lines[i].length = length;
            // x 1.12: the in-loop damping filter costs a little energy on every pass
            lines[i].gain = (float) std::pow (10.0, -3.0 * length / (1.12 * std::max (0.1f, decay) * sampleRate));
        }
        dampCoeff = 0.05f + 0.9f * (1.0f - damping);
        if (predelay.empty())
            return; // not prepared yet; prepare() applies these settings
        predelaySamples = (size_t) std::clamp ((double) predelayMs * 0.001 * sampleRate, 0.0, (double) predelay.size() - 2.0);
    }

    void setMix (float newMix) { mix = newMix; }

    void process (float* left, float* right, int n)
    {
        if (mix <= 0.0f && mixSmoothed < 1.0e-4f)
            return;

        constexpr double modDepth = 12.0; // samples at 48 kHz
        const double depth = modDepth * sampleRate / 48000.0;

        for (int s = 0; s < n; ++s)
        {
            mixSmoothed += (mix - mixSmoothed) * 0.001f;

            // Pre-delay and diffusion on the mono sum
            predelay[predelayWrite] = 0.5f * (left[s] + right[s]);
            auto readPos = predelayWrite >= predelaySamples ? predelayWrite - predelaySamples
                                                            : predelayWrite + predelay.size() - predelaySamples;
            predelayWrite = predelayWrite + 1 == predelay.size() ? 0 : predelayWrite + 1;
            float x = predelay[readPos];
            for (auto& d : diffusers)
                x = d.process (x, 0.6f);

            // Read the lines (every other one modulated), damp them
            if (++renormaliseCounter == 4096)
            {
                renormaliseCounter = 0;
                for (auto& p : lfo)
                {
                    const double m = 1.0 / std::sqrt (p.c * p.c + p.s * p.s); // stop rounding drift
                    p.c *= m;
                    p.s *= m;
                }
            }
            std::array<float, 8> y;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                double mod = 0.0;
                if (i % 2 == 0)
                {
                    auto& p = lfo[i];
                    const double c = p.c * p.stepC - p.s * p.stepS;
                    p.s = p.s * p.stepC + p.c * p.stepS;
                    p.c = c;
                    mod = depth * p.s;
                }
                auto& line = lines[i];
                const float raw = line.read (line.length + mod);
                line.damp += (raw - line.damp) * dampCoeff;
                y[i] = line.damp * line.gain;
            }

            // Hadamard 8x8 (fast Walsh-Hadamard butterflies), normalised to be orthogonal
            for (size_t h = 1; h < 8; h <<= 1)
                for (size_t i = 0; i < 8; i += 2 * h)
                    for (size_t j = i; j < i + h; ++j)
                    {
                        const float a = y[j], b = y[j + h];
                        y[j] = a + b;
                        y[j + h] = a - b;
                    }
            const float norm = 1.0f / std::sqrt (8.0f);

            float outL = 0.0f, outR = 0.0f;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                lines[i].push (y[i] * norm + x);
                // Different sign patterns per side decorrelate left and right
                outL += (i % 2 == 0 ? 1.0f : -1.0f) * lines[i].damp;
                outR += ((i / 2) % 2 == 0 ? 1.0f : -1.0f) * lines[i].damp;
            }

            left[s] += outL * 0.25f * mixSmoothed;
            right[s] += outR * 0.25f * mixSmoothed;
        }
    }

private:
    struct Allpass
    {
        std::vector<float> buffer;
        size_t pos = 0;

        float process (float x, float g)
        {
            const float delayed = buffer[pos];
            const float v = x + g * delayed;
            buffer[pos] = v;
            pos = pos + 1 == buffer.size() ? 0 : pos + 1;
            return delayed - g * v;
        }
    };

    struct Line
    {
        std::vector<float> buffer;
        size_t write = 0;
        double length = 1000.0;
        float gain = 0.5f, damp = 0.0f;

        float read (double delay) const
        {
            double pos = (double) write - delay;
            const auto size = (double) buffer.size();
            if (pos < 0.0)
                pos += size;
            const auto i0 = (size_t) pos;
            const auto i1 = i0 + 1 == buffer.size() ? 0 : i0 + 1;
            const auto t = (float) (pos - (double) i0);
            return buffer[i0] + (buffer[i1] - buffer[i0]) * t;
        }

        void push (float v)
        {
            buffer[write] = v;
            write = write + 1 == buffer.size() ? 0 : write + 1;
        }
    };

    double sampleRate = 48000.0;
    float size = 0.5f, decay = 2.5f, damping = 0.5f, predelayMs = 10.0f;
    float mix = 0.0f, mixSmoothed = 0.0f, dampCoeff = 0.5f;

    std::vector<float> predelay;
    size_t predelayWrite = 0, predelaySamples = 0;
    std::array<Allpass, 4> diffusers;
    std::array<Line, 8> lines;
    struct Phasor
    {
        double c = 1.0, s = 0.0, stepC = 1.0, stepS = 0.0;
    };
    std::array<Phasor, 8> lfo {};
    int renormaliseCounter = 0;
};

} // namespace dustify
