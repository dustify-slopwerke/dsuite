#include "Looper.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

namespace damp
{
using looper::LoopPool;
using looper::LoopStore;
using looper::Take;

namespace
{
int64_t floorMod (int64_t a, int64_t b)
{
    const int64_t m = a % b;
    return m < 0 ? m + b : m;
}

int64_t nowNanos()
{
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::max<int64_t> (1, std::chrono::duration_cast<std::chrono::nanoseconds> (t).count());
}

constexpr int64_t minRecording = 64; // a shorter recording (a double tap) is discarded

// Equal-power weights across a seam: w 0..1 from the loop's end to its start
void seamWeights (int64_t phi, const int64_t period, int fade, float& a, float& b)
{
    const float w = ((float) (phi - (period - fade)) + 0.5f) / (float) fade;
    a = std::cos (w * 0.5f * std::numbers::pi_v<float>);
    b = std::sin (w * 0.5f * std::numbers::pi_v<float>);
}

// A loop shorter than its period fades out before the silence that follows it
float tailGain (int64_t phi, int64_t loop, int64_t period, int fade)
{
    if (phi >= loop)
        return 0.0f;
    if (loop >= period || fade <= 0)
        return 1.0f;
    return std::min (1.0f, ((float) (loop - phi) - 0.5f) / (float) fade);
}
} // namespace

Looper::Looper() = default;
Looper::~Looper() = default;

// ======================================================================================
// Reading takes

void Looper::readRecorded (const LoopPool& pool, const Take& take, int64_t i, int fade, float& l, float& r)
{
    if (i < 0 || i >= take.length)
    {
        l = r = 0.0f;
        return;
    }
    const int c = take.chunks[(size_t) (i >> LoopPool::chunkShift)];
    if (c == looper::silentChunk)
    {
        l = r = 0.0f;
        return;
    }
    const auto o = (size_t) (i & LoopPool::chunkMask);
    l = pool.samples (c, 0)[o];
    r = pool.samples (c, 1)[o];
    if (fade > 0 && (i < fade || i >= take.length - fade))
    {
        // The recording's own edges fade in and out
        const float g = std::min ({ 1.0f, ((float) i + 0.5f) / (float) fade, ((float) (take.length - i) - 0.5f) / (float) fade });
        l *= g;
        r *= g;
    }
}

// The loop's content at phi (0 <= phi < period), seam included
void Looper::contentAt (const LoopPool& pool, const Take& take, const Mapping& m, int64_t phi, float& l, float& r)
{
    const int64_t i = m.start + phi;
    l = r = 0.0f;
    if (const float g = tailGain (phi, m.loop, m.period, m.fade); g > 0.0f)
    {
        readRecorded (pool, take, i, m.fade, l, r);
        l *= g;
        r *= g;
    }
    if (m.fade > 0 && phi >= m.period - m.fade)
    {
        // Blend into what precedes the start, which leads into it
        float a, b, pl, pr;
        seamWeights (phi, m.period, m.fade, a, b);
        readRecorded (pool, take, i - m.period, m.fade, pl, pr);
        l = l * a + pl * b;
        r = r * a + pr * b;
    }
}

void Looper::voiceAt (const LoopPool& pool, const Take& take, const Mapping& m, int64_t phase, float& l, float& r)
{
    if (! m.half)
    {
        contentAt (pool, take, m, m.reverse ? m.period - 1 - phase : phase, l, r);
        return;
    }
    // Half speed: every other output frame falls between two source frames
    const int64_t q = m.reverse ? 2 * m.period - 1 - phase : phase;
    contentAt (pool, take, m, q >> 1, l, r);
    if ((q & 1) != 0)
    {
        float l2, r2;
        contentAt (pool, take, m, ((q >> 1) + 1) % m.period, l2, r2);
        l = 0.5f * (l + l2);
        r = 0.5f * (r + r2);
    }
}

int64_t Looper::sourcePhase (const Mapping& m, int64_t phase)
{
    if (! m.half)
        return m.reverse ? m.period - 1 - phase : phase;
    return (m.reverse ? 2 * m.period - 1 - phase : phase) >> 1;
}

int64_t Looper::phaseForSource (const Mapping& m, int64_t source)
{
    const int64_t s = floorMod (source, m.period);
    if (! m.half)
        return m.reverse ? m.period - 1 - s : s;
    return m.reverse ? 2 * m.period - 1 - 2 * s : 2 * s;
}

int64_t Looper::phaseAt (const Voice& v, int64_t clockAt) const
{
    return v.m.sync ? floorMod (clockAt - v.m.anchor - v.m.start, v.m.cycle()) : v.phase;
}

// ======================================================================================
// Audio thread

void Looper::prepare (double sampleRate)
{
    for (int expected = 0; ! owner.compare_exchange_weak (expected, 2, std::memory_order_acquire); expected = 0)
        std::this_thread::yield();
    rate = sampleRate;
    gateStep = (float) (1.0 / (0.003 * rate));
    blendStep = (float) (1.0 / (0.01 * rate));
    writeStep = (float) (1.0 / (0.005 * rate));
    gainCoeff = (float) (1.0 - std::exp (-1.0 / (0.01 * rate)));
    for (int t = 0; t < numTracks; ++t)
        remap (t, false); // the fade length is in milliseconds
    owner.store (0, std::memory_order_release);
}

void Looper::setPatch (const Patch& p)
{
    controls.on = p.on (PID::LooperOn);
    controls.selected = std::clamp (p.index (PID::LooperTrack), 0, numTracks - 1);
    for (int t = 0; t < numTracks; ++t)
    {
        const float db = p[trackParam (t, TrackParam::Level)];
        controls.level[(size_t) t] = db <= -48.0f ? 0.0f : std::pow (10.0f, db / 20.0f);
        controls.mute[(size_t) t] = p.on (trackParam (t, TrackParam::Mute));
        controls.reverse[(size_t) t] = p.on (trackParam (t, TrackParam::Reverse));
        controls.half[(size_t) t] = p.on (trackParam (t, TrackParam::Half));
        controls.sync[(size_t) t] = p.on (trackParam (t, TrackParam::Sync));
    }

    controls.quantise = p.on (PID::LooperQuantise);
    controls.metronome = p.on (PID::MetronomeOn);
    controls.recOnly = p.on (PID::MetronomeRecOnly);
    controls.countIn = p.on (PID::CountIn);
    controls.beatsPerBar = beatsPerBar (std::clamp (p.index (PID::BeatsPerBar), 0, 5));

    constexpr PID ids[] { PID::LooperRec, PID::LooperPlayStop, PID::LooperUndo, PID::LooperRedo, PID::LooperClear };
    constexpr Action actions[] { Action::Rec, Action::PlayStop, Action::Undo, Action::Redo, Action::Clear };
    const bool adopt = ! buttonsKnown || resync.exchange (false);
    for (size_t b = 0; b < buttons.size(); ++b)
    {
        const bool now = p.on (ids[b]);
        if (now && ! buttons[b] && ! adopt && numEdgeActions < (int) edgeActions.size())
            edgeActions[(size_t) numEdgeActions++] = (int) actions[b] * 16 + controls.selected;
        buttons[b] = now;
    }
    buttonsKnown = true;
}

void Looper::setTransport (double bpm, bool hostRunning, double ppq)
{
    if (! std::isfinite (bpm) || bpm <= 0.0)
        bpm = 120.0;
    transport.bpm = std::clamp (bpm, 10.0, 1000.0);
    transport.host = hostRunning && std::isfinite (ppq);
    transport.ppq = transport.host ? ppq : 0.0;
    tempo.store (transport.bpm, std::memory_order_relaxed);
}

LoopStore* Looper::usable() const
{
    return store != nullptr && store->pool.sampleRate() == rate ? store : nullptr;
}

void Looper::process (float* left, float* right, int n)
{
    lastBlock.store (nowNanos(), std::memory_order_relaxed);
    numClicks = 0;
    if (int expected = 0; ! owner.compare_exchange_strong (expected, 1, std::memory_order_acquire))
        return; // a message call holds the engine (only ever while audio was idle): the input passes

    // The controls, then what the message thread sent
    if (controls.on != on)
    {
        on = controls.on;
        if (! on)
        {
            cancelArm();
            stopAll();
        }
    }
    if (controls.metronome && ! metronomeWasOn)
    {
        // Switched on: the free clock starts on a bar, at once
        beatPos = 0.0;
        lastClick = std::numeric_limits<int64_t>::min() / 2;
    }
    metronomeWasOn = controls.metronome;
    selected = controls.selected;
    for (int t = 0; t < numTracks; ++t)
    {
        auto& tr = tracks[(size_t) t];
        tr.level = controls.level[(size_t) t];
        tr.mute = controls.mute[(size_t) t];
        const bool rev = controls.reverse[(size_t) t], half = controls.half[(size_t) t], sync = controls.sync[(size_t) t];
        if (rev != tr.reverse || half != tr.half || sync != tr.sync)
        {
            tr.reverse = rev;
            tr.half = half;
            tr.sync = sync;
            if (! canOverdub (t) && tr.writeTarget > 0.0f)
                punchOut (t, false);
            remap (t, true);
        }
    }

    service();

    // Transport buttons (kept for a moment if the pool is still on its way)
    if (usable() != nullptr || ! on)
    {
        for (int i = 0; i < numEdgeActions; ++i)
            if (on)
                doAction ((Action) (edgeActions[(size_t) i] / 16), edgeActions[(size_t) i] % 16);
        numEdgeActions = 0;
    }

    // The beats run with or without loops (the metronome needs no pool)
    for (int at = 0; at < n;)
    {
        int chunk = std::min (n - at, maxBlock);
        if (arm.track >= 0)
        {
            // An armed recording starts on its frame exactly: the chunk ends there
            if (usable() == nullptr)
                cancelArm();
            else if (elapsed >= arm.at)
            {
                const int t = arm.track;
                const bool quantised = arm.quantised;
                arm = {};
                startRecording (t, quantised);
            }
            else
            {
                chunk = (int) std::min<int64_t> (chunk, arm.at - elapsed);
            }
        }
        clickGrid (at, chunk);

        if (usable() != nullptr)
        {
            float* l = left + at;
            float* r = right == nullptr ? nullptr : right + at;
            std::copy_n (l, chunk, inL.begin());
            std::copy_n (r == nullptr ? l : r, chunk, inR.begin());
            std::fill_n (loopL.begin(), chunk, 0.0f);
            std::fill_n (loopR.begin(), chunk, 0.0f);

            const bool run = running();
            for (int t = 0; t < numTracks; ++t)
                renderTrack (t, chunk, r == nullptr, loopL.data(), loopR.data());
            if (run)
                clock += chunk;

            for (int i = 0; i < chunk; ++i)
                l[i] += loopL[(size_t) i];
            if (r != nullptr)
                for (int i = 0; i < chunk; ++i)
                    r[i] += loopR[(size_t) i];

            // Ops wait for their dip to reach silence; edits for the crossfade before them
            for (int t = 0; t < numTracks; ++t)
            {
                auto& tr = tracks[(size_t) t];
                if (tr.pending != Op::None && tr.gate <= 0.0f)
                    applyOp (t);
                const bool silent = tr.gate <= 0.0f && tr.gateTarget <= 0.0f;
                if (tr.remapPending && (tr.blend >= 1.0f || silent))
                {
                    // A silent track doesn't render, so its crossfade wouldn't finish: drop it
                    tr.remapPending = false;
                    if (silent)
                        tr.blend = 1.0f;
                    remap (t, true);
                }
            }
        }
        elapsed += chunk;
        if (transport.host)
            transport.ppq += (double) chunk * transport.bpm / (60.0 * rate);
        at += chunk;
    }

    publishStatus();
    owner.store (0, std::memory_order_release);
}

bool Looper::running() const
{
    for (const auto& tr : tracks)
        if (tr.state == State::Recording || tr.state == State::Playing || tr.state == State::Overdubbing || tr.gate > 0.0f)
            return true;
    return false;
}

void Looper::renderTrack (int t, int n, bool mono, float* outL, float* outR)
{
    auto& tr = tracks[(size_t) t];
    int from = 0;
    if (tr.state == State::Recording)
    {
        // A recording that closes in this block plays from the frame after it
        from = record (t, n, mono);
        if (tr.state == State::Recording || from >= n)
            return;
    }
    if (tr.state == State::Empty || (tr.gate <= 0.0f && tr.gateTarget <= 0.0f && tr.writeGain <= 0.0f && tr.writeTarget <= 0.0f))
        return;

    const auto& pool = store->pool;
    const auto& take = store->tracks[(size_t) t].current;
    int64_t phase = phaseAt (tr.voice, clock + from), oldPhase = phaseAt (tr.old, clock + from);
    const int64_t cycle = tr.voice.m.cycle(), oldCycle = tr.old.m.cycle();
    const float gainTarget = tr.mute ? 0.0f : tr.level;

    for (int i = from; i < n; ++i)
    {
        tr.gate = tr.gate < tr.gateTarget ? std::min (tr.gateTarget, tr.gate + gateStep) : std::max (tr.gateTarget, tr.gate - gateStep);
        tr.gain += (gainTarget - tr.gain) * gainCoeff;

        float l, r;
        voiceAt (pool, take, tr.voice.m, phase, l, r);
        if (tr.blend < 1.0f)
        {
            tr.blend = std::min (1.0f, tr.blend + blendStep);
            float ol, orr;
            voiceAt (pool, take, tr.old.m, oldPhase, ol, orr);
            l = ol + tr.blend * (l - ol);
            r = orr + tr.blend * (r - orr);
        }
        const float g = tr.gain * tr.gate;
        if (mono)
        {
            outL[i] += 0.5f * (l + r) * g;
        }
        else
        {
            outL[i] += l * g;
            outR[i] += r * g;
        }

        if (tr.writeGain > 0.0f || tr.writeTarget > 0.0f)
        {
            tr.writeGain = tr.writeGain < tr.writeTarget ? std::min (tr.writeTarget, tr.writeGain + writeStep)
                                                         : std::max (tr.writeTarget, tr.writeGain - writeStep);
            if (! overdubAt (t, phase, inL[(size_t) i] * tr.writeGain, inR[(size_t) i] * tr.writeGain))
            {
                ++poolFull;
                punchOut (t, true); // the pool is full
            }
        }

        if (++phase == cycle)
            phase = 0;
        if (++oldPhase == oldCycle)
            oldPhase = 0;
    }
    if (! tr.voice.m.sync)
        tr.voice.phase = phase;
    if (! tr.old.m.sync)
        tr.old.phase = oldPhase;
    flushDirty (t);
}

int Looper::record (int t, int n, bool mono)
{
    auto& h = store->tracks[(size_t) t];
    auto& take = h.current;
    auto& pool = store->pool;
    auto& tr = tracks[(size_t) t];
    const int64_t target = tr.ending ? closeTarget (t, take.length + 1) : 0;
    if (tr.ending && target == 0)
    {
        closeRecording (t); // no grid any more (the track went free, or the master went)
        return 0;
    }
    for (int i = 0; i < n; ++i)
    {
        const int64_t at = take.length;
        const auto ci = (size_t) (at >> LoopPool::chunkShift);
        if ((at & LoopPool::chunkMask) == 0 && (int) ci == take.count)
        {
            const int c = allocateChunk();
            if (c < 0)
            {
                ++poolFull;
                flushDirty (t);
                h.publishedLength.store (take.length, std::memory_order_release);
                closeRecording (t); // as if Rec had been pressed
                return i;
            }
            take.chunks[ci] = c;
            ++take.count;
            h.published[ci].store (c, std::memory_order_relaxed);
        }
        const int c = take.chunks[ci];
        const auto o = (size_t) (at & LoopPool::chunkMask);
        pool.samples (c, 0)[o] = inL[(size_t) i];
        pool.samples (c, 1)[o] = mono ? inL[(size_t) i] : inR[(size_t) i];
        ++take.length;
        markDirty (t, at);
        if (take.length == target)
        {
            closeRecording (t); // the bar it was waiting for
            return i + 1;
        }
    }
    flushDirty (t);
    h.publishedLength.store (take.length, std::memory_order_release);
    return n;
}

bool Looper::overdubAt (int t, int64_t phase, float l, float r)
{
    // Only forward at normal speed, so the phase is the source frame
    const auto& m = tracks[(size_t) t].voice.m;
    float a = 1.0f, b = 0.0f;
    const bool seam = m.fade > 0 && phase >= m.period - m.fade;
    if (seam)
        seamWeights (phase, m.period, m.fade, a, b);
    // Written through the weights it's read with, so an overdub across the seam reconstructs
    if (const float g = a * tailGain (phase, m.loop, m.period, m.fade); g > 0.0f)
        if (! addAt (t, m.start + phase, l * g, r * g))
            return false;
    if (seam && ! addAt (t, m.start + phase - m.period, l * b, r * b))
        return false;
    return true;
}

bool Looper::addAt (int t, int64_t i, float l, float r)
{
    if (i < 0 || i >= store->tracks[(size_t) t].current.length)
    {
        // Over the padding: it becomes part of the recording. What the seam blends in from
        // before the start lands only on recorded frames, as ever (an unpadded loop doesn't grow).
        if (i < tracks[(size_t) t].start || i >= tracks[(size_t) t].end)
            return true;
        if ((i = grow (t, i)) < 0)
            return false;
    }
    const int c = writableChunk (t, i >> LoopPool::chunkShift);
    if (c < 0)
        return false;
    auto& pool = store->pool;
    const auto o = (size_t) (i & LoopPool::chunkMask);
    pool.samples (c, 0)[o] += l;
    pool.samples (c, 1)[o] += r;
    markDirty (t, i);
    return true;
}

int Looper::writableChunk (int t, int64_t chunkIndex)
{
    auto& h = store->tracks[(size_t) t];
    auto& take = h.current;
    auto& pool = store->pool;
    const auto ci = (size_t) chunkIndex;
    if (const int c = take.chunks[ci]; c != looper::silentChunk && ! pool.shared (c))
        return c;
    // Copy on write: the old chunk stays with the undo level (or snapshot) that holds it. A
    // silent chunk gets real, silent memory.
    const int fresh = allocateChunk();
    if (fresh < 0)
        return -1;
    const int c = take.chunks[ci];
    if (c == looper::silentChunk)
    {
        for (int ch = 0; ch < LoopPool::channels; ++ch)
            std::fill_n (pool.samples (fresh, ch), LoopPool::chunkFrames, 0.0f);
        for (int bin = 0; bin < LoopPool::binsPerChunk; ++bin)
            pool.setBin (fresh, bin, 0.0f, 0.0f);
    }
    else if (pool.shared (c))
    {
        pool.copy (c, fresh);
        pool.release (c);
    }
    else
    {
        pool.release (fresh); // giving up history unshared it
        return c;
    }
    take.chunks[ci] = fresh;
    h.published[ci].store (fresh, std::memory_order_relaxed);
    return fresh;
}

int64_t Looper::grow (int t, int64_t i)
{
    auto& tr = tracks[(size_t) t];
    auto& h = store->tracks[(size_t) t];
    auto& take = h.current;
    const int64_t capacity = (int64_t) take.chunks.size();
    const int64_t room = tr.voice.m.fade; // the take's own edge fades land in silence
    flushDirty (t);
    if (i < 0)
    {
        const int64_t add = (room - i + LoopPool::chunkFrames - 1) >> LoopPool::chunkShift;
        if (take.count + add > capacity || ! bakeEdge (t, true))
            return -1;
        std::move_backward (take.chunks.begin(), take.chunks.begin() + take.count, take.chunks.begin() + take.count + add);
        std::fill_n (take.chunks.begin(), add, looper::silentChunk);
        take.count += (int) add;
        const int64_t shift = add << LoopPool::chunkShift;
        take.length += shift;
        take.front += shift;
        // The same frames, renumbered: nothing moves in time
        tr.start += shift;
        tr.end += shift;
        tr.anchor -= shift;
        for (Voice* v : { &tr.voice, &tr.old })
        {
            v->m.start += shift;
            v->m.anchor -= shift;
        }
        i += shift;
    }
    else
    {
        const int64_t count = ((i + room) >> LoopPool::chunkShift) + 1;
        if (count > capacity || ! bakeEdge (t, false))
            return -1;
        // The last chunk's frames past the recording may hold anything
        if (const int64_t tail = take.length & LoopPool::chunkMask; tail != 0 && take.chunks[(size_t) take.count - 1] != looper::silentChunk)
        {
            const int c = writableChunk (t, take.count - 1);
            if (c < 0)
                return -1;
            for (int ch = 0; ch < LoopPool::channels; ++ch)
                std::fill (store->pool.samples (c, ch) + tail, store->pool.samples (c, ch) + LoopPool::chunkFrames, 0.0f);
        }
        std::fill (take.chunks.begin() + take.count, take.chunks.begin() + count, looper::silentChunk);
        take.count = (int) count;
        take.length = count << LoopPool::chunkShift;
    }
    store->publish (t);
    return i;
}

bool Looper::bakeEdge (int t, bool front)
{
    const auto& take = store->tracks[(size_t) t].current;
    const int fade = tracks[(size_t) t].voice.m.fade;
    if (fade <= 0)
        return true;
    // As readRecorded fades it
    const int64_t from = front ? 0 : std::max<int64_t> (0, take.length - fade), to = front ? std::min<int64_t> (fade, take.length) : take.length;
    for (int64_t i = from; i < to; ++i)
    {
        const auto ci = i >> LoopPool::chunkShift;
        if (take.chunks[(size_t) ci] == looper::silentChunk)
            continue; // silence already
        const int c = writableChunk (t, ci);
        if (c < 0)
            return false;
        const float g = front ? ((float) i + 0.5f) / (float) fade : ((float) (take.length - i) - 0.5f) / (float) fade;
        const auto o = (size_t) (i & LoopPool::chunkMask);
        store->pool.samples (c, 0)[o] *= g;
        store->pool.samples (c, 1)[o] *= g;
        markDirty (t, i);
    }
    flushDirty (t);
    return true;
}

void Looper::markDirty (int t, int64_t i)
{
    auto& d = dirty[(size_t) t];
    if (d.to > d.from && i >= d.from - 1 && i <= d.to)
    {
        d.from = std::min (d.from, i);
        d.to = std::max (d.to, i + 1);
        return;
    }
    flushDirty (t);
    d.from = i;
    d.to = i + 1;
}

// The overview over what was written
void Looper::flushDirty (int t)
{
    auto& d = dirty[(size_t) t];
    if (d.to <= d.from)
        return;
    auto& h = store->tracks[(size_t) t];
    const auto& take = h.current;
    for (int64_t ci = d.from >> LoopPool::chunkShift; ci <= (d.to - 1) >> LoopPool::chunkShift; ++ci)
    {
        if (take.chunks[(size_t) ci] == looper::silentChunk)
            continue;
        const int64_t base = ci << LoopPool::chunkShift;
        const auto from = (int) (std::max (d.from, base) - base), to = (int) (std::min (d.to, base + LoopPool::chunkFrames) - base);
        const auto valid = (int) std::min (LoopPool::chunkFrames, take.length - base);
        store->pool.refreshBins (take.chunks[(size_t) ci], from, to, valid);
    }
    d.to = d.from;
    h.version.fetch_add (1, std::memory_order_release);
}

int Looper::allocateChunk()
{
    int c = store->pool.allocate();
    while (c < 0 && dropHistory())
        c = store->pool.allocate();
    return c;
}

// Gives up the least valuable history: redo levels first, then the oldest undo level of the
// track with the most
bool Looper::dropHistory()
{
    auto& pool = store->pool;
    auto dropOldest = [&pool] (auto& levels, int& count)
    {
        levels[0].clear (pool);
        std::rotate (levels.begin(), levels.begin() + 1, levels.begin() + count);
        --count;
    };
    for (auto& h : store->tracks)
        if (h.redoCount > 0)
        {
            dropOldest (h.redo, h.redoCount);
            return true;
        }
    LoopStore::Track* most = nullptr;
    for (auto& h : store->tracks)
        if (h.undoCount > 0 && (most == nullptr || h.undoCount > most->undoCount))
            most = &h;
    if (most == nullptr)
        return false;
    dropOldest (most->undo, most->undoCount);
    return true;
}

// ======================================================================================
// Transport (owner)

void Looper::doAction (Action a, int t)
{
    if (usable() == nullptr)
        return;
    if (t < 0 || t >= numTracks)
        t = selected;
    if (arm.track >= 0)
    {
        // Waiting to record: these give up (Rec: see rec)
        const bool onArmed = t == arm.track;
        if (a == Action::PlayStop || a == Action::ClearAll || (onArmed && (a == Action::Stop || a == Action::Clear || a == Action::Undo)))
        {
            cancelArm();
            if (a != Action::ClearAll)
                return;
        }
    }
    switch (a)
    {
        case Action::Rec: rec (t); break;
        case Action::PlayStop:
        {
            bool active = false;
            for (const auto& tr : tracks)
                active = active || tr.state == State::Recording || tr.state == State::Playing || tr.state == State::Overdubbing;
            if (active)
                stopAll();
            else
                playAll();
            break;
        }
        case Action::Undo: if (store->tracks[(size_t) t].undoCount > 0 || tracks[(size_t) t].state == State::Recording) schedule (t, Op::Undo); break;
        case Action::Redo: if (store->tracks[(size_t) t].redoCount > 0) schedule (t, Op::Redo); break;
        case Action::Clear: schedule (t, Op::Clear); break;
        case Action::ClearAll:
            for (int i = 0; i < numTracks; ++i)
                schedule (i, Op::Clear);
            break;
        case Action::Play: playTrack (t); break;
        case Action::Stop: stopTrack (t); break;
    }
}

void Looper::rec (int t)
{
    if (arm.track >= 0)
    {
        // Armed: Rec on another empty track moves the arm there; on the armed one it gives up
        if (arm.track != t && tracks[(size_t) t].state == State::Empty && tracks[(size_t) t].pending == Op::None)
        {
            arm.track = t;
            return;
        }
        const bool same = arm.track == t;
        cancelArm();
        if (same)
            return;
    }

    // One track records at a time: pressing Rec on another closes the one that was
    for (int o = 0; o < numTracks; ++o)
        if (o != t)
        {
            if (tracks[(size_t) o].state == State::Recording)
                closeRecording (o);
            else if (tracks[(size_t) o].writeTarget > 0.0f)
                punchOut (o, false);
        }

    auto& tr = tracks[(size_t) t];
    if (tr.pending != Op::None)
        return;
    switch (tr.state)
    {
        case State::Empty:
            if (isFirstRecording (t))
                beginRecording (t);
            else
                startRecording (t);
            break;
        case State::Recording: requestClose (t); break;
        case State::Playing: if (canOverdub (t)) punchIn (t); break;
        case State::Overdubbing: punchOut (t, false); break;
        case State::Stopped:
            if (canOverdub (t))
            {
                playTrack (t);
                punchIn (t);
            }
            break;
    }
}

bool Looper::isFirstRecording (int t) const
{
    for (int o = 0; o < numTracks; ++o)
        if (o != t && (store->tracks[(size_t) o].current.length > 0 || tracks[(size_t) o].state == State::Recording))
            return false;
    return true;
}

void Looper::beginRecording (int t)
{
    // The grid it counts in or waits on: the host's bar lines, or the free clock's
    const int bpb = controls.beatsPerBar;
    const double beat = tempo::beatFrames (transport.bpm, rate);
    const double pos = transport.host ? transport.ppq : beatPos;
    const double intoBar = pos - std::floor (pos / bpb) * bpb; // beats since its last bar line
    frozenBpm = transport.bpm;
    frozenBeatsPerBar = bpb;

    if (controls.countIn)
    {
        // One bar of clicks, from now or (following a host) its next bar line, then record
        const double toBar = ! transport.host || intoBar < 1.0e-6 ? 0.0 : bpb - intoBar;
        gridOrigin = elapsed + std::llround (toBar * beat);
        frozen = true;
        arm = { t, gridOrigin + std::llround (bpb * beat), true, controls.quantise };
        return;
    }
    if (controls.quantise)
    {
        frozen = true;
        const auto since = std::llround (intoBar * beat);
        const auto tolerance = std::llround (closeToleranceMs * 0.001 * rate);
        if (controls.metronome && ! controls.recOnly && since > tolerance)
        {
            // The clicks are heard: start on the next bar line
            gridOrigin = elapsed + std::llround ((bpb - intoBar) * beat);
            arm = { t, gridOrigin, false, true };
            return;
        }
        // Bar 1 is now, the take starting there. The host's grid can't move: the beats stay on it.
        gridOrigin = transport.host ? elapsed - since : elapsed;
    }
    startRecording (t, controls.quantise);
}

void Looper::cancelArm()
{
    if (arm.track < 0)
        return;
    arm = {};
    frozen = false; // the grid carries on from where it was
}

void Looper::startRecording (int t, bool quantised)
{
    const bool first = isFirstRecording (t);
    if (first)
    {
        // The first track: it will set the master length
        masterTrack = t;
        masterLength = 0;
        clock = origin = 0;
    }
    auto& tr = tracks[(size_t) t];
    tr.quantised = quantised && first;
    if (tr.quantised)
        quantisedBar = tempo::barFrames (frozenBpm, rate, frozenBeatsPerBar);
    clearHistory (t, false);
    store->tracks[(size_t) t].current.clear (store->pool);
    tr.start = tr.end = 0;
    pushUndo (t); // undoing the recording empties the track again
    tr.anchor = clock;
    tr.state = State::Recording;
    tr.gate = tr.gateTarget = 0.0f;
    tr.writeGain = tr.writeTarget = 0.0f;
    tr.blend = 1.0f;
}

int64_t Looper::closeTarget (int t, int64_t length) const
{
    const auto& tr = tracks[(size_t) t];
    if (tr.quantised && t == masterTrack && masterLength == 0)
    {
        // A quantised master: the next whole bar at the tempo it started at
        for (int64_t k = std::max<int64_t> (1, (int64_t) std::floor ((double) length / quantisedBar));; ++k)
            if (const auto at = std::llround ((double) k * quantisedBar); at >= length)
                return at;
    }
    // A sync track: the next multiple of the master length. Free tracks have no grid.
    if (t != masterTrack && tr.sync && masterLength > 0)
        return std::max<int64_t> (1, (length + masterLength - 1) / masterLength) * masterLength;
    return 0;
}

void Looper::requestClose (int t)
{
    auto& tr = tracks[(size_t) t];
    if (tr.ending)
        return; // already waiting for the bar
    const int64_t length = store->tracks[(size_t) t].current.length;
    if (tr.quantised && t == masterTrack && masterLength == 0 && length >= minRecording)
    {
        const auto tolerance = std::min<int64_t> (std::llround (closeToleranceMs * 0.001 * rate), (int64_t) (quantisedBar / 4.0));
        const int64_t at = tempo::quantisedClose (length, quantisedBar, tolerance);
        if (at > length)
        {
            tr.ending = true;
            return;
        }
        truncateRecording (t, at); // on a bar, or just past one: close at it
    }
    else if (const int64_t bar = t != masterTrack && tr.sync ? masterLength : 0; bar > 0 && length >= minRecording)
    {
        const int64_t over = length % bar;
        const int64_t tolerance = std::min ((int64_t) std::llround (closeToleranceMs * 0.001 * rate), bar / 4);
        if (over == 0 || (length >= bar && over <= tolerance))
            truncateRecording (t, length - over); // just past a bar: close at it
        else
        {
            tr.ending = true;
            return;
        }
    }
    closeRecording (t);
}

// Drops the frames of a recording past `length`
void Looper::truncateRecording (int t, int64_t length)
{
    auto& h = store->tracks[(size_t) t];
    auto& take = h.current;
    flushDirty (t);
    const auto keep = (int) ((length + LoopPool::chunkFrames - 1) >> LoopPool::chunkShift);
    for (int i = keep; i < take.count; ++i)
        if (take.chunks[(size_t) i] != looper::silentChunk)
            store->pool.release (take.chunks[(size_t) i]);
    take.count = std::min (take.count, keep);
    take.length = std::min (take.length, length);
    if (const int64_t base = (int64_t) (keep - 1) << LoopPool::chunkShift; keep > 0 && take.chunks[(size_t) keep - 1] != looper::silentChunk)
        store->pool.refreshBins (take.chunks[(size_t) keep - 1], (int) (take.length - base), (int) LoopPool::chunkFrames, (int) (take.length - base));
    h.publishedLength.store (take.length, std::memory_order_release);
}

void Looper::closeRecording (int t)
{
    auto& tr = tracks[(size_t) t];
    auto& h = store->tracks[(size_t) t];
    const bool first = t == masterTrack && masterLength == 0, quantised = first && tr.quantised;
    tr.ending = false;
    tr.quantised = false;
    if (first)
        frozen = false; // the master's grid takes over
    flushDirty (t);
    if (h.current.length < minRecording)
    {
        h.current.clear (store->pool);
        if (h.undoCount > 0)
            h.undo[(size_t) --h.undoCount].clear (store->pool);
        tr.state = State::Empty;
        trackEmptied (t);
        store->publish (t);
        return;
    }
    tr.start = 0;
    tr.end = h.current.length;
    if (quantised)
        tr.end = tempo::wholeBars (tr.end, quantisedBar); // trimmed, or padded with silence
    if (first)
    {
        masterLength = tr.end - tr.start;
        origin = tr.anchor;
    }
    tr.voice.m = mappingFor (t);
    tr.voice.phase = 0; // free tracks carry on from their start
    tr.blend = 1.0f;
    tr.state = State::Playing;
    tr.gateTarget = 1.0f;
    store->publish (t);
}

void Looper::punchIn (int t)
{
    clearHistory (t, false);
    pushUndo (t);
    tracks[(size_t) t].state = State::Overdubbing;
    tracks[(size_t) t].writeTarget = 1.0f;
}

void Looper::punchOut (int t, bool now)
{
    auto& tr = tracks[(size_t) t];
    tr.writeTarget = 0.0f;
    if (now)
        tr.writeGain = 0.0f;
    if (tr.state == State::Overdubbing)
        tr.state = State::Playing;
}

void Looper::playAll()
{
    if (! running())
        clock = origin; // from the top of the master loop
    for (int t = 0; t < numTracks; ++t)
    {
        auto& tr = tracks[(size_t) t];
        if (tr.state != State::Stopped || tr.pending != Op::None)
            continue;
        tr.voice.phase = 0;
        tr.blend = 1.0f;
        tr.state = State::Playing;
        tr.gateTarget = 1.0f;
    }
}

void Looper::stopAll()
{
    for (int t = 0; t < numTracks; ++t)
        stopTrack (t);
}

void Looper::playTrack (int t)
{
    auto& tr = tracks[(size_t) t];
    if (tr.state != State::Stopped || tr.pending != Op::None)
        return;
    if (! running())
        clock = origin;
    tr.voice.phase = 0;
    tr.blend = 1.0f;
    tr.state = State::Playing;
    tr.gateTarget = 1.0f;
}

void Looper::stopTrack (int t)
{
    auto& tr = tracks[(size_t) t];
    if (tr.state == State::Recording)
        closeRecording (t);
    punchOut (t, false);
    if (tr.state == State::Playing)
    {
        tr.state = State::Stopped;
        tr.gateTarget = 0.0f;
    }
    tr.resume = false;
}

// Undo, redo, clear and installs swap the take at silence
void Looper::schedule (int t, Op op)
{
    auto& tr = tracks[(size_t) t];
    if (tr.state == State::Recording)
        closeRecording (t);
    punchOut (t, true);
    if (tr.pending != Op::None)
        applyOp (t);
    tr.pending = op;
    tr.resume = tr.state == State::Playing || tr.state == State::Overdubbing;
    tr.gateTarget = 0.0f;
    if (tr.gate <= 0.0f)
        applyOp (t);
}

void Looper::applyOp (int t)
{
    auto& tr = tracks[(size_t) t];
    auto& h = store->tracks[(size_t) t];
    auto& pool = store->pool;
    const Op op = tr.pending;
    tr.pending = Op::None;
    const int64_t before = h.current.length;
    h.current.start = tr.start;
    h.current.end = tr.end;
    h.current.anchor = tr.anchor;

    bool adopt = false;
    switch (op)
    {
        case Op::None: return;
        case Op::Undo:
        case Op::Redo:
        {
            auto& from = op == Op::Undo ? h.undo : h.redo;
            auto& to = op == Op::Undo ? h.redo : h.undo;
            int& fromCount = op == Op::Undo ? h.undoCount : h.redoCount;
            int& toCount = op == Op::Undo ? h.redoCount : h.undoCount;
            if (fromCount == 0)
                break;
            if (toCount == looper::maxUndo)
            {
                to[0].clear (pool);
                std::rotate (to.begin(), to.begin() + 1, to.end());
                --toCount;
            }
            std::swap (to[(size_t) toCount++], h.current);
            std::swap (h.current, from[(size_t) --fromCount]);
            adopt = before == 0 || h.current.length == 0 || h.current.length != before;
            break;
        }
        case Op::Clear:
            clearHistory (t, true);
            h.current.clear (pool);
            break;
        case Op::Install:
            clearHistory (t, true);
            h.current.clear (pool);
            std::swap (h.current, h.incoming);
            adopt = true;
            break;
    }
    flushDirty (t);
    takeChanged (t, adopt);
    store->publish (t);
}

void Looper::finishPending()
{
    if (store == nullptr)
        return;
    for (int t = 0; t < numTracks; ++t)
    {
        auto& tr = tracks[(size_t) t];
        tr.gate = tr.gateTarget;
        tr.blend = 1.0f;
        tr.writeGain = tr.writeTarget;
        if (tr.pending != Op::None)
            applyOp (t);
        if (tr.remapPending)
        {
            tr.remapPending = false;
            remap (t, false);
        }
        tr.gate = tr.gateTarget;
    }
}

void Looper::pushUndo (int t)
{
    auto& h = store->tracks[(size_t) t];
    auto& tr = tracks[(size_t) t];
    if (h.undoCount == looper::maxUndo)
    {
        h.undo[0].clear (store->pool);
        std::rotate (h.undo.begin(), h.undo.begin() + 1, h.undo.end());
        --h.undoCount;
    }
    h.current.start = tr.start;
    h.current.end = tr.end;
    h.current.anchor = tr.anchor;
    h.undo[(size_t) h.undoCount++].assign (h.current, store->pool);
}

void Looper::clearHistory (int t, bool undoToo)
{
    auto& h = store->tracks[(size_t) t];
    for (int i = 0; i < h.redoCount; ++i)
        h.redo[(size_t) i].clear (store->pool);
    h.redoCount = 0;
    if (undoToo)
    {
        for (int i = 0; i < h.undoCount; ++i)
            h.undo[(size_t) i].clear (store->pool);
        h.undoCount = 0;
    }
}

void Looper::takeChanged (int t, bool adopt)
{
    auto& tr = tracks[(size_t) t];
    const auto& take = store->tracks[(size_t) t].current;
    if (take.length == 0)
    {
        tr.state = State::Empty;
        tr.gate = tr.gateTarget = 0.0f;
        tr.resume = false;
        trackEmptied (t);
        return;
    }
    if (adopt)
    {
        tr.start = take.start;
        tr.end = take.end;
        tr.anchor = take.anchor;
    }
    if (masterLength == 0)
    {
        masterTrack = t;
        masterLength = std::max<int64_t> (1, tr.end - tr.start);
        origin = tr.anchor + tr.start;
    }
    tr.state = tr.resume ? State::Playing : State::Stopped;
    tr.gateTarget = tr.resume ? 1.0f : 0.0f;
    tr.resume = false;
    tr.voice.m = mappingFor (t);
    tr.voice.phase = floorMod (tr.voice.phase, tr.voice.m.cycle());
    tr.blend = 1.0f;
}

void Looper::trackEmptied (int t)
{
    if (masterTrack == t)
        masterTrack = -1; // its length stays the reference for the others
    for (int o = 0; o < numTracks; ++o)
        if (store->tracks[(size_t) o].current.length > 0 || tracks[(size_t) o].state == State::Recording)
            return;
    masterTrack = -1;
    masterLength = 0;
    origin = clock = 0;
}

void Looper::clearTrack (int t)
{
    auto& tr = tracks[(size_t) t];
    clearHistory (t, true);
    store->tracks[(size_t) t].current.clear (store->pool);
    store->tracks[(size_t) t].incoming.clear (store->pool);
    tr.state = State::Empty;
    tr.pending = Op::None;
    tr.ending = false;
    tr.quantised = false;
    tr.gate = tr.gateTarget = tr.writeGain = tr.writeTarget = 0.0f;
    tr.start = tr.end = tr.anchor = 0;
    tr.blend = 1.0f;
    tr.remapPending = false;
    dirty[(size_t) t] = {};
    store->publish (t);
}

// ======================================================================================
// Edits and mappings (owner)

Looper::Mapping Looper::mappingFor (int t) const
{
    const auto& tr = tracks[(size_t) t];
    Mapping m;
    m.start = tr.start;
    m.loop = std::max<int64_t> (1, tr.end - tr.start);
    m.anchor = tr.anchor;
    m.reverse = tr.reverse;
    m.half = tr.half;
    if (t == masterTrack)
    {
        m.period = m.loop;
        m.sync = true;
    }
    else if (tr.sync && masterLength > 0)
    {
        const auto k = std::max<int64_t> (1, (int64_t) std::llround ((double) m.loop / (double) masterLength));
        m.period = k * masterLength;
        m.sync = true;
    }
    else
    {
        m.period = m.loop;
    }
    const auto fade = (int64_t) std::llround (tr.crossfadeMs * 0.001 * rate);
    m.fade = (int) std::clamp<int64_t> (fade, 0, std::min (m.period, m.loop) / 2);
    return m;
}

void Looper::remap (int t, bool crossfade)
{
    auto& tr = tracks[(size_t) t];
    if (store == nullptr || tr.state == State::Empty || tr.state == State::Recording)
        return;
    const Mapping next = mappingFor (t);
    if (next == tr.voice.m)
        return;
    const bool audible = tr.gate > 0.0f || tr.gateTarget > 0.0f;
    if (crossfade && audible && tr.blend < 1.0f)
    {
        tr.remapPending = true; // after the crossfade under way
        return;
    }
    if (crossfade && audible && tr.writeGain > 0.0f && (next.reverse || next.half))
    {
        punchOut (t, false);
        tr.remapPending = true;
        return;
    }
    // A free track keeps the frame it was playing; a sync one follows the clock
    const int64_t source = tr.voice.m.start + sourcePhase (tr.voice.m, phaseAt (tr.voice, clock));
    if (crossfade && audible)
    {
        tr.old = tr.voice;
        tr.old.phase = phaseAt (tr.voice, clock);
        tr.old.m.sync = false; // it carries on from where it was
        tr.blend = 0.0f;
    }
    tr.voice.m = next;
    tr.voice.phase = phaseForSource (next, source - next.start);
}

void Looper::remapAll (bool crossfade)
{
    for (int t = 0; t < numTracks; ++t)
        remap (t, crossfade);
}

void Looper::setEdits (int t, int64_t start, int64_t end, int64_t front)
{
    auto& tr = tracks[(size_t) t];
    const auto& take = store->tracks[(size_t) t].current;
    const auto length = take.length;
    if (length == 0 || tr.state == State::Recording)
        return;
    if (front >= 0)
    {
        // Made before an overdub grew the recording at the front: renumbered as it was
        start += take.front - front;
        end += take.front - front;
    }
    const auto pad = (int64_t) store->pool.numChunks() * LoopPool::chunkFrames; // padding up to the pool's length
    tr.start = std::clamp (start, -pad, length + pad - minLoop);
    tr.end = std::clamp (end, tr.start + minLoop, length + pad);
    if (t == masterTrack)
    {
        masterLength = tr.end - tr.start;
        origin = tr.anchor + tr.start;
        remapAll (true);
    }
    else
    {
        remap (t, true);
    }
}

void Looper::jumpFrom (Track& tr)
{
    if (tr.gate <= 0.0f && tr.gateTarget <= 0.0f)
        return; // silent: nothing to crossfade from
    // A crossfade already under way is cut short: the new one starts from the voice it was going to
    tr.old = tr.voice;
    tr.old.phase = phaseAt (tr.voice, clock);
    tr.old.m.sync = false; // it carries on from where it was
    tr.blend = 0.0f;
}

void Looper::doSeek (int t, int64_t offset)
{
    auto& tr = tracks[(size_t) t];
    if (tr.state == State::Empty || tr.state == State::Recording || tr.pending != Op::None)
        return;
    const bool sync = tr.voice.m.sync;
    for (int o = 0; o < numTracks; ++o)
    {
        const auto& other = tracks[(size_t) o];
        const bool writing = other.state == State::Recording || other.state == State::Overdubbing || other.writeGain > 0.0f;
        if (writing && (sync || o == t))
            return; // moving the clock (or this track) under a recording would misplace it
    }

    const auto& m = tr.voice.m;
    const int64_t phase = phaseForSource (m, std::clamp<int64_t> (offset, 0, std::max<int64_t> (0, m.loop - 1)));
    if (tr.state == State::Stopped)
    {
        if (sync && ! running())
            clock = origin;
        tr.state = State::Playing;
        tr.gateTarget = 1.0f;
        tr.resume = false;
    }

    if (! sync)
    {
        jumpFrom (tr);
        tr.voice.phase = phase;
        return;
    }
    // Move the shared clock by the least that puts this track at `phase`: tracks spanning several
    // master loops stay on the same pass where they can
    const int64_t cycle = m.cycle();
    int64_t delta = floorMod (phase - phaseAt (tr.voice, clock), cycle);
    if (delta > cycle / 2)
        delta -= cycle;
    if (delta == 0)
        return;
    for (int o = 0; o < numTracks; ++o)
    {
        auto& other = tracks[(size_t) o];
        if (other.state != State::Empty && other.voice.m.sync)
            jumpFrom (other);
    }
    clock += delta;
}

// ======================================================================================
// Commands

void Looper::service()
{
    uint64_t d = done.load (std::memory_order_relaxed);
    const uint64_t w = written.load (std::memory_order_acquire);
    while (d < w)
    {
        apply (queue[d % queueSize]);
        done.store (++d, std::memory_order_release);
    }
}

void Looper::apply (const Command& c)
{
    const int t = c.track;
    switch (c.type)
    {
        case Command::Type::Swap:
            store = static_cast<LoopStore*> (c.payload);
            for (auto& tr : tracks)
            {
                const auto keep = tr;
                tr = Track {};
                tr.reverse = keep.reverse;
                tr.half = keep.half;
                tr.sync = keep.sync;
                tr.mute = keep.mute;
                tr.level = keep.level;
                tr.crossfadeMs = keep.crossfadeMs;
            }
            masterTrack = -1;
            masterLength = clock = origin = 0;
            dirty = {};
            cancelArm();
            frozen = false;
            return;
        case Command::Type::Action:
            if (on || c.action == (int) Action::ClearAll || c.action == (int) Action::Clear || c.action == (int) Action::Undo
                || c.action == (int) Action::Redo)
                doAction ((Action) c.action, t);
            return;
        default: break;
    }

    if (usable() == nullptr)
    {
        if (c.type == Command::Type::Claim)
            static_cast<Claim*> (c.payload)->got = 0;
        return;
    }
    switch (c.type)
    {
        case Command::Type::Edits: setEdits (t, c.a, c.b, c.c); break;
        case Command::Type::Seek: if (on) doSeek (t, c.a); break;
        case Command::Type::Crossfade:
            tracks[(size_t) t].crossfadeMs = std::clamp (c.f, 0.0f, maxCrossfadeMs);
            remap (t, true);
            break;
        case Command::Type::Claim:
        {
            auto* claim = static_cast<Claim*> (c.payload);
            claim->got = 0;
            for (int64_t i = 0; i < c.a; ++i)
            {
                const int chunk = store->pool.allocate();
                if (chunk < 0)
                    break;
                claim->ids[(size_t) claim->got++] = chunk;
            }
            break;
        }
        case Command::Type::Unclaim:
        {
            const auto* claim = static_cast<const Claim*> (c.payload);
            for (int i = 0; i < claim->got; ++i)
                store->pool.release (claim->ids[(size_t) i]);
            break;
        }
        case Command::Type::Install:
        {
            const auto* in = static_cast<const Install*> (c.payload);
            auto& incoming = store->tracks[(size_t) in->track].incoming;
            incoming.clear (store->pool);
            std::copy_n (in->ids.begin(), in->count, incoming.chunks.begin()); // their references move over
            incoming.count = in->count;
            incoming.length = in->length;
            incoming.start = in->start;
            incoming.end = in->end;
            incoming.anchor = in->explicitAnchor ? in->anchor : origin - in->start; // on the master's loop start
            tracks[(size_t) in->track].crossfadeMs = std::clamp (in->crossfadeMs, 0.0f, maxCrossfadeMs);
            schedule (in->track, Op::Install);
            break;
        }
        case Command::Type::Pin:
        {
            auto* p = static_cast<Pinned*> (c.payload);
            for (int i = 0; i < numTracks; ++i)
            {
                if (! p->wanted[(size_t) i])
                    continue;
                flushDirty (i);
                p->takes[(size_t) i].assign (store->tracks[(size_t) i].current, store->pool);
                p->maps[(size_t) i] = tracks[(size_t) i].voice.m;
                p->maps[(size_t) i].start = tracks[(size_t) i].start;
                p->maps[(size_t) i].loop = std::max<int64_t> (1, tracks[(size_t) i].end - tracks[(size_t) i].start);
                p->maps[(size_t) i].anchor = tracks[(size_t) i].anchor;
                p->crossfadeMs[(size_t) i] = tracks[(size_t) i].crossfadeMs;
            }
            p->masterTrack = masterTrack;
            p->masterLength = masterLength;
            p->origin = origin;
            break;
        }
        case Command::Type::Unpin:
        {
            auto* p = static_cast<Pinned*> (c.payload);
            for (auto& take : p->takes)
                take.clear (store->pool);
            break;
        }
        case Command::Type::SetMaster:
            masterTrack = t;
            masterLength = c.a;
            origin = clock = 0;
            if (t >= 0 && store->tracks[(size_t) t].current.length > 0)
            {
                masterLength = std::max<int64_t> (1, tracks[(size_t) t].end - tracks[(size_t) t].start);
                origin = tracks[(size_t) t].anchor + tracks[(size_t) t].start;
            }
            remapAll (false);
            break;
        case Command::Type::ClearAll:
            for (int i = 0; i < numTracks; ++i)
                clearTrack (i);
            masterTrack = -1;
            masterLength = clock = origin = 0;
            cancelArm();
            frozen = false;
            break;
        case Command::Type::Action:
        case Command::Type::Swap: break;
    }
}

void Looper::publishStatus()
{
    const auto* s = usable();
    for (int t = 0; t < numTracks; ++t)
    {
        const auto& tr = tracks[(size_t) t];
        auto& p = published[(size_t) t];
        const int64_t length = s == nullptr ? 0 : s->tracks[(size_t) t].current.length;
        const auto& m = tr.voice.m;
        const bool hasLoop = tr.state != State::Empty && tr.state != State::Recording;
        const int64_t phase = hasLoop ? phaseAt (tr.voice, clock) : 0;
        p.state.store ((int) (s == nullptr ? State::Empty : tr.state), std::memory_order_relaxed);
        p.length.store (length, std::memory_order_relaxed);
        p.start.store (tr.start, std::memory_order_relaxed);
        p.end.store (tr.state == State::Recording ? length : tr.end, std::memory_order_relaxed);
        p.loop.store (hasLoop ? m.loop : length, std::memory_order_relaxed);
        p.period.store (hasLoop ? m.cycle() : 0, std::memory_order_relaxed);
        p.multiple.store (hasLoop && m.sync && masterLength > 0 ? (int) (m.period / masterLength) : 0, std::memory_order_relaxed);
        p.position.store (hasLoop ? (double) phase / (double) m.cycle() : 0.0, std::memory_order_relaxed);
        p.source.store (hasLoop ? m.start + sourcePhase (m, phase) : length, std::memory_order_relaxed);
        p.crossfade.store (tr.crossfadeMs, std::memory_order_relaxed);
        p.flags.store ((m.sync ? 1 : 0) | (tr.reverse ? 2 : 0) | (tr.half ? 4 : 0) | (tr.mute ? 8 : 0) | (t == masterTrack ? 16 : 0)
                           | (tr.ending ? 32 : 0) | (t == arm.track ? 64 : 0),
                       std::memory_order_relaxed);
        p.front.store (s == nullptr ? 0 : s->tracks[(size_t) t].current.front, std::memory_order_relaxed);
        p.undo.store (s == nullptr ? 0 : s->tracks[(size_t) t].undoCount, std::memory_order_relaxed);
        p.redo.store (s == nullptr ? 0 : s->tracks[(size_t) t].redoCount, std::memory_order_relaxed);
    }
    pubMaster.store (masterTrack, std::memory_order_relaxed);
    pubMasterLength.store (masterLength, std::memory_order_relaxed);
    pubSelected.store (selected, std::memory_order_relaxed);
    pubOn.store (on, std::memory_order_relaxed);
    pubRunning.store (running(), std::memory_order_relaxed);

    const int bpb = grid() == Grid::Armed ? frozenBeatsPerBar : controls.beatsPerBar;
    const double whole = std::floor (beatPos);
    pubBeatsPerBar.store (bpb, std::memory_order_relaxed);
    pubBeat.store ((int) floorMod ((int64_t) whole, bpb), std::memory_order_relaxed);
    pubBeatPhase.store (beatPos - whole, std::memory_order_relaxed);
    pubMasterBars.store (masterLength > 0 ? masterBeats() / controls.beatsPerBar : 0, std::memory_order_relaxed);
    pubHostGrid.store (grid() == Grid::Host, std::memory_order_relaxed);
    pubArmed.store (arm.track, std::memory_order_relaxed);
    const double beat = tempo::beatFrames (frozenBpm, rate);
    pubCountIn.store (arm.track >= 0 && arm.countIn ? (int) std::ceil ((double) (arm.at - elapsed) / beat - 1.0e-9) : 0,
                      std::memory_order_relaxed);
}

// ======================================================================================
// The beat grid (owner)

Looper::Grid Looper::grid() const
{
    if (masterLength > 0 && running())
        return Grid::Master;
    if (arm.track >= 0 || frozen)
        return Grid::Armed;
    return transport.host ? Grid::Host : Grid::Free;
}

int Looper::masterBeats() const
{
    const double bar = tempo::barFrames (transport.bpm, rate, controls.beatsPerBar);
    return (int) std::clamp<int64_t> (std::llround ((double) masterLength / bar), 1, 4096) * controls.beatsPerBar;
}

void Looper::click (int at, int64_t frame, bool accent, bool audible)
{
    // Where grids hand over, one beat may come up twice a frame or two apart: once is enough
    // (unless the second is an accent and the first wasn't)
    const auto guard = std::llround (closeToleranceMs * 0.001 * rate);
    if (frame - lastClick < guard && ! (accent && ! lastAccent))
        return;
    lastClick = frame;
    lastAccent = accent;
    if (audible && numClicks < maxClicks)
        clickList[(size_t) numClicks++] = { at + (int) (frame - elapsed), accent };
}

void Looper::clickGrid (int at, int n)
{
    // A count-in always clicks (even with the metronome off); otherwise the metronome's switches
    // decide. (Its level still applies: at -48 dB nothing is heard.)
    const bool countingIn = arm.track >= 0 && arm.countIn;
    bool writing = countingIn;
    for (const auto& tr : tracks)
        writing = writing || tr.state == State::Recording || tr.state == State::Overdubbing;
    const bool audible = countingIn || (controls.metronome && (! controls.recOnly || writing));

    // Whole beats b in (pos - step, pos + (n - 1) * step], each on the first frame at or past it
    auto steady = [&] (double pos, double step, int bpb)
    {
        for (auto b = (int64_t) std::floor (pos - step) + 1; (double) b <= pos + (n - 1) * step; ++b)
        {
            const auto i = std::clamp<int64_t> ((int64_t) std::ceil (((double) b - pos) / step - 1.0e-6), 0, n - 1);
            click (at, elapsed + i, floorMod (b, bpb) == 0, audible);
        }
    };

    switch (grid())
    {
        case Grid::Master:
        {
            // Beat k of the loop's B at phase round (k * M / B)
            const int64_t m = masterLength, beats = masterBeats();
            auto beatAt = [&] (int64_t k) { return std::llround ((double) k * (double) m / (double) beats); };
            int64_t phase = floorMod (clock - origin, m);
            for (int i = 0; i < n;)
            {
                const int64_t len = std::min<int64_t> (n - i, m - phase);
                auto k = std::min<int64_t> (beats, (int64_t) std::ceil ((double) phase * (double) beats / (double) m));
                while (k > 0 && beatAt (k - 1) >= phase)
                    --k;
                while (beatAt (k) < phase)
                    ++k;
                for (; k < beats && beatAt (k) < phase + len; ++k)
                    click (at, elapsed + i + beatAt (k) - phase, k % controls.beatsPerBar == 0, audible);
                i += (int) len;
                phase = phase + len == m ? 0 : phase + len;
            }
            beatPos = (double) phase * (double) beats / (double) m;
            break;
        }
        case Grid::Armed:
        {
            // Beat k (negative before bar 1) on frame gridOrigin + round (k * beat)
            const double beat = tempo::beatFrames (frozenBpm, rate);
            auto beatAt = [&] (int64_t k) { return gridOrigin + std::llround ((double) k * beat); };
            auto k = (int64_t) std::ceil ((double) (elapsed - gridOrigin) / beat);
            while (beatAt (k - 1) >= elapsed)
                --k;
            while (beatAt (k) < elapsed)
                ++k;
            for (; beatAt (k) < elapsed + n; ++k)
                click (at, beatAt (k), floorMod (k, frozenBeatsPerBar) == 0, audible);
            beatPos = (double) (elapsed + n - gridOrigin) / beat;
            break;
        }
        case Grid::Host:
        {
            const double step = transport.bpm / (60.0 * rate);
            steady (transport.ppq, step, controls.beatsPerBar);
            beatPos = transport.ppq + n * step;
            break;
        }
        case Grid::Free:
        {
            const double step = transport.bpm / (60.0 * rate);
            steady (beatPos, step, controls.beatsPerBar);
            beatPos += n * step;
            break;
        }
    }
}

// ======================================================================================
// Message thread

uint64_t Looper::post (const Command& c)
{
    const std::lock_guard lock (postLock);
    const uint64_t w = written.load (std::memory_order_relaxed);
    while (w - done.load (std::memory_order_acquire) >= queueSize)
        if (! serviceIfIdle())
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    queue[w % queueSize] = c;
    written.store (w + 1, std::memory_order_release);
    return w + 1;
}

bool Looper::audioIdle() const
{
    const int64_t last = lastBlock.load (std::memory_order_relaxed);
    return last == 0 || nowNanos() - last >= idleTimeout.load (std::memory_order_relaxed);
}

bool Looper::serviceIfIdle()
{
    if (! audioIdle())
        return false;
    int expected = 0;
    if (! owner.compare_exchange_strong (expected, 2, std::memory_order_acquire))
        return false;
    service();
    finishPending(); // no audio to dip under
    publishStatus();
    owner.store (0, std::memory_order_release);
    return true;
}

void Looper::flush (uint64_t sequence)
{
    const auto started = std::chrono::steady_clock::now();
    while (done.load (std::memory_order_acquire) < sequence)
    {
        if (serviceIfIdle())
            continue;
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds (2))
        {
            // The audio thread claims to run but doesn't serve the queue: take the engine between blocks
            for (int expected = 0; ! owner.compare_exchange_weak (expected, 2, std::memory_order_acquire); expected = 0)
                std::this_thread::yield();
            service();
            owner.store (0, std::memory_order_release);
            continue;
        }
        std::this_thread::sleep_for (std::chrono::microseconds (200));
    }
}

