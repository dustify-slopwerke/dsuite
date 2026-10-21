// Headless plugin-level checks that need JUCE (the DSP tests don't): state save/restore through
// a fresh instance, and parameter value <-> text round trips. Mirrors what pluginval and
// clap-validator check, so regressions show up in ctest without a host.

#include <juce_audio_processors/juce_audio_processors.h>

#if DUSTIFY_CHECKS_DAMP
 #include <juce_audio_formats/juce_audio_formats.h>
 #include "PluginProcessor.h"
 #include "dsp/PitchDetector.h"
#endif
#if DUSTIFY_CHECKS_DPIANO
 #include "PluginProcessor.h"
#endif

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>

namespace
{
int failures = 0;

// createPluginFilter() is the plugin's factory, declared by JUCE
std::unique_ptr<juce::AudioProcessor> createPlugin()
{
    return std::unique_ptr<juce::AudioProcessor> (createPluginFilter());
}

void fail (const juce::String& message)
{
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

// Sets a value the way plugin wrappers do: setValue, then tell the listeners
void wrapperSet (juce::AudioProcessorParameter& p, float normalised)
{
    p.setValue (normalised);
    p.sendValueChangedMessageToListeners (normalised);
}

void checkStateRoundTrip (bool viaWrapper)
{
    juce::Random rng (42);
    auto instanceA = createPlugin();
    auto& a = *instanceA;
    a.prepareToPlay (48000.0, 256);

    std::vector<float> expected;
    for (auto* p : a.getParameters())
    {
        const float v = rng.nextFloat();
        if (viaWrapper)
            wrapperSet (*p, v);
        else
            p->setValueNotifyingHost (v);
        expected.push_back (p->getValue());
    }

    juce::MemoryBlock state;
    a.getStateInformation (state);

    auto instanceB = createPlugin();
    auto& b = *instanceB;
    b.prepareToPlay (48000.0, 256);
    b.setStateInformation (state.getData(), (int) state.getSize());

    const auto params = b.getParameters();
    int mismatches = 0;
    for (int i = 0; i < params.size(); ++i)
    {
        // Compare what the parameter means (its text), since bools/choices quantise
        auto* pa = a.getParameters()[i];
        auto* pb = params[i];
        if (pa->getText (expected[(size_t) i], 64) != pb->getText (pb->getValue(), 64))
        {
            if (++mismatches <= 5)
                fail (juce::String (viaWrapper ? "[wrapper] " : "[notifying] ") + pb->getName (64) + ": saved "
                      + pa->getText (expected[(size_t) i], 64) + ", restored " + pb->getText (pb->getValue(), 64));
        }
    }
    if (mismatches > 5)
        fail (juce::String (mismatches - 5) + " more state mismatches");

    // Saving again must give identical bytes (deterministic state)
    juce::MemoryBlock again;
    b.getStateInformation (again);
    if (again != state)
    {
        // Show where: the state is binary-wrapped XML text
        const juce::String first ((const char*) state.getData() + 8, state.getSize() - 8);
        const juce::String second ((const char*) again.getData() + 8, again.getSize() - 8);
        int at = 0;
        while (at < first.length() && at < second.length() && first[at] == second[at])
            ++at;
        fail (juce::String (viaWrapper ? "[wrapper] " : "[notifying] ") + "re-saved state differs from the loaded one at:\n  saved:    "
              + first.substring (at - 60, at + 60).replace ("\n", " ") + "\n  re-saved: " + second.substring (at - 60, at + 60).replace ("\n", " "));
    }
}

// A state blob saved by an earlier build (tests/fixtures) must still load to the same parameter
// values and re-save to the same bytes, so refactors can't silently break users' projects.
// Regenerate only on a deliberate format change: <plugin>_plugin_checks --write-state <file>
juce::MemoryBlock fixtureState()
{
    juce::Random rng (7);
    auto instanceA = createPlugin();
    auto& a = *instanceA;
    a.prepareToPlay (48000.0, 256);
    for (auto* p : a.getParameters())
        p->setValueNotifyingHost (rng.nextFloat());
    juce::MemoryBlock state;
    a.getStateInformation (state);
    return state;
}

void checkSavedFixture (const juce::File& file)
{
    juce::MemoryBlock saved;
    if (! file.loadFileAsData (saved))
        return fail ("can't read state fixture " + file.getFullPathName());

    auto instanceB = createPlugin();
    auto& b = *instanceB;
    b.prepareToPlay (48000.0, 256);
    b.setStateInformation (saved.getData(), (int) saved.getSize());
    juce::MemoryBlock again;
    b.getStateInformation (again);

    // Every parameter it holds loads with its saved value (even when the bytes change, e.g. after
    // parameters were added)
    const auto before = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
    const auto after = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
    if (before == nullptr || after == nullptr)
        return fail ("state fixture " + file.getFileName() + " isn't XML");
    std::map<juce::String, juce::String> loaded;
    for (auto* p : after->getChildWithTagNameIterator ("PARAM"))
        loaded[p->getStringAttribute ("id")] = p->getStringAttribute ("value");
    int changed = 0;
    for (auto* p : before->getChildWithTagNameIterator ("PARAM"))
        if (const auto id = p->getStringAttribute ("id"); loaded[id] != p->getStringAttribute ("value") && ++changed <= 5)
            fail ("state fixture: " + id + " saved " + p->getStringAttribute ("value") + ", loads as " + loaded[id]);

    if (again != saved)
        fail ("state fixture " + file.getFileName() + " no longer round-trips byte for byte");
    if (fixtureState() != saved)
        fail ("the same random parameters no longer save to the fixture's bytes");
}

void checkTextRoundTrips()
{
    auto p = createPlugin();
    for (auto* param : p->getParameters())
    {
        for (int step = 0; step <= 110; ++step)
        {
            const float v = (float) step / 110.0f;
            const auto text = param->getText (v, 64);
            const float back = param->getValueForText (text);
            const auto text2 = param->getText (back, 64);
            if (text != text2)
            {
                fail (param->getName (64) + ": '" + text + "' -> " + juce::String (back) + " -> '" + text2 + "'");
                break;
            }
        }
    }
}
#if DUSTIFY_CHECKS_DPIANO
// dpiano's keyboard calibration: kept in the session, and an old session without it uses the
// machine-wide settings file (here in a temporary DUSTIFY_SETTINGS_DIR)
void checkVelocityCalibration()
{
    auto make = [] { return std::unique_ptr<DpianoAudioProcessor> (dynamic_cast<DpianoAudioProcessor*> (createPluginFilter())); };
    auto mapText = [] (const dpiano::VelocityMap& m)
    { return juce::String (m.inMin) + ".." + juce::String (m.inMax) + " @ " + juce::String (m.sensitivity); };
    const auto settings = DpianoAudioProcessor::settingsFile();
    settings.deleteFile();

    if (! make()->getVelocityMap().isIdentity())
        fail ("velocity map: not the identity without a settings file");

    dpiano::VelocityMap session;
    session.inMin = 0.1417f;
    session.inMax = 0.8189f;
    session.sensitivity = 0.25f;
    auto a = make();
    a->setVelocityMap (session, false);
    if (settings.exists())
        fail ("velocity map: applying it to one instance wrote the settings file");
    juce::MemoryBlock state;
    a->getStateInformation (state);

    auto b = make();
    b->setStateInformation (state.getData(), (int) state.getSize());
    if (b->getVelocityMap() != session)
        fail ("velocity map: session round trip gave " + mapText (b->getVelocityMap()));
    juce::MemoryBlock again;
    b->getStateInformation (again);
    if (again != state)
        fail ("velocity map: a session with calibration doesn't re-save byte for byte");

    // Calibrating writes the machine-wide file, which new instances pick up
    dpiano::VelocityMap machine;
    machine.inMin = 0.0787f;
    machine.inMax = 0.6299f;
    a->setVelocityMap (machine, true);
    if (! settings.existsAsFile())
        fail ("velocity map: calibrating didn't write " + settings.getFullPathName());
    if (DpianoAudioProcessor::loadMachineVelocityMap() != machine || make()->getVelocityMap() != machine)
        fail ("velocity map: the settings file loads as " + mapText (DpianoAudioProcessor::loadMachineVelocityMap()));

    // The session's calibration still wins over this machine's
    auto c = make();
    c->setStateInformation (state.getData(), (int) state.getSize());
    if (c->getVelocityMap() != session)
        fail ("velocity map: a session's calibration didn't win over the machine's");

    // A session saved before calibration existed uses the machine's
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    if (xml == nullptr || xml->getChildByName ("KEYBOARD") == nullptr)
        return fail ("velocity map: no KEYBOARD element in the session");
    xml->deleteAllChildElementsWithTagName ("KEYBOARD");
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*xml, old);
    c->setStateInformation (old.getData(), (int) old.getSize());
    if (c->getVelocityMap() != machine)
        fail ("velocity map: an old session loads " + mapText (c->getVelocityMap()) + ", not the machine's");

    // Out of range values in a session are made usable
    xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    auto* keyboard = xml->getChildByName ("KEYBOARD");
    keyboard->setAttribute ("velocityMin", "0.9");
    keyboard->setAttribute ("velocityMax", "0.2");
    keyboard->setAttribute ("sensitivity", "nan");
    juce::MemoryBlock bad;
    juce::AudioProcessor::copyXmlToBinary (*xml, bad);
    c->setStateInformation (bad.getData(), (int) bad.getSize());
    const auto sane = c->getVelocityMap();
    if (! (sane.inMin >= 0.0f && sane.inMax <= 1.0f && sane.inMax - sane.inMin >= dpiano::VelocityMap::minSpread - 1.0e-6f
           && std::isfinite (sane.sensitivity)))
        fail ("velocity map: bad session values load as " + mapText (sane));

    // The audio thread reports note-on velocities as played
    c->setVelocityMap ({}, false);
    c->prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 23), 10);
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 101), 20);
    c->processBlock (buffer, midi);
    float first = 0.0f, second = 0.0f, none = 0.0f;
    if (c->playedNoteCount() != 2 || std::abs (c->lastPlayedVelocity() - 101.0f / 127.0f) > 1.0e-6f
        || ! c->popPlayedVelocity (first) || ! c->popPlayedVelocity (second) || c->popPlayedVelocity (none)
        || std::abs (first - 23.0f / 127.0f) > 1.0e-6f || std::abs (second - 101.0f / 127.0f) > 1.0e-6f)
        fail ("velocity capture: played velocities aren't reported");

    settings.deleteFile(); // the fixture checks expect no calibration
}
#endif

