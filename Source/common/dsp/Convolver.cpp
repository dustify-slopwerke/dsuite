#include "Convolver.h"

#include <algorithm>

namespace dustify
{

ConvolutionKernel::ConvolutionKernel (const float* impulse, int length)
    : taps (std::max (0, length))
{
    for (int i = 0; i < std::min (taps, blockSize); ++i)
        head[(size_t) i] = impulse[i];

    partitions = std::max (0, (taps - 1) / blockSize); // partitions after the head
    spectra.resize ((size_t) partitions * bins);

    const Fft fft (fftSize);
    std::vector<std::complex<float>> frame ((size_t) fftSize);
    for (int p = 0; p < partitions; ++p)
    {
        // Overlap-save: the taps in the first half, zeros after
        std::fill (frame.begin(), frame.end(), std::complex<float> {});
        const int from = (p + 1) * blockSize;
        for (int i = 0; i < blockSize && from + i < taps; ++i)
            frame[(size_t) i] = impulse[from + i];
        fft.forward (frame.data());
        std::copy (frame.begin(), frame.begin() + bins, spectra.begin() + (std::ptrdiff_t) p * bins);
    }
}

Convolver::Convolver() : fft (ConvolutionKernel::fftSize) {}

void Convolver::prepare (int maxLength, int crossfadeSamples)
{
    maxPartitions = std::max (1, (maxLength - 1) / blockSize);
    history.assign ((size_t) maxPartitions * ConvolutionKernel::bins, {});
    fadeLength = std::max (1, crossfadeSamples);
    reset();
}

void Convolver::reset()
{
    std::fill (history.begin(), history.end(), std::complex<float> {});
    recent.fill (0.0f);
    previousBlock.fill (0.0f);
    currentBlock.fill (0.0f);
    tail.fill (0.0f);
    fadingTail.fill (0.0f);
    newest = recentPos = pos = 0;
    fadeLeft = 0;
    fadingFrom = nullptr;
}

void Convolver::setKernelNow (const ConvolutionKernel* k)
{
    kernel = k;
    hasPending = false;
    fadeLeft = 0;
    fadingFrom = nullptr;
    computeTail (kernel, tail); // from the input history already seen
}

float Convolver::headOutput (const ConvolutionKernel* k, float x) const
{
    if (k == nullptr)
        return x;
    // recent[recentPos .. recentPos + blockSize) holds the inputs newest first
    const auto& h = k->headTaps();
    const float* in = recent.data() + recentPos;
    float sum = 0.0f;
    for (int i = 0; i < blockSize; ++i)
        sum += h[(size_t) i] * in[i];
    return sum;
}

void Convolver::process (float* io, int n)
{
    for (int i = 0; i < n; ++i)
    {
        const float x = io[i];
        recentPos = recentPos == 0 ? blockSize - 1 : recentPos - 1;
        recent[(size_t) recentPos] = x;
        recent[(size_t) (recentPos + blockSize)] = x;

        float y = headOutput (kernel, x) + tail[(size_t) pos];
        if (fadeLeft > 0)
        {
            const float old = headOutput (fadingFrom, x) + fadingTail[(size_t) pos];
            const float a = (float) fadeLeft / (float) fadeLength;
            y += a * (old - y);
            if (--fadeLeft == 0)
                fadingFrom = nullptr;
        }
        io[i] = y;

        currentBlock[(size_t) pos] = x;
        if (++pos == blockSize)
            finishBlock();
    }
}

void Convolver::finishBlock()
{
    pos = 0;

    // Spectrum of the last two blocks, kept in the history ring
    for (int i = 0; i < blockSize; ++i)
    {
        scratch[(size_t) i] = previousBlock[(size_t) i];
        scratch[(size_t) (i + blockSize)] = currentBlock[(size_t) i];
    }
    fft.forward (scratch.data());
    newest = newest == 0 ? maxPartitions - 1 : newest - 1;
    std::copy (scratch.begin(), scratch.begin() + ConvolutionKernel::bins,
               history.begin() + (std::ptrdiff_t) newest * ConvolutionKernel::bins);
    previousBlock = currentBlock;

    if (hasPending)
    {
        hasPending = false;
        if (pending != kernel)
        {
            fadingFrom = kernel;
            kernel = pending;
            fadeLeft = fadeLength;
        }
    }

    computeTail (kernel, tail);
    if (fadeLeft > 0)
        computeTail (fadingFrom, fadingTail);
}

void Convolver::computeTail (const ConvolutionKernel* k, std::array<float, blockSize>& out)
{
    const int parts = k == nullptr ? 0 : std::min (k->numPartitions(), maxPartitions);
    if (parts == 0)
    {
        out.fill (0.0f);
        return;
    }

    // The next block's output from taps blockSize onwards: partition q (taps (q + 1) B ...) meets
    // the input spectrum q blocks old
    accumulator.fill ({});
    for (int q = 0; q < parts; ++q)
    {
        const auto* x = history.data() + (size_t) ((newest + q) % maxPartitions) * ConvolutionKernel::bins;
        const auto* h = k->partition (q);
        for (int b = 0; b < ConvolutionKernel::bins; ++b)
            accumulator[(size_t) b] += x[b] * h[b];
    }

    // Back to time: rebuild the conjugate-symmetric half and keep the second (valid) half
    for (int b = 0; b < ConvolutionKernel::bins; ++b)
        scratch[(size_t) b] = accumulator[(size_t) b];
    for (int b = 1; b < blockSize; ++b)
        scratch[(size_t) (ConvolutionKernel::fftSize - b)] = std::conj (accumulator[(size_t) b]);
    fft.inverse (scratch.data());
    constexpr float scale = 1.0f / (float) ConvolutionKernel::fftSize;
    for (int i = 0; i < blockSize; ++i)
        out[(size_t) i] = scratch[(size_t) (blockSize + i)].real() * scale;
}

} // namespace dustify