void Looper::createPool (double poolRate, double seconds)
{
    const std::lock_guard lock (api);
    auto fresh = std::make_unique<LoopStore> (poolRate, seconds);
    flush (post ({ Command::Type::Swap, -1, 0, 0, 0, 0.0f, fresh.get() }));
    ownedStore = std::move (fresh); // the old one, if any, is no longer in use
}

void Looper::releasePool()
{
    const std::lock_guard lock (api);
    flush (post ({ Command::Type::Swap, -1, 0, 0, 0, 0.0f, nullptr }));
    ownedStore.reset();
}

bool Looper::hasPool() const
{
    const std::lock_guard lock (api);
    return ownedStore != nullptr;
}

double Looper::poolRate() const
{
    const std::lock_guard lock (api);
    return ownedStore == nullptr ? 0.0 : ownedStore->pool.sampleRate();
}

void Looper::press (Action a, int track)
{
    post ({ Command::Type::Action, track, (int) a });
}

void Looper::setTrackEdits (int track, int64_t start, int64_t end, int64_t front)
{
    if (track >= 0 && track < numTracks)
        post ({ Command::Type::Edits, track, 0, start, end, 0.0f, nullptr, front });
}

void Looper::seek (int track, int64_t offset)
{
    if (track >= 0 && track < numTracks)
        post ({ Command::Type::Seek, track, 0, offset });
}