#if DUSTIFY_CHECKS_DAMP
// damp keeps a loaded cabinet IR in its state: it must come back sample for sample
void checkImpulseInState()
{
    std::vector<float> impulse (3000);
    juce::Random rng (5);
    for (size_t i = 0; i < impulse.size(); ++i)
        impulse[i] = (rng.nextFloat() * 2.0f - 1.0f) * std::exp (-(float) i / 400.0f);

    DampAudioProcessor a;
    a.prepareToPlay (48000.0, 256);
    a.setUserImpulse ("Test Cab", impulse, 44100.0);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    DampAudioProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    double rate = 0.0;
    if (b.getUserImpulse (rate) != impulse || std::abs (rate - 44100.0) > 1.0e-9 || b.getUserImpulseName() != "Test Cab")
        fail ("the user IR didn't survive a state round trip");
    juce::MemoryBlock again;
    b.getStateInformation (again);
    if (again != state)
        fail ("state with a user IR doesn't re-save identically");

    // And a state without one clears it
    DampAudioProcessor c;
    juce::MemoryBlock empty;
    c.getStateInformation (empty);
    b.setStateInformation (empty.getData(), (int) empty.getSize());
    if (! b.getUserImpulse (rate).empty() || b.getUserImpulseName().isNotEmpty())
        fail ("loading a state without an IR left the old one in place");
}

