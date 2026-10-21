// Offline renderer: runs a WAV (or a test tone) through the amp and writes a 32-bit float WAV.
//
//   damp_render [--preset file.damp] [--set id=value ...]
//               (-i in.wav | --sine hz:seconds[:dBFS] | --sweep from:to:seconds[:dBFS] | --guitar [notes])
//               [--level peak-dBFS] [--rate hz]
//               [--ir cab.wav] [--board "pedal, ..., amp, ..."] [--bpm tempo] [--tail seconds]
//               [--block samples] [--keep-latency] [--loop actions] [--loop-out prefix] -o out.wav
//
// A mono input runs one amp (the output is the same on both sides), a stereo one two. The output
// is shifted back by the plugin's latency unless --keep-latency. --guitar plays plucked strings (a
// stand-in for a DI'd guitar: "note:start:dur[:vel],..." or "demo"), peaking at --level (-12 dBFS). --ir loads a cabinet impulse
// response and selects it (over any preset's cab). --bpm sets the tempo synced pedals and the
// looper's beats follow (120); --tail adds that much silence after the input so delays and reverbs
// ring out. Prints peak, RMS and CPU time.
//
// --loop drives the looper (switching it on): a comma-separated list of action[:track]@seconds,
// actions rec, play (play / stop all), stop / start (one track), undo, redo, clear, clearall;
// edit:track=start:end@s (seconds into the recording) and fade:track=ms@s set edits, and
// set:id=value@s sets any parameter then (e.g. set:track2_reverse=on@6). Tracks count from 1
// (default: the selected one). Actions land at the start of the block containing the time. E.g.
//   --guitar demo --tail 8 --loop "rec@0.5,rec@2.5,rec:2@2.5,rec:2@4.5,set:track1_half=on@7"
// Session settings go the same way, e.g. the metronome with a count-in and a quantised first loop:
//   --loop "set:metronome_on=on@0,set:count_in=on@0,set:looper_quantise=on@0,rec@0.5,rec@4.6"
// --loop-out writes each non-empty track's recording to prefix<N>.wav afterwards.

#include "../common/RenderUtil.h"
#include "common/dsp/Convolver.h"
#include "dsp/Rig.h"
#include "dsp/Cabinet.h"
#include "dsp/Preset.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// "a:b:c[:d]" -> numbers; the optional last one defaults to `last`
std::vector<double> parseFields (const std::string& spec, size_t required, double last)
{
    std::vector<double> v;
    size_t start = 0;
    while (start <= spec.size())
    {
        const auto end = std::min (spec.find (':', start), spec.size());
        v.push_back (std::stod (spec.substr (start, end - start)));
        start = end + 1;
    }
    if (v.size() < required || v.size() > required + 1)
        throw std::runtime_error ("bad test signal '" + spec + "'");
    if (v.size() == required)
        v.push_back (last);
    return v;
}