void Looper::setTrackCrossfade (int track, float ms)
{
    if (track >= 0 && track < numTracks)
        post ({ Command::Type::Crossfade, track, 0, 0, 0, ms });
}

Looper::TrackStatus Looper::trackStatus (int track) const
{
    TrackStatus s;
    if (track < 0 || track >= numTracks)
        return s;
    const auto& p = published[(size_t) track];
    s.state = (State) p.state.load (std::memory_order_relaxed);
    s.length = p.length.load (std::memory_order_relaxed);
    s.start = p.start.load (std::memory_order_relaxed);
    s.end = p.end.load (std::memory_order_relaxed);
    s.crossfadeMs = p.crossfade.load (std::memory_order_relaxed);
    s.loopLength = p.loop.load (std::memory_order_relaxed);
    s.period = p.period.load (std::memory_order_relaxed);
    s.multiple = p.multiple.load (std::memory_order_relaxed);
    s.position = p.position.load (std::memory_order_relaxed);
    s.sourcePosition = p.source.load (std::memory_order_relaxed);
    const int f = p.flags.load (std::memory_order_relaxed);
    s.sync = (f & 1) != 0;
    s.reverse = (f & 2) != 0;
    s.half = (f & 4) != 0;
    s.muted = (f & 8) != 0;
    s.master = (f & 16) != 0;
    s.ending = (f & 32) != 0;
    s.armed = (f & 64) != 0;
    s.front = p.front.load (std::memory_order_relaxed);
    s.canOverdub = ! s.reverse && ! s.half;
    s.undoLevels = p.undo.load (std::memory_order_relaxed);
    s.redoLevels = p.redo.load (std::memory_order_relaxed);
    return s;
}

