#pragma once

#include <cassert>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace dustify
{

// In-place radix-2 complex FFT of a fixed power-of-two size. Tables are built by the constructor,
// so construct off the audio thread; transforms don't allocate. The inverse is unscaled
// (inverse (forward (x)) = size * x).
class Fft
{
public:
    explicit Fft (int size) : n (size), twiddles ((size_t) size / 2), reversed ((size_t) size)
    {
        assert (size >= 2 && (size & (size - 1)) == 0);
        for (int i = 0; i < n / 2; ++i)
            twiddles[(size_t) i] = std::polar (1.0, -2.0 * std::numbers::pi * i / n);
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            reversed[(size_t) i] = r;
        }
    }

    int size() const { return n; }

    void forward (std::complex<float>* x) const { transform (x, false); }
    void inverse (std::complex<float>* x) const { transform (x, true); }

private:
    void transform (std::complex<float>* x, bool inverse) const
    {
        for (int i = 0; i < n; ++i)
            if (const int r = reversed[(size_t) i]; i < r)
                std::swap (x[i], x[r]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int start = 0; start < n; start += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    const auto& tw = twiddles[(size_t) (k * step)];
                    const std::complex<float> w ((float) tw.real(), inverse ? (float) -tw.imag() : (float) tw.imag());
                    const auto u = x[start + k];
                    const auto v = x[start + k + half] * w;
                    x[start + k] = u + v;
                    x[start + k + half] = u - v;
                }
            }
        }
    }

    int n;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> reversed;
};

} // namespace dustify