// The pedal order is kept in the session and in presets
void checkBoardOrder()
{
    const auto custom = damp::BoardOrder::parse ("boost, rodent, mid_drive, big_fuzz, face_fuzz, distortion, blues_drive, amp");
    DampAudioProcessor a;
    a.setBoard (custom);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    DampAudioProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    if (b.getBoard() != custom)
        fail ("the board order didn't survive a state round trip: " + juce::String (b.getBoard().format()));

    DampAudioProcessor c;
    c.loadPresetText (a.savePresetText ("Board Test"));
    if (c.getBoard() != custom)
        fail ("the board order didn't survive a preset round trip: " + juce::String (c.getBoard().format()));
}

// The tuner end to end: audio through processBlock, the decimated feed, the detector
void checkTuner()
{
    DampAudioProcessor p;
    p.setPlayConfigDetails (2, 2, 44100.0, 512);
    p.prepareToPlay (44100.0, 512);
    p.setTunerActive (true);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    const double f = 82.41 * std::pow (2.0, 12.0 / 1200.0); // low E, 12 cents sharp
    double phase = 0.0;
    float loudest = 0.0f;
    for (int block = 0; block < 60; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const auto v = (float) (0.2 * std::sin (phase) + 0.1 * std::sin (2.0 * phase));
            phase += 2.0 * juce::MathConstants<double>::pi * f / 44100.0;
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
        }
        p.processBlock (buffer, midi);
        loudest = juce::jmax (loudest, buffer.getMagnitude (0, 512));
    }
    if (loudest > 0.0f)
        fail ("the output isn't muted while tuning");

    std::vector<float> feed (8192);
    const int got = p.readTunerSamples (feed.data(), (int) feed.size());
    damp::PitchDetector detector (p.tunerSampleRate(), 2048);
    if (got < 2048)
        return fail ("the tuner feed is short: " + juce::String (got) + " samples");
    const auto reading = damp::nearestNote (detector.detect (feed.data() + got - 2048).hz);
    if (reading.midiNote != 40 || std::abs (reading.cents - 12.0) > 1.0)
        fail ("tuner read note " + juce::String (reading.midiNote) + " " + juce::String (reading.cents, 1) + " cents (expected E2 +12)");
}