Looper::Status Looper::status() const
{
    Status s;
    {
        const std::lock_guard lock (api);
        if (ownedStore != nullptr)
        {
            const auto& pool = ownedStore->pool;
            s.poolReady = true;
            s.sampleRate = pool.sampleRate();
            s.poolUsedSeconds = (double) pool.usedChunks() * (double) LoopPool::chunkFrames / pool.sampleRate();
            s.poolTotalSeconds = (double) pool.numChunks() * (double) LoopPool::chunkFrames / pool.sampleRate();
        }
    }
    s.on = pubOn.load (std::memory_order_relaxed);
    s.tempo = tempo.load (std::memory_order_relaxed);
    s.selected = pubSelected.load (std::memory_order_relaxed);
    s.masterTrack = pubMaster.load (std::memory_order_relaxed);
    s.masterLength = pubMasterLength.load (std::memory_order_relaxed);
    s.running = pubRunning.load (std::memory_order_relaxed);
    s.poolFullCount = poolFull.load (std::memory_order_relaxed);
    s.beatsPerBar = pubBeatsPerBar.load (std::memory_order_relaxed);
    s.beat = pubBeat.load (std::memory_order_relaxed);
    s.beatPhase = pubBeatPhase.load (std::memory_order_relaxed);
    s.masterBars = pubMasterBars.load (std::memory_order_relaxed);
    s.hostGrid = pubHostGrid.load (std::memory_order_relaxed);
    s.armedTrack = pubArmed.load (std::memory_order_relaxed);
    s.countInBeatsLeft = pubCountIn.load (std::memory_order_relaxed);
    return s;
}

