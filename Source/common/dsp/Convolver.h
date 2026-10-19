#pragma once

#include "Fft.h"

#include <array>
#include <complex>
#include <vector>

namespace dustify
{

// An impulse response prepared for Convolver: the first blockSize taps for direct convolution,
// the rest as the spectra of blockSize-long partitions. Build it off the audio thread; it's
// immutable afterwards, so any number of Convolvers can share one.
class ConvolutionKernel
{
public:
    static constexpr int blockSize = 64;
    static constexpr int fftSize = 2 * blockSize;
    static constexpr int bins = blockSize + 1; // non-negative frequencies of a real signal

    ConvolutionKernel (const float* impulse, int length);

    int length() const { return taps; }
    int numPartitions() const { return partitions; } // after the head

    const std::array<float, blockSize>& headTaps() const { return head; }
    const std::complex<float>* partition (int p) const { return spectra.data() + (size_t) p * bins; }

private:
    int taps = 0, partitions = 0;
    std::array<float, blockSize> head {};
    std::vector<std::complex<float>> spectra;
};

// Zero-latency uniformly partitioned convolution (one channel), for any host block size: the head
// of the response is convolved directly, sample by sample, and the tail by overlap-save FFT
// partitions, whose results arrive exactly when the head's taps run out. A new kernel takes over
// at the next block boundary with a short crossfade; nothing allocates after prepare().
class Convolver
{
public:
    static constexpr int blockSize = ConvolutionKernel::blockSize;

    Convolver();

    // maxLength: the longest response that will be used (longer ones are cut short)
    void prepare (int maxLength, int crossfadeSamples);
    void reset();

    // Audio thread. nullptr passes the signal straight through. The switch waits for the next
    // block boundary, then crossfades: the kernel in use before it is read until then, and must
    // stay alive until isSwitching() is false. Another setKernel before that replaces the
    // pending one (which is then never read) but doesn't hurry the switch: a kernel freed as
    // soon as a newer one was handed over may still be playing.
    void setKernel (const ConvolutionKernel* k) { pending = k; hasPending = true; }
    // Switches at once, no crossfade (before processing starts, or after reset())
    void setKernelNow (const ConvolutionKernel* k);
    bool isFading() const { return fadeLeft > 0; }
    // A kernel is waiting for the block boundary, or still fading in: until neither, the one
    // before it is still read
    bool isSwitching() const { return hasPending || fadeLeft > 0; }
    // The kernel it plays once any switch is done
    const ConvolutionKernel* latestKernel() const { return hasPending ? pending : kernel; }

    void process (float* io, int n);

private:
    void finishBlock();
    void computeTail (const ConvolutionKernel*, std::array<float, blockSize>& out);
    float headOutput (const ConvolutionKernel*, float x) const;

    Fft fft;
    int maxPartitions = 0, newest = 0, pos = 0;
    std::vector<std::complex<float>> history; // input spectra, newest first via `newest`
    std::array<float, 2 * blockSize> recent {}; // last blockSize inputs, mirrored for contiguous reads
    int recentPos = 0;
    std::array<float, blockSize> previousBlock {}, currentBlock {};
    std::array<float, blockSize> tail {}, fadingTail {};
    std::array<std::complex<float>, ConvolutionKernel::fftSize> scratch {};
    std::array<std::complex<float>, ConvolutionKernel::bins> accumulator {};

    const ConvolutionKernel* kernel = nullptr;
    const ConvolutionKernel* fadingFrom = nullptr;
    const ConvolutionKernel* pending = nullptr;
    bool hasPending = false;
    int fadeLength = 1024, fadeLeft = 0;
};

} // namespace dustify
