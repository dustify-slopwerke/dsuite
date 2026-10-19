// Offline renderer: plays notes through the engine and writes a 32-bit float WAV.
//
//   dsynth_render [--preset file.dsynth] [--set id=value ...] [--notes note:start:dur[:vel],...]
//                 [--length seconds] [--rate hz] [--bpm tempo] -o out.wav
//
// --bpm simulates a playing host transport (for the arpeggiator's host sync).
//
// Without --notes it plays a short demo phrase. Prints peak and RMS for quick sanity checks.

#include "../common/RenderUtil.h"
#include "dsp/LayeredSynth.h"
#include "dsp/Preset.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using render::Note;

struct Event
{
    int64_t sample;
    bool on;
    int note;
    float velocity;
};

std::vector<Note> demoPhrase()
{
    // A bass/lead line with overlapping (legato) notes, then a chord, then a long held note
    std::vector<Note> notes;
    const int line[] { 36, 48, 43, 46, 36, 39, 41, 43 };
    for (int i = 0; i < 8; ++i)
        notes.push_back ({ line[i], i * 0.25, i % 2 == 0 ? 0.2 : 0.3, 0.8f });
    for (int n : { 48, 52, 55, 59 })
        notes.push_back ({ n, 2.25, 1.0, 0.7f });
    notes.push_back ({ 45, 3.75, 2.0, 0.9f });
    return notes;
}

} // namespace

int main (int argc, char** argv)
try
{
    std::string presetText, outPath;
    std::vector<Note> notes;
    double length = 0.0;
    int rate = 48000;
    double bpm = 0.0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]
        {
            if (i + 1 >= argc)
                throw std::runtime_error ("missing value for " + arg);
            return std::string (argv[++i]);
        };

        if (arg == "--preset") presetText = render::readFile (value()) + "\n" + presetText;
        else if (arg == "--set") presetText += value() + "\n";
        else if (arg == "--notes") notes = render::parseNotes (value());
        else if (arg == "--length") length = std::stod (value());
        else if (arg == "--rate") rate = std::stoi (value());
        else if (arg == "--bpm") bpm = std::stod (value());
        else if (arg == "-o") outPath = value();
        else throw std::runtime_error ("unknown argument " + arg);
    }

    if (outPath.empty())
        throw std::runtime_error ("usage: dsynth_render [--preset f] [--set id=value] [--notes n:start:dur[:vel],...] [--length s] [--rate hz] -o out.wav");

    const auto preset = dsynth::parsePreset (presetText);
    for (const auto& w : preset.warnings)
        std::cerr << "warning: " << w << "\n";

    if (notes.empty())
        notes = demoPhrase();

    std::vector<Event> events;
    double end = 0.0;
    for (const auto& n : notes)
    {
        events.push_back ({ (int64_t) std::llround (n.start * rate), true, n.note, n.velocity });
        events.push_back ({ (int64_t) std::llround ((n.start + n.duration) * rate), false, n.note, 0.0f });
        end = std::max (end, n.start + n.duration);
    }
    // Note-offs before note-ons at the same time, so repeated notes retrigger
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
                      { return a.sample != b.sample ? a.sample < b.sample : (! a.on && b.on); });

    if (length <= 0.0)
        length = end + 1.5;

    dsynth::LayeredSynth engine;
    engine.setPatches (preset.patch, preset.layerB);
    engine.prepare (rate);

    const auto frames = (size_t) (length * rate);
    std::vector<float> left (frames, 0.0f), right (frames, 0.0f);
    constexpr int block = 256;
    size_t next = 0;
    for (int64_t pos = 0; pos < (int64_t) frames;)
    {
        const int64_t blockEnd = std::min<int64_t> (pos + block, (int64_t) frames);
        engine.setPatches (preset.patch, preset.layerB);
        engine.setTransport (bpm, bpm > 0.0, bpm > 0.0 ? (double) pos / rate * bpm / 60.0 : -1.0);
        while (pos < blockEnd)
        {
            int64_t until = blockEnd;
            if (next < events.size() && events[next].sample < until)
                until = std::max (pos, events[next].sample);
            if (until > pos)
            {
                engine.process (left.data() + pos, right.data() + pos, (int) (until - pos));
                pos = until;
            }
            while (next < events.size() && events[next].sample <= pos)
            {
                const auto& e = events[next++];
                if (e.on)
                    engine.noteOn (e.note, e.velocity);
                else
                    engine.noteOff (e.note);
            }
        }
    }

    std::vector<float> out;
    out.reserve (2 * frames);
    for (size_t i = 0; i < frames; ++i)
    {
        out.push_back (left[i]);
        out.push_back (right[i]);
    }

    double peak = 0.0, sumSq = 0.0;
    bool finite = true;
    for (float s : out)
    {
        finite = finite && std::isfinite (s);
        peak = std::max (peak, (double) std::abs (s));
        sumSq += (double) s * s;
    }
    const double rms = std::sqrt (sumSq / (double) out.size());
    auto db = [] (double x) { return x > 0.0 ? 20.0 * std::log10 (x) : -999.0; };

    render::writeWav (outPath, out, rate);
    std::printf ("%s: \"%s\" %.2fs peak %.1f dBFS rms %.1f dBFS%s\n", outPath.c_str(),
                 preset.name.empty() ? "(defaults)" : preset.name.c_str(), length, db (peak), db (rms),
                 finite ? "" : " NON-FINITE SAMPLES");
    return finite ? 0 : 2;
}
catch (const std::exception& e)
{
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