int Looper::overviewBins (int track) const
{
    const std::lock_guard lock (api);
    if (ownedStore == nullptr || track < 0 || track >= numTracks)
        return 0;
    const auto length = ownedStore->tracks[(size_t) track].publishedLength.load (std::memory_order_acquire);
    return (int) ((length + overviewBinFrames - 1) / overviewBinFrames);
}

int Looper::readOverview (int track, int firstBin, int numBins, float* mins, float* maxs) const
{
    const std::lock_guard lock (api);
    if (ownedStore == nullptr || track < 0 || track >= numTracks)
        return 0;
    const auto& h = ownedStore->tracks[(size_t) track];
    const auto length = h.publishedLength.load (std::memory_order_acquire);
    const auto bins = (int) ((length + overviewBinFrames - 1) / overviewBinFrames);
    int got = 0;
    for (int b = std::max (0, firstBin); b < bins && got < numBins; ++b, ++got)
    {
        const int c = h.published[(size_t) (b / LoopPool::binsPerChunk)].load (std::memory_order_relaxed);
        if (c == looper::silentChunk)
            mins[got] = maxs[got] = 0.0f;
        else
            ownedStore->pool.readBin (c, b % LoopPool::binsPerChunk, mins[got], maxs[got]);
    }
    return got;
}