// Loops travel with the session (audio and edits, sample for sample), not with presets
void checkLoopsInState()
{
    DampAudioProcessor a;
    a.setPlayConfigDetails (2, 2, 48000.0, 256);
    a.prepareToPlay (48000.0, 256);
    auto& looper = a.getLooper();
    looper.setIdleTimeout (0.0); // this thread runs the audio too
    looper.createPool (48000.0, 30.0);
    auto* on = a.apvts.getParameter ("looper_on");
    auto* rec = a.apvts.getParameter ("looper_rec");
    on->setValueNotifyingHost (1.0f);

    // Track 1 recorded through processBlock with the footswitch parameter
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    juce::Random rng (9);
    auto blocks = [&] (int count, bool loud)
    {
        for (int b = 0; b < count; ++b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    buffer.setSample (ch, i, loud ? 0.2f * (rng.nextFloat() * 2.0f - 1.0f) : 0.0f);
            a.processBlock (buffer, midi);
        }
    };
    for (int press = 0; press < 2; ++press)
    {
        rec->setValueNotifyingHost (1.0f);
        blocks (1, true);
        rec->setValueNotifyingHost (0.0f);
        blocks (100, true);
    }
    // Track 3 from the API, with edits
    std::vector<float> l (30000), r (30000);
    for (size_t i = 0; i < l.size(); ++i)
    {
        l[i] = std::sin ((float) i * 0.01f);
        r[i] = -l[i];
    }
    looper.setTrackAudio (2, l.data(), r.data(), (int64_t) l.size());
    looper.setTrackEdits (2, -1000, 25000);
    looper.setTrackCrossfade (2, 20.0f);
    blocks (4, false);
    if (looper.trackStatus (0).length < 20000 || looper.trackStatus (2).length != 30000)
        return fail ("the looper didn't record (track 1: " + juce::String (looper.trackStatus (0).length) + " frames)");

    // Presets leave the loops (and the looper's switches) alone
    a.loadPresetText (a.savePresetText ("Loop Test"));
    if (looper.trackStatus (2).length != 30000 || on->getValue() < 0.5f)
        fail ("loading a preset touched the looper");

    juce::MemoryBlock state;
    a.getStateInformation (state);
    const auto expected = looper.saveSession();

    // The audio comes back as 24 bits at a power-of-two scale over each track's peak: within
    // half a step of it (see damp::looper::encodeSession)
    auto same = [] (const damp::looper::LoopSession& x, const damp::looper::LoopSession& y)
    {
        bool ok = x.masterTrack == y.masterTrack && x.masterLength == y.masterLength;
        for (size_t t = 0; t < x.tracks.size(); ++t)
        {
            const auto &p = x.tracks[t], &q = y.tracks[t];
            ok = ok && p.left.size() == q.left.size() && p.right.size() == q.right.size() && p.start == q.start && p.end == q.end
                 && std::abs (p.crossfadeMs - q.crossfadeMs) < 1.0e-6f && p.anchor == q.anchor;
            float peak = 0.0f, error = 0.0f;
            for (size_t i = 0; ok && i < p.left.size(); ++i)
            {
                peak = std::max ({ peak, std::abs (p.left[i]), std::abs (p.right[i]) });
                error = std::max ({ error, std::abs (p.left[i] - q.left[i]), std::abs (p.right[i] - q.right[i]) });
            }
            ok = ok && error <= peak * std::ldexp (1.0f, -23);
        }
        return ok;
    };

    // Into a running instance, and into one not yet prepared (the loops wait for the rate)
    for (bool prepared : { true, false })
    {
        DampAudioProcessor b;
        b.getLooper().setIdleTimeout (0.0);
        if (prepared)
            b.prepareToPlay (48000.0, 256);
        b.setStateInformation (state.getData(), (int) state.getSize());
        juce::MemoryBlock again;
        b.getStateInformation (again);
        if (again != state)
            fail (juce::String ("a session with loops doesn't re-save identically") + (prepared ? "" : " (before prepare)"));
        if (! prepared)
            b.prepareToPlay (48000.0, 256);
        if (! same (b.getLooper().saveSession(), expected))
            fail (juce::String ("the loops didn't survive a state round trip") + (prepared ? "" : " (before prepare)"));
    }

    // At another rate the loops are resampled
    DampAudioProcessor c;
    c.getLooper().setIdleTimeout (0.0);
    c.prepareToPlay (44100.0, 256);
    c.setStateInformation (state.getData(), (int) state.getSize());
    if (std::abs (c.getLooper().trackStatus (2).length - 27563) > 1)
        fail ("loops loaded at 44.1 kHz weren't resampled: " + juce::String (c.getLooper().trackStatus (2).length));

    // A session saved before the loops were compressed (a float32 'dlop' block, version 1) still
    // loads, sample for sample
    {
        DampAudioProcessor old;
        old.getLooper().setIdleTimeout (0.0);
        old.prepareToPlay (48000.0, 256);
        juce::MemoryBlock v1;
        DampAudioProcessor().getStateInformation (v1);
        const auto& t = expected.tracks[2];
        juce::MemoryOutputStream block;
        block.writeInt (0x706f6c64); // "dlop"
        block.writeInt (1);
        block.writeInt64 (8 + 4 + 8 + 4 + 8 * (8 + 8 + 8 + 4 + 8) + (juce::int64) t.left.size() * 8);
        block.writeDouble (48000.0);
        block.writeInt (2);    // the master track
        block.writeInt64 (26000);
        block.writeInt (8);
        for (int i = 0; i < 8; ++i)
        {
            const bool audio = i == 2;
            block.writeInt64 (audio ? (juce::int64) t.left.size() : 0);
            block.writeInt64 (audio ? t.start : 0);
            block.writeInt64 (audio ? t.end : 0);
            block.writeFloat (audio ? t.crossfadeMs : 5.0f);
            block.writeInt64 (0);
            if (audio)
            {
                block.write (t.left.data(), t.left.size() * sizeof (float));
                block.write (t.right.data(), t.right.size() * sizeof (float));
            }
        }
        v1.append (block.getData(), block.getDataSize());
        old.setStateInformation (v1.getData(), (int) v1.getSize());
        const auto loaded = old.getLooper().saveSession();
        if (loaded.tracks[2].left != t.left || loaded.tracks[2].right != t.right || loaded.tracks[2].start != t.start
            || loaded.tracks[2].end != t.end || loaded.masterTrack != 2 || loaded.masterLength != 26000)
            fail ("a version 1 (float32) looper session didn't load");
    }

    // A session without loops clears them
    DampAudioProcessor d;
    juce::MemoryBlock empty;
    d.getStateInformation (empty);
    a.setStateInformation (empty.getData(), (int) empty.getSize());
    if (looper.trackStatus (0).state != damp::Looper::State::Empty)
        fail ("loading a session without loops left the old ones");
}

