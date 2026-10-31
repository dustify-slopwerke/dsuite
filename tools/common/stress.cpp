// Headless stress test for a plugin: random sample rates, block sizes (1 .. far over the promised
// maximum), mono and stereo buses, MIDI (notes, pedals, all-notes/all-sound off, bends, pressure,
// MPE channels), parameter jumps to extremes, program changes, layer switches (dsynth) and state
// restores (saved, truncated, corrupted), some of it from a second "message" thread while the
// audio thread renders. Fails on a non-finite or runaway output sample. Meant to run under
// ASan/UBSan (or TSan).
//
//   <plugin>_stress [seconds = 60] [seed = 1]

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace
{
std::unique_ptr<juce::AudioProcessor> createPlugin()
{
    return std::unique_ptr<juce::AudioProcessor> (createPluginFilter());
}

float randomValue (juce::Random& rng)
{
    switch (rng.nextInt (4))
    {
        case 0: return 0.0f;
        case 1: return 1.0f;
        default: return rng.nextFloat();
    }
}

juce::MidiMessage randomMidi (juce::Random& rng)
{
    const int channel = 1 + (rng.nextInt (3) == 0 ? rng.nextInt (16) : 0);
    const int note = rng.nextInt (3) == 0 ? rng.nextInt (128) : 36 + rng.nextInt (48);
    switch (rng.nextInt (20))
    {
        case 0: case 1: case 2: case 3: case 4: case 5:
            return juce::MidiMessage::noteOn (channel, note, (juce::uint8) rng.nextInt (128));
        case 6: case 7: case 8: case 9: case 10:
            return juce::MidiMessage::noteOff (channel, note, (juce::uint8) rng.nextInt (128));
        case 11: return juce::MidiMessage::controllerEvent (channel, 64, rng.nextBool() ? 127 : rng.nextInt (128));
        case 12: return juce::MidiMessage::controllerEvent (channel, rng.nextBool() ? 66 : 67, rng.nextInt (128));
        case 13: return juce::MidiMessage::controllerEvent (channel, rng.nextBool() ? 1 : 74, rng.nextInt (128));
        case 14: return rng.nextInt (8) == 0 ? juce::MidiMessage::allSoundOff (channel) : juce::MidiMessage::allNotesOff (channel);
        case 15: return juce::MidiMessage::pitchWheel (channel, rng.nextBool() ? (rng.nextBool() ? 0 : 16383) : rng.nextInt (16384));
        case 16: return juce::MidiMessage::channelPressureChange (channel, rng.nextInt (128));
        case 17: return juce::MidiMessage::aftertouchChange (channel, note, rng.nextInt (128));
        case 18: return juce::MidiMessage::controllerEvent (channel, rng.nextInt (128), rng.nextInt (128));
        default: return juce::MidiMessage::programChange (channel, rng.nextInt (128));
    }
}

// What the "message thread" does: restores, program changes, layer switches, preset loads
void messageThreadAction (juce::AudioProcessor& plugin, juce::Random& rng, const std::vector<juce::MemoryBlock>& states)
{
    switch (rng.nextInt (6))
    {
        case 0:
            if (! states.empty())
            {
                const auto& s = states[(size_t) rng.nextInt ((int) states.size())];
                plugin.setStateInformation (s.getData(), (int) s.getSize());
            }
            break;
        case 1:
        {
            juce::MemoryBlock s;
            plugin.getStateInformation (s);
            if (s.getSize() > 0 && rng.nextBool())
            {
                // Corrupt or truncate it
                auto* bytes = static_cast<char*> (s.getData());
                if (rng.nextBool())
                    s.setSize ((size_t) rng.nextInt ((int) s.getSize()));
                else
                    for (int i = 0; i < 8; ++i)
                        bytes[rng.nextInt ((int) s.getSize())] = (char) rng.nextInt (256);
            }
            plugin.setStateInformation (s.getData(), (int) s.getSize());
            break;
        }
        case 2:
            plugin.setCurrentProgram (rng.nextInt (juce::jmax (1, plugin.getNumPrograms())));
            break;
        case 3:
#if DUSTIFY_STRESS_DSYNTH
            if (auto* d = dynamic_cast<DsynthAudioProcessor*> (&plugin))
            {
                if (rng.nextInt (4) == 0)
                    d->copyEditLayerToOther();
                else
                    d->setEditLayer (rng.nextInt (2));
            }
#endif
            break;
        default:
        {
            const auto& params = plugin.getParameters();
            for (int i = 0; i < 8; ++i)
                params[rng.nextInt (params.size())]->setValueNotifyingHost (randomValue (rng));
            break;
        }
    }
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    const double seconds = argc > 1 ? std::atof (argv[1]) : 60.0;
    const auto seed = argc > 2 ? std::atoll (argv[2]) : 1;
    juce::Random rng (seed);

    std::vector<juce::MemoryBlock> states;
    {
        auto p = createPlugin();
        for (int i = 0; i < p->getNumPrograms(); i += 3)
        {
            p->setCurrentProgram (i);
            states.emplace_back();
            p->getStateInformation (states.back());
        }
    }

    const auto start = juce::Time::getMillisecondCounterHiRes();
    long long blocks = 0, rounds = 0;
    int failures = 0;
    while (juce::Time::getMillisecondCounterHiRes() - start < seconds * 1000.0 && failures == 0)
    {
        ++rounds;
        auto plugin = createPlugin();
        const double rates[] { 22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
        const bool mono = rng.nextInt (5) == 0;
        plugin->setPlayConfigDetails (plugin->getTotalNumInputChannels() > 0 ? (mono ? 1 : 2) : 0, mono ? 1 : 2, 48000.0, 512);
        plugin->enableAllBuses();

        std::atomic<bool> stop { false };
        std::mutex messageLock; // the message thread's actions don't overlap prepareToPlay
        juce::Random messageRng (rng.nextInt64());
        const bool concurrent = rng.nextBool();
        std::thread message ([&]
        {
            while (concurrent && ! stop.load())
            {
                {
                    const std::lock_guard lock (messageLock);
                    messageThreadAction (*plugin, messageRng, states);
                }
                std::this_thread::sleep_for (std::chrono::microseconds (200 + messageRng.nextInt (3000)));
            }
        });

        for (int prep = 0; prep < 1 + rng.nextInt (3) && failures == 0; ++prep)
        {
            const double rate = rates[rng.nextInt (6)];
            const int maxBlock = rng.nextInt (4) == 0 ? 1 + rng.nextInt (16) : 32 << rng.nextInt (8);
            {
                const std::lock_guard lock (messageLock);
                plugin->setRateAndBufferSizeDetails (rate, maxBlock);
                plugin->prepareToPlay (rate, maxBlock);
            }
            juce::AudioBuffer<float> buffer (mono ? 1 : 2, maxBlock * 4 + 16);
            const int numBlocks = (int) (rate * (0.5 + 2.0 * rng.nextDouble()) / maxBlock) + 4;
            for (int b = 0; b < numBlocks && failures == 0; ++b)
            {
                int n = rng.nextInt (8) == 0 ? 1 + rng.nextInt (maxBlock) : maxBlock;
                if (rng.nextInt (200) == 0)
                    n = maxBlock * 4 + 16; // the host breaks its promise
                buffer.setSize (buffer.getNumChannels(), n, false, false, true);
                // An instrument must not read its (output-only) buffer: sometimes it holds NaNs
                const bool instrument = plugin->getTotalNumInputChannels() == 0;
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    for (int i = 0; i < n; ++i)
                        buffer.setSample (c, i, instrument ? (rng.nextInt (50) == 0 ? NAN : 0.0f) : 0.5f * rng.nextFloat() - 0.25f);

                juce::MidiBuffer midi;
                const int events = rng.nextInt (4) == 0 ? rng.nextInt (40) : rng.nextInt (3);
                for (int e = 0; e < events; ++e)
                    midi.addEvent (randomMidi (rng), rng.nextInt (n));

                if (! concurrent && rng.nextInt (60) == 0)
                    messageThreadAction (*plugin, rng, states);
                else if (rng.nextInt (6) == 0)
                {
                    // Automation on the audio thread
                    const auto& params = plugin->getParameters();
                    for (int i = 0; i < 1 + rng.nextInt (6); ++i)
                        params[rng.nextInt (params.size())]->setValue (randomValue (rng));
                }

                if (rng.nextInt (300) == 0)
                    plugin->processBlockBypassed (buffer, midi);
                else
                    plugin->processBlock (buffer, midi);
                ++blocks;

                for (int c = 0; c < buffer.getNumChannels() && failures == 0; ++c)
                    for (int i = 0; i < n; ++i)
                    {
                        const float x = buffer.getSample (c, i);
                        if (! std::isfinite (x) || std::abs (x) > 64.0f)
                        {
                            std::cerr << "FAIL: sample " << x << " at round " << rounds << " block " << b << " ch " << c
                                      << " i " << i << " (rate " << rate << ", block " << n << "/" << maxBlock
                                      << (mono ? ", mono" : "") << (concurrent ? ", concurrent" : "") << ")\n";
                            ++failures;
                            break;
                        }
                    }
            }
        }
        stop = true;
        message.join();
        plugin->releaseResources();
    }

    std::cout << (failures == 0 ? "OK" : "FAILED") << ": " << rounds << " instances, " << blocks << " blocks\n";
    return failures == 0 ? 0 : 1;
}