// Plucked strings (Karplus-Strong) through a humbucker-ish pickup resonance: close enough to a
// guitar DI to judge levels and tone
render::Audio guitar (std::vector<render::Note> notes, int rate, double peakDb)
{
    if (notes.empty())
    {
        // Open-position chord strums, a palm-muted riff, then single notes up the neck
        const int chordE[] { 40, 47, 52, 56, 59, 64 }, chordA[] { 45, 52, 57, 61, 64 };
        for (int strum = 0; strum < 4; ++strum)
            for (int i = 0; i < 6; ++i)
                notes.push_back ({ chordE[i], strum * 0.5 + i * 0.012, 0.45, 0.6f + 0.1f * (strum % 2) });
        for (int i = 0; i < 5; ++i)
            notes.push_back ({ chordA[i], 2.0 + i * 0.015, 1.4, 0.8f });
        for (int i = 0; i < 8; ++i)
            notes.push_back ({ i % 4 == 3 ? 43 : 40, 3.5 + i * 0.18, 0.12, 0.9f });
        const int lead[] { 64, 67, 69, 71, 74, 76, 79, 76 };
        for (int i = 0; i < 8; ++i)
            notes.push_back ({ lead[i], 5.2 + i * 0.22, i == 7 ? 1.2 : 0.21, 0.75f });
    }

    double end = 0.0;
    for (const auto& n : notes)
        end = std::max (end, n.start + n.duration + 0.3);
    std::vector<double> out ((size_t) (end * rate), 0.0);
    uint32_t seed = 12345;
    auto random = [&] { seed = seed * 1664525u + 1013904223u; return (double) seed / 4294967296.0 * 2.0 - 1.0; };

    for (const auto& n : notes)
    {
        const double f = 440.0 * std::pow (2.0, (n.note - 69) / 12.0);
        const auto period = (size_t) std::max (2.0, std::round (rate / f));
        std::vector<double> line (period);
        // Excitation: noise, brighter when picked harder, shaped by the pick position (1/7)
        double lp = 0.0;
        const double brightness = 0.3 + 0.6 * n.velocity;
        for (size_t i = 0; i < period; ++i)
        {
            lp += brightness * (random() - lp);
            line[i] = lp;
        }
        const size_t pick = period / 7;
        std::vector<double> combed (period);
        for (size_t i = 0; i < period; ++i)
            combed[i] = line[i] - line[(i + period - pick) % period];

        const auto start = (size_t) (n.start * rate), length = (size_t) ((n.duration + 0.3) * rate);
        const auto release = (size_t) (n.duration * rate);
        const double sustain = std::pow (0.001, 1.0 / (f * 3.0)), damped = std::pow (0.001, 1.0 / (f * 0.08));
        double previous = 0.0;
        for (size_t i = 0; i < length && start + i < out.size(); ++i)
        {
            const size_t at = i % period;
            const double y = combed[at];
            const double next = 0.5 * (y + previous) * (i < release ? sustain : damped);
            previous = y;
            combed[at] = next;
            out[start + i] += n.velocity * y;
        }
    }

    // Pickup: resonance around 3.5 kHz into the cable's capacitance
    const double w = 2.0 * std::numbers::pi * 3500.0 / rate, q = 2.0, alpha = std::sin (w) / (2.0 * q);
    const double a0 = 1.0 + alpha, b0 = (1.0 - std::cos (w)) / 2.0 / a0, b1 = (1.0 - std::cos (w)) / a0, b2 = b0;
    const double a1 = -2.0 * std::cos (w) / a0, a2 = (1.0 - alpha) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, peak = 1.0e-9;
    for (auto& s : out)
    {
        const double y = b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = s; y2 = y1; y1 = y;
        s = y;
        peak = std::max (peak, std::abs (y));
    }

    const double scale = std::pow (10.0, peakDb / 20.0) / peak;
    render::Audio a { rate, { std::vector<float> (out.size()) } };
    for (size_t i = 0; i < out.size(); ++i)
        a.channels[0][i] = (float) (out[i] * scale);
    return a;
}

// --loop: what to do to the looper, and when
struct LoopEvent
{
    double time = 0.0;
    std::string action;
    int track = -1;
    std::string setting; // edit / fade / set
};

std::vector<LoopEvent> parseLoopEvents (const std::string& spec)
{
    std::vector<LoopEvent> events;
    std::stringstream list (spec);
    std::string item;
    while (std::getline (list, item, ','))
    {
        const auto at = item.rfind ('@');
        if (at == std::string::npos)
            throw std::runtime_error ("bad looper action '" + item + "' (expected action[:track]@seconds)");
        LoopEvent e;
        e.time = std::stod (item.substr (at + 1));
        std::string head = item.substr (0, at);
        if (const auto eq = head.find ('='); eq != std::string::npos)
        {
            e.setting = head.substr (eq + 1);
            head = head.substr (0, eq);
        }
        if (const auto colon = head.find (':'); colon != std::string::npos)
        {
            const auto what = head.substr (colon + 1);
            head = head.substr (0, colon);
            if (head == "set")
                e.setting = what + "=" + e.setting;
            else
                e.track = std::stoi (what) - 1;
        }
        e.action = head;
        events.push_back (e);
    }
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
    return events;
}

render::Audio sine (const std::string& spec, int rate)
{
    const auto v = parseFields (spec, 2, -12.0);
    const double amp = std::pow (10.0, v[2] / 20.0);
    render::Audio a { rate, { std::vector<float> ((size_t) (v[1] * rate)) } };
    for (size_t i = 0; i < a.channels[0].size(); ++i)
        a.channels[0][i] = (float) (amp * std::sin (2.0 * std::numbers::pi * v[0] * (double) i / rate));
    return a;
}

// Exponential sine sweep
render::Audio sweep (const std::string& spec, int rate)
{
    const auto v = parseFields (spec, 3, -12.0);
    const double amp = std::pow (10.0, v[3] / 20.0), f0 = v[0], f1 = v[1], seconds = v[2];
    const double k = std::log (f1 / f0);
    render::Audio a { rate, { std::vector<float> ((size_t) (seconds * rate)) } };
    for (size_t i = 0; i < a.channels[0].size(); ++i)
    {
        const double t = (double) i / rate;
        const double phase = 2.0 * std::numbers::pi * f0 * seconds / k * (std::exp (t / seconds * k) - 1.0);
        a.channels[0][i] = (float) (amp * std::sin (phase));
    }
    return a;
}
} // namespace

