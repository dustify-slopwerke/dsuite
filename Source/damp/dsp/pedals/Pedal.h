#pragma once

#include "../Params.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace damp
{

// One stomp box. Pedals before the amp run at the amp's oversampled rate (so their clipping
// doesn't alias), those after it at the host rate; either way a pedal keeps separate state for
// each of two channels. prepare() is the only call that may allocate.
class Pedal
{
public:
    virtual ~Pedal() = default;

    // maxRate: the highest rate process() will be called at
    virtual void prepare (double maxRate) = 0;
    virtual void reset() = 0;
    // Once per block, before process(); `rate` is what process() will run at this block
    virtual void setPatch (const Patch&, double rate, int blockSize) = 0;
    // In place
    virtual void process (int channel, float* x, int n) = 0;
    // After the amp with a stereo output: both channels together, so a pedal can spread a mono
    // input across them. By default each channel on its own.
    virtual void processStereo (float* left, float* right, int n)
    {
        process (0, left, n);
        process (1, right, n);
    }

    // The host's tempo (quarter notes per minute), before setPatch(); 120 when there is no host
    virtual void setTempo (double bpm) { (void) bpm; }

    // Pedals with tails (delay, reverb) switch themselves: Board doesn't crossfade them, it calls
    // setEngaged() before each setPatch() and keeps running them while they're on or ringing().
    // Switched off, such a pedal stops taking new input and either lets what it holds ring out
    // (trails) or fades it out; either way the dry signal passes at unity.
    virtual bool hasTails() const { return false; }
    virtual void setEngaged (bool on) { (void) on; }
    virtual bool ringing() const { return false; }
};

// Every pedal, in the order their parameters were added. Only append.
enum class PedalId : int
{
    Boost, MidDrive, BluesDrive, Distortion, Rodent, FaceFuzz, BigFuzz,
    Compressor, Wah, Octave, Chorus, Flanger, Phaser, Tremolo, Vibrato,
    Delay, Reverb, Pitch, Eq,
    Count
};

// Where a pedal may sit relative to the amp
enum class Placement : int
{
    BeforeAmp, // clipping pedals: after the amp they'd run without oversampling
    Either,
    AfterAmp // long delay lines: before the amp they'd run at the oversampled rate
};
constexpr int numPedals = (int) PedalId::Count;

struct PedalInfo
{
    PedalId id;
    std::string_view key;  // in presets and saved board orders: never change
    std::string_view name; // shown on the board
    PID onParam;
    std::span<const PID> params; // knobs, in panel order (not including the on switch)
    Placement placement;

    bool beforeAmpAllowed() const { return placement != Placement::AfterAmp; }
    bool afterAmpAllowed() const { return placement != Placement::BeforeAmp; }
};

const PedalInfo& pedalInfo (PedalId);

// The signal chain: every pedal once, plus the amp (with its cab), in order. Pedals before the
// amp feed it; pedals after it process the amp's output.
struct BoardOrder
{
    static constexpr int amp = -1; // the amp's place in `slots`
    static constexpr int size = numPedals + 1;
    std::array<int, size> slots {};

    static BoardOrder defaults();

    // "face_fuzz, mid_drive, amp, ...": unknown names are skipped, a missing pedal goes just before
    // the first one that follows it in the default order, pedals that can't go after the amp are
    // moved before it and those that can't go before it just after it
    static BoardOrder parse (std::string_view);
    std::string format() const;

    int ampPosition() const;
    bool operator== (const BoardOrder&) const = default;
};

} // namespace damp
