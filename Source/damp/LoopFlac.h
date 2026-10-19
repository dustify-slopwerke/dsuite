#pragma once

#include "dsp/looper/LoopSession.h"

// Packs saved loops with FLAC (JUCE's FlacAudioFormat, 24-bit stereo), for encodeSession and
// decodeSession: about half the size of the 24-bit samples on a guitar loop, and silence (the
// padding overdubs grew into) next to nothing.
struct LoopFlacPacker final : damp::looper::SamplePacker
{
    static constexpr uint32_t packerId = 2;
    uint32_t id() const override { return packerId; }
    bool pack (const int32_t* left, const int32_t* right, size_t frames, std::vector<std::byte>& out) const override;
    bool unpack (const std::byte* data, size_t size, size_t frames, int32_t* left, int32_t* right) const override;

    static const LoopFlacPacker& instance();
};