int main (int argc, char** argv)
try
{
    std::string presetText, outPath, inPath, sineSpec, sweepSpec, irPath, guitarSpec, boardSpec, loopSpec, loopOut;
    double level = -12.0, bpm = 120.0, tail = 0.0;
    int rate = 48000, block = 256;
    bool keepLatency = false, dry = false;

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
        else if (arg == "-i") inPath = value();
        else if (arg == "--sine") sineSpec = value();
        else if (arg == "--sweep") sweepSpec = value();
        else if (arg == "--ir") irPath = value();
        else if (arg == "--board") boardSpec = value();
        else if (arg == "--guitar") guitarSpec = value();
        else if (arg == "--level") level = std::stod (value());
        else if (arg == "--rate") rate = std::stoi (value());
        else if (arg == "--bpm") bpm = std::stod (value());
        else if (arg == "--tail") tail = std::max (0.0, std::stod (value()));
        else if (arg == "--block") block = std::max (1, std::stoi (value()));
        else if (arg == "--keep-latency") keepLatency = true;
        else if (arg == "--loop") loopSpec = value();
        else if (arg == "--loop-out") loopOut = value();
        else if (arg == "--dry") dry = true; // write the input as it is (e.g. the --guitar DI)
        else if (arg == "-o") outPath = value();
        else throw std::runtime_error ("unknown argument " + arg);
    }

    if (outPath.empty() || (inPath.empty() && sineSpec.empty() && sweepSpec.empty() && guitarSpec.empty()))
        throw std::runtime_error ("usage: damp_render [--preset f] [--set id=value] (-i in.wav | --sine hz:s[:dBFS] | "
                                  "--sweep from:to:s[:dBFS] | --guitar notes|demo) [--level dBFS] [--rate hz] [--bpm t] [--tail s] [--block n] [--keep-latency] -o out.wav");

    if (! irPath.empty())
        presetText += "\ncab = User IR\n";
    if (! boardSpec.empty())
        presetText += "\nboard = " + boardSpec + "\n";
    if (! loopSpec.empty())
        presetText = "looper_on = on\n" + presetText;
    auto preset = damp::parsePreset (presetText);
    for (const auto& w : preset.warnings)
        std::cerr << "warning: " << w << "\n";

    auto input = ! inPath.empty()      ? render::readWav (inPath)
                 : ! guitarSpec.empty() ? guitar (guitarSpec == "demo" ? std::vector<render::Note> {} : render::parseNotes (guitarSpec), rate, level)
                 : ! sineSpec.empty()   ? sine (sineSpec, rate)
                                        : sweep (sweepSpec, rate);
    rate = input.rate;
    const bool stereo = input.channels.size() >= 2;
    const size_t inFrames = input.channels[0].size() + (dry ? 0 : (size_t) (tail * rate));
    const size_t latency = keepLatency ? 0 : (size_t) damp::Rig::latencySamples;

    // Pad with silence so the latency doesn't cut off the end
    std::vector<float> left (inFrames + latency, 0.0f), right (stereo ? left.size() : 0, 0.0f);
    std::copy (input.channels[0].begin(), input.channels[0].end(), left.begin());
    if (stereo)
        std::copy (input.channels[1].begin(), input.channels[1].end(), right.begin());

    damp::Rig rig;
    rig.setPatch (preset.patch);
    rig.setBoard (preset.board);

    // Cabinet
    const auto settings = damp::cabSettings (preset.patch);
    std::vector<float> impulse { 1.0f };
    if (settings.cab == damp::CabType::User && ! irPath.empty())
    {
        const auto ir = render::readWav (irPath);
        impulse = damp::prepareUserImpulse (ir.channels[0], ir.rate, rate, damp::maxImpulseSeconds);
    }
    else
    {
        impulse = damp::modelledCabinet (settings, rate);
    }
    const dustify::ConvolutionKernel kernel (impulse.data(), (int) impulse.size());
    rig.prepare (rate, (int) impulse.size());
    rig.setCabKernelNow (&kernel);

    auto& looper = rig.looper();
    const auto loopEvents = parseLoopEvents (loopSpec);
    size_t nextEvent = 0;
    if (! loopSpec.empty())
    {
        looper.setIdleTimeout (0.0); // one thread plays both sides
        looper.createPool (rate);
    }
    auto runEvent = [&] (const LoopEvent& e)
    {
        using A = damp::Looper::Action;
        const std::pair<const char*, A> actions[] { { "rec", A::Rec }, { "play", A::PlayStop }, { "undo", A::Undo }, { "redo", A::Redo },
                                                    { "clear", A::Clear }, { "clearall", A::ClearAll }, { "start", A::Play },
                                                    { "stop", A::Stop } };
        for (const auto& [name, action] : actions)
            if (e.action == name)
                return looper.press (action, e.track);
        if (e.action == "edit")
        {
            double from = 0.0, to = 0.0;
            if (std::sscanf (e.setting.c_str(), "%lf:%lf", &from, &to) != 2)
                throw std::runtime_error ("bad edit '" + e.setting + "' (expected start:end in seconds)");
            return looper.setTrackEdits (e.track, std::llround (from * rate), std::llround (to * rate));
        }
        if (e.action == "fade")
            return looper.setTrackCrossfade (e.track, std::stof (e.setting));
        if (e.action == "set")
        {
            const auto set = damp::parsePreset (e.setting);
            for (const auto& w : set.warnings)
                std::cerr << "warning: " << w << "\n";
            std::string id = e.setting.substr (0, e.setting.find ('='));
            std::erase (id, ' ');
            if (const auto* info = damp::findParam (id))
                preset.patch[info->pid] = set.patch[info->pid];
            return;
        }
        throw std::runtime_error ("unknown looper action '" + e.action + "'");
    };

    // A mono input comes out stereo (pedals after the amp may make it so)
    if (! stereo)
        right.assign (left.size(), 0.0f);
    const auto started = std::chrono::steady_clock::now();
    for (size_t pos = 0; ! dry && pos < left.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, left.size() - pos);
        while (nextEvent < loopEvents.size() && loopEvents[nextEvent].time * rate < (double) (pos + (size_t) n))
            runEvent (loopEvents[nextEvent++]);
        rig.setTempo (bpm);
        rig.setPatch (preset.patch);
        rig.process (left.data() + pos, right.data() + pos, n, ! stereo);
    }
    const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    const double length = (double) inFrames / rate;

    std::vector<float> out;
    out.reserve (2 * inFrames);
    for (size_t i = dry ? 0 : latency; i < (dry ? 0 : latency) + inFrames; ++i)
    {
        out.push_back (left[i]);
        out.push_back (stereo || ! dry ? right[i] : left[i]);
    }

    double peak = 0.0, sumSq = 0.0;
    bool finite = true;
    for (float s : out)
    {
        finite = finite && std::isfinite (s);
        peak = std::max (peak, (double) std::abs (s));
        sumSq += (double) s * s;
    }
    const double rms = std::sqrt (sumSq / (double) std::max<size_t> (1, out.size()));
    auto db = [] (double x) { return x > 0.0 ? 20.0 * std::log10 (x) : -999.0; };

    render::writeWav (outPath, out, rate);
    if (! loopSpec.empty())
    {
        for (int t = 0; t < damp::Looper::numTracks; ++t)
        {
            const auto s = looper.trackStatus (t);
            if (s.state == damp::Looper::State::Empty)
                continue;
            std::printf ("loop track %d: %.3fs recorded, loop %.3fs, period %.3fs%s%s\n", t + 1, (double) s.length / rate,
                         (double) s.loopLength / rate, (double) s.period / rate, s.master ? " (master)" : "",
                         s.sync ? "" : " (free)");
            std::vector<float> l, r, inter;
            if (! loopOut.empty() && looper.copyTrackAudio (t, l, r))
            {
                for (size_t i = 0; i < l.size(); ++i)
                {
                    inter.push_back (l[i]);
                    inter.push_back (r[i]);
                }
                render::writeWav (loopOut + std::to_string (t + 1) + ".wav", inter, rate);
            }
        }
    }
    std::printf ("%s: \"%s\" %.2fs %s peak %.1f dBFS rms %.1f dBFS, rendered in %.1f%% of real time%s\n", outPath.c_str(),
                 preset.name.empty() ? "(defaults)" : preset.name.c_str(), length, stereo ? "stereo" : "mono", db (peak),
                 db (rms), 100.0 * seconds / length, finite ? "" : " NON-FINITE SAMPLES");
    return finite ? 0 : 2;
}
catch (const std::exception& e)
{
    std::cerr << "error: " << e.what() << "\n";
    return 1;
}
