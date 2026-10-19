// Offline renderer: plays notes through the piano and writes a 32-bit float WAV.
//
//   dpiano_render [--preset file.dpiano] [--set id=value ...] [--notes note:start:dur[:vel],...]
//                 [--pedal start:dur,...] [--length seconds] [--rate hz] -o out.wav
//
// --pedal holds the sustain pedal down over those spans. Without --notes it plays a short demo
// (scale, chords, a pedalled arpeggio). Prints peak, RMS and CPU time.

#include "../common/RenderUtil.h"
#include "dsp/Piano.h"
#include "dsp/Preset.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using render::Note;

struct Event
{
    int64_t sample;
    enum Kind { Off, PedalUp, PedalDown, On } kind; // same-sample order: releases first
    int note;
    float velocity;
};

std::vector<Note> demoPhrase()
{
    std::vector<Note> notes;
    // C major scale up from middle C, mezzo forte
    const int scale[] { 60, 62, 64, 65, 67, 69, 71, 72 };
    for (int i = 0; i < 8; ++i)
        notes.push_back ({ scale[i], i * 0.25, 0.24, 0.6f });
    // Chords, piano then forte
    for (int n : { 48, 55, 64, 67 })
        notes.push_back ({ n, 2.25, 1.2, 0.45f });
    for (int n : { 41, 53, 57, 60, 65 })
        notes.push_back ({ n, 3.75, 1.2, 0.85f });
    // Pedalled arpeggio over three octaves, then a low A ringing out
    const int arp[] { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76, 79, 84 };
    for (int i = 0; i < 12; ++i)
        notes.push_back ({ arp[i], 5.25 + i * 0.12, 0.1, 0.55f + 0.02f * i });
    notes.push_back ({ 21, 8.0, 3.0, 0.8f });
    return notes;
}

std::vector<std::pair<double, double>> parsePedal (const std::string& spec)
{
    std::vector<std::pair<double, double>> spans;
    std::stringstream list (spec);
    std::string item;
    while (std::getline (list, item, ','))
    {
        double start = 0.0, duration = 0.0;
        if (std::sscanf (item.c_str(), "%lf:%lf", &start, &duration) != 2)
            throw std::runtime_error ("bad pedal span '" + item + "' (expected start:dur)");
        spans.emplace_back (start, duration);
    }
    return spans;
}
} // namespace

int main (int argc, char** argv)
try
{
    std::string presetText, outPath;
    std::vector<Note> notes;
    std::vector<std::pair<double, double>> pedal;
    bool pedalGiven = false;
    double length = 0.0;
    int rate = 48000;

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
        else if (arg == "--pedal") { pedal = parsePedal (value()); pedalGiven = true; }
        else if (arg == "--length") length = std::stod (value());
        else if (arg == "--rate") rate = std::stoi (value());
        else if (arg == "-o") outPath = value();
        else throw std::runtime_error ("unknown argument " + arg);
    }

    if (outPath.empty())
        throw std::runtime_error ("usage: dpiano_render [--preset f] [--set id=value] [--notes n:start:dur[:vel],...] [--pedal start:dur,...] [--length s] [--rate hz] -o out.wav");

    const auto preset = dpiano::parsePreset (presetText);
    for (const auto& w : preset.warnings)
        std::cerr << "warning: " << w << "\n";

    if (notes.empty())
    {
        notes = demoPhrase();
        if (! pedalGiven)
            pedal = { { 5.2, 3.9 } };
    }

    std::vector<Event> events;
    double end = 0.0;
    for (const auto& n : notes)
    {
        events.push_back ({ (int64_t) std::llround (n.start * rate), Event::On, n.note, n.velocity });
        events.push_back ({ (int64_t) std::llround ((n.start + n.duration) * rate), Event::Off, n.note, 0.0f });
        end = std::max (end, n.start + n.duration);
    }
    for (const auto& [start, duration] : pedal)
    {
        events.push_back ({ (int64_t) std::llround (start * rate), Event::PedalDown, 0, 0.0f });
        events.push_back ({ (int64_t) std::llround ((start + duration) * rate), Event::PedalUp, 0, 0.0f });
        end = std::max (end, start + duration);
    }
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
                      { return a.sample != b.sample ? a.sample < b.sample : a.kind < b.kind; });

    if (length <= 0.0)
        length = end + 2.0;

    dpiano::Piano piano;
    piano.setPatch (preset.patch);
    piano.prepare (rate);

    const auto frames = (size_t) (length * rate);
    std::vector<float> left (frames, 0.0f), right (frames, 0.0f);
    constexpr int block = 256;
    size_t next = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int64_t pos = 0; pos < (int64_t) frames;)
    {
        const int64_t blockEnd = std::min<int64_t> (pos + block, (int64_t) frames);
        piano.setPatch (preset.patch);
        while (pos < blockEnd)
        {
            int64_t until = blockEnd;
            if (next < events.size() && events[next].sample < until)
                until = std::max (pos, events[next].sample);
            if (until > pos)
            {
                piano.process (left.data() + pos, right.data() + pos, (int) (until - pos));
                pos = until;
            }
            while (next < events.size() && events[next].sample <= pos)
            {
                const auto& e = events[next++];
                switch (e.kind)
                {
                    case Event::On: piano.noteOn (e.note, e.velocity); break;
                    case Event::Off: piano.noteOff (e.note); break;
                    case Event::PedalDown: piano.sustainPedal (true); break;
                    case Event::PedalUp: piano.sustainPedal (false); break;
                }
            }
        }
    }
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();

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
    std::printf ("%s: \"%s\" %.2fs peak %.1f dBFS rms %.1f dBFS, rendered in %.0f%% of real time%s\n", outPath.c_str(),
                 preset.name.empty() ? "(defaults)" : preset.name.c_str(), length, db (peak), db (rms),
                 100.0 * seconds / length, finite ? "" : " NON-FINITE SAMPLES");
    return finite ? 0 : 2;
}
catch (const std::exception& e)
{
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
