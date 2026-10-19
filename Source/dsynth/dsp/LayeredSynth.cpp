#include "LayeredSynth.h"

#include <algorithm>
#include <cmath>

namespace dsynth
{

void LayeredSynth::prepare (double sampleRate)
{
    a.prepare (sampleRate);
    b.prepare (sampleRate);
    routed.fill (0);
    gainCoeff = (float) (1.0 - std::exp (-1.0 / (0.02 * sampleRate)));
    gain[0] = targetGain[0];
    gain[1] = targetGain[1];
}

void LayeredSynth::setPatches (const Patch& patchA, const Patch& patchB)
{
    // Notes already sounding keep their routing for note-off; new notes follow the new setup
    mode = patchA.choice<LayerMode> (PID::LayerMode);
    splitNote = (int) patchA[PID::SplitNote];

    const float balance = patchA[PID::LayerBalance];
    targetGain[0] = mode == LayerMode::Single ? 1.0f : std::min (1.0f, 1.0f - balance);
    targetGain[1] = mode == LayerMode::Single ? 0.0f : std::min (1.0f, 1.0f + balance);

    a.setPatch (patchA);

    Patch pb = patchB;
    pb[PID::MasterTune] += patchA[PID::LayerDetune];
    for (const auto& info : allParams())
        if (info.global)
            pb[info.pid] = patchA[info.pid]; // layer B follows the shared setup (MPE etc.)
    b.setPatch (pb);
}

void LayeredSynth::setTransport (double bpm, bool playing, double ppqPosition)
{
    a.setTransport (bpm, playing, ppqPosition);
    b.setTransport (bpm, playing, ppqPosition);
}

void LayeredSynth::noteOn (int note, float velocity, int channel)
{
    if (note < 0 || note > 127)
        return;

    uint8_t to = toA;
    if (mode == LayerMode::Dual)
        to = toA | toB;
    else if (mode == LayerMode::Split)
        to = note < splitNote ? toB : toA;

    // A re-pressed note may have gone elsewhere last time: release it there first
    const uint8_t stale = routed[(size_t) note] & ~to;
    if (stale & toA) a.noteOff (note, channel);
    if (stale & toB) b.noteOff (note, channel);

    routed[(size_t) note] = to;
    if (to & toA) a.noteOn (note, velocity, channel);
    if (to & toB) b.noteOn (note, velocity, channel);
}

void LayeredSynth::noteOff (int note, int channel)
{
    if (note < 0 || note > 127)
        return;
    const uint8_t to = routed[(size_t) note];
    routed[(size_t) note] = 0;
    if (to & toA) a.noteOff (note, channel);
    if (to & toB) b.noteOff (note, channel);
}

void LayeredSynth::pitchBend (float v, int channel) { a.pitchBend (v, channel); b.pitchBend (v, channel); }
void LayeredSynth::modWheel (float v) { a.modWheel (v); b.modWheel (v); }
void LayeredSynth::aftertouch (float v, int channel) { a.aftertouch (v, channel); b.aftertouch (v, channel); }
void LayeredSynth::polyPressure (int note, float v) { a.polyPressure (note, v); b.polyPressure (note, v); }
void LayeredSynth::timbre (float v, int channel) { a.timbre (v, channel); b.timbre (v, channel); }
void LayeredSynth::sustainPedal (bool down) { a.sustainPedal (down); b.sustainPedal (down); }

void LayeredSynth::allNotesOff()
{
    routed.fill (0);
    a.allNotesOff();
    b.allNotesOff();
}

void LayeredSynth::process (float* left, float* right, int n)
{
    a.process (left, right, n);

    const bool withB = layerBNeeded();
    for (int offset = 0; offset < n; offset += chunk)
    {
        const int m = std::min (chunk, n - offset);
        if (withB)
            b.process (scratchL.data(), scratchR.data(), m);

        for (int i = 0; i < m; ++i)
        {
            gain[0] += (targetGain[0] - gain[0]) * gainCoeff;
            gain[1] += (targetGain[1] - gain[1]) * gainCoeff;
            left[offset + i] *= gain[0];
            right[offset + i] *= gain[0];
            if (withB)
            {
                left[offset + i] += scratchL[(size_t) i] * gain[1];
                right[offset + i] += scratchR[(size_t) i] * gain[1];
            }
        }
    }
}

} // namespace dsynth