uint32_t Looper::overviewVersion (int track) const
{
    const std::lock_guard lock (api);
    if (ownedStore == nullptr || track < 0 || track >= numTracks)
        return 0;
    return ownedStore->tracks[(size_t) track].version.load (std::memory_order_acquire);
}

std::unique_ptr<Looper::Pinned> Looper::pin (uint32_t trackMask)
{
    auto p = std::make_unique<Pinned>();
    for (int t = 0; t < numTracks; ++t)
        if ((trackMask >> t) & 1u)
        {
            p->wanted[(size_t) t] = true;
            p->takes[(size_t) t].chunks.resize ((size_t) LoopStore::takeCapacity (ownedStore->pool));
        }
    flush (post ({ Command::Type::Pin, -1, 0, 0, 0, 0.0f, p.get() }));
    return p;
}

void Looper::unpin (std::unique_ptr<Pinned> p)
{
    flush (post ({ Command::Type::Unpin, -1, 0, 0, 0, 0.0f, p.get() }));
}

bool Looper::copyTrackAudio (int track, std::vector<float>& left, std::vector<float>& right)
{
    const std::lock_guard lock (api);
    left.clear();
    right.clear();
    if (ownedStore == nullptr || track < 0 || track >= numTracks)
        return false;
    auto p = pin (1u << track);
    const auto& take = p->takes[(size_t) track];
    left.resize ((size_t) take.length);
    right.resize ((size_t) take.length);
    for (int64_t at = 0; at < take.length; at += LoopPool::chunkFrames)
    {
        const int c = take.chunks[(size_t) (at >> LoopPool::chunkShift)];
        const auto n = (size_t) std::min (LoopPool::chunkFrames, take.length - at);
        if (c == looper::silentChunk)
            continue; // already zero
        std::copy_n (ownedStore->pool.samples (c, 0), n, left.begin() + at);
        std::copy_n (ownedStore->pool.samples (c, 1), n, right.begin() + at);
    }
    unpin (std::move (p));
    return ! left.empty();
}

