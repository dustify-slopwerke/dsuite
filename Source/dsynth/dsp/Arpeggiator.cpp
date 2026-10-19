#include "Arpeggiator.h"

#include <algorithm>
#include <cmath>

namespace dsynth
{

void Arpeggiator::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    reset();
    pendingOff = false;
    sounding = -1;
}

void Arpeggiator::setParams (ArpMode newMode, int newOctaves, double newStepBeats, float newGate, bool newLatch)
{
    const bool changed = newMode != mode || newOctaves != octaves;
    mode = newMode;
    octaves = std::clamp (newOctaves, 1, 4);
    gate = newGate;

    if (newStepBeats != stepBeats)
    {
        // Keep the same musical position when the rate changes
        position = position * stepBeats / newStepBeats;
        nextStep = std::ceil (position - eps);
        stepBeats = newStepBeats;
    }

    if (latch && ! newLatch && heldCount == 0 && patternSize > 0)
    {
        // Unlatching with no keys down stops the pattern
        patternSize = 0;
        patternChanged (false);
    }
    latch = newLatch;

    if (changed)
        rebuildSequence();
}

void Arpeggiator::setClock (double newBpm, bool hostPlaying, double hostPpq)
{
    bpm = std::max (newBpm, 1.0);
    synced = hostPlaying && hostPpq >= 0.0;

    if (synced)
    {
        const double hostPosition = hostPpq / stepBeats;
        if (std::abs (hostPosition - position) > 0.01) // transport started, looped or jumped
        {
            position = hostPosition;
            nextStep = std::ceil (position - eps);
            if (gateOffAt >= 0.0)
                gateOffAt = std::min (gateOffAt, position);
        }
    }
}

void Arpeggiator::keyDown (int note, float velocity)
{
    if (note < 0 || note > 127)
        return;

    const bool wasEmpty = patternSize == 0;

    // With latch, the first key after all were released starts a new pattern
    if (latch && heldCount == 0)
        patternSize = 0;

    if (std::find (held.begin(), held.begin() + heldCount, note) == held.begin() + heldCount)
        held[(size_t) heldCount++] = note;

    if (std::find (pattern.begin(), pattern.begin() + patternSize, note) == pattern.begin() + patternSize)
    {
        pattern[(size_t) patternSize] = note;
        velocities[(size_t) patternSize] = velocity;
        ++patternSize;
    }

    patternChanged (wasEmpty);
}

void Arpeggiator::keyUp (int note)
{
    auto end = held.begin() + heldCount;
    auto it = std::find (held.begin(), end, note);
    if (it == end)
        return;
    std::rotate (it, it + 1, end);
    --heldCount;

    if (latch)
        return;

    auto pEnd = pattern.begin() + patternSize;
    auto p = std::find (pattern.begin(), pEnd, note);
    if (p != pEnd)
    {
        const auto i = p - pattern.begin();
        std::rotate (p, p + 1, pEnd);
        std::rotate (velocities.begin() + i, velocities.begin() + i + 1, velocities.begin() + patternSize);
        --patternSize;
    }
    patternChanged (false);
}

void Arpeggiator::reset()
{
    heldCount = patternSize = sequenceSize = 0;
    if (sounding >= 0)
        pendingOff = true;
    gateOffAt = -1.0;
}

void Arpeggiator::patternChanged (bool wasEmpty)
{
    rebuildSequence();

    if (patternSize == 0)
    {
        if (sounding >= 0)
            pendingOff = true;
        gateOffAt = -1.0;
        return;
    }

    if (wasEmpty)
    {
        stepIndex = 0;
        if (synced)
        {
            nextStep = std::ceil (position - eps); // wait for the host's grid
        }
        else
        {
            position = nextStep = 0.0;              // free-running: start right away
        }
    }
}

void Arpeggiator::rebuildSequence()
{
    std::array<int, 128> order {}; // indices into pattern
    for (int i = 0; i < patternSize; ++i)
        order[(size_t) i] = i;
    if (mode != ArpMode::AsPlayed)
        std::sort (order.begin(), order.begin() + patternSize,
                   [this] (int a, int b) { return pattern[(size_t) a] < pattern[(size_t) b]; });

    sequenceSize = 0;
    for (int o = 0; o < octaves; ++o)
        for (int i = 0; i < patternSize; ++i)
        {
            const int source = order[(size_t) i];
            if (const int n = pattern[(size_t) source] + 12 * o; n <= 127)
            {
                sequence[(size_t) sequenceSize] = n;
                sequenceVelocity[(size_t) sequenceSize] = velocities[(size_t) source];
                ++sequenceSize;
            }
        }

    if (mode == ArpMode::Down)
    {
        std::reverse (sequence.begin(), sequence.begin() + sequenceSize);
        std::reverse (sequenceVelocity.begin(), sequenceVelocity.begin() + sequenceSize);
    }
    else if (mode == ArpMode::UpDown && sequenceSize > 2)
    {
        // 1 2 3 4 3 2 | 1 2 3 4 ...: the top and bottom aren't repeated
        const int up = sequenceSize;
        for (int i = up - 2; i >= 1; --i)
        {
            sequence[(size_t) sequenceSize] = sequence[(size_t) i];
            sequenceVelocity[(size_t) sequenceSize] = sequenceVelocity[(size_t) i];
            ++sequenceSize;
        }
    }
}

int Arpeggiator::samplesUntilNextEvent (int maxSamples) const
{
    if (pendingOff)
        return 0;

    const double inc = bpm / 60.0 / sampleRate / stepBeats;
    double target = 1.0e300;
    if (patternSize > 0)
        target = nextStep;
    if (sounding >= 0 && gateOffAt >= 0.0)
        target = std::min (target, gateOffAt);

    if (target > 1.0e299)
        return maxSamples;

    const double samples = std::ceil ((target - position) / inc - eps);
    return (int) std::clamp (samples, 0.0, (double) maxSamples);
}

void Arpeggiator::advance (int samples)
{
    position += samples * (bpm / 60.0 / sampleRate / stepBeats);
}

int Arpeggiator::popEvents (Event* out)
{
    int count = 0;

    if (pendingOff)
    {
        if (sounding >= 0)
            out[count++] = { false, sounding, 0.0f };
        sounding = -1;
        pendingOff = false;
    }

    if (sounding >= 0 && gateOffAt >= 0.0 && position >= gateOffAt - eps)
    {
        out[count++] = { false, sounding, 0.0f };
        sounding = -1;
        gateOffAt = -1.0;
    }

    if (patternSize > 0 && sequenceSize > 0 && position >= nextStep - eps)
    {
        int index = stepIndex % sequenceSize;
        if (mode == ArpMode::Random && sequenceSize > 1)
        {
            index = (int) (rng.unipolar() * sequenceSize);
            if (sequence[(size_t) index] == sounding)
                index = (index + 1) % sequenceSize;
        }

        const int note = sequence[(size_t) index];
        const float velocity = sequenceVelocity[(size_t) index];

        // Full gate is legato: the new note starts before the old one ends
        const int previous = sounding;
        out[count++] = { true, note, velocity };
        if (previous >= 0 && previous != note)
            out[count++] = { false, previous, 0.0f };

        sounding = note;
        gateOffAt = gate < 0.999f ? nextStep + gate : -1.0;
        nextStep += 1.0;
        ++stepIndex;
    }

    return count;
}

} // namespace dsynth
