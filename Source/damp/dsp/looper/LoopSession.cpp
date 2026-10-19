#include "LoopSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace damp::looper
{
namespace
{
constexpr uint32_t codecFloat = 0, codecPlain = 1;
constexpr int32_t maxSample = (1 << 23) - 1;
constexpr double maxTrackSeconds = 1200.0; // longer than any take (a 5 minute pool, padding grown into): malformed

// Little-endian on every platform the suite builds for, so values go out as they are in memory
template <typename T>
void put (std::vector<std::byte>& out, const T& v)
{
    const auto* p = reinterpret_cast<const std::byte*> (&v);
    out.insert (out.end(), p, p + sizeof (T));
}

struct Reader
{
    const std::byte* p;
    size_t left;

    template <typename T>
    bool get (T& v)
    {
        if (left < sizeof (T))
            return false;
        std::memcpy (&v, p, sizeof (T));
        p += sizeof (T);
        left -= sizeof (T);
        return true;
    }
    bool floats (std::vector<float>& v, size_t n)
    {
        if (left / sizeof (float) < n)
            return false;
        v.resize (n);
        std::memcpy (v.data(), p, n * sizeof (float));
        p += n * sizeof (float);
        left -= n * sizeof (float);
        return true;
    }
    bool skip (size_t n)
    {
        if (left < n)
            return false;
        p += n;
        left -= n;
        return true;
    }
};

struct PlainPacker final : SamplePacker
{
    uint32_t id() const override { return codecPlain; }
    bool pack (const int32_t* left, const int32_t* right, size_t frames, std::vector<std::byte>& out) const override
    {
        out.reserve (out.size() + frames * 6);
        for (const int32_t* x : { left, right })
            for (size_t i = 0; i < frames; ++i)
                for (int b = 0; b < 3; ++b)
                    out.push_back ((std::byte) ((uint32_t) x[i] >> (8 * b)));
        return true;
    }
    bool unpack (const std::byte* data, size_t size, size_t frames, int32_t* left, int32_t* right) const override
    {
        if (size != frames * 6)
            return false;
        for (int32_t* x : { left, right })
            for (size_t i = 0; i < frames; ++i, data += 3)
            {
                const auto u = (uint32_t) data[0] | (uint32_t) data[1] << 8 | (uint32_t) data[2] << 16;
                x[i] = (int32_t) (u << 8) >> 8; // sign-extended
            }
        return true;
    }
};

float finite (float x) { return std::isfinite (x) ? x : 0.0f; }

// The exponent e with every sample below 2^e (see encodeSession)
int exponentFor (const std::vector<float>& left, const std::vector<float>& right)
{
    float peak = 0.0f;
    for (const auto* x : { &left, &right })
        for (float v : *x)
            peak = std::max (peak, std::abs (finite (v)));
    if (peak == 0.0f)
        return 0;
    int e = 0;
    std::frexp (peak, &e); // peak = m * 2^e, m in [0.5, 1)
    return std::clamp (e, -100, 100);
}

void quantise (const std::vector<float>& in, int e, std::vector<int32_t>& out)
{
    const double scale = std::ldexp (1.0, 23 - e);
    out.resize (in.size());
    for (size_t i = 0; i < in.size(); ++i)
        out[i] = (int32_t) std::clamp<long> (std::lrint ((double) finite (in[i]) * scale), -maxSample, maxSample);
}

bool decodeTrack (Reader& in, uint32_t version, size_t frames, const SamplePacker* packer, LoopTrackData& t)
{
    if (version == 1)
        return in.floats (t.left, frames) && in.floats (t.right, frames);
    if (frames == 0)
        return true;
    uint32_t codec = 0;
    int32_t e = 0;
    uint64_t bytes = 0;
    if (! in.get (codec) || ! in.get (e) || ! in.get (bytes) || bytes > in.left)
        return false;
    const auto* data = in.p;
    in.skip ((size_t) bytes);
    if (codec == codecFloat)
    {
        Reader r { data, (size_t) bytes };
        return r.floats (t.left, frames) && r.floats (t.right, frames) && r.left == 0;
    }
    const SamplePacker* by = codec == codecPlain ? &plainPacker() : packer != nullptr && packer->id() == codec ? packer : nullptr;
    if (by == nullptr || e < -100 || e > 100)
        return false;
    std::vector<int32_t> l (frames), r (frames);
    if (! by->unpack (data, (size_t) bytes, frames, l.data(), r.data()))
        return false;
    const float step = std::ldexp (1.0f, e - 23);
    t.left.resize (frames);
    t.right.resize (frames);
    for (size_t i = 0; i < frames; ++i)
    {
        t.left[i] = (float) std::clamp (l[i], -maxSample, maxSample) * step;
        t.right[i] = (float) std::clamp (r[i], -maxSample, maxSample) * step;
    }
    return true;
}
} // namespace

const SamplePacker& plainPacker()
{
    static const PlainPacker packer;
    return packer;
}

bool LoopSession::empty() const
{
    return std::all_of (tracks.begin(), tracks.end(), [] (const auto& t) { return t.empty(); });
}

size_t LoopSession::frames() const
{
    size_t n = 0;
    for (const auto& t : tracks)
        n += t.left.size();
    return n;
}

void encodeSession (const LoopSession& s, std::vector<std::byte>& out, const SamplePacker* packer)
{
    put (out, sessionMagic);
    put (out, sessionVersion);
    const size_t sizeAt = out.size();
    put (out, uint64_t { 0 });
    put (out, s.sampleRate);
    put (out, (int32_t) s.masterTrack);
    put (out, (int64_t) s.masterLength);
    put (out, (int32_t) numTracks);
    std::vector<int32_t> l, r;
    for (const auto& t : s.tracks)
    {
        put (out, (int64_t) t.left.size());
        put (out, t.start);
        put (out, t.end);
        put (out, t.crossfadeMs);
        put (out, t.anchor);
        if (t.left.empty())
            continue;
        const auto& right = t.right.size() == t.left.size() ? t.right : t.left;
        const int e = exponentFor (t.left, right);
        const size_t codecAt = out.size();
        put (out, codecFloat);
        put (out, (int32_t) e);
        put (out, uint64_t { 0 });
        const size_t dataAt = out.size();
        bool packed = false;
        if (packer != nullptr)
        {
            quantise (t.left, e, l);
            quantise (right, e, r);
            packed = packer->pack (l.data(), r.data(), l.size(), out);
            if (packed)
            {
                const uint32_t id = packer->id();
                std::memcpy (out.data() + codecAt, &id, sizeof (id));
            }
            else
            {
                out.resize (dataAt); // shouldn't happen: float32 rather than lose the track
            }
        }
        if (! packed)
        {
            const auto* x = reinterpret_cast<const std::byte*> (t.left.data());
            out.insert (out.end(), x, x + t.left.size() * sizeof (float));
            const auto* y = reinterpret_cast<const std::byte*> (right.data());
            out.insert (out.end(), y, y + t.left.size() * sizeof (float));
        }
        const uint64_t bytes = out.size() - dataAt;
        std::memcpy (out.data() + dataAt - sizeof (uint64_t), &bytes, sizeof (bytes));
    }
    const uint64_t size = out.size() - sizeAt - sizeof (uint64_t);
    std::memcpy (out.data() + sizeAt, &size, sizeof (size));
}

std::optional<LoopSession> decodeSession (const void* data, size_t size, const SamplePacker* packer)
{
    Reader in { static_cast<const std::byte*> (data), size };
    uint32_t magic = 0, version = 0;
    uint64_t bytes = 0;
    if (! in.get (magic) || magic != sessionMagic || ! in.get (version) || version < 1 || ! in.get (bytes) || bytes > in.left)
        return std::nullopt;
    in.left = (size_t) bytes; // anything a later version appends after its own fields is skipped

    LoopSession s;
    int32_t master = -1, count = 0;
    int64_t masterLength = 0;
    if (! in.get (s.sampleRate) || ! in.get (master) || ! in.get (masterLength) || ! in.get (count))
        return std::nullopt;
    if (! (s.sampleRate >= 8000.0 && s.sampleRate <= 768000.0) || count < 0)
        return std::nullopt;
    s.masterTrack = master >= 0 && master < numTracks ? master : -1;
    s.masterLength = std::max<int64_t> (0, masterLength);
    for (int i = 0; i < count; ++i)
    {
        LoopTrackData t;
        int64_t frames = 0;
        if (! in.get (frames) || frames < 0 || ! in.get (t.start) || ! in.get (t.end) || ! in.get (t.crossfadeMs) || ! in.get (t.anchor))
            return std::nullopt;
        if ((double) frames > s.sampleRate * maxTrackSeconds || ! decodeTrack (in, version, (size_t) frames, packer, t))
            return std::nullopt;
        if (! std::isfinite (t.crossfadeMs))
            t.crossfadeMs = 5.0f;
        if (i < numTracks)
            s.tracks[(size_t) i] = std::move (t);
    }
    return s;
}

std::vector<float> resample (const std::vector<float>& in, double fromRate, double toRate)
{
    if (in.empty() || fromRate <= 0.0 || toRate <= 0.0 || std::abs (fromRate - toRate) < 1.0e-9)
        return in;

    // Windowed sinc from a table: `zeros` crossings each side, cut off below the lower Nyquist
    constexpr int zeros = 16, phases = 512;
    const double ratio = toRate / fromRate, cutoff = 0.95 * std::min (1.0, ratio);
    const double reach = zeros / cutoff;
    const int taps = (int) std::ceil (reach) + 1;
    std::vector<float> table ((size_t) (taps * phases + 2));
    for (size_t k = 0; k < table.size(); ++k)
    {
        const double x = (double) k / phases;
        if (x >= reach)
            continue;
        const double s = x == 0.0 ? 1.0 : std::sin (std::numbers::pi * cutoff * x) / (std::numbers::pi * cutoff * x);
        const double w = 0.42 + 0.5 * std::cos (std::numbers::pi * x / reach) + 0.08 * std::cos (2.0 * std::numbers::pi * x / reach);
        table[k] = (float) (cutoff * s * w);
    }
    auto kernel = [&] (double x)
    {
        const double at = std::abs (x) * phases;
        const auto i = (size_t) at;
        if (i + 1 >= table.size())
            return 0.0f;
        const auto f = (float) (at - (double) i);
        return table[i] + f * (table[i + 1] - table[i]);
    };

    const auto outLength = (size_t) std::llround ((double) in.size() * ratio);
    std::vector<float> out (outLength);
    const auto n = (int64_t) in.size();
    for (size_t i = 0; i < outLength; ++i)
    {
        const double t = (double) i / ratio;
        const auto centre = (int64_t) std::floor (t);
        float sum = 0.0f;
        for (int64_t j = std::max<int64_t> (0, centre - taps + 1); j <= std::min<int64_t> (n - 1, centre + taps); ++j)
            sum += in[(size_t) j] * kernel (t - (double) j);
        out[i] = sum;
    }
    return out;
}

LoopSession resampleSession (const LoopSession& s, double toRate)
{
    if (std::abs (s.sampleRate - toRate) < 1.0e-9)
        return s;
    const double ratio = toRate / s.sampleRate;
    auto scale = [ratio] (int64_t v) { return (int64_t) std::llround ((double) v * ratio); };
    LoopSession out;
    out.sampleRate = toRate;
    out.masterTrack = s.masterTrack;
    out.masterLength = scale (s.masterLength);
    for (size_t t = 0; t < s.tracks.size(); ++t)
    {
        const auto& in = s.tracks[t];
        auto& o = out.tracks[t];
        if (in.empty())
            continue;
        o.left = resample (in.left, s.sampleRate, toRate);
        o.right = resample (in.right.size() == in.left.size() ? in.right : in.left, s.sampleRate, toRate);
        o.start = scale (in.start);
        o.end = scale (in.end);
        o.crossfadeMs = in.crossfadeMs;
        o.anchor = scale (in.anchor);
    }
    return out;
}

} // namespace damp::looper
