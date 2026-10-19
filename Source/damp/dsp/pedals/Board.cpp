#include "Board.h"
#include "Compressor.h"
#include "Delay.h"
#include "Drives.h"
#include "Eq.h"
#include "Modulation.h"
#include "Octave.h"
#include "Pitch.h"
#include "Reverb.h"
#include "Wah.h"

#include <algorithm>

namespace damp
{

Board::Board()
{
    pedals[(size_t) PedalId::Boost] = std::make_unique<pedals::Boost>();
    pedals[(size_t) PedalId::MidDrive] = std::make_unique<pedals::MidDrive>();
    pedals[(size_t) PedalId::BluesDrive] = std::make_unique<pedals::BluesDrive>();
    pedals[(size_t) PedalId::Distortion] = std::make_unique<pedals::Distortion>();
    pedals[(size_t) PedalId::Rodent] = std::make_unique<pedals::Rodent>();
    pedals[(size_t) PedalId::FaceFuzz] = std::make_unique<pedals::FaceFuzz>();
    pedals[(size_t) PedalId::BigFuzz] = std::make_unique<pedals::BigFuzz>();
    pedals[(size_t) PedalId::Compressor] = std::make_unique<pedals::Compressor>();
    pedals[(size_t) PedalId::Wah] = std::make_unique<pedals::Wah>();
    pedals[(size_t) PedalId::Octave] = std::make_unique<pedals::Octave>();
    pedals[(size_t) PedalId::Chorus] = std::make_unique<pedals::Chorus>();
    pedals[(size_t) PedalId::Flanger] = std::make_unique<pedals::Flanger>();
    pedals[(size_t) PedalId::Phaser] = std::make_unique<pedals::Phaser>();
    pedals[(size_t) PedalId::Tremolo] = std::make_unique<pedals::Tremolo>();
    pedals[(size_t) PedalId::Vibrato] = std::make_unique<pedals::Vibrato>();
    pedals[(size_t) PedalId::Delay] = std::make_unique<pedals::Delay>();
    pedals[(size_t) PedalId::Reverb] = std::make_unique<pedals::Reverb>();
    pedals[(size_t) PedalId::Pitch] = std::make_unique<pedals::Pitch>();
    pedals[(size_t) PedalId::Eq] = std::make_unique<pedals::Eq>();
}

Board::~Board() = default;

void Board::prepare (double hostRate, double maxRate)
{
    for (int i = 0; i < numPedals; ++i)
        pedals[(size_t) i]->prepare (pedalInfo ((PedalId) i).beforeAmpAllowed() ? maxRate : hostRate);
    reset();
}

void Board::setTempo (double bpm)
{
    for (auto& p : pedals)
        p->setTempo (bpm);
}

bool Board::active (int id) const
{
    const auto& f = fade[(size_t) id];
    return on[(size_t) id] || f[0] > 0.0f || f[1] > 0.0f || pedals[(size_t) id]->ringing();
}

void Board::reset()
{
    for (auto& p : pedals)
        p->reset();
    for (int i = 0; i < numPedals; ++i)
        fade[(size_t) i].fill (on[(size_t) i] ? 1.0f : 0.0f);
}

void Board::setPatch (const Patch& p)
{
    patch = p;
    for (int i = 0; i < numPedals; ++i)
    {
        const bool now = patch.on (pedalInfo ((PedalId) i).onParam);
        // Coming on from fully off: start from a clean state rather than whatever it held
        if (now && ! active (i))
            pedals[(size_t) i]->reset();
        on[(size_t) i] = now;
        if (pedals[(size_t) i]->hasTails())
            pedals[(size_t) i]->setEngaged (now);
    }
}

void Board::begin (int from, int to, double rate, int n)
{
    fadeStep = (float) (1.0 / (0.01 * rate)); // 10 ms
    for (int s = from; s < to; ++s)
    {
        const int id = board.slots[(size_t) s];
        if (id != BoardOrder::amp && active (id))
            pedals[(size_t) id]->setPatch (patch, rate, n);
    }
}

void Board::beginBeforeAmp (double rate, int n)
{
    begin (0, board.ampPosition(), rate, n);
}

void Board::processBeforeAmp (int channel, float* x, int n)
{
    for (int s = 0; s < board.ampPosition(); ++s)
        runPedal (board.slots[(size_t) s], channel, x, n);
}

void Board::processAfterAmp (float* left, float* right, int n, double rate)
{
    const int from = board.ampPosition() + 1;
    begin (from, BoardOrder::size, rate, n);
    for (int s = from; s < BoardOrder::size; ++s)
    {
        if (right != nullptr)
            runPedalStereo (board.slots[(size_t) s], left, right, n);
        else
            runPedal (board.slots[(size_t) s], 0, left, n);
    }
}

void Board::runPedal (int id, int channel, float* x, int n)
{
    if (pedals[(size_t) id]->hasTails())
    {
        if (active (id))
            pedals[(size_t) id]->process (channel, x, n);
        return;
    }

    float& f = fade[(size_t) id][(size_t) channel];
    const bool target = on[(size_t) id];
    if (! target && f <= 0.0f)
        return; // bypassed
    if (target && f >= 1.0f)
    {
        pedals[(size_t) id]->process (channel, x, n);
        return;
    }

    // Crossfading in or out
    std::copy_n (x, n, dry.begin());
    pedals[(size_t) id]->process (channel, x, n);
    for (int i = 0; i < n; ++i)
    {
        f = std::clamp (f + (target ? fadeStep : -fadeStep), 0.0f, 1.0f);
        x[i] = dry[(size_t) i] + f * (x[i] - dry[(size_t) i]);
    }
}

void Board::runPedalStereo (int id, float* left, float* right, int n)
{
    if (pedals[(size_t) id]->hasTails())
    {
        if (active (id))
            pedals[(size_t) id]->processStereo (left, right, n);
        return;
    }

    auto& f = fade[(size_t) id];
    const bool target = on[(size_t) id];
    if (! target && f[0] <= 0.0f && f[1] <= 0.0f)
        return; // bypassed
    if (target && f[0] >= 1.0f && f[1] >= 1.0f)
    {
        pedals[(size_t) id]->processStereo (left, right, n);
        return;
    }

    // Crossfading in or out
    std::copy_n (left, n, dry.begin());
    std::copy_n (right, n, dryRight.begin());
    pedals[(size_t) id]->processStereo (left, right, n);
    for (int i = 0; i < n; ++i)
    {
        for (auto& c : f)
            c = std::clamp (c + (target ? fadeStep : -fadeStep), 0.0f, 1.0f);
        left[i] = dry[(size_t) i] + f[0] * (left[i] - dry[(size_t) i]);
        right[i] = dryRight[(size_t) i] + f[1] * (right[i] - dryRight[(size_t) i]);
    }
}

} // namespace damp
