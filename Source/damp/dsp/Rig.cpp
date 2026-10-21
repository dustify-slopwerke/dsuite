#include "Rig.h"

#include "common/dsp/Denormals.h"

#include <algorithm>
#include <cmath>

namespace damp
{

Rig::Rig()
{
    amp.setBoard (&pedals);
}

void Rig::setPatch (const Patch& p)
{
    amp.setPatch (p);
    pedals.setPatch (p);
    loops.setPatch (p);
    outputGain.target = p[PID::OutputVolume] <= -48.0f ? 0.0f : std::pow (10.0f, p[PID::OutputVolume] / 20.0f);
    masterGain.target = p[PID::MasterOutput] <= -48.0f ? 0.0f : std::pow (10.0f, p[PID::MasterOutput] / 20.0f);
    metronome.setPatch (p[PID::MetronomeLevel], p.on (PID::MetronomeAccent));
}

void Rig::setBoard (const BoardOrder& b)
{
    if (b == (boardPending ? pendingBoard : pedals.order()))
        return;
    pendingBoard = b;
    boardPending = true;
}

void Rig::prepare (double sampleRate, int maxImpulseLength)
{
    rate = sampleRate;
    smoothing = dustify::Smoother::coefficient (0.02, rate);
    dipStep = (float) (1.0 / (0.003 * rate)); // 3 ms each way
    amp.prepare (rate);
    loops.prepare (rate); // keeps its loops
    metronome.prepare (rate);
    pedals.prepare (rate, rate * (1 << dustify::Oversampler::maxStages));
    for (auto& c : cab)
        c.prepare (std::max (1, maxImpulseLength), (int) (0.03 * rate));
    reset();
}

void Rig::reset()
{
    amp.reset();
    pedals.reset();
    for (auto& c : cab)
        c.reset();
    if (boardPending)
    {
        pedals.setOrder (pendingBoard);
        boardPending = false;
    }
    dip = 1.0f;
    outputGain.snap();
    masterGain.snap();
    metronome.reset();
}

void Rig::setCabKernel (const dustify::ConvolutionKernel* k)
{
    for (auto& c : cab)
        c.setKernel (k);
}

void Rig::setCabKernelNow (const dustify::ConvolutionKernel* k)
{
    for (auto& c : cab)
        c.setKernelNow (k);
    rightCabIdle = false;
}

bool Rig::cabSwitching() const
{
    return cab[0].isSwitching() || (! rightCabIdle && cab[1].isSwitching());
}

void Rig::process (float* left, float* right, int n, bool monoInput)
{
    dustify::ScopedFlushDenormals noDenormals;
    for (int done = 0; done < n;)
    {
        const int chunk = std::min (n - done, Board::maxBlock);
        processChunk (left + done, right == nullptr ? nullptr : right + done, chunk, monoInput);
        done += chunk;
    }
}

void Rig::processChunk (float* left, float* right, int n, bool monoInput)
{
    // Amp and cab: on the left only when the input is mono
    const bool stereoAmp = right != nullptr && ! monoInput;
    amp.process (left, stereoAmp ? right : nullptr, n);
    cab[0].process (left, n);
    if (stereoAmp)
    {
        if (rightCabIdle)
        {
            // It may still point at kernels freed while it sat out: catch up with the left one
            cab[1].reset();
            cab[1].setKernelNow (cab[0].latestKernel());
            rightCabIdle = false;
        }
        cab[1].process (right, n);
    }
    else
    {
        rightCabIdle = true;
        if (right != nullptr)
            std::copy_n (left, n, right);
    }

    pedals.processAfterAmp (left, right, n, rate);

    // The dip around a board change (the live signal only: the loops don't change)
    for (int i = 0; i < n; ++i)
    {
        if (boardPending)
        {
            dip = std::max (0.0f, dip - dipStep);
            if (dip <= 0.0f)
            {
                pedals.setOrder (pendingBoard); // between samples, at silence
                boardPending = false;
            }
        }
        else if (dip < 1.0f)
        {
            dip = std::min (1.0f, dip + dipStep);
        }
        if (dip < 1.0f)
        {
            left[i] *= dip * dip;
            if (right != nullptr)
                right[i] *= dip * dip;
        }
    }

    // The preset's level: recorded with the loops
    for (int i = 0; i < n; ++i)
    {
        const float g = outputGain.next (smoothing);
        left[i] *= g;
        if (right != nullptr)
            right[i] *= g;
    }

    loops.setTransport (bpmNow, hostTransport, hostPpq);
    loops.process (left, right, n);
    if (hostTransport)
        hostPpq += (double) n * bpmNow / (60.0 * rate);

    // The clicks, never recorded; then the master output over everything
    metronome.render (left, right, n, loops.clicks(), loops.clickCount());
    for (int i = 0; i < n; ++i)
    {
        const float g = masterGain.next (smoothing);
        left[i] *= g;
        if (right != nullptr)
            right[i] *= g;
    }
}

} // namespace damp