// A session saved before the master output existed applied output_volume after the looper: it
// loads with that level moved into every loop's level (if it has loops), so loops and live sound
// keep their balance, and the parameters it doesn't mention go to their defaults
void checkOldSessionMigration()
{
    auto value = [] (DampAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
    auto set = [] (DampAudioProcessor& p, const char* id, float v)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (v));
    };
    // Saved without the parameters added with master_out
    auto oldSession = [&] (bool withLoops)
    {
        DampAudioProcessor a;
        a.getLooper().setIdleTimeout (0.0);
        a.prepareToPlay (48000.0, 256);
        a.getLooper().createPool (48000.0, 10.0);
        set (a, "output_volume", -9.0f);
        set (a, "track2_level", -3.0f);
        set (a, "track3_level", -44.0f);
        set (a, "track4_level", -48.0f);
        if (withLoops)
        {
            std::vector<float> l (20000, 0.1f);
            a.getLooper().setTrackAudio (0, l.data(), l.data(), (int64_t) l.size());
        }
        juce::MemoryBlock state;
        a.getStateInformation (state);
        auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        const auto xmlBytes = (size_t) juce::ByteOrder::littleEndianInt (juce::addBytesToPointer (state.getData(), 4)) + 9;
        for (const char* id : { "master_out", "tempo", "tempo_follow_host", "looper_quantise", "metronome_on", "metronome_level",
                                "metronome_accent", "beats_per_bar", "count_in", "metronome_rec_only" })
            if (auto* p = xml->getChildByAttribute ("id", id))
                xml->removeChildElement (p, true);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);
        if (state.getSize() > xmlBytes)
            old.append (juce::addBytesToPointer (state.getData(), (int) xmlBytes), state.getSize() - xmlBytes);
        return old;
    };

    for (bool withLoops : { true, false })
    {
        const auto old = oldSession (withLoops);
        DampAudioProcessor b;
        b.getLooper().setIdleTimeout (0.0);
        b.prepareToPlay (48000.0, 256);
        set (b, "master_out", 6.0f); // absent from the session: back to its default
        set (b, "tempo", 90.0f);
        b.setStateInformation (old.getData(), (int) old.getSize());
        const float shift = withLoops ? -9.0f : 0.0f;
        if (std::abs (value (b, "output_volume") + 9.0f) > 1.0e-4f || std::abs (value (b, "track1_level") - shift) > 1.0e-4f
            || std::abs (value (b, "track2_level") - (-3.0f + shift)) > 1.0e-4f
            || std::abs (value (b, "track3_level") - (withLoops ? -48.0f : -44.0f)) > 1.0e-4f
            || std::abs (value (b, "track4_level") + 48.0f) > 1.0e-4f)
            fail (juce::String ("an old session's loop levels didn't take its output volume") + (withLoops ? "" : " (no loops)") + ": track 1 "
                  + juce::String (value (b, "track1_level")) + " dB, track 2 " + juce::String (value (b, "track2_level")) + " dB");
        if (std::abs (value (b, "master_out")) > 1.0e-4f || std::abs (value (b, "tempo") - 120.0f) > 1.0e-3f)
            fail ("parameters an old session doesn't mention didn't go to their defaults");
        if (withLoops && b.getLooper().trackStatus (0).length != 20000)
            fail ("an old session's loops didn't load");
    }

    // A current session is left as it is
    DampAudioProcessor a;
    set (a, "output_volume", -9.0f);
    set (a, "master_out", 3.0f);
    juce::MemoryBlock state;
    a.getStateInformation (state);
    DampAudioProcessor c;
    c.setStateInformation (state.getData(), (int) state.getSize());
    if (std::abs (value (c, "track1_level")) > 1.0e-4f || std::abs (value (c, "master_out") - 3.0f) > 1.0e-4f)
        fail ("a current session's levels changed on loading");
}

