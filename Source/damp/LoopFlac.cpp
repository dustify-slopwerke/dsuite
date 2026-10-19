#include "LoopFlac.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr int blockFrames = 1 << 16;
constexpr int bits = 24;
// FLAC wants a rate in its header; the session keeps the real one, so this is only a label
constexpr double headerRate = 48000.0;
} // namespace

const LoopFlacPacker& LoopFlacPacker::instance()
{
    static const LoopFlacPacker packer;
    return packer;
}

bool LoopFlacPacker::pack (const int32_t* left, const int32_t* right, size_t frames, std::vector<std::byte>& out) const
{
    juce::MemoryBlock block;
    {
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
        // Level 5 (FLAC's default): on a full pool of guitar loops 8 saves ~3% for 2.5x the time, 1 costs ~25% more space
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                        .withSampleRate (headerRate)
                                                        .withNumChannels (2)
                                                        .withBitsPerSample (bits)
                                                        .withQualityOptionIndex (5));
        if (writer == nullptr)
            return false;
        // The writer takes samples left-justified in 32 bits
        std::vector<int> l ((size_t) blockFrames), r ((size_t) blockFrames);
        for (size_t at = 0; at < frames; at += (size_t) blockFrames)
        {
            const auto n = std::min (frames - at, (size_t) blockFrames);
            for (size_t i = 0; i < n; ++i)
            {
                l[i] = (int) ((uint32_t) left[at + i] << (32 - bits));
                r[i] = (int) ((uint32_t) right[at + i] << (32 - bits));
            }
            const int* channels[] { l.data(), r.data(), nullptr };
            if (! writer->write (channels, (int) n))
                return false;
        }
    } // the writer finishes the stream (its header) as it goes
    const auto* p = static_cast<const std::byte*> (block.getData());
    out.insert (out.end(), p, p + block.getSize());
    return true;
}

bool LoopFlacPacker::unpack (const std::byte* data, size_t size, size_t frames, int32_t* left, int32_t* right) const
{
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (
        flac.createReaderFor (new juce::MemoryInputStream (data, size, false), true));
    if (reader == nullptr || reader->numChannels != 2 || reader->bitsPerSample != bits || reader->usesFloatingPointData
        || reader->lengthInSamples != (juce::int64) frames)
        return false;
    std::vector<int> l ((size_t) blockFrames), r ((size_t) blockFrames);
    for (size_t at = 0; at < frames; at += (size_t) blockFrames)
    {
        const auto n = std::min (frames - at, (size_t) blockFrames);
        int* channels[] { l.data(), r.data() };
        if (! reader->read (channels, 2, (juce::int64) at, (int) n, false))
            return false;
        for (size_t i = 0; i < n; ++i)
        {
            left[at + i] = l[i] >> (32 - bits);
            right[at + i] = r[i] >> (32 - bits);
        }
    }
    return true;
}
