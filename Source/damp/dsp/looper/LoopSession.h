#pragma once

#include "LoopPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace damp::looper
{

// Every track's audio and edits, as saved in a session. Positions are in frames at sampleRate;
// anchors are relative to the master loop's start, so a loaded session plays from the top.
struct LoopTrackData
{
    std::vector<float> left, right; // empty: no audio
    int64_t start = 0, end = 0;
    float crossfadeMs = 5.0f;
    int64_t anchor = 0;

    bool empty() const { return left.empty(); }
};

struct LoopSession
{
    double sampleRate = 48000.0;
    int masterTrack = -1;     // -1: none (masterLength may still hold, from a cleared master)
    int64_t masterLength = 0;
    std::array<LoopTrackData, numTracks> tracks;

    bool empty() const;
    size_t frames() const; // total over the tracks
};

// Packs a track's audio, as 24-bit integers, losslessly: FLAC on the plugin side (LoopFlac), which
// this library can't link. Its id is stored with each track.
struct SamplePacker
{
    virtual ~SamplePacker() = default;
    virtual uint32_t id() const = 0; // 0 and 1 are taken (float32, plainPacker)
    // Samples in [-(2^23 - 1), 2^23 - 1]
    virtual bool pack (const int32_t* left, const int32_t* right, size_t frames, std::vector<std::byte>& out) const = 0;
    virtual bool unpack (const std::byte* data, size_t size, size_t frames, int32_t* left, int32_t* right) const = 0;
};
// 24-bit little-endian, uncompressed: the integer path without a codec (tests, JUCE-free tools)
const SamplePacker& plainPacker();

// The binary block a session appends after its XML: magic "dlop", a version, the byte count of
// what follows, then the fields and every track's edits and audio.
//   Version 1: the audio as little-endian float32 (still read).
//   Version 2: each track's audio as float32 (codec 0, with no packer: sample-identical) or as
//     24-bit integers through `packer`, scaled by a power of two: the track's peak (over both
//     channels) sets the exponent e with peak < 2^e, and a sample is round (x * 2^(23 - e)),
//     held to +-(2^23 - 1). Nothing clips, however loud an overdub got, and the error is at most
//     half a step: 2^(e - 24), 138 to 144 dB below the peak. Being a power of two, the scale makes
//     decoding exact in float and re-encoding a decoded session gives the same bytes.
// Version 2 needs no more of the reader than its packer for any packed track.
void encodeSession (const LoopSession&, std::vector<std::byte>& out, const SamplePacker* packer = nullptr);
// nullopt if the data doesn't start with the magic, is malformed, or has a track packed by
// something other than `packer` (or plainPacker)
std::optional<LoopSession> decodeSession (const void* data, size_t size, const SamplePacker* packer = nullptr);
constexpr uint32_t sessionMagic = 0x706f6c64; // "dlop"
constexpr uint32_t sessionVersion = 2;

// Band-limited resampling (windowed sinc), for audio loaded at another rate
std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate);
// Audio and positions moved to another rate
LoopSession resampleSession (const LoopSession&, double toRate);

} // namespace damp::looper
