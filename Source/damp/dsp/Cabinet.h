#pragma once

#include "Params.h"

#include <vector>

namespace damp
{

// Speaker cabinets as impulse responses. Modelled cabs are built from a magnitude model (speaker
// resonance, open-back cancellation, cone breakup, the mic's own response, its position across
// the cone and its distance) made minimum-phase, plus the reflections a real recording picks up.
// User IRs get resampled and levelled the same way. All of this allocates: never on the audio
// thread.
enum class CabType : int { Off, Open1x12, Open2x12, Closed4x12, User };
enum class MicType : int { Dynamic, Ribbon, Condenser };

constexpr double maxImpulseSeconds = 0.5; // longer user IRs are cut short

struct CabSettings
{
    CabType cab = CabType::Closed4x12;
    MicType mic = MicType::Dynamic;
    float position = 0.3f; // 0 = cone centre, 1 = edge
    float distance = 0.2f; // 0 = on the grille, 1 = a couple of feet back

    bool operator== (const CabSettings&) const = default;
};

CabSettings cabSettings (const Patch&);

// {1} (a pass-through) for Off and User
std::vector<float> modelledCabinet (const CabSettings&, double sampleRate);

// Resampled to `sampleRate` (windowed sinc), trimmed to `maxSeconds`, levelled like the modelled
// ones. Output starts at the IR's first sample (no added delay), so an IR that starts abruptly
// loses the interpolator's leading half there: under 1 dB above 10 kHz when upsampling.
std::vector<float> prepareUserImpulse (const std::vector<float>& impulse, double impulseRate, double sampleRate,
                                       double maxSeconds);

// Scales an impulse response so its average level from 100 Hz to 5 kHz is 0 dB: cabs swap at
// matching loudness
void levelImpulse (std::vector<float>&, double sampleRate);

} // namespace damp
