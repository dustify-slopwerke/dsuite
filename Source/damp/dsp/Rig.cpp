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
    inputMode = p.choice<InputMode> (PID::InputMode);
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
    inputStep = (float) (1.0 / (inputFadeMs * 0.001 * rate));
    rightStep = (float) (1.0 / (rightFadeMs * 0.001 * rate));
    rightWarmFrames = std::max (1, (int) std::lround (rightWarmMs * 0.001 * rate));
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
    inputKnown = false;
    inputFade = 1.0f;
    rightMix = 0.0f;
    rightWarm = 0;
    rightFresh = true;
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

namespace
{
// What the left carries in each mode (Stereo: its own channel, as Left)
InputMode leftSource (InputMode m) { return m == InputMode::Stereo ? InputMode::Left : m; }

float selected (InputMode m, float l, float r)
{
    switch (m)
    {
        case InputMode::Right: return r;
        case InputMode::Sum: return 0.5f * (l + r);
        case InputMode::Left:
        case InputMode::Stereo: break;
    }
    return l;
}
} // namespace

bool Rig::selectInput (InputMode mode, float* left, const float* right, int n)
{
    const auto wanted = leftSource (mode);
    if (! inputKnown)
    {
        inputNow = wanted; // the first block: as it is
        inputKnown = true;
    }
    else if (wanted != inputNow)
    {
        inputBefore = inputNow;
        inputNow = wanted;
        inputFade = 0.0f;
    }
    if (inputFade < 1.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            inputFade = std::min (1.0f, inputFade + inputStep);
            const float from = selected (inputBefore, left[i], right[i]), to = selected (inputNow, left[i], right[i]);
            left[i] = from + (to - from) * inputFade;
        }
    }
    else if (inputNow == InputMode::Right)
    {
        std::copy_n (right, n, left);
    }
    else if (inputNow == InputMode::Sum)
    {
        for (int i = 0; i < n; ++i)
            left[i] = 0.5f * (left[i] + right[i]);
    }
    return mode != InputMode::Stereo;
}

void Rig::processSelected (float* left, float* right, int n, bool monoInput)
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
    // Amp and cab: on the left only when the input is mono, on the right too while the right side
    // warms up, is heard or fades out (see processSelected)
    const bool stereoIn = right != nullptr && ! monoInput;
    if (stereoIn && rightMix <= 0.0f && rightWarm == 0)
    {
        // Into Stereo: from a reset both sides are alike, so at once; else warm the right up first
        if (rightFresh)
            rightMix = 1.0f;
        else
            rightWarm = rightWarmFrames;
    }
    if (! stereoIn)
        rightWarm = 0;
    rightFresh = false;
    const bool stereoAmp = right != nullptr && (stereoIn || rightMix > 0.0f);
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

        // Warming up, fading in or out: the right between the left's copy and its own
        if (rightMix < 1.0f || ! stereoIn)
            for (int i = 0; i < n; ++i)
            {
                if (! stereoIn)
                    rightMix = std::max (0.0f, rightMix - rightStep);
                else if (rightWarm == 0 || --rightWarm == 0)
                    rightMix = std::min (1.0f, rightMix + rightStep);
                right[i] = left[i] + rightMix * (right[i] - left[i]);
            }
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
