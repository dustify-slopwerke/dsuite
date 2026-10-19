#include "LoopPool.h"

#include <algorithm>
#include <cmath>

namespace damp::looper
{

LoopPool::LoopPool (double sampleRate, double seconds)
    : rate (sampleRate),
      total (std::max (1, (int) std::ceil (seconds * sampleRate / (double) chunkFrames)))
{
    const auto floats = (size_t) total * channels * (size_t) chunkFrames;
    data.reset (new float[floats]);
    std::fill_n (data.get(), floats, 0.0f); // touch every page now, not while recording
    overview.reset (new std::atomic<float>[(size_t) total * binsPerChunk * 2]);
    refs.assign ((size_t) total, 0);
    freeList.resize ((size_t) total);
    for (int i = 0; i < total; ++i)
        freeList[(size_t) i] = total - 1 - i; // hands out chunk 0 first
    freeCount = total;
}

int LoopPool::allocate()
{
    if (freeCount == 0)
        return -1;
    const int c = freeList[(size_t) --freeCount];
    refs[(size_t) c] = 1;
    used.store (total - freeCount, std::memory_order_relaxed);
    return c;
}

void LoopPool::release (int c)
{
    if (--refs[(size_t) c] > 0)
        return;
    freeList[(size_t) freeCount++] = c;
    used.store (total - freeCount, std::memory_order_relaxed);
}

void LoopPool::setBin (int c, int bin, float lo, float hi)
{
    auto* b = overview.get() + ((size_t) c * binsPerChunk + (size_t) bin) * 2;
    b[0].store (lo, std::memory_order_relaxed);
    b[1].store (hi, std::memory_order_relaxed);
}

void LoopPool::readBin (int c, int bin, float& lo, float& hi) const
{
    const auto* b = overview.get() + ((size_t) c * binsPerChunk + (size_t) bin) * 2;
    lo = b[0].load (std::memory_order_relaxed);
    hi = b[1].load (std::memory_order_relaxed);
}

void LoopPool::refreshBins (int c, int from, int to, int valid)
{
    for (int bin = from >> binShift; bin <= (to - 1) >> binShift && bin < binsPerChunk; ++bin)
    {
        const int a = bin << binShift, b = std::min (a + binFrames, valid);
        float lo = 0.0f, hi = 0.0f;
        if (a < b)
            lo = hi = samples (c, 0)[a];
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* x = samples (c, ch);
            for (int i = a; i < b; ++i)
            {
                lo = std::min (lo, x[i]);
                hi = std::max (hi, x[i]);
            }
        }
        setBin (c, bin, lo, hi);
    }
}

void LoopPool::copy (int from, int to)
{
    for (int ch = 0; ch < channels; ++ch)
        std::copy_n (samples (from, ch), chunkFrames, samples (to, ch));
    for (int bin = 0; bin < binsPerChunk; ++bin)
    {
        float lo, hi;
        readBin (from, bin, lo, hi);
        setBin (to, bin, lo, hi);
    }
}

void Take::clear (LoopPool& pool)
{
    for (int i = 0; i < count; ++i)
        if (chunks[(size_t) i] != silentChunk)
            pool.release (chunks[(size_t) i]);
    count = 0;
    length = 0;
    front = 0;
}

void Take::assign (const Take& other, LoopPool& pool)
{
    clear (pool);
    std::copy_n (other.chunks.begin(), other.count, chunks.begin());
    count = other.count;
    for (int i = 0; i < count; ++i)
        if (chunks[(size_t) i] != silentChunk)
            pool.retain (chunks[(size_t) i]);
    length = other.length;
    front = other.front;
    start = other.start;
    end = other.end;
    anchor = other.anchor;
}

LoopStore::LoopStore (double sampleRate, double seconds)
    : pool (sampleRate, seconds)
{
    const auto n = (size_t) takeCapacity (pool);
    for (auto& t : tracks)
    {
        t.current.chunks.resize (n);
        t.incoming.chunks.resize (n);
        for (auto& u : t.undo)
            u.chunks.resize (n);
        for (auto& r : t.redo)
            r.chunks.resize (n);
        t.published = std::vector<std::atomic<int>> (n);
    }
}

void LoopStore::publish (int track)
{
    auto& t = tracks[(size_t) track];
    for (int i = 0; i < t.current.count; ++i)
        t.published[(size_t) i].store (t.current.chunks[(size_t) i], std::memory_order_relaxed);
    t.publishedLength.store (t.current.length, std::memory_order_release);
    t.version.fetch_add (1, std::memory_order_release);
}

} // namespace damp::looper
