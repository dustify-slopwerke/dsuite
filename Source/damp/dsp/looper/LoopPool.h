#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace damp::looper
{

constexpr int numTracks = 8;
constexpr int maxUndo = 4; // undo levels per track (and as many redo levels)

// Sample memory shared by every looper track: a fixed number of chunks of `chunkFrames` stereo
// frames, handed out from a free list, so tracks of any length share it without fragmenting.
// A chunk can be held by more than one take (an undo level, a snapshot being saved): it's
// reference counted, and an overdub copies a shared chunk before changing it (copy on write).
// Each chunk carries a min/max overview of its audio in bins of `binFrames`, which the UI may
// read at any time (the bins are atomics).
//
// Built, and its memory touched, on the message thread; after that only the engine's owner (see
// Looper) allocates, shares, frees and writes, except that whoever claimed a chunk may fill it
// before handing it over. Nothing here allocates after construction.
class LoopPool
{
public:
    static constexpr int chunkShift = 14;
    static constexpr int64_t chunkFrames = int64_t { 1 } << chunkShift; // 0.34 s at 48 kHz
    static constexpr int64_t chunkMask = chunkFrames - 1;
    static constexpr int binShift = 8;
    static constexpr int binFrames = 1 << binShift;
    static constexpr int binsPerChunk = (int) (chunkFrames >> binShift);
    static constexpr int channels = 2;

    LoopPool (double sampleRate, double seconds);

    double sampleRate() const { return rate; }
    int numChunks() const { return total; }
    int usedChunks() const { return used.load (std::memory_order_relaxed); }
    int freeChunks() const { return freeCount; }

    int allocate(); // a chunk with one reference, or -1 when the pool is full
    void retain (int c) { ++refs[(size_t) c]; }
    void release (int c);
    bool shared (int c) const { return refs[(size_t) c] > 1; }

    float* samples (int c, int channel) { return data.get() + ((size_t) c * channels + (size_t) channel) * (size_t) chunkFrames; }
    const float* samples (int c, int channel) const { return data.get() + ((size_t) c * channels + (size_t) channel) * (size_t) chunkFrames; }

    // Overview: the lowest and highest sample of both channels in each bin
    void setBin (int c, int bin, float lo, float hi);
    void readBin (int c, int bin, float& lo, float& hi) const;
    // Recomputes the bins covering frames [from, to) of chunk c, counting only frames below `valid`
    void refreshBins (int c, int from, int to, int valid);

    // Samples and overview
    void copy (int from, int to);

private:
    double rate;
    int total;
    std::unique_ptr<float[]> data;
    std::unique_ptr<std::atomic<float>[]> overview; // total * binsPerChunk * (min, max)
    std::vector<int> refs, freeList;
    int freeCount = 0;
    std::atomic<int> used { 0 };
};

// One version of a track's recording: its chunks in order and how many frames it holds, with
// the edits it had when it was put aside (restored when an undo or redo brings it back from or
// to an empty track). A chunk may be `silentChunk`: frames of silence that use no memory (where
// an overdub grew the take into its padding but hasn't written yet).
constexpr int silentChunk = -1;
struct Take
{
    std::vector<int> chunks; // sized to takeCapacity, so it never reallocates
    int count = 0;
    int64_t length = 0;
    int64_t start = 0, end = 0, anchor = 0;
    int64_t front = 0; // frames put before the recording by overdubs into the padding (start, end and anchor moved by as much)

    void clear (LoopPool&);
    void assign (const Take&, LoopPool&); // shares the other take's chunks
};

// The pool and every track's takes: what the message thread builds and hands to the audio thread
// in one piece. `published` mirrors each track's current chunk list for the UI's overview.
struct LoopStore
{
    LoopStore (double sampleRate, double seconds);

    // Chunks a take may span: the recording (at most the pool) and its padding grown into on
    // either side (each at most the pool's length, see Looper::setEdits), silent chunks included
    static int takeCapacity (const LoopPool& p) { return 3 * p.numChunks() + 4; }

    struct Track
    {
        Take current, incoming; // incoming: audio handed over from the message thread
        std::array<Take, maxUndo> undo, redo; // oldest first
        int undoCount = 0, redoCount = 0;
        std::vector<std::atomic<int>> published;
        std::atomic<int64_t> publishedLength { 0 };
        std::atomic<uint32_t> version { 0 }; // bumped whenever the audio changes
    };

    LoopPool pool;
    std::array<Track, numTracks> tracks;

    void publish (int track); // the whole chunk list
};

} // namespace damp::looper