// --session-bench a.wav b.wav ...: fills a full looper pool (8 tracks, 5 minutes at 48 kHz)
// with the files' audio, cycled, and reports what the session's loops cost against float32, and
// how long saving and loading them take
int sessionBench (const juce::StringArray& files)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<std::vector<float>> sources;
    for (const auto& name : files)
        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (juce::File (name)) })
        {
            juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);
            sources.emplace_back (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        }
    if (sources.empty())
        return 1;

    DampAudioProcessor a;
    a.getLooper().setIdleTimeout (0.0);
    a.prepareToPlay (48000.0, 256);
    auto& looper = a.getLooper();
    looper.createPool (48000.0);
    const auto chunks = (int64_t) std::ceil (damp::Looper::poolSeconds * 48000.0 / (double) damp::looper::LoopPool::chunkFrames);
    const int64_t perTrack = chunks / 8 * damp::looper::LoopPool::chunkFrames;
    for (int t = 0; t < 8; ++t)
    {
        const auto& src = sources[(size_t) t % sources.size()];
        std::vector<float> l ((size_t) perTrack), r ((size_t) perTrack);
        for (size_t i = 0; i < l.size(); ++i)
        {
            l[i] = src[(i + (size_t) t * 7919) % src.size()];
            r[i] = src[(i + (size_t) t * 7919 + 480) % src.size()]; // a little width
        }
        looper.setTrackAudio (t, l.data(), r.data(), perTrack);
    }

    using Clock = std::chrono::steady_clock;
    auto ms = [] (Clock::time_point from) { return std::chrono::duration<double, std::milli> (Clock::now() - from).count(); };
    juce::MemoryBlock state;
    auto started = Clock::now();
    a.getStateInformation (state);
    const double save = ms (started);
    started = Clock::now();
    const auto session = looper.saveSession();
    const double copy = ms (started);
    std::vector<std::byte> v1;
    v1.reserve (session.frames() * 8 + 1024);
    started = Clock::now();
    damp::looper::encodeSession (session, v1);
    const double encodeFloat = ms (started);

    DampAudioProcessor b;
    b.getLooper().setIdleTimeout (0.0);
    b.prepareToPlay (48000.0, 256);
    started = Clock::now();
    b.setStateInformation (state.getData(), (int) state.getSize());
    const double load = ms (started);

    const double seconds = (double) session.frames() / 48000.0;
    std::cout << "session bench: " << sources.size() << " source file(s), 8 tracks, " << juce::String (seconds, 1) << " s of stereo\n"
              << "  float32 loops:    " << juce::String ((double) v1.size() / 1.0e6, 2) << " MB\n"
              << "  session (FLAC):   " << juce::String ((double) state.getSize() / 1.0e6, 2) << " MB ("
              << juce::String (100.0 * (double) state.getSize() / (double) v1.size(), 1) << "% of float32)\n"
              << "  save (getStateInformation): " << juce::String (save, 1) << " ms, of which copying the loops out "
              << juce::String (copy, 1) << " ms (float32 encoding alone: " << juce::String (encodeFloat, 1) << " ms)\n"
              << "  load (setStateInformation): " << juce::String (load, 1) << " ms\n";
    return 0;
}
#endif
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;

    // Settings files (dpiano's keyboard calibration) go to a temporary folder, never the user's
    const auto settingsDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("dustify-checks", "");