bool Looper::renderLoop (int track, std::vector<float>& left, std::vector<float>& right)
{
    const std::lock_guard lock (api);
    left.clear();
    right.clear();
    if (ownedStore == nullptr || track < 0 || track >= numTracks)
        return false;
    auto p = pin (1u << track);
    const auto& take = p->takes[(size_t) track];
    if (take.length > 0)
    {
        const auto& m = p->maps[(size_t) track];
        left.resize ((size_t) m.cycle());
        right.resize ((size_t) m.cycle());
        for (int64_t i = 0; i < m.cycle(); ++i)
            voiceAt (ownedStore->pool, take, m, i, left[(size_t) i], right[(size_t) i]);
    }
    unpin (std::move (p));
    return ! left.empty();
}

int64_t Looper::nearestZeroCrossing (int track, int64_t frame, int64_t radius)
{
    std::vector<float> l, r;
    if (! copyTrackAudio (track, l, r))
        return frame;
    const auto length = (int64_t) l.size();
    auto mean = [&] (int64_t i) { return 0.5f * (l[(size_t) i] + r[(size_t) i]); };
    auto crossing = [&] (int64_t i) { return i > 0 && i < length && (mean (i - 1) < 0.0f) != (mean (i) < 0.0f); };
    for (int64_t d = 0; d <= radius; ++d)
    {
        if (crossing (frame - d))
            return frame - d;
        if (crossing (frame + d))
            return frame + d;
    }
    return frame;
}

