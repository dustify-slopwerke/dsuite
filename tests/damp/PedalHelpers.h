#pragma once

#include "dsp/pedals/Compressor.h"
#include "dsp/pedals/Delay.h"
#include "dsp/pedals/Drives.h"
#include "dsp/pedals/Eq.h"
#include "dsp/pedals/Modulation.h"
#include "dsp/pedals/Octave.h"
#include "dsp/pedals/Pitch.h"
#include "dsp/pedals/Reverb.h"
#include "dsp/pedals/Wah.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

// Shared by the pedal test files
namespace damp::pedaltest
{
constexpr double highRate = 192000.0; // pedals before the amp run at the oversampled rate

inline std::vector<float> sine (double f, double seconds, double amplitude, double rate)
{
    std::vector<float> x ((size_t) (seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (amplitude * std::sin (2.0 * std::numbers::pi * f * (double) i / rate));
    return x;
}

inline std::unique_ptr<Pedal> makePedal (PedalId id)
{
    switch (id)
    {
        case PedalId::Boost: return std::make_unique<pedals::Boost>();
        case PedalId::MidDrive: return std::make_unique<pedals::MidDrive>();
        case PedalId::BluesDrive: return std::make_unique<pedals::BluesDrive>();
        case PedalId::Distortion: return std::make_unique<pedals::Distortion>();
        case PedalId::Rodent: return std::make_unique<pedals::Rodent>();
        case PedalId::FaceFuzz: return std::make_unique<pedals::FaceFuzz>();
        case PedalId::BigFuzz: return std::make_unique<pedals::BigFuzz>();
        case PedalId::Compressor: return std::make_unique<pedals::Compressor>();
        case PedalId::Wah: return std::make_unique<pedals::Wah>();
        case PedalId::Octave: return std::make_unique<pedals::Octave>();
        case PedalId::Chorus: return std::make_unique<pedals::Chorus>();
        case PedalId::Flanger: return std::make_unique<pedals::Flanger>();
        case PedalId::Phaser: return std::make_unique<pedals::Phaser>();
        case PedalId::Tremolo: return std::make_unique<pedals::Tremolo>();
        case PedalId::Vibrato: return std::make_unique<pedals::Vibrato>();
        case PedalId::Delay: return std::make_unique<pedals::Delay>();
        case PedalId::Reverb: return std::make_unique<pedals::Reverb>();
        case PedalId::Pitch: return std::make_unique<pedals::Pitch>();
        case PedalId::Eq: return std::make_unique<pedals::Eq>();
        case PedalId::Count: break;
    }
    return nullptr;
}

// One pedal on its own, in blocks of 256
inline std::vector<float> runPedal (PedalId id, std::vector<float> x, std::initializer_list<std::pair<PID, float>> settings = {},
                             double rate = highRate)
{
    auto patch = Patch::defaults();
    for (auto [pid, v] : settings)
        patch[pid] = v;
    auto pedal = makePedal (id);
    pedal->prepare (rate);
    for (size_t pos = 0; pos < x.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, x.size() - pos);
        pedal->setPatch (patch, rate, n);
        pedal->process (0, x.data() + pos, n);
    }
    return x;
}

} // namespace damp::pedaltest
