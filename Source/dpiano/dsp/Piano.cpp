#include "Piano.h"

#include "common/dsp/Denormals.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dpiano
{
namespace
{
// Effective bridge admittance (m/(N s)) at full "Sympathetic". Real boards measure around 5e-4,
// but this one-way model has no back-reaction from the driven strings, so it's scaled down until
// a partial driven exactly at its resonance answers at about -10 dB (C4)
constexpr float sympatheticCoupling = 2.4e-5f;

// Hammer force (N) -> board input (output units) at a key's full knock level
constexpr float knockScale = 3.56e-3f;

// Makeup gain (dB) on the whole output, ahead of the volume: everyday mezzo-forte playing comes out
// around -18 LUFS at volume 0 dB, with the limiter catching the loudest chords. Applied to the
// finished mix, so the tone doesn't change
constexpr float outputMakeupDb = 9.5f;
} // namespace

void Piano::prepare (double newRate)
{
    sampleRate = newRate;
    soundboard.prepare (sampleRate);
    room.prepare (sampleRate);
    noises.prepare (sampleRate);
    pedalStir.prepare (sampleRate);
    limiter.prepare (sampleRate);
    for (auto& k : keys)
        k.reset();
    held.fill (false);
    sostenutoHeld.fill (false);
    sustain = 0.0f;
    sostenuto = unaCorda = false;
    ++generation;
    setPatch (patch);
    for (int note = lowestKey; note <= highestKey; ++note)
    {
        configureKey (note);
        refreshDamper (note);
    }
}

void Piano::configureKey (int note)
{
    keys[index (note)].configure (note, settingsFor (note));
    keyGeneration[index (note)] = generation;
}

void Piano::setPatch (const Patch& newPatch)
{
    // Anything but the volume changes the strings: keys pick it up when next struck
    bool changed = false;
    for (const auto& info : allParams())
        changed |= info.pid != PID::MasterVolume && newPatch[info.pid] != patch[info.pid];
    if (changed)
        ++generation;
    if (changed || ! stretchValid)
    {
        stretch = stretchCents (newPatch[PID::Inharmonicity]);
        stretchValid = true;
    }

    patch = newPatch;
    volume = std::pow (10.0f, (patch[PID::MasterVolume] + outputMakeupDb) / 20.0f);
    coupling = sympatheticCoupling * patch[PID::Sympathetic];
    noiseScale = patch[PID::MechanicalNoise];
    soundboard.setParameters (patch[PID::SoundboardBody], patch[PID::Lid], patch[PID::MicDistance], patch[PID::Bloom]);
    soundboard.setRadiation (patch[PID::BassRadiation], patch[PID::Warmth], patch[PID::Air]);
    knockLevel = knockScale * patch[PID::Knock];

    // Keys spread along the bridge: bass on the left from the player's seat, on the right for
    // the audience
    const float side = patch.choice<Perspective> (PID::Perspective) == Perspective::Player ? 1.0f : -1.0f;
    for (size_t i = 0; i < (size_t) numKeys; ++i)
    {
        const float pan = side * patch[PID::StereoWidth] * ((float) i / (numKeys - 1) - 0.5f); // -0.5 .. 0.5
        const float angle = (pan + 0.5f) * 0.5f * std::numbers::pi_v<float>;
        panLeft[i] = std::cos (angle) * std::numbers::sqrt2_v<float>;
        panRight[i] = std::sin (angle) * std::numbers::sqrt2_v<float>;
    }

    const float size = patch[PID::RoomSize];
    room.setParameters (size, 0.5f + 2.5f * size, 0.55f, 5.0f + 25.0f * size);
    room.setMix (patch[PID::RoomMix]);
}

double Piano::firstPartialHz (int note) const
{
    const auto root = patch.index (PID::TemperamentRoot);
    const auto& temperament = tuningOffsets (patch.choice<Tuning> (PID::Temperament));
    const double semitones = (double) (note - 69) + temperament[(size_t) (((note - root) % 12 + 12) % 12)]
                             - temperament[(size_t) (((69 - root) % 12 + 12) % 12)] // keep A4 at A4
                             + (patch[PID::StretchTuning] * stretch[index (note)] + patch[PID::MasterTune]) / 100.0;
    return 440.0 * std::exp2 (semitones / 12.0);
}

PianoKey::Settings Piano::settingsFor (int note) const
{
    PianoKey::Settings s;
    s.sampleRate = sampleRate;
    s.firstPartialHz = firstPartialHz (note);
    s.inharmonicityScale = patch[PID::Inharmonicity];
    s.detuneCents = patch[PID::UnisonDetune];
    s.aftersoundGain = std::pow (10.0, patch[PID::Aftersound] / 20.0);
    s.decayScale = patch[PID::Decay];
    s.highLossScale = std::pow (4.0, -patch[PID::Brilliance]);
    // Treble felt: an extra offset that grows from middle C to the top
    s.hardness = patch[PID::HammerHardness] + patch[PID::TrebleHardness] * std::clamp ((note - 60) / 48.0, 0.0, 1.0);
    s.exponentOffset = patch[PID::FeltNonlinearity];
    s.boardDrain = patch[PID::BoardDrain];
    s.strikeScale = std::exp2 (0.6 * patch[PID::StrikePosition]);
    s.damperScale = 1.0 / patch[PID::DamperStrength];
    return s;
}

double Piano::hammerSpeed (float velocity) const
{
    constexpr double curves[] { 0.7, 1.0, 1.4 };
    const double v = std::pow (std::clamp ((double) velocity, 0.0, 1.0), curves[std::clamp (patch.index (PID::VelocityCurve), 0, 2)]);
    // Up to 6.4 m/s fortissimo. The default 28 dB range (pp .. ff, as measured on a grand) spans
    // 6.4 m/s down to 0.08 m/s (barely sounding)
    const double speedRatio = std::pow (80.0, patch[PID::DynamicRange] / 28.0);
    return 6.4 * std::pow (speedRatio, v - 1.0);
}

float Piano::damperAmount (int note) const
{
    const auto i = index (note);
    if (held[i] || sostenutoHeld[i])
        return 0.0f;
    // The dampers start to lift a quarter of the way down and are clear at three quarters
    return std::clamp (1.0f - (sustain - 0.25f) / 0.5f, 0.0f, 1.0f);
}

void Piano::refreshDamper (int note)
{
    keys[index (note)].setDamper (damperAmount (note));
}

void Piano::noteOn (int note, float playedVelocity)
{
    if (note < lowestKey || note > highestKey)
        return;
    const float velocity = velocityMap.apply (playedVelocity);
    const auto i = index (note);
    if (keyGeneration[i] != generation)
        configureKey (note);
    held[i] = true;
    refreshDamper (note);
    keys[i].strike (hammerSpeed (velocity), unaCorda);
    noises.keyThump (velocity, noiseScale);
}

void Piano::noteOff (int note)
{
    if (note < lowestKey || note > highestKey)
        return;
    held[index (note)] = false;
    refreshDamper (note);
    if (damperAmount (note) > 0.5f && keys[index (note)].isActive() && keys[index (note)].data().damperT60 > 0.0)
        noises.damperNoise (noiseScale);
}

void Piano::sustainPedalLevel (float level)
{
    level = std::clamp (level, 0.0f, 1.0f);
    if ((level >= 0.5f) != (sustain >= 0.5f))
    {
        noises.pedalThump (noiseScale);
        if (level >= 0.5f)
            pedalStir.pedalThump (noiseScale);
    }
    sustain = level;
    for (int n = lowestKey; n <= highestKey; ++n)
        refreshDamper (n);
}

void Piano::sostenutoPedal (bool down)
{
    if (down == sostenuto)
        return;
    sostenuto = down;
    // The sostenuto rod catches whichever dampers are up right now: the keys held, or every
    // damper if the sustain pedal is down too, as on a grand
    for (size_t i = 0; i < held.size(); ++i)
        sostenutoHeld[i] = down && (held[i] || sustain >= 0.75f);
    for (int n = lowestKey; n <= highestKey; ++n)
        refreshDamper (n);
}

void Piano::softPedal (bool down)
{
    unaCorda = down;
}

void Piano::allNotesOff()
{
    held.fill (false);
    sostenutoHeld.fill (false);
    sustain = 0.0f;
    sostenuto = unaCorda = false;
    for (int note = lowestKey; note <= highestKey; ++note)
        refreshDamper (note);
}

int Piano::activeKeys() const
{
    return (int) std::count_if (keys.begin(), keys.end(), [] (const PianoKey& k) { return k.isActive(); });
}

int Piano::resonatingKeys() const
{
    return (int) std::count_if (keys.begin(), keys.end(), [] (const PianoKey& k) { return k.isResonating(); });
}

void Piano::process (float* left, float* right, int numSamples)
{
    dustify::ScopedFlushDenormals noDenormals;
    std::fill (left, left + numSamples, 0.0f);
    std::fill (right, right + numSamples, 0.0f);

    // Bring a few silent keys up to date with the patch (struck keys catch up when struck), so a
    // parameter change never costs all 88 at once
    for (int count = 0, tried = 0; count < 4 && tried < numKeys; ++tried)
    {
        nextToRefresh = (nextToRefresh + 1) % numKeys;
        if (keyGeneration[(size_t) nextToRefresh] != generation && ! keys[(size_t) nextToRefresh].isActive())
        {
            configureKey (lowestKey + nextToRefresh);
            ++count;
        }
    }

    for (int done = 0; done < numSamples;)
    {
        const int n = std::min (chunk, numSamples - done);

        // Struck strings -> bridge, and panned by key for the direct sound
        std::fill (bridge.begin(), bridge.begin() + n, 0.0f);
        std::fill (knock.begin(), knock.begin() + n, 0.0f);
        std::fill (directLeft.begin(), directLeft.begin() + n, 0.0f);
        std::fill (directRight.begin(), directRight.begin() + n, 0.0f);
        for (size_t k = 0; k < keys.size(); ++k)
        {
            if (! keys[k].isActive())
            {
                keys[k].process (bridge.data(), n); // keeps its damper ramp moving
                continue;
            }
            std::fill (keyOut.begin(), keyOut.begin() + n, 0.0f);
            keys[k].process (keyOut.data(), n);
            if (keys[k].wasHammering())
            {
                // The impact itself reaches the board through bridge and frame: the knock
                const float gain = knockLevel * (float) keys[k].data().knock;
                const float* force = keys[k].lastHammerForce();
                for (int i = 0; i < n; ++i)
                    knock[(size_t) i] += gain * force[i];
            }
            for (int i = 0; i < n; ++i)
            {
                bridge[(size_t) i] += keyOut[(size_t) i];
                directLeft[(size_t) i] += panLeft[k] * keyOut[(size_t) i];
                directRight[(size_t) i] += panRight[k] * keyOut[(size_t) i];
            }
        }

        // The bridge (and the stir of the dampers lifting) drives every undamped string
        std::copy (bridge.begin(), bridge.begin() + n, drive.begin());
        pedalStir.process (drive.data(), n);
        std::copy (bridge.begin(), bridge.begin() + n, board.begin());
        if (coupling > 0.0f)
            for (auto& k : keys)
                k.processSympathetic (drive.data(), board.data(), n, coupling);

        // Plus the knocks and the action's noises; these, and the sympathetic halo, come from the
        // whole board
        for (int i = 0; i < n; ++i)
            board[(size_t) i] += knock[(size_t) i];
        noises.process (board.data(), n);
        for (int i = 0; i < n; ++i)
        {
            const float centre = board[(size_t) i] - bridge[(size_t) i];
            directLeft[(size_t) i] += centre;
            directRight[(size_t) i] += centre;
        }
        soundboard.process (board.data(), directLeft.data(), directRight.data(), left + done, right + done, n);
        room.process (left + done, right + done, n);

        for (int i = done; i < done + n; ++i)
        {
            left[i] *= volume;
            right[i] *= volume;
        }
        done += n;
    }

    limiter.process (left, right, numSamples);
}

} // namespace dpiano