int64_t Looper::installTrack (int track, const float* left, const float* right, int64_t frames, int64_t start, int64_t end,
                              float crossfadeMs, bool explicitAnchor, int64_t anchor)
{
    auto& pool = ownedStore->pool;
    Claim claim;
    const auto want = (int) ((std::max<int64_t> (0, frames) + LoopPool::chunkFrames - 1) >> LoopPool::chunkShift);
    claim.ids.resize ((size_t) want);
    if (want > 0)
        flush (post ({ Command::Type::Claim, -1, 0, want, 0, 0.0f, &claim }));
    frames = std::min<int64_t> (frames, (int64_t) claim.got * LoopPool::chunkFrames);

    // The claimed chunks are ours until they're installed
    for (int k = 0; k < claim.got; ++k)
    {
        const int c = claim.ids[(size_t) k];
        const int64_t base = (int64_t) k << LoopPool::chunkShift;
        const auto n = (size_t) std::min (LoopPool::chunkFrames, frames - base);
        std::copy_n (left + base, n, pool.samples (c, 0));
        std::copy_n ((right == nullptr ? left : right) + base, n, pool.samples (c, 1));
        std::fill (pool.samples (c, 0) + n, pool.samples (c, 0) + LoopPool::chunkFrames, 0.0f);
        std::fill (pool.samples (c, 1) + n, pool.samples (c, 1) + LoopPool::chunkFrames, 0.0f);
        pool.refreshBins (c, 0, (int) LoopPool::chunkFrames, (int) n);
    }

    Install in;
    in.track = track;
    in.ids = claim.ids;
    in.count = claim.got;
    in.length = frames;
    in.start = frames > 0 ? std::min (start, frames - 1) : 0;
    in.end = frames > 0 ? std::max (end, in.start + minLoop) : 0;
    in.anchor = anchor;
    in.explicitAnchor = explicitAnchor;
    in.crossfadeMs = crossfadeMs;
    flush (post ({ Command::Type::Install, track, 0, 0, 0, 0.0f, &in }));
    return frames;
}

int64_t Looper::setTrackAudio (int track, const float* left, const float* right, int64_t frames)
{
    const std::lock_guard lock (api);
    if (ownedStore == nullptr || track < 0 || track >= numTracks || left == nullptr)
        return 0;
    return installTrack (track, left, right, frames, 0, frames, 5.0f, false, 0);
}

looper::LoopSession Looper::saveSession()
{
    const std::lock_guard lock (api);
    looper::LoopSession s;
    if (ownedStore == nullptr)
        return s;
    s.sampleRate = ownedStore->pool.sampleRate();
    auto p = pin ((1u << numTracks) - 1);
    s.masterTrack = p->masterTrack;
    s.masterLength = p->masterLength;
    for (int t = 0; t < numTracks; ++t)
    {
        const auto& take = p->takes[(size_t) t];
        auto& out = s.tracks[(size_t) t];
        if (take.length == 0)
            continue;
        out.left.resize ((size_t) take.length);
        out.right.resize ((size_t) take.length);
        for (int64_t at = 0; at < take.length; at += LoopPool::chunkFrames)
        {
            const int c = take.chunks[(size_t) (at >> LoopPool::chunkShift)];
            const auto n = (size_t) std::min (LoopPool::chunkFrames, take.length - at);
            if (c == looper::silentChunk)
                continue; // already zero
            std::copy_n (ownedStore->pool.samples (c, 0), n, out.left.begin() + at);
            std::copy_n (ownedStore->pool.samples (c, 1), n, out.right.begin() + at);
        }
        const auto& m = p->maps[(size_t) t];
        out.start = m.start;
        out.end = m.start + m.loop;
        out.crossfadeMs = p->crossfadeMs[(size_t) t];
        out.anchor = m.anchor - p->origin;
    }
    unpin (std::move (p));
    return s;
}

bool Looper::loadSession (const looper::LoopSession& s)
{
    const std::lock_guard lock (api);
    if (ownedStore == nullptr || ownedStore->pool.sampleRate() != s.sampleRate)
        return false;
    post ({ Command::Type::ClearAll });
    for (int t = 0; t < numTracks; ++t)
    {
        const auto& in = s.tracks[(size_t) t];
        if (! in.empty())
            installTrack (t, in.left.data(), in.right.size() == in.left.size() ? in.right.data() : nullptr, (int64_t) in.left.size(),
                          in.start, in.end, in.crossfadeMs, true, in.anchor);
    }
    flush (post ({ Command::Type::SetMaster, s.masterTrack, 0, s.masterLength }));
    return true;
}

} // namespace damp