#if JUCE_WINDOWS
    _putenv_s ("DUSTIFY_SETTINGS_DIR", settingsDir.getFullPathName().toRawUTF8());
#else
    setenv ("DUSTIFY_SETTINGS_DIR", settingsDir.getFullPathName().toRawUTF8(), 1);
#endif
    const struct RemoveDir { juce::File dir; ~RemoveDir() { dir.deleteRecursively(); } } removeSettingsDir { settingsDir };

    if (argc == 3 && juce::String (argv[1]) == "--write-state")
    {
        const auto state = fixtureState();
        return juce::File (argv[2]).replaceWithData (state.getData(), state.getSize()) ? 0 : 1;
    }

#if DUSTIFY_CHECKS_DAMP
    if (argc >= 3 && juce::String (argv[1]) == "--session-bench")
    {
        juce::StringArray files;
        for (int i = 2; i < argc; ++i)
            files.add (argv[i]);
        return sessionBench (files);
    }
#endif

    checkStateRoundTrip (false);
    checkStateRoundTrip (true);
    checkTextRoundTrips();
#if DUSTIFY_CHECKS_DAMP
    checkImpulseInState();
    checkBoardOrder();
    checkTuner();
    checkLoopsInState();
    checkOldSessionMigration();
#endif
#if DUSTIFY_CHECKS_DPIANO
    checkVelocityCalibration();
#endif
    if (argc == 2)
        checkSavedFixture (juce::File (argv[1]));

    std::cout << (failures == 0 ? "plugin checks passed\n" : "plugin checks FAILED\n");
    return failures == 0 ? 0 : 1;
}
