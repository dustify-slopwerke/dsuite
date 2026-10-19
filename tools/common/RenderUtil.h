#pragma once

// Shared bits of the offline renderers: note lists, WAV input and output, file reading

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace render
{

struct Note
{
    int note;
    double start, duration;
    float velocity;
};

// "note:start:dur[:vel],..." (times in seconds, velocity 0..1)
inline std::vector<Note> parseNotes (const std::string& spec)
{
    std::vector<Note> notes;
    std::stringstream list (spec);
    std::string item;
    while (std::getline (list, item, ','))
    {
        Note n { 60, 0.0, 1.0, 0.8f };
        if (std::sscanf (item.c_str(), "%d:%lf:%lf:%f", &n.note, &n.start, &n.duration, &n.velocity) < 3)
            throw std::runtime_error ("bad note '" + item + "' (expected note:start:dur[:vel])");
        notes.push_back (n);
    }
    return notes;
}

// Interleaved stereo, 32-bit float
inline void writeWav (const std::string& path, const std::vector<float>& samples, int rate)
{
    std::ofstream f (path, std::ios::binary);
    if (! f)
        throw std::runtime_error ("can't write " + path);

    auto u32 = [&] (uint32_t v) { f.write (reinterpret_cast<const char*> (&v), 4); };
    auto u16 = [&] (uint16_t v) { f.write (reinterpret_cast<const char*> (&v), 2); };
    const auto dataBytes = (uint32_t) (samples.size() * sizeof (float));

    f.write ("RIFF", 4); u32 (36 + dataBytes);
    f.write ("WAVE", 4);
    f.write ("fmt ", 4); u32 (16); u16 (3 /* IEEE float */); u16 (2); u32 ((uint32_t) rate);
    u32 ((uint32_t) rate * 8); u16 (8); u16 (32);
    f.write ("data", 4); u32 (dataBytes);
    f.write (reinterpret_cast<const char*> (samples.data()), dataBytes);
}

struct Audio
{
    int rate = 48000;
    std::vector<std::vector<float>> channels; // one vector per channel, all the same length
};

// PCM 16/24/32-bit or 32-bit float WAV (plain or WAVE_FORMAT_EXTENSIBLE), any channel count
inline Audio readWav (const std::string& path)
{
    std::ifstream f (path, std::ios::binary);
    if (! f)
        throw std::runtime_error ("can't read " + path);
    const std::string bytes { std::istreambuf_iterator<char> (f), {} };

    auto u16 = [&] (size_t at) { uint16_t v; std::memcpy (&v, bytes.data() + at, 2); return v; };
    auto u32 = [&] (size_t at) { uint32_t v; std::memcpy (&v, bytes.data() + at, 4); return v; };
    if (bytes.size() < 12 || bytes.compare (0, 4, "RIFF") != 0 || bytes.compare (8, 4, "WAVE") != 0)
        throw std::runtime_error (path + " is not a WAV file");

    int format = 0, numChannels = 0, bits = 0;
    Audio audio;
    for (size_t at = 12; at + 8 <= bytes.size();)
    {
        const auto id = bytes.substr (at, 4);
        const size_t size = u32 (at + 4), body = at + 8;
        if (body + size > bytes.size() && id != "data")
            break;
        if (id == "fmt " && size >= 16)
        {
            format = u16 (body);
            numChannels = u16 (body + 2);
            audio.rate = (int) u32 (body + 4);
            bits = u16 (body + 14);
            if (format == 0xfffe && size >= 26)
                format = u16 (body + 24); // the sub-format GUID starts with the plain format tag
        }
        else if (id == "data")
        {
            if (numChannels <= 0 || ! ((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32)))
                throw std::runtime_error (path + ": unsupported WAV format (need 16/24/32-bit PCM or 32-bit float)");
            const size_t frameBytes = (size_t) numChannels * (size_t) bits / 8;
            const size_t frames = std::min (size, bytes.size() - body) / frameBytes;
            audio.channels.assign ((size_t) numChannels, std::vector<float> (frames));
            for (size_t i = 0; i < frames; ++i)
            {
                for (size_t ch = 0; ch < (size_t) numChannels; ++ch)
                {
                    const size_t p = body + i * frameBytes + ch * (size_t) bits / 8;
                    float v = 0.0f;
                    if (format == 3)
                        std::memcpy (&v, bytes.data() + p, 4);
                    else if (bits == 16)
                        v = (float) (int16_t) u16 (p) / 32768.0f;
                    else if (bits == 24)
                        v = (float) ((int32_t) ((uint32_t) (uint8_t) bytes[p] << 8 | (uint32_t) (uint8_t) bytes[p + 1] << 16
                                                | (uint32_t) (uint8_t) bytes[p + 2] << 24) >> 8) / 8388608.0f;
                    else
                        v = (float) ((double) (int32_t) u32 (p) / 2147483648.0);
                    audio.channels[ch][i] = v;
                }
            }
            return audio;
        }
        at = body + size + (size & 1);
    }
    throw std::runtime_error (path + ": no audio data");
}

inline std::string readFile (const std::string& path)
{
    std::ifstream f (path);
    if (! f)
        throw std::runtime_error ("can't read " + path);
    return { std::istreambuf_iterator<char> (f), {} };
}

} // namespace render
